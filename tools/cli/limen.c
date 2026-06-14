/*
 * limen — command-line client for the limen VCV Rack module
 *
 * Usage:
 *   limen [--port N] [--host H] [--json] <command> [args]
 *
 * Commands:
 *   plugins                                      list all loaded plugins
 *   models [<plugin-slug>]                       list available models (opt. plugin filter)
 *   modules [<plugin-slug>]                      list modules currently in the rack (opt. plugin filter)
 *   cables [-v] [<module-id>]                    list cables (opt. module filter, -v for verbose)
 *   ports <module-id>                            list input/output port names
 *   params <module-id>                           list params for a module
 *   set <module-id> <param-id> <value>           set a parameter value
 *   get <module-id>                              get module detail
 *   add <plugin-slug> <model-slug>               add a module to the patch
 *   rm <module-id>                               remove a module from the patch
 *   connect <out-mod>:<out-port> <in-mod>:<in-port>  connect two ports with a cable
 *   disconnect <cable-id>                        remove a cable from the patch
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#define BUF_SIZE (1 << 20)  /* 1 MB response buffer */

static int   opt_port = 7000;
static char  opt_host[64] = "127.0.0.1";
static int   opt_json = 0;

/* ── socket helpers ─────────────────────────────────────────────────────── */

static int connect_to(const char *host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((unsigned short)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        fprintf(stderr, "limen: invalid host: %s\n", host);
        close(fd);
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "limen: cannot connect to %s:%d: %s\n",
                host, port, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

/* Send a line, read one line response. Returns malloc'd string or NULL. */
static char *transact(int fd, const char *req) {
    size_t len = strlen(req);
    if (write(fd, req, len) != (ssize_t)len) {
        perror("write");
        return NULL;
    }

    char *buf = malloc(BUF_SIZE);
    if (!buf) { perror("malloc"); return NULL; }
    size_t pos = 0;
    for (;;) {
        if (pos >= BUF_SIZE - 1) {
            fprintf(stderr, "limen: response too large\n");
            free(buf);
            return NULL;
        }
        ssize_t n = read(fd, buf + pos, BUF_SIZE - 1 - pos);
        if (n < 0) { perror("read"); free(buf); return NULL; }
        if (n == 0) break;
        pos += (size_t)n;
        if (memchr(buf, '\n', pos)) break;
    }
    buf[pos] = '\0';
    /* strip trailing newline */
    if (pos > 0 && buf[pos - 1] == '\n') buf[pos - 1] = '\0';
    return buf;
}

/* ── minimal JSON extraction ────────────────────────────────────────────── */

/* Returns 1 if JSON has "ok":true, 0 otherwise. */
static int json_ok(const char *s) {
    return strstr(s, "\"ok\":true") != NULL;
}

/* Returns pointer to the value of "error" key in s, or NULL.
   Caller should NOT free the returned pointer (points into s). */
static const char *json_error_msg(const char *s) {
    const char *p = strstr(s, "\"error\":");
    if (!p) return NULL;
    p += 8;
    while (*p == ' ') p++;
    if (*p != '"') return NULL;
    return p + 1;  /* points at start of error string (may contain escape) */
}

/* Print error and return 1. */
static int print_error(const char *resp) {
    const char *msg = json_error_msg(resp);
    if (msg) {
        /* print until closing quote */
        fprintf(stderr, "limen: error: ");
        while (*msg && *msg != '"') fputc(*msg++, stderr);
        fputc('\n', stderr);
    } else {
        fprintf(stderr, "limen: unknown error\n");
    }
    return 1;
}

/*
 * Iterate over a JSON array of objects in "result":[...].
 * Calls cb(item_json, user) for each item.
 */
typedef void (*item_cb)(const char *, void *);

static void each_result_item(const char *resp, item_cb cb, void *user) {
    const char *p = strstr(resp, "\"result\":[");
    if (!p) return;
    p += 9; /* point at '[' */
    int depth = 0;
    const char *item_start = NULL;
    for (; *p; p++) {
        if (*p == '{') {
            if (depth == 0) item_start = p;
            depth++;
        } else if (*p == '}') {
            depth--;
            if (depth == 0 && item_start) {
                /* extract item_start..p inclusive as a substring */
                size_t len = (size_t)(p - item_start + 1);
                char *item = malloc(len + 1);
                if (!item) break;
                memcpy(item, item_start, len);
                item[len] = '\0';
                cb(item, user);
                free(item);
                item_start = NULL;
            }
        } else if (*p == ']' && depth == 0) {
            break;
        }
    }
}

/* Extract a string field value from a flat JSON object.
   Writes into dest up to dest_size bytes. Returns 1 on success. */
static int json_str(const char *obj, const char *key, char *dest, size_t dest_size) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(obj, search);
    if (!p) return 0;
    p += strlen(search);
    while (*p == ' ') p++;
    if (*p != '"') return 0;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i < dest_size - 1) {
        if (*p == '\\' && *(p + 1)) {
            p++;
            switch (*p) {
                case '"':  dest[i++] = '"';  break;
                case '\\': dest[i++] = '\\'; break;
                case '/':  dest[i++] = '/';  break;
                case 'n':  dest[i++] = '\n'; break;
                case 'r':  dest[i++] = '\r'; break;
                case 't':  dest[i++] = '\t'; break;
                default:   dest[i++] = *p;   break;
            }
        } else {
            dest[i++] = *p;
        }
        p++;
    }
    dest[i] = '\0';
    return 1;
}

