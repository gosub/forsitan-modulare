# Offline test harnesses. They link against libRack from the SDK
# (RACK_DIR must be set, as for the plugin build itself).
RACK_DIR ?= ../../Rack-SDK

CXXFLAGS += -O2 -std=c++11 -I$(RACK_DIR)/include -I$(RACK_DIR)/dep/include
LDFLAGS += -L$(RACK_DIR) -lRack -Wl,-rpath,$(RACK_DIR)

# One smoke binary per module: `make -j` builds them in parallel and a change
# to one module only rebuilds its own harness.
SMOKE_MODULES = rete ululo tabes lustro bulla perge vorax textor imber sylla \
                guttur vespae quadrare vestigia antrum caligo raucus tundo \
                cartilago scrupea viginti gradus materiae aether vates
SMOKE_BINS = $(addprefix smoke_,$(SMOKE_MODULES))

# Cross-module harnesses, run by `check` alongside the per-module ones.
# smoke_oom drives imber and sylla against a render they cannot allocate.
# The invariant harnesses are property-based rather than per-point: they
# randomize and sweep the whole control space and assert invariants over it.
CROSS_BINS = smoke_oom caligo_invariants raucus_invariants tundo_invariants \
             cartilago_invariants scrupea_invariants viginti_invariants

all: draen_sweep perge_render guttur_probe vespae_probe antrum_probe \
     caligo_probe raucus_probe tundo_probe cartilago_probe textor_probe \
     scrupea_probe viginti_probe materiae_probe materiae_presets materiae_random \
     aether_probe vates_probe $(SMOKE_BINS) $(CROSS_BINS)

# Measures and auditions the generated banks: every family of the four ported
# pages64 kit engines, the imber-derived tonal families, a whole six-bank set
# from one seed, and `wav <dir>` to write them out and listen.
vates_probe: vates_probe.cpp ../src/vates/kits.hpp ../src/vates/bank.hpp \
             ../src/imber/imber_gen.hpp ../src/imber/imber_dsp.hpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

# Measurement and audition harness for the square-relation percussion voice.
# It drives the engine header alone and needs no Rack. `ops` is the one that
# earns its keep: the pairwise spectral distance between operators is what
# says whether the operator set is worth its knob positions.
materiae_probe: materiae_probe.cpp ../src/materiae_dsp.hpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

# Loads the factory presets back into a real module and measures them: audible,
# inside the swing, finite, and no two of them the same sound.
materiae_presets: materiae_presets.cpp smoke_harness.hpp ../src/materiae.cpp \
                  ../src/materiae_dsp.hpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

# What Ctrl-R actually gives you: how many uniform randomizations come out
# inaudible and which knobs are responsible, plus level against the GAIN knob.
materiae_random: materiae_random.cpp smoke_harness.hpp ../src/materiae.cpp \
                 ../src/materiae_dsp.hpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

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

scrupea_invariants: scrupea_invariants.cpp smoke_harness.hpp ../src/scrupea.cpp ../src/scrupea_dsp.hpp
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
smoke_viginti: ../src/viginti_dsp.hpp
smoke_materiae: ../src/materiae_dsp.hpp
smoke_aether: ../src/aether_dsp.hpp $(wildcard ../src/guttur/*.hpp)

# Run every smoke test, printing one CSV header for the lot.
# Fails if any binary does.
# Measurement harness for the chaotic bank: levels per topology, what the
# macros do to level and brightness, a largest-Lyapunov estimate that says
# whether the bank is really chaotic, and CPU. Built by `make all`.
scrupea_probe: scrupea_probe.cpp smoke_harness.hpp ../src/scrupea.cpp ../src/scrupea_dsp.hpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

# Measurement harness for the KORG35 filter: the Lambert-W solver, the limits
# of the diode nonlinearity, the production scheme against the RK4 reference
# across sample rates, frequency response, level dependence, the
# self-oscillation threshold and CPU. Built by `make all`.
# Neither of the two viginti harnesses links Rack: the DSP header is free of
# Rack headers, so they build straight from it.
viginti_probe: viginti_probe.cpp ../src/viginti_dsp.hpp
	$(CXX) $(CXXFLAGS) $< -o $@

viginti_invariants: viginti_invariants.cpp ../src/viginti_dsp.hpp
	$(CXX) $(CXXFLAGS) $< -o $@

# Measurement harness for the transmission line: the converter's pulse rate
# law, the loop's lock and capture range per phase comparator, the ratio that
# the distortion is made of, bandwidth against TONE, the standalone
# oscillator, where the carrier folds the input, and CPU. Rack-free, like the
# two viginti harnesses.
aether_probe: aether_probe.cpp ../src/aether_dsp.hpp
	$(CXX) $(CXXFLAGS) $< -o $@

check: $(SMOKE_BINS) $(CROSS_BINS)
	@echo "module,check,value,pass"; \
	rc=0; \
	for t in $(SMOKE_BINS) $(CROSS_BINS); do ./$$t --no-header || rc=1; done; \
	exit $$rc

clean:
	rm -f draen_sweep perge_render guttur_probe vespae_probe antrum_probe \
	      caligo_probe raucus_probe tundo_probe cartilago_probe \
	      textor_probe scrupea_probe viginti_probe aether_probe \
	      $(SMOKE_BINS) $(CROSS_BINS)

.PHONY: all check clean
