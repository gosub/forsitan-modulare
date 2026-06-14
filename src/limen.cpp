#include "forsitan.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
// Do not redefine close(): it collides with Window::close() used by the quit
// command. Use closesock() for sockets instead.
#  define closesock(s)    closesocket(s)
#  define read(s,b,n)     recv(s,(char*)(b),(int)(n),0)
#  define write(s,b,n)    send(s,(const char*)(b),(int)(n),0)
#  define SHUT_RDWR       SD_BOTH
#  define SOCK_ERRNO      WSAGetLastError()
#  define SOCK_EAGAIN     WSAEWOULDBLOCK
#  define SOCK_EWOULDBLOCK WSAEWOULDBLOCK
#  define SOCK_EINTR      WSAEINTR
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <unistd.h>
#  include <cerrno>
#  define closesock(s)    close(s)
#  define SOCK_ERRNO      errno
#  define SOCK_EAGAIN     EAGAIN
#  define SOCK_EWOULDBLOCK EWOULDBLOCK
#  define SOCK_EINTR      EINTR
#endif

#include <jansson.h>


// ── JSON helpers ─────────────────────────────────────────────────────────────

static std::string json_response(bool ok, json_t* payload, const char* error = nullptr) {
	json_t* root = json_object();
	json_object_set_new(root, "ok", json_boolean(ok));
	if (ok && payload)
		json_object_set_new(root, "result", payload);
	else if (!ok)
		json_object_set_new(root, "error", json_string(error ? error : "unknown error"));
	else if (payload)
		json_decref(payload);

	char* s = json_dumps(root, JSON_COMPACT);
	std::string out(s);
	free(s);
	json_decref(root);
	return out + "\n";
}

static std::string ok_response(json_t* result) {
	return json_response(true, result);
}

static std::string err_response(const char* msg) {
	return json_response(false, nullptr, msg);
}


// ── Protocol version ─────────────────────────────────────────────────────────
// Bump when the wire protocol changes in a way clients must adapt to.
static const int LIMEN_PROTOCOL_VERSION = 1;


// ── Read-only commands (safe to call from any thread) ────────────────────────

// Capability discovery: clients call this first to learn the protocol version
// and the set of supported commands. Keep the list in sync with dispatch().
static std::string cmd_hello() {
	static const char* commands[] = {
		"hello",
		"list_plugins", "list_models", "list_modules", "get_module",
		"list_ports", "list_params", "get_param", "set_param",
		"list_cables", "add_module", "remove_module", "add_cable", "remove_cable",
		"set_fullscreen", "zoom_to_modules", "quit",
	};
	json_t* cmds = json_array();
	for (const char* c : commands)
		json_array_append_new(cmds, json_string(c));
	json_t* obj = json_object();
	json_object_set_new(obj, "protocol", json_integer(LIMEN_PROTOCOL_VERSION));
	json_object_set_new(obj, "commands", cmds);
	return ok_response(obj);
}

static std::string cmd_list_plugins() {
	json_t* arr = json_array();
	for (plugin::Plugin* p : rack::plugin::plugins) {
		json_t* obj = json_object();
		json_object_set_new(obj, "slug",    json_string(p->slug.c_str()));
		json_object_set_new(obj, "name",    json_string(p->name.c_str()));
		json_object_set_new(obj, "version", json_string(p->version.c_str()));
		json_array_append_new(arr, obj);
	}
	return ok_response(arr);
}

static std::string cmd_list_models(const std::string& pluginFilter = "") {
	json_t* arr = json_array();
	for (plugin::Plugin* p : rack::plugin::plugins) {
		if (!pluginFilter.empty() && p->slug != pluginFilter) continue;
		for (plugin::Model* mdl : p->models) {
			json_t* obj = json_object();
			json_object_set_new(obj, "plugin",      json_string(p->slug.c_str()));
			json_object_set_new(obj, "slug",        json_string(mdl->slug.c_str()));
			json_object_set_new(obj, "name",        json_string(mdl->name.c_str()));
			json_object_set_new(obj, "description", json_string(mdl->description.c_str()));
			json_array_append_new(arr, obj);
		}
	}
	return ok_response(arr);
}

