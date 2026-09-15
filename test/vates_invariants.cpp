// vates_invariants - property-based checks for the sample player's output.
//
// smoke_vates checks the module at fixed points. This harness asserts what
// must hold *everywhere*: whatever the knobs, the CV and the gestures, both
// audio outputs stay finite, stay inside the rails, and never latch.
//
// It exists because a user reported vates' right channel sitting at a solid
// 10 V with the left one fine, and smoke_vates' abuse test could not have
// caught it: process() ends in clamp(x, -10, 10), and rack::clamp is
// fmax(fmin(x, hi), lo), so fmin(NaN, 10) = 10. A NaN leaves the module as
// exactly +10.000 V. Stats::nans counts nothing and the peak check passes.
// A non-finite sample therefore has to be looked for at the output as a
// *latch*, not as a NaN.
//
// ── the invariants ─────────────────────────────────────────────────────────
//
//   P1 finite      randomized params, CV and gestures: no output sample is
//                  non-finite and none leaves +-10 V
//   P2 no_latch    neither channel holds one constant non-zero value for
//                  longer than 50 ms. Silence between hits is flat and
//                  legal; a stuck value is not.
//   P3 no_rail     neither channel sits at exactly +-10 V for longer than
//                  5 ms - a real signal crosses the rail, it does not rest
//                  on it
//   P4 recovers    a single non-finite sample poked into each piece of state
//                  that can hold one must be gone from the output within
//                  200 ms. This is the invariant the report is about: state
//                  that feeds itself keeps a NaN for ever.
//
// Failures print the offending case to stderr, and the RNG is seeded
// (--seed) so any failure reproduces exactly.

#include "smoke_harness.hpp"
#include "../src/vates.cpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <thread>

static uint32_t gSeed = 0x1234567u;
static int gTrials = 24;

struct Rng {
	uint32_t s;
	explicit Rng(uint32_t seed) : s(seed ? seed : 1u) {}
	uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
	float uni() { return (float)(next() >> 8) * (1.f / 16777216.f); }
	float range(float lo, float hi) { return lo + (hi - lo) * uni(); }
	int pick(int n) { return (int)(next() % (uint32_t)n); }
};

static void run(Vates& m, long& frame, double seconds) {
	long n = (long)(seconds * SR);
	for (long i = 0; i < n; i++)
		m.process(makeArgs(frame++));
}

static bool waitForBanks(Vates& m, long& frame) {
	for (int i = 0; i < 400; i++) {
		run(m, frame, 0.01);
		if (m.banks && m.banks->ready)
			return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(25));
	}
	return false;
}

// What a run of the module looks like from outside: the worst thing each
// channel did, and where.
struct Watch {
	long nonFinite[2] = {0, 0};
	float peak[2] = {0.f, 0.f};
	// the longest stretch of one repeated value, and of one sitting on a rail
	long flat[2] = {0, 0}, rail[2] = {0, 0};
	long flatRun[2] = {0, 0}, railRun[2] = {0, 0};
	float last[2] = {0.f, 0.f};
	long firstBad[2] = {-1, -1};
	bool started = false;
	long frame = 0;

	void add(float l, float r) {
		float v[2] = {l, r};
		for (int c = 0; c < 2; c++) {
			if (!std::isfinite(v[c])) {
				nonFinite[c]++;
				if (firstBad[c] < 0) firstBad[c] = frame;
			}
			else {
				peak[c] = std::max(peak[c], std::fabs(v[c]));
			}
			// Silence is flat and perfectly legal: the gap between two hits
			// is exactly 0 V for as long as the gap lasts. Only a stuck
			// *value* is a latch.
			if (started && v[c] == last[c] && std::fabs(v[c]) > 1e-6f) flatRun[c]++;
			else flatRun[c] = 0;
			flat[c] = std::max(flat[c], flatRun[c]);
			if (std::fabs(v[c]) >= 9.999f) railRun[c]++;
			else railRun[c] = 0;
			if (railRun[c] > rail[c]) {
				rail[c] = railRun[c];
				if (firstBad[c] < 0 && rail[c] > (long)(0.005 * SR))
					firstBad[c] = frame;
			}
			last[c] = v[c];
		}
		started = true;
		frame++;
	}
	long worstFlat() const { return std::max(flat[0], flat[1]); }
	long worstRail() const { return std::max(rail[0], rail[1]); }
};

static void step(Vates& m, Watch& w, long& frame) {
	m.process(makeArgs(frame++));
	w.add(m.outputs[Vates::LEFT_OUTPUT].getVoltage(),
	      m.outputs[Vates::RIGHT_OUTPUT].getVoltage());
}

static void runWatched(Vates& m, Watch& w, long& frame, double seconds) {
	long n = (long)(seconds * SR);
	for (long i = 0; i < n; i++)
		step(m, w, frame);
}

