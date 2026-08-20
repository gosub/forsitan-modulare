// smoke_vates — offline sanity checks for the vates sample player.
// See smoke_harness.hpp for the shared scaffolding and CSV format.
//
// The generated banks themselves are measured by vates_probe, which does not
// involve Rack. These are the module's checks: that the banks arrive, that a
// hit plays, that the length knob reverses it, what a retrigger does, and
// that the clock, the LFO and the pattern generator behave.
//
// The banks are built on a detached worker, so the first thing every test
// does is wait for it — with wall-clock sleeps, as in smoke_imber.

#include "smoke_harness.hpp"
#include "../src/vates.cpp"

#include <chrono>
#include <thread>

static void run(Vates& m, long& frame, double seconds) {
	long n = (long)(seconds * SR);
	for (long i = 0; i < n; i++)
		m.process(makeArgs(frame++));
}

static Stats runStats(Vates& m, long& frame, double seconds, int outId) {
	Stats s;
	long n = (long)(seconds * SR);
	for (long i = 0; i < n; i++) {
		m.process(makeArgs(frame++));
		s.add(m.outputs[outId].getVoltage());
	}
	return s;
}

// Drive the module until the bank worker has delivered, or give up.
static bool waitForBanks(Vates& m, long& frame) {
	for (int i = 0; i < 400; i++) {
		run(m, frame, 0.01);
		if (m.banks && m.banks->ready)
			return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(25));
	}
	return false;
}

static void selectSample(Vates& m, int bank, int sample) {
	int nb = m.bankCount();
	m.params[Vates::BANK_PARAM].setValue((bank + 0.5f) / std::max(nb, 1));
	m.params[Vates::SAMPLE_PARAM].setValue((sample + 0.5f) / vates_bank::kSamplesPerBank);
}

// The button fires on the rising edge, so four frames is a press — and the
// measurement must start there: a 2 ms micro sample is over before a longer
// press ends, and the window would see nothing but silence.
static void pressTrigger(Vates& m, long& frame) {
	m.params[Vates::TRIG_PARAM].setValue(1.f);
	for (int i = 0; i < 4; i++)
		m.process(makeArgs(frame++));
	m.params[Vates::TRIG_PARAM].setValue(0.f);
}

// Process until the pattern generator has advanced n steps, whatever the
// tempo works out to in frames.
static void runSteps(Vates& m, long& frame, int n) {
	int was = m.step;
	int seen = 0;
	long guard = (long)(30.0 * SR);
	while (seen < n && guard-- > 0) {
		m.process(makeArgs(frame++));
		if (m.step != was) {
			was = m.step;
			seen++;
		}
	}
}

// ── the banks arrive, and every sample in them plays ──────────────────────────
static void testBanks() {
	Vates m;
	long fr = 0;
	bool ok = waitForBanks(m, fr);
	report("vates", "banks_ready", ok ? 1 : 0, ok);
	if (!ok)
		return;

	int quiet = 0;
	long nans = 0;
	float loudest = 0.f;
	for (int b = 0; b < vates_bank::kNumBanks; b++)
		for (int s = 0; s < vates_bank::kSamplesPerBank; s++) {
			selectSample(m, b, s);
			m.params[Vates::LENGTH_PARAM].setValue(1.f);   // play it whole
			run(m, fr, 0.01);
			pressTrigger(m, fr);
			Stats st = runStats(m, fr, 0.35, Vates::LEFT_OUTPUT);
			nans += st.nans;
			loudest = std::max(loudest, st.peak);
			if (st.rms() < 1e-4)
				quiet++;
		}
	report("vates", "bank_sample_nans", nans, nans == 0);
	report("vates", "bank_samples_quiet", quiet, quiet == 0);
	report("vates", "bank_peak_v", loudest, loudest > 0.5f && loudest < 11.f);
}

