# Offline test harnesses. They link against libRack from the SDK
# (RACK_DIR must be set, as for the plugin build itself).
RACK_DIR ?= ../../Rack-SDK

CXXFLAGS += -O2 -std=c++11 -I$(RACK_DIR)/include -I$(RACK_DIR)/dep/include
LDFLAGS += -L$(RACK_DIR) -lRack -Wl,-rpath,$(RACK_DIR)

all: draen_sweep new_modules_smoke perge_render

draen_sweep: draen_sweep.cpp ../src/draen_ugens.hpp ../src/draen_engines.hpp ../src/draen_alt_engines.hpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

new_modules_smoke: new_modules_smoke.cpp ../src/rete.cpp ../src/ululo.cpp ../src/tabes.cpp ../src/lustro.cpp ../src/bulla.cpp ../src/perge.cpp ../src/scando_engine.hpp ../src/imber.cpp ../src/sylla.cpp ../src/imber/imber_dsp.hpp ../src/imber/imber_gen.hpp ../src/imber/imber_fx.hpp ../src/imber/imber_engine.hpp ../src/guttur.cpp ../src/guttur/guttur_banks.hpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

perge_render: perge_render.cpp ../src/perge.cpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

clean:
	rm -f draen_sweep new_modules_smoke perge_render

.PHONY: all clean