/* Extract a numeric field (integer or float) as double. Returns 1 on success. */
static int json_num(const char *obj, const char *key, double *out) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(obj, search);
    if (!p) return 0;
    p += strlen(search);
    while (*p == ' ') p++;
    char *end;
    *out = strtod(p, &end);
    return end != p;
}

/* Extract an integer field. Returns 1 on success. */
static int json_int64(const char *obj, const char *key, long long *out) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(obj, search);
    if (!p) return 0;
    p += strlen(search);
    while (*p == ' ') p++;
    char *end;
    *out = strtoll(p, &end, 10);
    return end != p;
}

/* ── id prefix resolution ────────────────────────────────────────────────── */

/*
 * Resolve a prefix against the "id" field of items returned by list_req.
 * If prefix uniquely matches one item, returns that ID.
 * On ambiguity or no match, prints an error and returns -1.
 */
static long long resolve_prefix(int fd, const char *list_req,
                                const char *kind, const char *prefix) {
    char *resp = transact(fd, list_req);
    if (!resp) return -1;
    if (!json_ok(resp)) { print_error(resp); free(resp); return -1; }

    size_t prefix_len = strlen(prefix);
    long long matched_id = -1;
    int match_count = 0;

    const char *p = strstr(resp, "\"result\":[");
    if (!p) { free(resp); return -1; }
    p += 9; /* point at '[' */
    int depth = 0;
    const char *item_start = NULL;
    for (; *p; p++) {
        if (*p == '{') {
            if (depth == 0) item_start = p;
            depth++;
        } else if (*p == '}') {
            depth--;
            if (depth == 0 && item_start) {
                size_t len = (size_t)(p - item_start + 1);
                char *item = malloc(len + 1);
                if (!item) break;
                memcpy(item, item_start, len);
                item[len] = '\0';
                long long id;
                if (json_int64(item, "id", &id)) {
                    char id_str[32];
                    snprintf(id_str, sizeof(id_str), "%lld", id);
                    if (strncmp(id_str, prefix, prefix_len) == 0) {
                        matched_id = id;
                        match_count++;
                    }
                }
                free(item);
                item_start = NULL;
            }
        } else if (*p == ']' && depth == 0) {
            break;
        }
    }
    free(resp);

    if (match_count == 0) {
        fprintf(stderr, "limen: no %s matches '%s'\n", kind, prefix);
        return -1;
    }
    if (match_count > 1) {
        fprintf(stderr, "limen: ambiguous prefix '%s' matches %d %ss\n",
                prefix, match_count, kind);
        return -1;
    }
    return matched_id;
}