static std::string cmd_list_modules(const std::string& pluginFilter = "") {
	json_t* arr = json_array();
	auto ids = APP->engine->getModuleIds();
	for (int64_t id : ids) {
		engine::Module* m = APP->engine->getModule(id);
		if (!m) continue;
		if (!pluginFilter.empty()) {
			if (!m->model || !m->model->plugin) continue;
			if (m->model->plugin->slug != pluginFilter) continue;
		}
		json_t* obj = json_object();
		json_object_set_new(obj, "id", json_integer(id));
		if (m->model) {
			json_object_set_new(obj, "plugin", json_string(m->model->plugin ? m->model->plugin->slug.c_str() : ""));
			json_object_set_new(obj, "model",  json_string(m->model->slug.c_str()));
			json_object_set_new(obj, "name",   json_string(m->model->name.c_str()));
		}
		json_object_set_new(obj, "numParams",  json_integer(m->getNumParams()));
		json_object_set_new(obj, "numInputs",  json_integer(m->getNumInputs()));
		json_object_set_new(obj, "numOutputs", json_integer(m->getNumOutputs()));
		json_array_append_new(arr, obj);
	}
	return ok_response(arr);
}

static std::string cmd_get_module(int64_t id) {
	engine::Module* m = APP->engine->getModule(id);
	if (!m)
		return err_response("module not found");
	json_t* obj = json_object();
	json_object_set_new(obj, "id", json_integer(id));
	if (m->model) {
		json_object_set_new(obj, "plugin", json_string(m->model->plugin ? m->model->plugin->slug.c_str() : ""));
		json_object_set_new(obj, "model",  json_string(m->model->slug.c_str()));
		json_object_set_new(obj, "name",   json_string(m->model->name.c_str()));
	}
	json_object_set_new(obj, "numParams",  json_integer(m->getNumParams()));
	json_object_set_new(obj, "numInputs",  json_integer(m->getNumInputs()));
	json_object_set_new(obj, "numOutputs", json_integer(m->getNumOutputs()));
	return ok_response(obj);
}

static std::string cmd_list_params(int64_t id) {
	engine::Module* m = APP->engine->getModule(id);
	if (!m)
		return err_response("module not found");
	json_t* arr = json_array();
	for (int i = 0; i < m->getNumParams(); i++) {
		json_t* obj = json_object();
		json_object_set_new(obj, "id", json_integer(i));
		float value = APP->engine->getParamValue(m, i);
		json_object_set_new(obj, "value", json_real(value));
		engine::ParamQuantity* pq = m->getParamQuantity(i);
		if (pq) {
			json_object_set_new(obj, "name", json_string(pq->name.c_str()));
			json_object_set_new(obj, "min",  json_real(pq->minValue));
			json_object_set_new(obj, "max",  json_real(pq->maxValue));
			json_object_set_new(obj, "unit", json_string(pq->unit.c_str()));
		}
		json_array_append_new(arr, obj);
	}
	return ok_response(arr);
}

static std::string cmd_get_param(int64_t id, int paramId) {
	engine::Module* m = APP->engine->getModule(id);
	if (!m)
		return err_response("module not found");
	if (paramId < 0 || paramId >= m->getNumParams())
		return err_response("param not found");
	json_t* obj = json_object();
	json_object_set_new(obj, "id", json_integer(paramId));
	json_object_set_new(obj, "value", json_real(APP->engine->getParamValue(m, paramId)));
	engine::ParamQuantity* pq = m->getParamQuantity(paramId);
	if (pq) {
		json_object_set_new(obj, "name", json_string(pq->name.c_str()));
		json_object_set_new(obj, "min",  json_real(pq->minValue));
		json_object_set_new(obj, "max",  json_real(pq->maxValue));
		json_object_set_new(obj, "unit", json_string(pq->unit.c_str()));
	}
	return ok_response(obj);
}