// ── the length knob: decay one way, a reversed swell the other ────────────────
static void testLength() {
	Vates m;
	long fr = 0;
	if (!waitForBanks(m, fr)) {
		report("vates", "length_setup", 0, false);
		return;
	}
	selectSample(m, 4, 0);            // tones: 2.5 s of sustained material
	m.params[Vates::LENGTH_PARAM].setValue(0.55f);
	run(m, fr, 0.01);
	pressTrigger(m, fr);
	Stats fwd1 = runStats(m, fr, 0.25, Vates::LEFT_OUTPUT);
	Stats fwd2 = runStats(m, fr, 0.25, Vates::LEFT_OUTPUT);
	report("vates", "forward_decays", fwd2.rms() / std::max(fwd1.rms(), 1e-9),
	       fwd2.rms() < fwd1.rms());

	m.params[Vates::LENGTH_PARAM].setValue(-0.55f);
	run(m, fr, 0.5);
	pressTrigger(m, fr);
	Stats rev1 = runStats(m, fr, 0.25, Vates::LEFT_OUTPUT);
	Stats rev2 = runStats(m, fr, 0.25, Vates::LEFT_OUTPUT);
	report("vates", "reverse_swells", rev2.rms() / std::max(rev1.rms(), 1e-9),
	       rev2.rms() > rev1.rms());

	// the envelope output follows, and stays inside its rails
	Stats env = runStats(m, fr, 0.2, Vates::ENV_OUTPUT);
	report("vates", "env_range", env.peak, env.peak <= 10.001f && env.nans == 0);
}

// ── a retrigger chokes a forward hit and is refused during a reverse swell ────
static void testRetrigger() {
	Vates m;
	long fr = 0;
	if (!waitForBanks(m, fr)) {
		report("vates", "retrigger_setup", 0, false);
		return;
	}
	selectSample(m, 4, 1);
	m.params[Vates::LENGTH_PARAM].setValue(0.35f);
	run(m, fr, 0.01);
	pressTrigger(m, fr);
	run(m, fr, 0.2);
	float before = m.env;
	pressTrigger(m, fr);
	float after = m.env;
	report("vates", "forward_retriggers", after - before, after > before && after > 0.8f);

	// a reversed hit that is still swelling keeps its position
	m.params[Vates::LENGTH_PARAM].setValue(-0.8f);
	run(m, fr, 0.6);
	pressTrigger(m, fr);
	run(m, fr, 0.05);
	double pos = m.voicePos;
	bool attacking = m.voiceAttack;
	pressTrigger(m, fr);
	double posAfter = m.voicePos;
	report("vates", "reverse_attack_holds", posAfter - pos,
	       attacking && posAfter < pos);
}

// ── play fires on a crossing, cue waits for the trigger ───────────────────────
static void testPlayCue() {
	Vates m;
	long fr = 0;
	if (!waitForBanks(m, fr)) {
		report("vates", "playcue_setup", 0, false);
		return;
	}
	selectSample(m, 0, 0);
	m.params[Vates::LENGTH_PARAM].setValue(0.2f);
	m.params[Vates::SAMPLE_ATT_PARAM].setValue(1.f);
	m.inputs[Vates::SAMPLE_INPUT].channels = 1;

	// a ramp across the whole bank, in play mode: every crossing is a hit
	m.params[Vates::MODE_PARAM].setValue(1.f);
	int hits = 0;
	float wasEnv = 0.f;
	for (int i = 0; i < (int)(0.8 * SR); i++) {
		m.inputs[Vates::SAMPLE_INPUT].setVoltage(5.f * i / (0.8f * SR));
		m.process(makeArgs(fr++));
		if (m.env > wasEnv + 0.5f)
			hits++;
		wasEnv = m.env;
	}
	report("vates", "play_mode_hits", hits, hits >= 4);

	// the same ramp in cue mode fires nothing
	m.params[Vates::MODE_PARAM].setValue(0.f);
	m.voiceActive = false;
	m.env = 0.f;
	int cueHits = 0;
	wasEnv = 0.f;
	for (int i = 0; i < (int)(0.8 * SR); i++) {
		m.inputs[Vates::SAMPLE_INPUT].setVoltage(5.f * i / (0.8f * SR));
		m.process(makeArgs(fr++));
		if (m.env > wasEnv + 0.5f)
			cueHits++;
		wasEnv = m.env;
	}
	report("vates", "cue_mode_hits", cueHits, cueHits == 0);
}