static long long resolve_id(int fd, const char *prefix) {
    return resolve_prefix(fd, "{\"cmd\":\"list_modules\"}\n", "module", prefix);
}

static long long resolve_cable_id(int fd, const char *prefix) {
    return resolve_prefix(fd, "{\"cmd\":\"list_cables\"}\n", "cable", prefix);
}

/* ── command implementations ────────────────────────────────────────────── */

static void print_plugin_item(const char *item, void *user) {
    (void)user;
    char slug[64], name[64], version[32];
    json_str(item, "slug",    slug,    sizeof(slug));
    json_str(item, "name",    name,    sizeof(name));
    json_str(item, "version", version, sizeof(version));
    printf("%-24s  %-32s  %s\n", slug, name, version);
}

static int cmd_hello(int fd) {
    char req[] = "{\"cmd\":\"hello\"}\n";
    char *resp = transact(fd, req);
    if (!resp) return 1;
    if (opt_json) { puts(resp); free(resp); return 0; }
    if (!json_ok(resp)) { int r = print_error(resp); free(resp); return r; }
    long long proto = 0;
    json_int64(resp, "protocol", &proto);
    printf("protocol %lld\n", proto);
    const char *c = strstr(resp, "\"commands\":[");
    if (c) {
        c += strlen("\"commands\":[");
        fputs("commands: ", stdout);
        int first = 1;
        while (*c && *c != ']') {
            if (*c == '"') {
                c++;
                if (!first) fputs(", ", stdout);
                first = 0;
                while (*c && *c != '"') putchar(*c++);
                if (*c == '"') c++;
            } else {
                c++;
            }
        }
        putchar('\n');
    }
    free(resp);
    return 0;
}

static int cmd_plugins(int fd) {
    char req[] = "{\"cmd\":\"list_plugins\"}\n";
    char *resp = transact(fd, req);
    if (!resp) return 1;
    if (opt_json) { puts(resp); free(resp); return 0; }
    if (!json_ok(resp)) { int r = print_error(resp); free(resp); return r; }
    printf("%-24s  %-32s  %s\n", "slug", "name", "version");
    each_result_item(resp, print_plugin_item, NULL);
    free(resp);
    return 0;
}

static void print_model_item(const char *item, void *user) {
    (void)user;
    char plugin[64], slug[64], name[64], desc[128];
    json_str(item, "plugin",      plugin, sizeof(plugin));
    json_str(item, "slug",        slug,   sizeof(slug));
    json_str(item, "name",        name,   sizeof(name));
    json_str(item, "description", desc,   sizeof(desc));
    printf("%-24s  %-24s  %-24s  %s\n", plugin, slug, name, desc);
}

static int cmd_models(int fd, const char *plugin_slug) {
    char req[256];
    if (plugin_slug)
        snprintf(req, sizeof(req),
                 "{\"cmd\":\"list_models\",\"plugin\":\"%s\"}\n", plugin_slug);
    else
        snprintf(req, sizeof(req), "{\"cmd\":\"list_models\"}\n");
    char *resp = transact(fd, req);
    if (!resp) return 1;
    if (opt_json) { puts(resp); free(resp); return 0; }
    if (!json_ok(resp)) { int r = print_error(resp); free(resp); return r; }
    printf("%-24s  %-24s  %-24s  %s\n", "plugin", "slug", "name", "description");
    each_result_item(resp, print_model_item, NULL);
    free(resp);
    return 0;
}

