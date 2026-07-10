# Offline test harnesses. They link against libRack from the SDK
# (RACK_DIR must be set, as for the plugin build itself).
RACK_DIR ?= ../../Rack-SDK

CXXFLAGS += -O2 -std=c++11 -I$(RACK_DIR)/include -I$(RACK_DIR)/dep/include
LDFLAGS += -L$(RACK_DIR) -lRack -Wl,-rpath,$(RACK_DIR)

all: draen_sweep new_modules_smoke

draen_sweep: draen_sweep.cpp ../src/draen_ugens.hpp ../src/draen_engines.hpp ../src/draen_alt_engines.hpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

new_modules_smoke: new_modules_smoke.cpp ../src/rete.cpp ../src/ululo.cpp ../src/tabes.cpp ../src/lustro.cpp ../src/bulla.cpp ../src/scando_engine.hpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

clean:
	rm -f draen_sweep new_modules_smoke

.PHONY: all clean