static std::string cmd_set_param(int64_t id, int paramId, float value) {
	engine::Module* m = APP->engine->getModule(id);
	if (!m)
		return err_response("module not found");
	if (paramId < 0 || paramId >= m->getNumParams())
		return err_response("param not found");
	APP->engine->setParamValue(m, paramId, value);
	return ok_response(json_null());
}

static std::string cmd_list_ports(int64_t id) {
	engine::Module* m = APP->engine->getModule(id);
	if (!m)
		return err_response("module not found");
	json_t* inputs = json_array();
	for (int i = 0; i < m->getNumInputs(); i++) {
		json_t* obj = json_object();
		json_object_set_new(obj, "id", json_integer(i));
		json_object_set_new(obj, "name", json_string(
			(m->inputInfos[i] && !m->inputInfos[i]->name.empty())
			? m->inputInfos[i]->name.c_str() : ""));
		json_array_append_new(inputs, obj);
	}
	json_t* outputs = json_array();
	for (int i = 0; i < m->getNumOutputs(); i++) {
		json_t* obj = json_object();
		json_object_set_new(obj, "id", json_integer(i));
		json_object_set_new(obj, "name", json_string(
			(m->outputInfos[i] && !m->outputInfos[i]->name.empty())
			? m->outputInfos[i]->name.c_str() : ""));
		json_array_append_new(outputs, obj);
	}
	json_t* result = json_object();
	json_object_set_new(result, "inputs",  inputs);
	json_object_set_new(result, "outputs", outputs);
	return ok_response(result);
}

static std::string cmd_list_cables(int64_t moduleFilter = -1, bool verbose = false) {
	json_t* arr = json_array();
	auto ids = APP->engine->getCableIds();
	for (int64_t id : ids) {
		engine::Cable* c = APP->engine->getCable(id);
		if (!c) continue;
		int64_t outId = c->outputModule ? c->outputModule->id : -1;
		int64_t inId  = c->inputModule  ? c->inputModule->id  : -1;
		if (moduleFilter >= 0 && outId != moduleFilter && inId != moduleFilter)
			continue;
		json_t* obj = json_object();
		json_object_set_new(obj, "id",           json_integer(id));
		json_object_set_new(obj, "outputModule", json_integer(outId));
		json_object_set_new(obj, "outputPort",   json_integer(c->outputId));
		json_object_set_new(obj, "inputModule",  json_integer(inId));
		json_object_set_new(obj, "inputPort",    json_integer(c->inputId));
		if (verbose) {
			engine::Module* om = c->outputModule;
			engine::Module* im = c->inputModule;
			auto port_name = [](std::vector<rack::engine::PortInfo*>& infos, int i) -> const char* {
				if (i >= 0 && i < (int)infos.size() && infos[i] && !infos[i]->name.empty())
					return infos[i]->name.c_str();
				return "";
			};
			json_object_set_new(obj, "outputModuleName", json_string(om && om->model ? om->model->name.c_str() : ""));
			json_object_set_new(obj, "outputPortName",   json_string(om ? port_name(om->outputInfos, c->outputId) : ""));
			json_object_set_new(obj, "inputModuleName",  json_string(im && im->model ? im->model->name.c_str() : ""));
			json_object_set_new(obj, "inputPortName",    json_string(im ? port_name(im->inputInfos,  c->inputId)  : ""));
		}
		json_array_append_new(arr, obj);
	}
	return ok_response(arr);
}


// ── Pending operation queue (mutation commands run on the main thread) ────────

struct PendingOp {
	std::function<std::string()> fn;
	std::string result;
	bool done = false;
};


// ── Module ────────────────────────────────────────────────────────────────────

struct Limen : Module {
	enum LightIds {
		STATUS_LIGHT,
		NUM_LIGHTS
	};

	int port = 7000;
	bool serverEnabled = true;
	int listenFd = -1;
	std::atomic<int> activeFd{-1};  // client fd currently in handleClient, or -1
	std::thread svrThread;
	std::atomic<bool> running{false};
	std::atomic<bool> listening{false};