static void print_module_item(const char *item, void *user) {
    (void)user;
    long long id; char plugin[64], model[64], name[64];
    double np, ni, no;
    json_int64(item, "id",         &id);
    json_str  (item, "plugin",  plugin,  sizeof(plugin));
    json_str  (item, "model",   model,   sizeof(model));
    json_str  (item, "name",    name,    sizeof(name));
    json_num  (item, "numParams", &np);
    json_num  (item, "numInputs", &ni);
    json_num  (item, "numOutputs",&no);
    printf("%-22lld  %-12s  %-16s  %-16s  %3d  %3d  %3d\n",
           id, plugin, model, name, (int)np, (int)ni, (int)no);
}

static int cmd_modules(int fd, const char *plugin_slug) {
    char req[256];
    if (plugin_slug) {
        snprintf(req, sizeof(req),
                 "{\"cmd\":\"list_modules\",\"plugin\":\"%s\"}\n", plugin_slug);
    } else {
        snprintf(req, sizeof(req), "{\"cmd\":\"list_modules\"}\n");
    }
    char *resp = transact(fd, req);
    if (!resp) return 1;
    if (opt_json) { puts(resp); free(resp); return 0; }
    if (!json_ok(resp)) { int r = print_error(resp); free(resp); return r; }
    printf("%-22s  %-12s  %-16s  %-16s  %3s  %3s  %3s\n",
           "id", "plugin", "model", "name", "par", "in", "out");
    each_result_item(resp, print_module_item, NULL);
    free(resp);
    return 0;
}

static int cmd_get(int fd, long long id) {
    char req[128];
    snprintf(req, sizeof(req), "{\"cmd\":\"get_module\",\"id\":%lld}\n", id);
    char *resp = transact(fd, req);
    if (!resp) return 1;
    if (opt_json) { puts(resp); free(resp); return 0; }
    if (!json_ok(resp)) { int r = print_error(resp); free(resp); return r; }
    /* re-use print_module_item on the result object */
    const char *p = strstr(resp, "\"result\":{");
    if (p) {
        p += 9;
        /* find matching } */
        const char *start = p;
        int depth = 0;
        for (; *p; p++) {
            if (*p == '{') depth++;
            else if (*p == '}') { depth--; if (!depth) break; }
        }
        size_t len = (size_t)(p - start + 1);
        char *item = malloc(len + 1);
        if (item) {
            memcpy(item, start, len); item[len] = '\0';
            printf("%-22s  %-12s  %-16s  %-16s  %3s  %3s  %3s\n",
                   "id", "plugin", "model", "name", "par", "in", "out");
            print_module_item(item, NULL);
            free(item);
        }
    }
    free(resp);
    return 0;
}

static void print_param_item(const char *item, void *user) {
    (void)user;
    long long id; double value, min, max; char name[64], unit[32];
    json_int64(item, "id",    &id);
    json_num  (item, "value", &value);
    json_str  (item, "name",  name,  sizeof(name));
    json_num  (item, "min",   &min);
    json_num  (item, "max",   &max);
    json_str  (item, "unit",  unit,  sizeof(unit));
    printf("%3lld  %-20s  %7.3f  %7.3f  %7.3f  %s\n",
           id, name, value, min, max, unit);
}

static int cmd_params(int fd, long long id) {
    char req[128];
    snprintf(req, sizeof(req), "{\"cmd\":\"list_params\",\"id\":%lld}\n", id);
    char *resp = transact(fd, req);
    if (!resp) return 1;
    if (opt_json) { puts(resp); free(resp); return 0; }
    if (!json_ok(resp)) { int r = print_error(resp); free(resp); return r; }
    printf("%3s  %-20s  %7s  %7s  %7s  %s\n",
           "id", "name", "value", "min", "max", "unit");
    each_result_item(resp, print_param_item, NULL);
    free(resp);
    return 0;
}

