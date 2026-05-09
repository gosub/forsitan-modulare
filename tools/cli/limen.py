#!/usr/bin/env python3
"""limen — command-line client for the limen VCV Rack module (Python version)

Usage:
    limen.py [--port N] [--host H] [--json] <command> [args]

No third-party dependencies; requires Python 3.6+.
"""

import argparse
import json
import socket
import sys

DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 7000


# ── transport ─────────────────────────────────────────────────────────────────

def transact(host, port, obj):
    """Send one JSON object, return parsed response."""
    with socket.create_connection((host, port)) as s:
        s.sendall((json.dumps(obj) + "\n").encode())
        buf = b""
        while b"\n" not in buf:
            chunk = s.recv(1 << 20)
            if not chunk:
                break
            buf += chunk
    return json.loads(buf.decode())


def send(args, obj):
    """Send request, handle errors, return result value or exit."""
    try:
        resp = transact(args.host, args.port, obj)
    except ConnectionRefusedError:
        sys.exit(f"limen: cannot connect to {args.host}:{args.port}")
    if args.json:
        print(json.dumps(resp))
        sys.exit(0)
    if not resp.get("ok"):
        sys.exit(f"limen: error: {resp.get('error', 'unknown error')}")
    return resp.get("result")


# ── id prefix resolution ──────────────────────────────────────────────────────

def resolve_id(args, prefix, kind="module"):
    cmd = "list_cables" if kind == "cable" else "list_modules"
    resp = transact(args.host, args.port, {"cmd": cmd})
    if not resp.get("ok"):
        sys.exit(f"limen: error: {resp.get('error')}")
    matches = [item for item in resp["result"]
               if str(item["id"]).startswith(prefix)]
    if not matches:
        sys.exit(f"limen: no {kind} matches '{prefix}'")
    if len(matches) > 1:
        sys.exit(f"limen: ambiguous prefix '{prefix}' matches {len(matches)} {kind}s")
    return matches[0]["id"]


def parse_endpoint(args, spec):
    """Parse 'modprefix:port' → (module_id, port_index)."""
    colon = spec.rfind(":")
    if colon < 0:
        sys.exit(f"limen: expected <module-id>:<port>, got: {spec}")
    mod_id = resolve_id(args, spec[:colon])
    try:
        port = int(spec[colon + 1:])
    except ValueError:
        sys.exit(f"limen: invalid port: {spec[colon + 1:]}")
    return mod_id, port


# ── commands ──────────────────────────────────────────────────────────────────

def cmd_plugins(args):
    result = send(args, {"cmd": "list_plugins"})
    print(f"{'slug':<24}  {'name':<32}  version")
    for p in result:
        print(f"{p['slug']:<24}  {p['name']:<32}  {p.get('version','')}")


def cmd_models(args):
    req = {"cmd": "list_models"}
    if args.plugin:
        req["plugin"] = args.plugin
    result = send(args, req)
    print(f"{'plugin':<24}  {'slug':<24}  {'name':<24}  description")
    for m in result:
        print(f"{m['plugin']:<24}  {m['slug']:<24}  {m['name']:<24}  {m.get('description','')}")


def cmd_modules(args):
    req = {"cmd": "list_modules"}
    if args.plugin:
        req["plugin"] = args.plugin
    result = send(args, req)
    print(f"{'id':<22}  {'plugin':<12}  {'model':<16}  {'name':<16}  par   in  out")
    for m in result:
        print(f"{m['id']:<22}  {m['plugin']:<12}  {m['model']:<16}  {m['name']:<16}"
              f"  {m['numParams']:>3}  {m['numInputs']:>3}  {m['numOutputs']:>3}")


def cmd_get(args):
    mid = resolve_id(args, args.id)
    result = send(args, {"cmd": "get_module", "id": mid})
    m = result
    print(f"{'id':<22}  {'plugin':<12}  {'model':<16}  {'name':<16}  par   in  out")
    print(f"{m['id']:<22}  {m['plugin']:<12}  {m['model']:<16}  {m['name']:<16}"
          f"  {m['numParams']:>3}  {m['numInputs']:>3}  {m['numOutputs']:>3}")


def cmd_ports(args):
    mid = resolve_id(args, args.id)
    result = send(args, {"cmd": "list_ports", "id": mid})
    print("inputs:")
    for p in result.get("inputs", []):
        print(f"  in  {p['id']:>2}  {p['name']}")
    print("outputs:")
    for p in result.get("outputs", []):
        print(f"  out {p['id']:>2}  {p['name']}")


def cmd_params(args):
    mid = resolve_id(args, args.id)
    result = send(args, {"cmd": "list_params", "id": mid})
    print(f"{'id':>3}  {'name':<20}  {'value':>7}  {'min':>7}  {'max':>7}  unit")
    for p in result:
        print(f"{p['id']:>3}  {p['name']:<20}  {p['value']:>7.3f}  "
              f"{p['min']:>7.3f}  {p['max']:>7.3f}  {p.get('unit','')}")