	// Main-thread operation queue for mutation commands.
	std::mutex pendingMtx;
	std::condition_variable pendingCv;
	std::vector<std::shared_ptr<PendingOp>> pendingOps;

	// zoom_to_modules must re-run over a few consecutive frames: the first call
	// computes its fit against a layout that has not settled after a viewport,
	// zoom, or module change, leaving the view off-centre. LimenWidget::step()
	// calls zoomToModules() while this counter is positive.
	std::atomic<int> zoomFrames{0};

	Limen() {
		config(0, 0, 0, NUM_LIGHTS);
	}

	~Limen() {
		// signalStop() may have already been called by onRemove(), that's fine.
		signalStop();
		if (svrThread.joinable())
			svrThread.join();
#ifdef _WIN32
		WSACleanup();
#endif
	}

	void onAdd(const AddEvent&) override {
		if (serverEnabled) startServer();
	}

	void onRemove(const RemoveEvent&) override {
		// onRemove is called while the engine holds its exclusive lock.
		// The server thread may be waiting to acquire a share-lock on the
		// same engine (inside dispatch()). Joining here would deadlock.
		// We only signal; the destructor (called after the lock is released)
		// does the join.
		signalStop();
	}

	// Unblocks the server thread and clears state. Safe to call from any
	// context, including while the engine lock is held.
	void signalStop() {
		running = false;
		listening = false;
		// Wake any server threads waiting for main-thread op completion.
		pendingCv.notify_all();
		if (listenFd >= 0) {
			closesock(listenFd);
			listenFd = -1;
		}
		// Unblock read() in handleClient without closing the fd (avoids the
		// fd-reuse race; the server thread itself closes the fd).
		int cfd = activeFd.load();
		if (cfd >= 0)
			shutdown(cfd, SHUT_RDWR);
	}

	// Signal + wait. Only call this when the engine lock is NOT held
	// (e.g. from the context menu port-change action).
	void stopServer() {
		signalStop();
		if (svrThread.joinable())
			svrThread.join();
	}

	void startServer() {
		// Full stop before restarting (safe: called from UI, no engine lock).
		stopServer();

#ifdef _WIN32
		WSADATA wsa; WSAStartup(MAKEWORD(2,2), &wsa);
#endif

		listenFd = socket(AF_INET, SOCK_STREAM, 0);
		if (listenFd < 0) {
			WARN("limen: socket() failed");
			return;
		}

		int yes = 1;
		setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));

		// Accept times out every 100ms so the thread can check `running`.
		// On Linux, close() from another thread does not reliably unblock accept().
#ifdef _WIN32
		DWORD tv_ms = 100;
		setsockopt(listenFd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv_ms, sizeof(tv_ms));
#else
		struct timeval tv{0, 100000};
		setsockopt(listenFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		addr.sin_port = htons((uint16_t)port);

		if (bind(listenFd, (sockaddr*)&addr, sizeof(addr)) < 0) {
			WARN("limen: bind() failed on port %d", port);
			closesock(listenFd);
			listenFd = -1;
			return;
		}

		if (listen(listenFd, 1) < 0) {
			WARN("limen: listen() failed");
			closesock(listenFd);
			listenFd = -1;
			return;
		}

		running = true;
		listening = true;
		Context* ctx = APP;
		svrThread = std::thread([this, ctx]{
			rack::contextSet(ctx);
			serverLoop();
		});
	}

	// Enqueue fn to execute on the main thread; block until done (≤2s timeout).
	// Returns the JSON response string produced by fn.
	std::string runOnMainThread(std::function<std::string()> fn) {
		auto op = std::make_shared<PendingOp>();
		op->fn = std::move(fn);
		{
			std::lock_guard<std::mutex> lk(pendingMtx);
			pendingOps.push_back(op);
		}
		std::unique_lock<std::mutex> lk(pendingMtx);
		bool ok = pendingCv.wait_for(lk, std::chrono::seconds(2),
			[&]{ return op->done || !running.load(); });
		if (!ok || !op->done)
			return err_response("timeout waiting for main thread");
		return op->result;
	}

	void serverLoop() {
		while (running) {
			int cfd = accept(listenFd, nullptr, nullptr);
			if (cfd < 0) {
				// Timeout (EAGAIN/EWOULDBLOCK) → check running and retry.
				// Any other error → socket closed or broken, exit.
				if (SOCK_ERRNO == SOCK_EAGAIN || SOCK_ERRNO == SOCK_EWOULDBLOCK || SOCK_ERRNO == SOCK_EINTR)
					continue;
				break;
			}
			handleClient(cfd);
			closesock(cfd);
		}
		listening = false;
	}

	void handleClient(int fd);  // defined after dispatch()

	void process(const ProcessArgs&) override {
		lights[STATUS_LIGHT].setBrightness(listening ? 1.f : 0.f);
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "port", json_integer(port));
		json_object_set_new(root, "serverEnabled", json_boolean(serverEnabled));
		return root;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* port_j = json_object_get(rootJ, "port");
		if (port_j && json_is_integer(port_j))
			port = (int)json_integer_value(port_j);
		json_t* enabled_j = json_object_get(rootJ, "serverEnabled");
		if (enabled_j && json_is_boolean(enabled_j))
			serverEnabled = json_boolean_value(enabled_j);
	}
};