static int cmd_param(int fd, long long modid, int paramid) {
    char req[128];
    snprintf(req, sizeof(req),
             "{\"cmd\":\"get_param\",\"id\":%lld,\"param\":%d}\n", modid, paramid);
    char *resp = transact(fd, req);
    if (!resp) return 1;
    if (opt_json) { puts(resp); free(resp); return 0; }
    if (!json_ok(resp)) { int r = print_error(resp); free(resp); return r; }
    const char *obj = strstr(resp, "\"result\":");
    if (obj) obj += 9; else obj = resp;
    printf("%3s  %-20s  %7s  %7s  %7s  %s\n",
           "id", "name", "value", "min", "max", "unit");
    print_param_item(obj, NULL);
    free(resp);
    return 0;
}

static int cmd_set(int fd, long long modid, int paramid, double value) {
    char req[256];
    snprintf(req, sizeof(req),
             "{\"cmd\":\"set_param\",\"id\":%lld,\"param\":%d,\"value\":%.6g}\n",
             modid, paramid, value);
    char *resp = transact(fd, req);
    if (!resp) return 1;
    if (opt_json) { puts(resp); free(resp); return 0; }
    if (!json_ok(resp)) { int r = print_error(resp); free(resp); return r; }
    puts("ok");
    free(resp);
    return 0;
}

static void print_cable_item(const char *item, void *user) {
    int verbose = *(int *)user;
    long long id, om, op, im, ip;
    json_int64(item, "id",           &id);
    json_int64(item, "outputModule", &om);
    json_int64(item, "outputPort",   &op);
    json_int64(item, "inputModule",  &im);
    json_int64(item, "inputPort",    &ip);
    if (verbose) {
        char omn[64], opn[64], imn[64], ipn[64];
        json_str(item, "outputModuleName", omn, sizeof(omn));
        json_str(item, "outputPortName",   opn, sizeof(opn));
        json_str(item, "inputModuleName",  imn, sizeof(imn));
        json_str(item, "inputPortName",    ipn, sizeof(ipn));
        printf("%20lld  %20lld %-16s  :%lld %-16s  →  %20lld %-16s  :%lld %-16s\n",
               id, om, omn, op, opn, im, imn, ip, ipn);
    } else {
        printf("%20lld  %20lld:%lld  →  %20lld:%lld\n", id, om, op, im, ip);
    }
}

static int cmd_cables(int fd, long long module_id, int verbose) {
    char req[256];
    if (module_id >= 0 && verbose)
        snprintf(req, sizeof(req),
                 "{\"cmd\":\"list_cables\",\"id\":%lld,\"verbose\":true}\n", module_id);
    else if (module_id >= 0)
        snprintf(req, sizeof(req),
                 "{\"cmd\":\"list_cables\",\"id\":%lld}\n", module_id);
    else if (verbose)
        snprintf(req, sizeof(req), "{\"cmd\":\"list_cables\",\"verbose\":true}\n");
    else
        snprintf(req, sizeof(req), "{\"cmd\":\"list_cables\"}\n");
    char *resp = transact(fd, req);
    if (!resp) return 1;
    if (opt_json) { puts(resp); free(resp); return 0; }
    if (!json_ok(resp)) { int r = print_error(resp); free(resp); return r; }
    if (strstr(resp, "\"result\":[]")) {
        puts("(no cables)");
        free(resp);
        return 0;
    }
    if (verbose)
        printf("%20s  %20s %-16s   %-3s %-16s      %20s %-16s   %-3s %-16s\n",
               "cable-id", "out-module-id", "out-module", "out", "out-port",
               "in-module-id", "in-module", "in", "in-port");
    else
        printf("%20s  %20s       %20s\n", "cable-id", "out-module:port", "in-module:port");
    each_result_item(resp, print_cable_item, &verbose);
    free(resp);
    return 0;
}