// ── the sample knob browses, it does not play ─────────────────────────────────
// Turning the knob by hand in play mode used to fire a hit at every step,
// which made looking for a sound unbearable. Only modulation crosses.
static void testKnobBrowsing() {
	Vates m;
	long fr = 0;
	if (!waitForBanks(m, fr)) {
		report("vates", "browse_setup", 0, false);
		return;
	}
	m.params[Vates::MODE_PARAM].setValue(1.f);   // play
	m.params[Vates::LENGTH_PARAM].setValue(0.5f);
	selectSample(m, 0, 0);
	run(m, fr, 0.05);

	int hits = 0;
	float wasEnv = m.env;
	long n = (long)(1.5 * SR);
	for (long i = 0; i < n; i++) {
		m.params[Vates::SAMPLE_PARAM].setValue((float)i / n);
		m.process(makeArgs(fr++));
		if (m.env > wasEnv + 0.5f)
			hits++;
		wasEnv = m.env;
	}
	report("vates", "sample_knob_silent", hits, hits == 0);

	// the same for the bank knob, which also shifts the sample index
	hits = 0;
	wasEnv = m.env;
	for (long i = 0; i < n; i++) {
		m.params[Vates::BANK_PARAM].setValue((float)i / n);
		m.process(makeArgs(fr++));
		if (m.env > wasEnv + 0.5f)
			hits++;
		wasEnv = m.env;
	}
	report("vates", "bank_knob_silent", hits, hits == 0);
}

// ── the knob spans the list end to end ────────────────────────────────────────
// Its top used to land back on the first entry, one step past the last.
static void testKnobRange() {
	Vates m;
	long fr = 0;
	if (!waitForBanks(m, fr)) {
		report("vates", "range_setup", 0, false);
		return;
	}
	m.params[Vates::BANK_PARAM].setValue(0.f);
	m.params[Vates::SAMPLE_PARAM].setValue(1.f);
	run(m, fr, 0.02);
	report("vates", "sample_knob_top", m.aimedSample,
	       m.aimedSample == vates_bank::kSamplesPerBank - 1);

	m.params[Vates::SAMPLE_PARAM].setValue(0.f);
	run(m, fr, 0.02);
	report("vates", "sample_knob_bottom", m.aimedSample, m.aimedSample == 0);

	// every sample reachable, exactly one eighth of the knob each
	int seen[16] = {0};
	for (int i = 0; i < 8; i++) {
		m.params[Vates::SAMPLE_PARAM].setValue((i + 0.5f) / 8.f);
		run(m, fr, 0.02);
		if (m.aimedSample >= 0 && m.aimedSample < 16)
			seen[m.aimedSample]++;
	}
	int covered = 0;
	for (int i = 0; i < vates_bank::kSamplesPerBank; i++)
		covered += seen[i] == 1 ? 1 : 0;
	report("vates", "sample_knob_covers", covered, covered == vates_bank::kSamplesPerBank);

	// and the bank knob's top is the last bank, not the first
	m.params[Vates::BANK_PARAM].setValue(1.f);
	run(m, fr, 0.02);
	report("vates", "bank_knob_top", m.bankIndex, m.bankIndex == m.bankCount() - 1);
}

// ── a module straight out of the browser makes a sound, not a click ───────────
static void testDefaults() {
	Vates m;
	long fr = 0;
	if (!waitForBanks(m, fr)) {
		report("vates", "defaults_setup", 0, false);
		return;
	}
	selectSample(m, 4, 0);        // a sustained sample, so length is what decides
	run(m, fr, 0.02);
	pressTrigger(m, fr);
	run(m, fr, 0.1);
	Stats late = runStats(m, fr, 0.1, Vates::LEFT_OUTPUT);
	report("vates", "default_length_audible", late.rms(), late.rms() > 0.05);
}