// ── Command dispatch ──────────────────────────────────────────────────────────

static std::string dispatch(const std::string& line, Limen* limen) {
	json_error_t err;
	json_t* req = json_loads(line.c_str(), 0, &err);
	if (!req)
		return err_response("invalid JSON");

	json_t* cmd_j = json_object_get(req, "cmd");
	if (!cmd_j || !json_is_string(cmd_j)) {
		json_decref(req);
		return err_response("missing cmd");
	}
	std::string cmd = json_string_value(cmd_j);

	std::string result;

	if (cmd == "hello") {
		result = cmd_hello();
	}
	else if (cmd == "list_plugins") {
		result = cmd_list_plugins();
	}
	else if (cmd == "list_models") {
		std::string filter;
		json_t* plugin_j = json_object_get(req, "plugin");
		if (plugin_j && json_is_string(plugin_j))
			filter = json_string_value(plugin_j);
		result = cmd_list_models(filter);
	}
	else if (cmd == "list_modules") {
		std::string filter;
		json_t* plugin_j = json_object_get(req, "plugin");
		if (plugin_j && json_is_string(plugin_j))
			filter = json_string_value(plugin_j);
		result = cmd_list_modules(filter);
	}
	else if (cmd == "get_module") {
		json_t* id_j = json_object_get(req, "id");
		if (!id_j || !json_is_integer(id_j)) {
			result = err_response("missing id");
		} else {
			result = cmd_get_module(json_integer_value(id_j));
		}
	}
	else if (cmd == "list_params") {
		json_t* id_j = json_object_get(req, "id");
		if (!id_j || !json_is_integer(id_j)) {
			result = err_response("missing id");
		} else {
			result = cmd_list_params(json_integer_value(id_j));
		}
	}
	else if (cmd == "get_param") {
		json_t* id_j    = json_object_get(req, "id");
		json_t* param_j = json_object_get(req, "param");
		if (!id_j || !json_is_integer(id_j) ||
		    !param_j || !json_is_integer(param_j)) {
			result = err_response("missing id or param");
		} else {
			result = cmd_get_param(
				json_integer_value(id_j),
				(int)json_integer_value(param_j));
		}
	}
	else if (cmd == "set_param") {
		json_t* id_j    = json_object_get(req, "id");
		json_t* param_j = json_object_get(req, "param");
		json_t* value_j = json_object_get(req, "value");
		if (!id_j || !json_is_integer(id_j) ||
		    !param_j || !json_is_integer(param_j) ||
		    !value_j || !json_is_number(value_j)) {
			result = err_response("missing id, param, or value");
		} else {
			result = cmd_set_param(
				json_integer_value(id_j),
				(int)json_integer_value(param_j),
				(float)json_number_value(value_j));
		}
	}
	else if (cmd == "list_ports") {
		json_t* id_j = json_object_get(req, "id");
		if (!id_j || !json_is_integer(id_j)) {
			result = err_response("missing id");
		} else {
			result = cmd_list_ports(json_integer_value(id_j));
		}
	}
	else if (cmd == "list_cables") {
		int64_t moduleFilter = -1;
		json_t* mod_j = json_object_get(req, "id");
		if (mod_j && json_is_integer(mod_j))
			moduleFilter = json_integer_value(mod_j);
		bool verbose = false;
		json_t* verbose_j = json_object_get(req, "verbose");
		if (verbose_j && json_is_boolean(verbose_j))
			verbose = json_boolean_value(verbose_j);
		result = cmd_list_cables(moduleFilter, verbose);
	}
	else if (cmd == "add_module") {
		json_t* plugin_j = json_object_get(req, "plugin");
		json_t* model_j  = json_object_get(req, "model");
		if (!plugin_j || !json_is_string(plugin_j) ||
		    !model_j  || !json_is_string(model_j)) {
			result = err_response("missing plugin or model");
		} else {
			std::string plugSlug  = json_string_value(plugin_j);
			std::string modelSlug = json_string_value(model_j);
			result = limen->runOnMainThread([plugSlug, modelSlug]() -> std::string {
				plugin::Plugin* plug = rack::plugin::getPlugin(plugSlug);
				if (!plug) return err_response("plugin not found");
				plugin::Model* mdl = plug->getModel(modelSlug);
				if (!mdl) return err_response("model not found");
				engine::Module* mod = mdl->createModule();
				if (!mod) return err_response("failed to create module");
				APP->engine->addModule(mod);
				app::ModuleWidget* mw = mdl->createModuleWidget(mod);
				if (!mw) {
					APP->engine->removeModule(mod);
					delete mod;
					return err_response("failed to create module widget");
				}
				APP->scene->rack->addModule(mw);
				// Place next to an existing module so it appears in the visible area.
				// Fall back to (0,0) if there are no other modules.
				math::Vec pos = math::Vec(0, 0);
				for (widget::Widget* w : APP->scene->rack->getModuleContainer()->children) {
					app::ModuleWidget* existing = dynamic_cast<app::ModuleWidget*>(w);
					if (existing && existing != mw) {
						pos = existing->box.pos;
						break;
					}
				}
				APP->scene->rack->setModulePosNearest(mw, pos);
				json_t* obj = json_object();
				json_object_set_new(obj, "id", json_integer(mod->id));
				return ok_response(obj);
			});
		}
	}
	else if (cmd == "remove_module") {
		json_t* id_j = json_object_get(req, "id");
		if (!id_j || !json_is_integer(id_j)) {
			result = err_response("missing id");
		} else {
			int64_t id = json_integer_value(id_j);
			result = limen->runOnMainThread([id]() -> std::string {
				app::ModuleWidget* mw = nullptr;
				for (widget::Widget* w : APP->scene->rack->getModuleContainer()->children) {
					app::ModuleWidget* candidate = dynamic_cast<app::ModuleWidget*>(w);
					if (candidate && candidate->module && candidate->module->id == id) {
						mw = candidate;
						break;
					}
				}
				if (!mw) return err_response("module not found");
				APP->scene->rack->removeModule(mw);
				delete mw;
				return ok_response(json_null());
			});
		}
	}
	else if (cmd == "add_cable") {
		json_t* om_j = json_object_get(req, "outputModule");
		json_t* op_j = json_object_get(req, "outputPort");
		json_t* im_j = json_object_get(req, "inputModule");
		json_t* ip_j = json_object_get(req, "inputPort");
		if (!om_j || !json_is_integer(om_j) ||
		    !op_j || !json_is_integer(op_j) ||
		    !im_j || !json_is_integer(im_j) ||
		    !ip_j || !json_is_integer(ip_j)) {
			result = err_response("missing outputModule, outputPort, inputModule, or inputPort");
		} else {
			int64_t outMod  = json_integer_value(om_j);
			int     outPort = (int)json_integer_value(op_j);
			int64_t inMod   = json_integer_value(im_j);
			int     inPort  = (int)json_integer_value(ip_j);
			result = limen->runOnMainThread([outMod, outPort, inMod, inPort]() -> std::string {
				engine::Module* outM = APP->engine->getModule(outMod);
				if (!outM) return err_response("output module not found");
				engine::Module* inM  = APP->engine->getModule(inMod);
				if (!inM) return err_response("input module not found");
				if (outPort < 0 || outPort >= outM->getNumOutputs())
					return err_response("output port out of range");
				if (inPort < 0 || inPort >= inM->getNumInputs())
					return err_response("input port out of range");

				engine::Cable* cable = new engine::Cable;
				cable->outputModule = outM;
				cable->outputId     = outPort;
				cable->inputModule  = inM;
				cable->inputId      = inPort;
				APP->engine->addCable(cable);

				app::CableWidget* cw = new app::CableWidget;
				cw->setCable(cable);
				APP->scene->rack->addCable(cw);

				json_t* obj = json_object();
				json_object_set_new(obj, "id", json_integer(cable->id));
				return ok_response(obj);
			});
		}
	}
	else if (cmd == "remove_cable") {
		json_t* id_j = json_object_get(req, "id");
		if (!id_j || !json_is_integer(id_j)) {
			result = err_response("missing id");
		} else {
			int64_t id = json_integer_value(id_j);
			result = limen->runOnMainThread([id]() -> std::string {
				app::CableWidget* cw = nullptr;
				for (widget::Widget* w : APP->scene->rack->getCableContainer()->children) {
					app::CableWidget* candidate = dynamic_cast<app::CableWidget*>(w);
					if (candidate && candidate->cable && candidate->cable->id == id) {
						cw = candidate;
						break;
					}
				}
				if (!cw) return err_response("cable not found");
				APP->scene->rack->removeCable(cw);
				delete cw;
				return ok_response(json_null());
			});
		}
	}
	else if (cmd == "set_fullscreen") {
		json_t* on_j = json_object_get(req, "on");
		if (!on_j || !json_is_boolean(on_j)) {
			result = err_response("missing on");
		} else {
			bool on = json_boolean_value(on_j);
			result = limen->runOnMainThread([on]() -> std::string {
				APP->window->setFullScreen(on);
				json_t* obj = json_object();
				json_object_set_new(obj, "fullscreen", json_boolean(APP->window->isFullScreen()));
				return ok_response(obj);
			});
		}
	}
	else if (cmd == "zoom_to_modules") {
		// Re-run over the next few frames so the final fit lands on a settled
		// layout (a single call right after a change leaves the view off-centre).
		limen->zoomFrames.store(3);
		result = ok_response(json_null());
	}
	else if (cmd == "quit") {
		result = limen->runOnMainThread([]() -> std::string {
			APP->window->close();
			return ok_response(json_null());
		});
	}
	else {
		result = err_response("unknown cmd");
	}

	json_decref(req);
	return result;
}