static int cmd_add(int fd, const char *plugin_slug, const char *model_slug) {
    char req[512];
    snprintf(req, sizeof(req),
             "{\"cmd\":\"add_module\",\"plugin\":\"%s\",\"model\":\"%s\"}\n",
             plugin_slug, model_slug);
    char *resp = transact(fd, req);
    if (!resp) return 1;
    if (opt_json) { puts(resp); free(resp); return 0; }
    if (!json_ok(resp)) { int r = print_error(resp); free(resp); return r; }
    long long id = 0;
    const char *p = strstr(resp, "\"result\":{");
    if (p) {
        p += 9;
        size_t start = 0;
        const char *s = p;
        /* find end of result object */
        int depth = 0;
        for (; *s; s++) {
            if (*s == '{') depth++;
            else if (*s == '}') { depth--; if (!depth) break; }
        }
        (void)start;
        size_t len = (size_t)(s - p + 1);
        char *item = malloc(len + 1);
        if (item) {
            memcpy(item, p, len); item[len] = '\0';
            json_int64(item, "id", &id);
            free(item);
        }
    }
    printf("%lld\n", id);
    free(resp);
    return 0;
}

static int cmd_rm(int fd, const char *id_prefix) {
    long long id = resolve_id(fd, id_prefix);
    if (id < 0) return 1;
    char req[128];
    snprintf(req, sizeof(req), "{\"cmd\":\"remove_module\",\"id\":%lld}\n", id);
    char *resp = transact(fd, req);
    if (!resp) return 1;
    if (opt_json) { puts(resp); free(resp); return 0; }
    if (!json_ok(resp)) { int r = print_error(resp); free(resp); return r; }
    puts("ok");
    free(resp);
    return 0;
}

/*
 * Parse "modid:port" where modid may be a prefix.
 * Returns 1 on success, sets *mod_id and *port.
 */
static int parse_endpoint(int fd, const char *arg, long long *mod_id, int *port) {
    const char *colon = strrchr(arg, ':');
    if (!colon) {
        fprintf(stderr, "limen: expected <module-id>:<port>, got: %s\n", arg);
        return 0;
    }
    /* extract module prefix */
    size_t prefix_len = (size_t)(colon - arg);
    char prefix[64];
    if (prefix_len >= sizeof(prefix)) {
        fprintf(stderr, "limen: module id too long\n");
        return 0;
    }
    memcpy(prefix, arg, prefix_len);
    prefix[prefix_len] = '\0';

    *mod_id = resolve_id(fd, prefix);
    if (*mod_id < 0) return 0;

    char *end;
    long p = strtol(colon + 1, &end, 10);
    if (end == colon + 1 || *end != '\0') {
        fprintf(stderr, "limen: invalid port: %s\n", colon + 1);
        return 0;
    }
    *port = (int)p;
    return 1;
}

static int cmd_connect(int fd, const char *out_arg, const char *in_arg) {
    long long out_mod, in_mod;
    int out_port, in_port;
    if (!parse_endpoint(fd, out_arg, &out_mod, &out_port)) return 1;
    if (!parse_endpoint(fd, in_arg,  &in_mod,  &in_port))  return 1;

    char req[256];
    snprintf(req, sizeof(req),
             "{\"cmd\":\"add_cable\","
             "\"outputModule\":%lld,\"outputPort\":%d,"
             "\"inputModule\":%lld,\"inputPort\":%d}\n",
             out_mod, out_port, in_mod, in_port);
    char *resp = transact(fd, req);
    if (!resp) return 1;
    if (opt_json) { puts(resp); free(resp); return 0; }
    if (!json_ok(resp)) { int r = print_error(resp); free(resp); return r; }
    long long id = 0;
    const char *p = strstr(resp, "\"result\":{");
    if (p) {
        p += 9;
        const char *s = p;
        int depth = 0;
        for (; *s; s++) {
            if (*s == '{') depth++;
            else if (*s == '}') { depth--; if (!depth) break; }
        }
        size_t len = (size_t)(s - p + 1);
        char *item = malloc(len + 1);
        if (item) {
            memcpy(item, p, len); item[len] = '\0';
            json_int64(item, "id", &id);
            free(item);
        }
    }
    printf("%lld\n", id);
    free(resp);
    return 0;
}