// ── clock, internal and external ──────────────────────────────────────────────
static void testClock() {
	Vates m;
	long fr = 0;
	m.params[Vates::TEMPO_PARAM].setValue(120.f);   // 8 sixteenths per second
	m.params[Vates::RHYTHM_PARAM].setValue(4.f);    // every step
	m.params[Vates::GSW_PARAM].setValue(1.f);
	run(m, fr, 0.05);

	int gates = 0;
	bool was = false;
	for (int i = 0; i < (int)(2.0 * SR); i++) {
		m.process(makeArgs(fr++));
		bool g = m.outputs[Vates::GATE_OUTPUT].getVoltage() > 5.f;
		if (g && !was)
			gates++;
		was = g;
	}
	report("vates", "internal_gates_2s", gates, gates >= 15 && gates <= 17);

	// an external clock takes the tempo over: 32 pulses in 2 s
	m.inputs[Vates::CLK_INPUT].channels = 1;
	int extGates = 0;
	was = false;
	long period = (long)(SR / 16.f);
	for (long i = 0; i < (long)(2.0 * SR); i++) {
		m.inputs[Vates::CLK_INPUT].setVoltage((i % period) < 40 ? 10.f : 0.f);
		m.process(makeArgs(fr++));
		bool g = m.outputs[Vates::GATE_OUTPUT].getVoltage() > 5.f;
		if (g && !was)
			extGates++;
		was = g;
	}
	report("vates", "external_gates_2s", extGates, extGates >= 30 && extGates <= 34);

	// and hands the tempo back when it stops
	m.inputs[Vates::CLK_INPUT].setVoltage(0.f);
	run(m, fr, 2.5);
	report("vates", "clock_handback", m.externalClock ? 1 : 0, !m.externalClock);
}

// ── the pattern switches rewrite the sequence ─────────────────────────────────
static void testPatternSwitches() {
	Vates m;
	long fr = 0;
	m.params[Vates::TEMPO_PARAM].setValue(240.f);
	m.params[Vates::RHYTHM_PARAM].setValue(0.f);    // four on the floor
	m.params[Vates::GSW_PARAM].setValue(1.f);
	run(m, fr, 0.5);
	uint16_t asIs = m.gateWork;
	report("vates", "rhythm_loaded", asIs, asIs == 0x8888);

	m.params[Vates::GSW_PARAM].setValue(0.f);       // invert, exactly one pass
	runSteps(m, fr, 16);
	uint16_t inverted = m.gateWork;
	report("vates", "gate_inverted", inverted, inverted == (uint16_t)~asIs);

	m.params[Vates::GSW_PARAM].setValue(2.f);       // randomize
	runSteps(m, fr, 16);
	report("vates", "gate_randomized", m.gateWork, m.gateWork != inverted);

	// the CV sequence stays inside its rails whatever the switches do
	m.params[Vates::CSW_PARAM].setValue(2.f);
	Stats cv = runStats(m, fr, 1.0, Vates::CV_OUTPUT);
	report("vates", "cv_range", cv.peak, cv.peak <= 10.001f && cv.nans == 0);
}

// ── the LFO ───────────────────────────────────────────────────────────────────
static void testLfo() {
	Vates m;
	long fr = 0;
	m.params[Vates::SYNC_PARAM].setValue(0.f);      // free
	m.params[Vates::RATE_PARAM].setValue(0.55f);
	run(m, fr, 0.2);

	int cycles = 0;
	bool was = false;
	Stats tri;
	for (int i = 0; i < (int)(2.0 * SR); i++) {
		m.process(makeArgs(fr++));
		tri.add(m.outputs[Vates::TRI_OUTPUT].getVoltage());
		bool p = m.outputs[Vates::PULSE_OUTPUT].getVoltage() > 5.f;
		if (p && !was)
			cycles++;
		was = p;
	}
	report("vates", "lfo_free_runs", cycles, cycles > 0);
	report("vates", "lfo_tri_range", tri.peak, tri.peak <= 10.001f && tri.nans == 0);

	// synced: the LFO period follows the step clock
	m.params[Vates::SYNC_PARAM].setValue(1.f);
	m.params[Vates::RATE_PARAM].setValue(0.3f);     // one step per cycle
	m.params[Vates::TEMPO_PARAM].setValue(120.f);
	run(m, fr, 0.5);
	int syncCycles = 0;
	was = false;
	for (int i = 0; i < (int)(2.0 * SR); i++) {
		m.process(makeArgs(fr++));
		bool p = m.outputs[Vates::PULSE_OUTPUT].getVoltage() > 5.f;
		if (p && !was)
			syncCycles++;
		was = p;
	}
	report("vates", "lfo_synced_cycles", syncCycles, syncCycles >= 14 && syncCycles <= 18);
}