// Print enough of the module's state to set a real Rack up the same way.
static void dumpState(Vates& m, uint32_t seed, int trial) {
	fprintf(stderr, "  seed=%u trial=%d bank=%d sample=%d\n",
	        seed, trial, m.bankIndex, m.sampleIndex);
	for (int i = 0; i < Vates::PARAMS_LEN; i++) {
		ParamQuantity* pq = m.getParamQuantity(i);
		fprintf(stderr, "  param[%2d] %-24s = %.4f\n", i,
		        pq ? pq->name.c_str() : "?", m.params[i].getValue());
	}
	for (int i = 0; i < Vates::INPUTS_LEN; i++)
		if (m.inputs[i].isConnected())
			fprintf(stderr, "  input[%2d] = %.3f V\n", i, m.inputs[i].getVoltage());
}

// ── P1/P2/P3: randomized knobs, CV and gestures ──────────────────────────────
//
// Every knob lands anywhere in its range and then keeps moving, every input
// is patched and fed an LFO, and triggers arrive at a musical rate. Each
// trial is a fresh module so a failure is a state one trial can reach on its
// own rather than the residue of the one before it.
static void testFuzz() {
	long worstFlat = 0, worstRail = 0, nonFinite = 0;
	float worstPeak = 0.f;
	int failedTrial = -1;

	for (int t = 0; t < gTrials && failedTrial < 0; t++) {
		Rng rng(gSeed + 7919u * (uint32_t)t);
		Vates m;
		long fr = 0;
		if (!waitForBanks(m, fr)) {
			report("vates", "fuzz_setup", 0, false);
			return;
		}

		// knobs anywhere, attenuverters included
		for (int i = 0; i < Vates::PARAMS_LEN; i++) {
			ParamQuantity* pq = m.getParamQuantity(i);
			if (!pq)
				continue;
			m.params[i].setValue(rng.range(pq->minValue, pq->maxValue));
		}
		m.params[Vates::TRIG_PARAM].setValue(0.f);
		m.params[Vates::BANK_UP_PARAM].setValue(0.f);
		m.params[Vates::BANK_DOWN_PARAM].setValue(0.f);
		m.params[Vates::LEVEL_PARAM].setValue(1.f);   // worst case for the rails

		// every input patched, each with its own LFO rate and depth
		float rate[Vates::INPUTS_LEN], depth[Vates::INPUTS_LEN], phase[Vates::INPUTS_LEN];
		for (int i = 0; i < Vates::INPUTS_LEN; i++) {
			m.inputs[i].channels = 1;
			rate[i] = rng.range(0.05f, 300.f);   // up into the audio band
			depth[i] = rng.range(0.f, 10.f);
			phase[i] = rng.uni();
		}

		Watch w;
		const double seconds = 4.0;
		long n = (long)(seconds * SR);
		for (long i = 0; i < n; i++) {
			float ph = (float)i / SR;
			for (int in = 0; in < Vates::INPUTS_LEN; in++)
				m.inputs[in].setVoltage(
				    depth[in] * std::sin(2.f * (float)M_PI * (rate[in] * ph + phase[in])));
			// gestures: a trigger a few times a second, and the fx and filter
			// knobs walking their whole travel so every mode is entered and
			// left while the state is hot
			if (i % (long)(SR / 6) == 0)
				m.params[Vates::TRIG_PARAM].setValue(1.f);
			else if (i % (long)(SR / 6) == 4)
				m.params[Vates::TRIG_PARAM].setValue(0.f);
			if (i % 64 == 0) {
				m.params[Vates::FX_PARAM].setValue(std::sin(2.f * (float)M_PI * 0.37f * ph));
				m.params[Vates::FILTER_PARAM].setValue(std::sin(2.f * (float)M_PI * 0.23f * ph));
			}
			step(m, w, fr);
		}

		nonFinite += w.nonFinite[0] + w.nonFinite[1];
		worstPeak = std::max(worstPeak, std::max(w.peak[0], w.peak[1]));
		worstFlat = std::max(worstFlat, w.worstFlat());
		worstRail = std::max(worstRail, w.worstRail());

		bool bad = (w.nonFinite[0] + w.nonFinite[1]) > 0
		           || w.worstRail() > (long)(0.005 * SR)
		           || w.worstFlat() > (long)(0.050 * SR)
		           || std::max(w.peak[0], w.peak[1]) > 10.001f;
		if (bad) {
			failedTrial = t;
			fprintf(stderr, "vates_invariants: trial %d failed"
			        " (flat L=%ld R=%ld, rail L=%ld R=%ld, nonfinite L=%ld R=%ld)\n",
			        t, w.flat[0], w.flat[1], w.rail[0], w.rail[1],
			        w.nonFinite[0], w.nonFinite[1]);
			dumpState(m, gSeed, t);
		}
	}

	report("vates", "fuzz_nonfinite", (double)nonFinite, nonFinite == 0);
	report("vates", "fuzz_peak_v", worstPeak, worstPeak <= 10.001f);
	report("vates", "fuzz_rail_ms", 1000.0 * worstRail / SR, worstRail <= (long)(0.005 * SR));
	report("vates", "fuzz_flat_ms", 1000.0 * worstFlat / SR, worstFlat <= (long)(0.050 * SR));
}

