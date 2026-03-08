#include "forsitan.hpp"

#include <atomic>
#include <thread>
#include <string>

#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cerrno>

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


// ── Command dispatch ──────────────────────────────────────────────────────────

static std::string cmd_list_modules() {
	json_t* arr = json_array();
	auto ids = APP->engine->getModuleIds();
	for (int64_t id : ids) {
		engine::Module* m = APP->engine->getModule(id);
		if (!m) continue;
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

static std::string cmd_set_param(int64_t id, int paramId, float value) {
	engine::Module* m = APP->engine->getModule(id);
	if (!m)
		return err_response("module not found");
	if (paramId < 0 || paramId >= m->getNumParams())
		return err_response("param not found");
	APP->engine->setParamValue(m, paramId, value);
	return ok_response(json_null());
}

static std::string cmd_list_cables() {
	json_t* arr = json_array();
	auto ids = APP->engine->getCableIds();
	for (int64_t id : ids) {
		engine::Cable* c = APP->engine->getCable(id);
		if (!c) continue;
		json_t* obj = json_object();
		json_object_set_new(obj, "id", json_integer(id));
		json_object_set_new(obj, "outputModule", json_integer(c->outputModule ? c->outputModule->id : -1));
		json_object_set_new(obj, "outputPort",   json_integer(c->outputId));
		json_object_set_new(obj, "inputModule",  json_integer(c->inputModule  ? c->inputModule->id  : -1));
		json_object_set_new(obj, "inputPort",    json_integer(c->inputId));
		json_array_append_new(arr, obj);
	}
	return ok_response(arr);
}

static std::string dispatch(const std::string& line) {
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

	if (cmd == "list_modules") {
		result = cmd_list_modules();
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
	else if (cmd == "list_cables") {
		result = cmd_list_cables();
	}
	else {
		result = err_response("unknown cmd");
	}

	json_decref(req);
	return result;
}


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

	Limen() {
		config(0, 0, 0, NUM_LIGHTS);
	}

	~Limen() {
		// signalStop() may have already been called by onRemove(), that's fine.
		signalStop();
		if (svrThread.joinable())
			svrThread.join();
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
		if (listenFd >= 0) {
			close(listenFd);
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

		listenFd = socket(AF_INET, SOCK_STREAM, 0);
		if (listenFd < 0) {
			WARN("limen: socket() failed");
			return;
		}

		int yes = 1;
		setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

		// Accept times out every 100ms so the thread can check `running`.
		// On Linux, close() from another thread does not reliably unblock accept().
		struct timeval tv;
		tv.tv_sec = 0;
		tv.tv_usec = 100000;
		setsockopt(listenFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		addr.sin_port = htons((uint16_t)port);

		if (bind(listenFd, (sockaddr*)&addr, sizeof(addr)) < 0) {
			WARN("limen: bind() failed on port %d", port);
			close(listenFd);
			listenFd = -1;
			return;
		}

		if (listen(listenFd, 1) < 0) {
			WARN("limen: listen() failed");
			close(listenFd);
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

	void serverLoop() {
		while (running) {
			int cfd = accept(listenFd, nullptr, nullptr);
			if (cfd < 0) {
				// Timeout (EAGAIN/EWOULDBLOCK) → check running and retry.
				// Any other error → socket closed or broken, exit.
				if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
					continue;
				break;
			}
			handleClient(cfd);
			close(cfd);
		}
		listening = false;
	}

	void handleClient(int fd) {
		// Same timeout on the client socket so read() doesn't block forever.
		struct timeval tv;
		tv.tv_sec = 0;
		tv.tv_usec = 100000;
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

		activeFd.store(fd);
		std::string buf;
		char tmp[256];
		while (running) {
			ssize_t n = read(fd, tmp, sizeof(tmp));
			if (n < 0) {
				if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
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
				std::string resp = dispatch(line);
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