void Limen::handleClient(int fd) {
	// Same timeout on the client socket so read() doesn't block forever.
#ifdef _WIN32
	DWORD tv_ms = 100;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv_ms, sizeof(tv_ms));
#else
	struct timeval tv{0, 100000};
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

	activeFd.store(fd);
	std::string buf;
	char tmp[256];
	while (running) {
		ssize_t n = read(fd, tmp, sizeof(tmp));
		if (n < 0) {
			if (SOCK_ERRNO == SOCK_EAGAIN || SOCK_ERRNO == SOCK_EWOULDBLOCK || SOCK_ERRNO == SOCK_EINTR)
				continue;
			break;
		}
		if (n == 0)
			break;
		buf.append(tmp, (size_t)n);
		size_t pos;
		while ((pos = buf.find('\n')) != std::string::npos) {
			std::string line = buf.substr(0, pos);
			buf.erase(0, pos + 1);
			if (line.empty()) continue;
			std::string resp = dispatch(line, this);
			const char* p = resp.c_str();
			size_t rem = resp.size();
			while (rem > 0) {
				ssize_t w = write(fd, p, rem);
				if (w <= 0) { activeFd.store(-1); return; }
				p += w;
				rem -= (size_t)w;
			}
		}
	}
	activeFd.store(-1);
}