// ── P4: one bad sample must not be permanent ─────────────────────────────────
//
// Each of these pokes a single non-finite value into a piece of state that
// feeds itself, with the fx knob parked in the mode that reads it, and asks
// whether the output is clean again six seconds later. Anything that answers no
// reproduces the report exactly: one channel stuck at +10 V, the other fine,
// for as long as the knob stays where it is.
static void injectOnce(const char* name, float fxKnob, int which) {
	Vates m;
	long fr = 0;
	if (!waitForBanks(m, fr)) {
		report("vates", "inject_setup", 0, false);
		return;
	}
	m.params[Vates::LEVEL_PARAM].setValue(1.f);
	m.params[Vates::LENGTH_PARAM].setValue(0.8f);
	m.params[Vates::FX_PARAM].setValue(fxKnob);
	m.bankBase = 0;
	m.params[Vates::SAMPLE_PARAM].setValue(4.5f / vates_bank::kSamplesPerBank);
	run(m, fr, 0.05);

	// a hit under way, so the fx lines have signal in them
	m.params[Vates::TRIG_PARAM].setValue(1.f);
	run(m, fr, 0.001);
	m.params[Vates::TRIG_PARAM].setValue(0.f);
	run(m, fr, 0.05);

	const float bad = std::numeric_limits<float>::quiet_NaN();
	// The slot the write head will reach LAST, which every read distance
	// shorter than the buffer therefore reaches first. write() lands on
	// buf[w] and then advances, so w is the NEXT slot to be overwritten and
	// w-1 the one that survives longest: poking anywhere ahead of the head
	// poisons nothing, and the test passes for the wrong reason.
	switch (which) {
		case 0: m.mod[1].buf[(m.mod[1].w + m.mod[1].size() - 1) % m.mod[1].size()] = bad; break;
		case 1: m.dly[1].buf[(m.dly[1].w + m.dly[1].size() - 1) % m.dly[1].size()] = bad; break;
		case 2: m.filt[1].ic1 = bad; break;
		case 3: m.filtB[1].ic2 = bad; break;
		case 4: m.filtSmooth = bad; break;
		// one bad frame of sample data, as a malformed or float-NaN wav would
		// give: the right channel only, which is the shape of the report
		case 5: {
			if (!m.voiceR)
				break;
			std::vector<float>& r = const_cast<std::vector<float>&>(*m.voiceR);
			size_t i = (size_t)m.voicePos + 64;
			if (i < r.size())
				r[i] = bad;
			break;
		}
	}

	// Long enough for a poisoned line to fill. One bad sample spreads by one
	// read distance a lap, so a 50 ms chorus line takes seconds to go solid:
	// a window right after the poke sees a tick and calls it recovered.
	run(m, fr, 6.0);
	Watch w;
	runWatched(m, w, fr, 0.5);

	bool clean = w.nonFinite[0] + w.nonFinite[1] == 0
	             && w.worstRail() < (long)(0.005 * SR);
	report("vates", name, (double)std::max(w.rail[0], w.rail[1]), clean);
	if (!clean)
		fprintf(stderr, "vates_invariants: %s latched"
		        " (rail L=%ld R=%ld samples, out L=%.3f R=%.3f)\n",
		        name, w.rail[0], w.rail[1],
		        m.outputs[Vates::LEFT_OUTPUT].getVoltage(),
		        m.outputs[Vates::RIGHT_OUTPUT].getVoltage());
}

static void testInjection() {
	injectOnce("recovers_chorus_line", 0.6f, 0);
	injectOnce("recovers_delay_line", -0.6f, 1);
	injectOnce("recovers_filter_a", -0.5f, 2);
	injectOnce("recovers_filter_b", 0.5f, 3);
	injectOnce("recovers_filter_knob", 0.5f, 4);
	injectOnce("recovers_bad_frame_dry", 0.f, 5);
	injectOnce("recovers_bad_frame_chorus", 0.6f, 5);
	injectOnce("recovers_bad_frame_delay", -0.6f, 5);
}