static void print_port_item(const char *item, void *user) {
    const char *label = (const char *)user;
    long long id; char name[64];
    json_int64(item, "id",   &id);
    json_str  (item, "name", name, sizeof(name));
    printf("  %s %2lld  %s\n", label, id, name);
}

/* Iterate items of a named sub-array inside "result":{...}. */
static void each_subarray_item(const char *resp, const char *key, item_cb cb, void *user) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":[", key);
    const char *p = strstr(resp, search);
    if (!p) return;
    p += strlen(search) - 1; /* point at '[' */
    int depth = 0;
    const char *item_start = NULL;
    for (; *p; p++) {
        if (*p == '{') {
            if (depth == 0) item_start = p;
            depth++;
        } else if (*p == '}') {
            depth--;
            if (depth == 0 && item_start) {
                size_t len = (size_t)(p - item_start + 1);
                char *item = malloc(len + 1);
                if (!item) break;
                memcpy(item, item_start, len);
                item[len] = '\0';
                cb(item, user);
                free(item);
                item_start = NULL;
            }
        } else if (*p == ']' && depth == 0) {
            break;
        }
    }
}

static int cmd_ports(int fd, long long id) {
    char req[128];
    snprintf(req, sizeof(req), "{\"cmd\":\"list_ports\",\"id\":%lld}\n", id);
    char *resp = transact(fd, req);
    if (!resp) return 1;
    if (opt_json) { puts(resp); free(resp); return 0; }
    if (!json_ok(resp)) { int r = print_error(resp); free(resp); return r; }
    puts("inputs:");
    each_subarray_item(resp, "inputs",  print_port_item, (void *)"in ");
    puts("outputs:");
    each_subarray_item(resp, "outputs", print_port_item, (void *)"out");
    free(resp);
    return 0;
}

static int cmd_disconnect(int fd, const char *id_str) {
    long long id = resolve_cable_id(fd, id_str);
    if (id < 0) return 1;
    char req[128];
    snprintf(req, sizeof(req), "{\"cmd\":\"remove_cable\",\"id\":%lld}\n", id);
    char *resp = transact(fd, req);
    if (!resp) return 1;
    if (opt_json) { puts(resp); free(resp); return 0; }
    if (!json_ok(resp)) { int r = print_error(resp); free(resp); return r; }
    puts("ok");
    free(resp);
    return 0;
}

/* ── main ───────────────────────────────────────────────────────────────── */

static void usage(void) {
    fprintf(stderr,
        "usage: limen [--port N] [--host H] [--json] <command> [args]\n"
        "\n"
        "commands:\n"
        "  hello                                        protocol version and supported commands\n"
        "  plugins                                      list all loaded plugins\n"
        "  models [<plugin-slug>]                       list available models (opt. plugin filter)\n"
        "  modules [<plugin-slug>]                      list modules currently in the rack (opt. plugin filter)\n"
        "  cables [-v] [<module-id>]                    list cables (opt. module filter, -v for names)\n"
        "  ports <module-id>                            list input/output port names\n"
        "  params <module-id>                           list params for a module\n"
        "  param <module-id> <param-id>                 get a single parameter value\n"
        "  set <module-id> <param-id> <value>           set a parameter value\n"
        "  get <module-id>                              get module detail\n"
        "  add <plugin-slug> <model-slug>               add a module to the patch\n"
        "  rm <module-id>                               remove a module from the patch\n"
        "  connect <out-mod>:<out-port> <in-mod>:<in-port>  connect two ports\n"
        "  disconnect <cable-id>                        remove a cable\n"
        "\n"
        "options:\n"
        "  --port N    TCP port (default: 7000)\n"
        "  --host H    host    (default: 127.0.0.1)\n"
        "  --json      print raw JSON response\n");
}