def cmd_set(args):
    mid = resolve_id(args, args.id)
    send(args, {"cmd": "set_param", "id": mid,
                "param": args.param, "value": args.value})
    print("ok")


def cmd_cables(args):
    req = {"cmd": "list_cables"}
    if args.id:
        req["id"] = resolve_id(args, args.id)
    if args.verbose:
        req["verbose"] = True
    result = send(args, req)
    if not result:
        print("(no cables)")
        return
    if args.verbose:
        print(f"{'cable-id':>20}  {'out-module':>20} {'out-name':<16}  "
              f":{'out':>3} {'out-port':<16}    {'in-module':>20} {'in-name':<16}  "
              f":{'in':>3} {'in-port':<16}")
        for c in result:
            print(f"{c['id']:>20}  {c['outputModule']:>20} {c.get('outputModuleName',''):16}  "
                  f":{c['outputPort']:>3} {c.get('outputPortName',''):16}    "
                  f"{c['inputModule']:>20} {c.get('inputModuleName',''):16}  "
                  f":{c['inputPort']:>3} {c.get('inputPortName',''):16}")
    else:
        print(f"{'cable-id':>20}  {'out-module:port':>20}       {'in-module:port':>20}")
        for c in result:
            print(f"{c['id']:>20}  {c['outputModule']:>20}:{c['outputPort']:<3}    "
                  f"{c['inputModule']:>20}:{c['inputPort']}")


def cmd_add(args):
    result = send(args, {"cmd": "add_module",
                         "plugin": args.plugin, "model": args.model})
    print(result["id"])


def cmd_rm(args):
    mid = resolve_id(args, args.id)
    send(args, {"cmd": "remove_module", "id": mid})
    print("ok")


def cmd_connect(args):
    out_mod, out_port = parse_endpoint(args, args.output)
    in_mod,  in_port  = parse_endpoint(args, args.input)
    result = send(args, {"cmd": "add_cable",
                         "outputModule": out_mod, "outputPort": out_port,
                         "inputModule":  in_mod,  "inputPort":  in_port})
    print(result["id"])


def cmd_disconnect(args):
    cid = resolve_id(args, args.id, kind="cable")
    send(args, {"cmd": "remove_cable", "id": cid})
    print("ok")


# ── argument parsing ──────────────────────────────────────────────────────────

def main():
    p = argparse.ArgumentParser(
        prog="limen.py",
        description="Command-line client for the limen VCV Rack module.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument("--host", default=DEFAULT_HOST, metavar="H")
    p.add_argument("--port", default=DEFAULT_PORT, type=int, metavar="N")
    p.add_argument("--json", action="store_true", help="print raw JSON response")

    sub = p.add_subparsers(dest="cmd", metavar="command")
    sub.required = True

    sub.add_parser("plugins", help="list all loaded plugins")

    sp = sub.add_parser("models", help="list available models")
    sp.add_argument("plugin", nargs="?", help="filter by plugin slug")

    sp = sub.add_parser("modules", help="list modules in the rack")
    sp.add_argument("plugin", nargs="?", help="filter by plugin slug")

    sp = sub.add_parser("get", help="get module detail")
    sp.add_argument("id", metavar="module-id")

    sp = sub.add_parser("ports", help="list input/output port names")
    sp.add_argument("id", metavar="module-id")

    sp = sub.add_parser("params", help="list params for a module")
    sp.add_argument("id", metavar="module-id")

    sp = sub.add_parser("set", help="set a parameter value")
    sp.add_argument("id", metavar="module-id")
    sp.add_argument("param", type=int, metavar="param-id")
    sp.add_argument("value", type=float)

    sp = sub.add_parser("cables", help="list cables")
    sp.add_argument("-v", "--verbose", action="store_true")
    sp.add_argument("id", nargs="?", metavar="module-id")

    sp = sub.add_parser("add", help="add a module to the patch")
    sp.add_argument("plugin", metavar="plugin-slug")
    sp.add_argument("model", metavar="model-slug")

    sp = sub.add_parser("rm", help="remove a module from the patch")
    sp.add_argument("id", metavar="module-id")

    sp = sub.add_parser("connect", help="connect two ports with a cable")
    sp.add_argument("output", metavar="out-mod:out-port")
    sp.add_argument("input",  metavar="in-mod:in-port")

    sp = sub.add_parser("disconnect", help="remove a cable")
    sp.add_argument("id", metavar="cable-id")

    args = p.parse_args()

    dispatch = {
        "plugins":    cmd_plugins,
        "models":     cmd_models,
        "modules":    cmd_modules,
        "get":        cmd_get,
        "ports":      cmd_ports,
        "params":     cmd_params,
        "set":        cmd_set,
        "cables":     cmd_cables,
        "add":        cmd_add,
        "rm":         cmd_rm,
        "connect":    cmd_connect,
        "disconnect": cmd_disconnect,
    }
    dispatch[args.cmd](args)


if __name__ == "__main__":
    main()
