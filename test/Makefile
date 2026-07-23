# Offline test harnesses. They link against libRack from the SDK
# (RACK_DIR must be set, as for the plugin build itself).
RACK_DIR ?= ../../Rack-SDK

CXXFLAGS += -O2 -std=c++11 -I$(RACK_DIR)/include -I$(RACK_DIR)/dep/include
LDFLAGS += -L$(RACK_DIR) -lRack -Wl,-rpath,$(RACK_DIR)

# One smoke binary per module: `make -j` builds them in parallel and a change
# to one module only rebuilds its own harness.
SMOKE_MODULES = rete ululo tabes lustro bulla perge vorax textor imber sylla \
                guttur vespae quadrare
SMOKE_BINS = $(addprefix smoke_,$(SMOKE_MODULES))

all: draen_sweep perge_render guttur_probe vespae_probe $(SMOKE_BINS)

draen_sweep: draen_sweep.cpp ../src/draen_ugens.hpp ../src/draen_engines.hpp ../src/draen_alt_engines.hpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

perge_render: perge_render.cpp ../src/perge.cpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

guttur_probe: guttur_probe.cpp smoke_harness.hpp ../src/guttur.cpp $(wildcard ../src/guttur/*.hpp)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

# Measurement harness for the Wasp filter: response, Q vs cutoff, THD,
# self-oscillation. Built by `make all`, not run by `make check`.
vespae_probe: vespae_probe.cpp smoke_harness.hpp ../src/vespae.cpp $(wildcard ../src/guttur/*.hpp)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

smoke_%: smoke_%.cpp smoke_harness.hpp ../src/%.cpp ../src/forsitan.hpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

# Header-only DSP libraries the modules pull in.
smoke_lustro: ../src/scando_engine.hpp
smoke_imber smoke_sylla: $(wildcard ../src/imber/*.hpp)
# vespae reuses guttur's vendored ChowDSP oversampler
smoke_guttur smoke_vespae: $(wildcard ../src/guttur/*.hpp)
smoke_quadrare: ../src/quadrare_dsp.hpp

# Run every smoke test, printing one CSV header for the lot.
# Fails if any binary does.
check: $(SMOKE_BINS)
	@echo "module,check,value,pass"; \
	rc=0; \
	for t in $(SMOKE_BINS); do ./$$t --no-header || rc=1; done; \
	exit $$rc

clean:
	rm -f draen_sweep perge_render guttur_probe $(SMOKE_BINS)

.PHONY: all check clean
