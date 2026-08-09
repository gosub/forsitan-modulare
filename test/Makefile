# Offline test harnesses. They link against libRack from the SDK
# (RACK_DIR must be set, as for the plugin build itself).
RACK_DIR ?= ../../Rack-SDK

CXXFLAGS += -O2 -std=c++11 -I$(RACK_DIR)/include -I$(RACK_DIR)/dep/include
LDFLAGS += -L$(RACK_DIR) -lRack -Wl,-rpath,$(RACK_DIR)

# One smoke binary per module: `make -j` builds them in parallel and a change
# to one module only rebuilds its own harness.
SMOKE_MODULES = rete ululo tabes lustro bulla perge vorax textor imber sylla \
                guttur vespae quadrare vestigia antrum caligo raucus tundo \
                cartilago stridor crepitus ruina
SMOKE_BINS = $(addprefix smoke_,$(SMOKE_MODULES))

# Cross-module harnesses, run by `check` alongside the per-module ones.
# smoke_oom drives imber and sylla against a render they cannot allocate.
# The invariant harnesses are property-based rather than per-point: they
# randomize and sweep the whole control space and assert invariants over it.
CROSS_BINS = smoke_oom caligo_invariants raucus_invariants tundo_invariants \
             cartilago_invariants

all: draen_sweep perge_render guttur_probe vespae_probe antrum_probe \
     caligo_probe raucus_probe tundo_probe cartilago_probe textor_probe \
     stridor_probe crepitus_probe ruina_probe $(SMOKE_BINS) $(CROSS_BINS)

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

# Measurement harness for the reverb: RT60 vs decay/size/absorb, echo
# density, tail brightness, stereo correlation. Built by `make all`.
antrum_probe: antrum_probe.cpp smoke_harness.hpp ../src/antrum.cpp ../src/antrum_dsp.hpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

# Measurement harness for the echo: echo timing, decay per pass, diffusion
# density, stereo width vs. spin, drift excursion. Built by `make all`.
caligo_probe: caligo_probe.cpp smoke_harness.hpp ../src/caligo.cpp ../src/caligo_dsp.hpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

# Measurement harness for the fuzz: tone stack response against the published
# figures, diode table accuracy, gain structure. Built by `make all`.
raucus_probe: raucus_probe.cpp smoke_harness.hpp ../src/raucus.cpp ../src/raucus_dsp.hpp $(wildcard ../src/guttur/*.hpp)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

# Measurement harness for the Gristleizer: LFO alias floors and the polyBLAMP
# scale they fix, the FET transfer curve. Built by `make all`.
cartilago_probe: cartilago_probe.cpp smoke_harness.hpp ../src/cartilago.cpp $(wildcard ../src/guttur/*.hpp)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

# Measurement harness for the drum voice: level and decay against the knob,
# harm staging, spread ratios, fold energy, pitch tracking, per-mode survey and
# a CPU figure. `--wav <dir>` dumps the five target sounds instead.
tundo_probe: tundo_probe.cpp smoke_harness.hpp ../src/tundo.cpp ../src/tundo_dsp.hpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

# Measurement harness for the weaver: sample-to-sample discontinuities in
# the woven output, with the declick option on and off. Built by `make all`.
textor_probe: textor_probe.cpp smoke_harness.hpp ../src/textor.cpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

# Measurement harness for the friction voice: the level plane, the creak to
# squeal transition as slip rate, the material sweep, CPU. Built by `make all`.
stridor_probe: stridor_probe.cpp smoke_harness.hpp ../src/stridor.cpp $(wildcard ../src/sdt/*.hpp)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

# Measurement harness for the fracture process: event rate and level over the
# drive x crit plane, the avalanche statistics, CPU. Built by `make all`.
crepitus_probe: crepitus_probe.cpp smoke_harness.hpp ../src/crepitus.cpp ../src/fracture_dsp.hpp $(wildcard ../src/sdt/*.hpp)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

# Measurement harness for the object under load: the cube law on the load, the
# toughness knob in seconds, the Weibull spread against brittleness, the
# acoustic emission ramp, the collapse. Built by `make all`.
ruina_probe: ruina_probe.cpp smoke_harness.hpp ../src/ruina.cpp ../src/fracture_dsp.hpp $(wildcard ../src/sdt/*.hpp)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

# Property-based harnesses. `--seed N` reproduces a run, `--long` sweeps 8x
# as many patches.
caligo_invariants: caligo_invariants.cpp smoke_harness.hpp ../src/caligo.cpp ../src/caligo_dsp.hpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

raucus_invariants: raucus_invariants.cpp smoke_harness.hpp ../src/raucus.cpp ../src/raucus_dsp.hpp $(wildcard ../src/guttur/*.hpp)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

tundo_invariants: tundo_invariants.cpp smoke_harness.hpp ../src/tundo.cpp ../src/tundo_dsp.hpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

cartilago_invariants: cartilago_invariants.cpp smoke_harness.hpp ../src/cartilago.cpp $(wildcard ../src/guttur/*.hpp)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

smoke_%: smoke_%.cpp smoke_harness.hpp ../src/%.cpp ../src/forsitan.hpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

# Not one module's harness: it exercises both users of src/imber/.
smoke_oom: smoke_oom.cpp smoke_harness.hpp ../src/imber.cpp ../src/sylla.cpp \
           $(wildcard ../src/imber/*.hpp)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

# Header-only DSP libraries the modules pull in.
smoke_lustro: ../src/scando_engine.hpp
smoke_imber smoke_sylla: $(wildcard ../src/imber/*.hpp)
# vespae, raucus and cartilago reuse guttur's vendored ChowDSP oversampler
smoke_guttur smoke_vespae smoke_raucus smoke_cartilago: $(wildcard ../src/guttur/*.hpp)
smoke_raucus: ../src/raucus_dsp.hpp
smoke_quadrare: ../src/quadrare_dsp.hpp
smoke_antrum: ../src/antrum_dsp.hpp
smoke_caligo: ../src/caligo_dsp.hpp
smoke_tundo: ../src/tundo_dsp.hpp
# the SDT port, shared by the friction/fracture/motor modules
smoke_stridor: $(wildcard ../src/sdt/*.hpp)
smoke_crepitus smoke_ruina: ../src/fracture_dsp.hpp $(wildcard ../src/sdt/*.hpp)

# Run every smoke test, printing one CSV header for the lot.
# Fails if any binary does.
check: $(SMOKE_BINS) $(CROSS_BINS)
	@echo "module,check,value,pass"; \
	rc=0; \
	for t in $(SMOKE_BINS) $(CROSS_BINS); do ./$$t --no-header || rc=1; done; \
	exit $$rc

clean:
	rm -f draen_sweep perge_render guttur_probe vespae_probe antrum_probe \
	      caligo_probe raucus_probe tundo_probe cartilago_probe \
	      textor_probe stridor_probe crepitus_probe ruina_probe $(SMOKE_BINS) $(CROSS_BINS)

.PHONY: all check clean