// ── P5: a wav the loader was handed, rather than one it made ─────────────────
//
// The generated banks are clean by construction, so the only sample data that
// can carry a non-finite value into the engine is a user kit. 32-bit float is
// a wav format like any other and nothing on the way in looks at what the
// bytes mean: a file with a NaN, an infinity or a wild value in it - a bad
// render, a truncated export, a converter that wrote garbage past the end -
// arrives in the voice as it was written.
//
// Only the right channel is poisoned here, which is the shape of the report.
static void writeFloatWav(const std::string& path, int frames, float poison,
                          int poisonAt) {
	FILE* f = fopen(path.c_str(), "wb");
	if (!f)
		return;
	auto w32 = [&](uint32_t v) { fputc(v & 255, f); fputc((v >> 8) & 255, f);
	                             fputc((v >> 16) & 255, f); fputc((v >> 24) & 255, f); };
	auto w16 = [&](uint16_t v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); };
	auto wf  = [&](float v) { uint32_t u; std::memcpy(&u, &v, 4); w32(u); };
	const int channels = 2;
	uint32_t bytes = (uint32_t)(frames * channels * 4);
	fwrite("RIFF", 1, 4, f); w32(36 + bytes); fwrite("WAVE", 1, 4, f);
	fwrite("fmt ", 1, 4, f); w32(16); w16(3); w16(channels); w32(44100);
	w32(44100 * channels * 4); w16((uint16_t)(channels * 4)); w16(32);
	fwrite("data", 1, 4, f); w32(bytes);
	for (int i = 0; i < frames; i++) {
		float v = 0.8f * std::sin(2.f * (float)M_PI * 220.f * i / 44100.f);
		wf(v);                                     // left, always clean
		wf(i == poisonAt ? poison : v);            // right
	}
	fclose(f);
}

static void badWav(const char* name, float poison) {
	const std::string root = "/tmp/vates_invariants_kits";
	const std::string kit = root + "/bad";
	rack::system::createDirectories(kit);
	writeFloatWav(kit + "/00.wav", 8820, poison, 400);
	forsitan_sampler::setKitsFolder(root, /*persist=*/false);

	Vates m;
	long fr = 0;
	if (!waitForBanks(m, fr)) {
		report("vates", "badwav_setup", 0, false);
		return;
	}
	m.refreshKits();
	int bank = vates_bank::kNumBanks;
	m.bankBase = bank;
	run(m, fr, 0.05);
	for (int i = 0; i < 200 && m.samplesInBank(bank) == 0; i++) {
		run(m, fr, 0.01);
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	if (m.samplesInBank(bank) < 1) {
		report("vates", name, 0, false);
		return;
	}
	m.params[Vates::LEVEL_PARAM].setValue(1.f);
	m.params[Vates::LENGTH_PARAM].setValue(0.8f);
	m.params[Vates::FX_PARAM].setValue(0.6f);      // the chorus, whose two lines
	m.params[Vates::SAMPLE_PARAM].setValue(0.5f);  // do not feed each other
	run(m, fr, 0.01);
	m.params[Vates::TRIG_PARAM].setValue(1.f);
	run(m, fr, 0.001);
	m.params[Vates::TRIG_PARAM].setValue(0.f);

	// long enough for a poisoned line to fill: the chorus buffer is 50 ms and
	// a NaN in it spreads by one read distance a lap
	run(m, fr, 6.0);
	Watch w;
	runWatched(m, w, fr, 0.5);
	bool clean = w.nonFinite[0] + w.nonFinite[1] == 0
	             && w.worstRail() < (long)(0.005 * SR);
	report("vates", name, (double)std::max(w.rail[0], w.rail[1]), clean);
	if (!clean)
		fprintf(stderr, "vates_invariants: %s latched (out L=%.4f R=%.4f)\n", name,
		        m.outputs[Vates::LEFT_OUTPUT].getVoltage(),
		        m.outputs[Vates::RIGHT_OUTPUT].getVoltage());
}

static void testBadWav() {
	badWav("wav_nan_right", std::numeric_limits<float>::quiet_NaN());
	badWav("wav_inf_right", std::numeric_limits<float>::infinity());
	badWav("wav_huge_right", 1e30f);
}

int main(int argc, char** argv) {
	bool header = true;
	for (int i = 1; i < argc; i++) {
		if (!std::strcmp(argv[i], "--no-header"))
			header = false;
		else if (!std::strcmp(argv[i], "--seed") && i + 1 < argc)
			gSeed = (uint32_t)std::strtoul(argv[++i], nullptr, 0);
		else if (!std::strcmp(argv[i], "--trials") && i + 1 < argc)
			gTrials = std::atoi(argv[++i]);
	}
	rack::random::init();
	if (header)
		printf("module,check,value,pass\n");
	testInjection();
	testBadWav();
	testFuzz();
	return failures ? 1 : 0;
}