// ── Widget ────────────────────────────────────────────────────────────────────

struct LimenWidget : ModuleWidget {
	LimenWidget(Limen* module) {
		setModule(module);
		setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, "res/limen.svg")));

		addChild(createWidget<ScrewSilver>(Vec(0, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(0, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		// Status LED: green = listening
		addChild(createLightCentered<MediumLight<GreenLight>>(
			mm2px(Vec(7.62, 64.0)), module, Limen::STATUS_LIGHT));
	}

	// Drain the pending-op queue each frame on the main thread.
	void step() override {
		ModuleWidget::step();
		Limen* m = dynamic_cast<Limen*>(module);
		if (!m) return;

		std::vector<std::shared_ptr<PendingOp>> ops;
		{
			std::lock_guard<std::mutex> lk(m->pendingMtx);
			ops.swap(m->pendingOps);
		}
		for (auto& op : ops) {
			op->result = op->fn();
			{
				std::lock_guard<std::mutex> lk(m->pendingMtx);
				op->done = true;
			}
			m->pendingCv.notify_all();
		}

		// Pending zoom_to_modules: run on consecutive frames until the layout
		// has settled, so the final fit is correctly centred.
		if (m->zoomFrames.load() > 0) {
			APP->scene->rackScroll->zoomToModules();
			m->zoomFrames--;
		}
	}

	void appendContextMenu(Menu* menu) override {
		Limen* module = dynamic_cast<Limen*>(this->module);
		assert(module);

		menu->addChild(new MenuSeparator);

		// Enable / disable toggle
		struct EnableItem : MenuItem {
			Limen* module;
			void onAction(const event::Action&) override {
				module->serverEnabled = !module->serverEnabled;
				if (module->serverEnabled)
					module->startServer();
				else
					module->signalStop();
			}
		};
		auto* ei = construct<EnableItem>(
			&MenuItem::text, "Server enabled",
			&MenuItem::rightText, module->serverEnabled ? "✓" : "",
			&EnableItem::module, module);
		menu->addChild(ei);

		menu->addChild(new MenuSeparator);

		// Preset port items
		struct PortItem : MenuItem {
			Limen* module;
			int newPort;
			void onAction(const event::Action&) override {
				module->port = newPort;
				if (module->serverEnabled) {
					module->stopServer();
					module->startServer();
				}
			}
		};

		menu->addChild(construct<MenuLabel>(&MenuLabel::text, "TCP port"));
		for (int p : {7000, 7001, 7002, 7777, 8000}) {
			auto* item = construct<PortItem>(&MenuItem::text,
				std::to_string(p) + (module->port == p ? " ✓" : ""),
				&PortItem::module, module,
				&PortItem::newPort, p);
			menu->addChild(item);
		}

		// Custom port text field
		struct PortTextField : ui::TextField {
			Limen* module;
			void onSelectKey(const event::SelectKey& e) override {
				if (e.action == GLFW_PRESS &&
				    (e.key == GLFW_KEY_ENTER || e.key == GLFW_KEY_KP_ENTER)) {
					try {
						int p = std::stoi(text);
						if (p >= 1 && p <= 65535) {
							module->port = p;
							if (module->serverEnabled) {
								module->stopServer();
								module->startServer();
							}
						}
					} catch (...) {}
					getAncestorOfType<ui::MenuOverlay>()->requestDelete();
					e.consume(this);
				}
				if (!e.getTarget())
					ui::TextField::onSelectKey(e);
			}
		};

		menu->addChild(construct<MenuLabel>(&MenuLabel::text, "Custom port (Enter to apply)"));
		auto* tf = new PortTextField;
		tf->module = module;
		tf->text = std::to_string(module->port);
		tf->box.size.x = 80.f;
		menu->addChild(tf);
	}
};


Model* limen = createModel<Limen, LimenWidget>("limen");