int main(int argc, char *argv[]) {
    int i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            opt_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            snprintf(opt_host, sizeof(opt_host), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--json") == 0) {
            opt_json = 1;
        } else {
            break;
        }
    }

    if (i >= argc) { usage(); return 1; }
    const char *cmd = argv[i++];

    int fd = connect_to(opt_host, opt_port);
    if (fd < 0) return 1;

    int ret = 1;
    if (strcmp(cmd, "hello") == 0) {
        ret = cmd_hello(fd);
    } else if (strcmp(cmd, "plugins") == 0) {
        ret = cmd_plugins(fd);
    } else if (strcmp(cmd, "models") == 0) {
        const char *slug = (i < argc) ? argv[i] : NULL;
        ret = cmd_models(fd, slug);
    } else if (strcmp(cmd, "modules") == 0) {
        const char *slug = (i < argc) ? argv[i] : NULL;
        ret = cmd_modules(fd, slug);
    } else if (strcmp(cmd, "get") == 0) {
        if (i >= argc) { fprintf(stderr, "limen: get requires module-id\n"); }
        else {
            long long id = resolve_id(fd, argv[i]);
            if (id >= 0) ret = cmd_get(fd, id);
        }
    } else if (strcmp(cmd, "ports") == 0) {
        if (i >= argc) { fprintf(stderr, "limen: ports requires module-id\n"); }
        else {
            long long id = resolve_id(fd, argv[i]);
            if (id >= 0) ret = cmd_ports(fd, id);
        }
    } else if (strcmp(cmd, "params") == 0) {
        if (i >= argc) { fprintf(stderr, "limen: params requires module-id\n"); }
        else {
            long long id = resolve_id(fd, argv[i]);
            if (id >= 0) ret = cmd_params(fd, id);
        }
    } else if (strcmp(cmd, "param") == 0) {
        if (i + 1 >= argc) {
            fprintf(stderr, "limen: param requires module-id param-id\n");
        } else {
            long long modid = resolve_id(fd, argv[i]);
            if (modid >= 0) {
                int paramid = (int)strtol(argv[i+1], NULL, 10);
                ret = cmd_param(fd, modid, paramid);
            }
        }
    } else if (strcmp(cmd, "set") == 0) {
        if (i + 2 >= argc) {
            fprintf(stderr, "limen: set requires module-id param-id value\n");
        } else {
            long long modid = resolve_id(fd, argv[i]);
            if (modid >= 0) {
                int    paramid = (int)strtol(argv[i+1], NULL, 10);
                double value   = strtod(argv[i+2],      NULL);
                ret = cmd_set(fd, modid, paramid, value);
            }
        }
    } else if (strcmp(cmd, "cables") == 0) {
        int verbose = 0;
        long long module_id = -1;
        for (; i < argc; i++) {
            if (strcmp(argv[i], "-v") == 0)
                verbose = 1;
            else {
                module_id = resolve_id(fd, argv[i]);
                if (module_id < 0) { close(fd); return 1; }
            }
        }
        ret = cmd_cables(fd, module_id, verbose);
    } else if (strcmp(cmd, "add") == 0) {
        if (i + 1 >= argc) {
            fprintf(stderr, "limen: add requires plugin-slug model-slug\n");
        } else {
            ret = cmd_add(fd, argv[i], argv[i+1]);
        }
    } else if (strcmp(cmd, "rm") == 0) {
        if (i >= argc) { fprintf(stderr, "limen: rm requires module-id\n"); }
        else { ret = cmd_rm(fd, argv[i]); }
    } else if (strcmp(cmd, "connect") == 0) {
        if (i + 1 >= argc) {
            fprintf(stderr, "limen: connect requires <out-mod>:<out-port> <in-mod>:<in-port>\n");
        } else {
            ret = cmd_connect(fd, argv[i], argv[i+1]);
        }
    } else if (strcmp(cmd, "disconnect") == 0) {
        if (i >= argc) { fprintf(stderr, "limen: disconnect requires cable-id\n"); }
        else { ret = cmd_disconnect(fd, argv[i]); }
    } else {
        fprintf(stderr, "limen: unknown command: %s\n", cmd);
        usage();
    }

    close(fd);
    return ret;
}