// ── the tone controls, at every extreme, into every corner ────────────────────
static void testToneAbuse() {
	Vates m;
	long fr = 0;
	if (!waitForBanks(m, fr)) {
		report("vates", "tone_setup", 0, false);
		return;
	}
	selectSample(m, 4, 2);
	m.params[Vates::LENGTH_PARAM].setValue(0.9f);
	m.params[Vates::PITCH_PARAM].setValue(2.f);
	long nans = 0;
	float peak = 0.f;
	static const float f[5] = {-1.f, -0.5f, 0.f, 0.5f, 1.f};
	for (int i = 0; i < 5; i++)
		for (int j = 0; j < 5; j++) {
			m.params[Vates::FILTER_PARAM].setValue(f[i]);
			m.params[Vates::FX_PARAM].setValue(f[j]);
			pressTrigger(m, fr);
			Stats l = runStats(m, fr, 0.25, Vates::LEFT_OUTPUT);
			Stats r = runStats(m, fr, 0.05, Vates::RIGHT_OUTPUT);
			nans += l.nans + r.nans;
			peak = std::max(peak, std::max(l.peak, r.peak));
		}
	report("vates", "tone_nans", nans, nans == 0);
	report("vates", "tone_peak_v", peak, peak <= 10.001f);
}

// ── nothing patched, nothing selected, everything at once ─────────────────────
static void testAbuse() {
	Vates m;
	long fr = 0;
	// before the banks exist, a trigger must be harmless
	pressTrigger(m, fr);
	Stats s = runStats(m, fr, 0.2, Vates::LEFT_OUTPUT);
	report("vates", "silent_before_banks", s.peak, s.peak < 1e-6f && s.nans == 0);

	if (!waitForBanks(m, fr)) {
		report("vates", "abuse_setup", 0, false);
		return;
	}
	// every input driven hard at once
	for (int i = 0; i < Vates::INPUTS_LEN; i++)
		m.inputs[i].channels = 1;
	m.params[Vates::SAMPLE_ATT_PARAM].setValue(1.f);
	m.params[Vates::BANK_ATT_PARAM].setValue(1.f);
	m.params[Vates::LENGTH_ATT_PARAM].setValue(-1.f);
	m.params[Vates::LFO_ATT_PARAM].setValue(1.f);
	m.params[Vates::PITCH_ATT_PARAM].setValue(1.f);
	Stats l, r;
	for (long i = 0; i < (long)(3.0 * SR); i++) {
		float ph = (float)i / SR;
		for (int in = 0; in < Vates::INPUTS_LEN; in++)
			m.inputs[in].setVoltage(5.f * std::sin(2.f * (float)M_PI * (3.1f + in) * ph));
		m.process(makeArgs(fr++));
		l.add(m.outputs[Vates::LEFT_OUTPUT].getVoltage());
		r.add(m.outputs[Vates::RIGHT_OUTPUT].getVoltage());
	}
	report("vates", "abuse_nans", l.nans + r.nans, l.nans + r.nans == 0);
	report("vates", "abuse_peak_v", std::max(l.peak, r.peak),
	       std::max(l.peak, r.peak) <= 10.001f);
}

SMOKE_MAIN(testBanks, testLength, testRetrigger, testPlayCue, testKnobBrowsing,
           testKnobRange, testDefaults, testClock, testPatternSwitches, testLfo,
           testToneAbuse, testAbuse)
