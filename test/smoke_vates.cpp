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
	m.bankBase = bank;
	m.params[Vates::SAMPLE_PARAM].setValue((sample + 0.5f) / vates_bank::kSamplesPerBank);
}

// One press of a bank button, edge and all.
static void pressBank(Vates& m, long& frame, bool up) {
	int id = up ? Vates::BANK_UP_PARAM : Vates::BANK_DOWN_PARAM;
	m.params[id].setValue(1.f);
	for (int i = 0; i < 4; i++)
		m.process(makeArgs(frame++));
	m.params[id].setValue(0.f);
	for (int i = 0; i < 4; i++)
		m.process(makeArgs(frame++));
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
	// The claim is about the envelope and the direction of travel, so that
	// is what is measured: the audio also carries the sample's own shape,
	// and a generated pad that fades into its own tail can fall while the
	// envelope rises. The audio checks here are only that it is there.
	selectSample(m, 4, 0);            // tones: 2.5 s of sustained material
	m.params[Vates::LENGTH_PARAM].setValue(0.55f);
	run(m, fr, 0.01);
	pressTrigger(m, fr);
	Stats fwdEnv1 = runStats(m, fr, 0.25, Vates::ENV_OUTPUT);
	double fwdPos1 = m.voicePos;
	Stats fwdEnv2 = runStats(m, fr, 0.25, Vates::ENV_OUTPUT);
	report("vates", "forward_env_decays", fwdEnv2.rms() / std::max(fwdEnv1.rms(), 1e-9),
	       fwdEnv2.rms() < fwdEnv1.rms());
	report("vates", "forward_plays_forward", m.voicePos - fwdPos1,
	       m.voicePos > fwdPos1);

	m.params[Vates::LENGTH_PARAM].setValue(-0.55f);
	run(m, fr, 0.5);
	pressTrigger(m, fr);
	Stats revEnv1 = runStats(m, fr, 0.1, Vates::ENV_OUTPUT);
	double revPos1 = m.voicePos;
	Stats revEnv2 = runStats(m, fr, 0.1, Vates::ENV_OUTPUT);
	Stats revAudio = runStats(m, fr, 0.2, Vates::LEFT_OUTPUT);
	report("vates", "reverse_env_swells", revEnv2.rms() / std::max(revEnv1.rms(), 1e-9),
	       revEnv2.rms() > revEnv1.rms());
	report("vates", "reverse_plays_backwards", revPos1 - m.voicePos,
	       m.voicePos < revPos1);
	report("vates", "reverse_audible", revAudio.rms(),
	       revAudio.rms() > 1e-3 && revAudio.nans == 0);

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

	// The panel puts "cue" above the switch and "play" below, so value 1 has
	// to read "cue" and value 0 has to be the mode that fires. Checking the
	// switch's own labels against its behaviour is what catches the two
	// being wired the opposite way round.
	// (read the labels straight off the quantity: ParamQuantity::setValue and
	// getDisplayValueString go through APP->engine, and the harness has no App)
	SwitchQuantity* modeQ = dynamic_cast<SwitchQuantity*>(m.getParamQuantity(Vates::MODE_PARAM));
	bool labelsOk = modeQ && modeQ->labels.size() == 2
	                && modeQ->labels[0] == "play" && modeQ->labels[1] == "cue";
	report("vates", "mode_labels", labelsOk ? 1 : 0, labelsOk);

	// a ramp across the whole bank, in play mode: every crossing is a hit
	m.params[Vates::MODE_PARAM].setValue(0.f);
	int hits = 0;
	float wasEnv = 0.f;
	for (int i = 0; i < (int)(0.8 * SR); i++) {
		m.inputs[Vates::SAMPLE_INPUT].setVoltage(10.f * i / (0.8f * SR));
		m.process(makeArgs(fr++));
		if (m.env > wasEnv + 0.5f)
			hits++;
		wasEnv = m.env;
	}
	report("vates", "play_mode_hits", hits, hits >= 4);

	// the same ramp in cue mode fires nothing
	m.params[Vates::MODE_PARAM].setValue(1.f);
	m.voiceActive = false;
	m.env = 0.f;
	int cueHits = 0;
	wasEnv = 0.f;
	for (int i = 0; i < (int)(0.8 * SR); i++) {
		m.inputs[Vates::SAMPLE_INPUT].setVoltage(10.f * i / (0.8f * SR));
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

	// the same for the bank buttons, which also shift the sample index
	hits = 0;
	wasEnv = m.env;
	for (int b = 0; b < 8; b++) {
		pressBank(m, fr, true);
		for (int i = 0; i < (int)(0.05 * SR); i++) {
			m.process(makeArgs(fr++));
			if (m.env > wasEnv + 0.5f)
				hits++;
			wasEnv = m.env;
		}
	}
	report("vates", "bank_button_silent", hits, hits == 0);
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
	m.bankBase = 0;
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

	// the bank buttons step and wrap, both ways
	m.bankBase = 0;
	run(m, fr, 0.02);
	pressBank(m, fr, false);
	report("vates", "bank_button_wraps_down", m.bankIndex,
	       m.bankIndex == m.bankCount() - 1);
	pressBank(m, fr, true);
	report("vates", "bank_button_wraps_up", m.bankIndex, m.bankIndex == 0);
	pressBank(m, fr, true);
	pressBank(m, fr, true);
	report("vates", "bank_button_steps", m.bankIndex, m.bankIndex == 2);
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

// ── the CV input spans a bank once, and its top is the last sample ────────────
// Ten volts at full attenuverter is one bank. At the very top the index used
// to have wrapped round to the first sample again — the knob's old fencepost,
// moved into the CV.
static void testCvRange() {
	Vates m;
	long fr = 0;
	if (!waitForBanks(m, fr)) {
		report("vates", "cvrange_setup", 0, false);
		return;
	}
	selectSample(m, 0, 0);
	m.params[Vates::SAMPLE_PARAM].setValue(0.f);
	m.params[Vates::SAMPLE_ATT_PARAM].setValue(1.f);
	m.params[Vates::MODE_PARAM].setValue(1.f);      // cue: measure, do not play
	m.inputs[Vates::SAMPLE_INPUT].channels = 1;

	int seen[8] = {0};
	int last = -1;
	bool monotonic = true;
	for (int i = 0; i <= 1000; i++) {
		m.inputs[Vates::SAMPLE_INPUT].setVoltage(10.f * i / 1000.f);
		run(m, fr, 0.001);
		int s = m.aimedSample;
		if (s >= 0 && s < 8)
			seen[s]++;
		if (last >= 0 && s < last)
			monotonic = false;      // it wrapped somewhere inside the sweep
		last = s;
	}
	int covered = 0;
	for (int i = 0; i < 8; i++)
		covered += seen[i] > 0 ? 1 : 0;
	report("vates", "cv_covers_bank", covered, covered == 8);
	report("vates", "cv_no_wrap_in_range", monotonic ? 1 : 0, monotonic);
	report("vates", "cv_top_is_last", last, last == 7);

	// past ten volts it *does* wrap: that is what makes a ramp a sequence
	m.inputs[Vates::SAMPLE_INPUT].setVoltage(11.5f);
	run(m, fr, 0.01);
	report("vates", "cv_wraps_past_range", m.aimedSample, m.aimedSample == 1);
}

// ── the rate knob means the same thing in both LFO modes ──────────────────────
static void testLfoDirection() {
	Vates m;
	long fr = 0;
	auto periodOf = [&](float knob, bool sync) {
		m.params[Vates::SYNC_PARAM].setValue(sync ? 1.f : 0.f);
		m.params[Vates::RATE_PARAM].setValue(knob);
		m.params[Vates::TEMPO_PARAM].setValue(120.f);
		run(m, fr, 0.3);
		int cycles = 0;
		bool was = false;
		for (int i = 0; i < (int)(4.0 * SR); i++) {
			m.process(makeArgs(fr++));
			bool p = m.outputs[Vates::PULSE_OUTPUT].getVoltage() > 5.f;
			if (p && !was)
				cycles++;
			was = p;
		}
		return cycles;
	};
	int freeSlow = periodOf(0.2f, false);
	int freeFast = periodOf(0.8f, false);
	int syncSlow = periodOf(0.2f, true);
	int syncFast = periodOf(0.8f, true);
	report("vates", "lfo_free_clockwise_faster", freeFast - freeSlow, freeFast > freeSlow);
	report("vates", "lfo_sync_clockwise_faster", syncFast - syncSlow, syncFast > syncSlow);
}

// ── user kits, and the paging that keeps play mode usable ─────────────────────
// Writes a throwaway kit of 20 files, points vates at it in memory (never at
// the user's settings file) and checks the pages: a 20-file kit is three
// banks of 8, 8 and 4, each page plays its own window of the kit, and the
// number of samples a bank offers never grows with the kit — which is what
// keeps crossings per LFO cycle from growing with it.
static void writeTestWav(const std::string& path, int frames, int channels, float amp) {
	FILE* f = fopen(path.c_str(), "wb");
	if (!f)
		return;
	auto w32 = [&](uint32_t v) { fputc(v & 255, f); fputc((v >> 8) & 255, f);
	                             fputc((v >> 16) & 255, f); fputc((v >> 24) & 255, f); };
	auto w16 = [&](uint16_t v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); };
	uint32_t bytes = (uint32_t)(frames * channels * 2);
	fwrite("RIFF", 1, 4, f); w32(36 + bytes); fwrite("WAVE", 1, 4, f);
	fwrite("fmt ", 1, 4, f); w32(16); w16(1); w16((uint16_t)channels); w32(44100);
	w32(44100 * channels * 2); w16((uint16_t)(channels * 2)); w16(16);
	fwrite("data", 1, 4, f); w32(bytes);
	for (int i = 0; i < frames; i++)
		for (int c = 0; c < channels; c++)
			w16((uint16_t)(int16_t)lrintf(amp * 32000.f
				* std::sin(2.f * (float)M_PI * 220.f * i / 44100.f)));
	fclose(f);
}

static void testUserKits() {
	const std::string root = "/tmp/vates_smoke_kits";
	const std::string kit = root + "/twenty";
	rack::system::createDirectories(kit);
	for (int i = 0; i < 20; i++)
		writeTestWav(rack::string::f("%s/%02d.wav", kit.c_str(), i),
		             4410, (i % 2) ? 2 : 1, 0.8f);
	forsitan_sampler::setKitsFolder(root, /*persist=*/false);

	Vates m;
	long fr = 0;
	if (!waitForBanks(m, fr)) {
		report("vates", "kits_setup", 0, false);
		return;
	}
	m.refreshKits();
	report("vates", "kit_pages", m.bankCount(),
	       m.bankCount() == vates_bank::kNumBanks + 3);

	// walk the three pages: 8, 8 and 4 samples, each one playing
	int counts[3] = {0, 0, 0};
	int played[3] = {0, 0, 0};
	for (int p = 0; p < 3; p++) {
		int bank = vates_bank::kNumBanks + p;
		m.bankBase = bank;
		run(m, fr, 0.05);
		for (int i = 0; i < 200 && m.samplesInBank(bank) == 0; i++) {
			run(m, fr, 0.01);
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		counts[p] = m.samplesInBank(bank);
		m.params[Vates::LENGTH_PARAM].setValue(0.7f);
		for (int s = 0; s < counts[p]; s++) {
			m.params[Vates::SAMPLE_PARAM].setValue((s + 0.5f) / std::max(counts[p], 1));
			run(m, fr, 0.01);
			pressTrigger(m, fr);
			Stats st = runStats(m, fr, 0.05, Vates::LEFT_OUTPUT);
			if (st.rms() > 1e-3 && st.nans == 0)
				played[p]++;
		}
	}
	report("vates", "kit_page_sizes", counts[0] * 100 + counts[1] * 10 + counts[2],
	       counts[0] == 8 && counts[1] == 8 && counts[2] == 4);
	report("vates", "kit_page_playback", played[0] + played[1] + played[2],
	       played[0] == 8 && played[1] == 8 && played[2] == 4);

	// a bank never offers more than the page size, whatever the kit holds
	int biggest = 0;
	for (int b = 0; b < m.bankCount(); b++)
		biggest = std::max(biggest, m.samplesInBank(b));
	report("vates", "bank_size_bounded", biggest, biggest <= 8);

	// asking for the whole kit in one bank is a deliberate choice, and works
	m.samplesPerBank = 0;
	m.refreshKits();
	m.bankBase = vates_bank::kNumBanks;
	run(m, fr, 0.05);
	report("vates", "whole_kit_one_bank", m.samplesInBank(vates_bank::kNumBanks),
	       m.samplesInBank(vates_bank::kNumBanks) == 20);

	forsitan_sampler::setKitsFolder("", /*persist=*/false);
	rack::system::removeRecursively(root);
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

// ── full clockwise on the pitch attenuverter is exactly 1V/oct ────────────────
// The hardware calibrates that endpoint — "this input tracks V/Oct standard
// when the PITCH MOD knob is fully clock-wise" — for both pitch inputs, so it
// is a number to hold, not a taste.
static void testPitchTracking() {
	Vates m;
	long fr = 0;
	if (!waitForBanks(m, fr)) {
		report("vates", "pitch_setup", 0, false);
		return;
	}
	selectSample(m, 4, 0);
	m.params[Vates::PITCH_PARAM].setValue(0.f);
	m.params[Vates::PITCH_ATT_PARAM].setValue(1.f);
	m.inputs[Vates::FREE_INPUT].channels = 1;

	m.inputs[Vates::FREE_INPUT].setVoltage(1.f);
	run(m, fr, 0.01);
	float oct = m.voiceRateNow();
	m.inputs[Vates::FREE_INPUT].setVoltage(2.f);
	run(m, fr, 0.01);
	float twoOct = m.voiceRateNow();
	report("vates", "free_1voct", oct, std::fabs(oct - 2.f) < 1e-4f);
	report("vates", "free_2voct", twoOct, std::fabs(twoOct - 4.f) < 1e-4f);

	// half attenuation is half the interval, in octaves
	m.params[Vates::PITCH_ATT_PARAM].setValue(0.5f);
	m.inputs[Vates::FREE_INPUT].setVoltage(1.f);
	run(m, fr, 0.01);
	report("vates", "free_half_att", m.voiceRateNow(),
	       std::fabs(m.voiceRateNow() - std::sqrt(2.f)) < 1e-4f);

	// the note input tracks too, quantized: a volt is a scale octave
	m.params[Vates::PITCH_ATT_PARAM].setValue(1.f);
	m.inputs[Vates::FREE_INPUT].setVoltage(0.f);
	m.inputs[Vates::NOTE_INPUT].channels = 1;
	m.inputs[Vates::NOTE_INPUT].setVoltage(1.f);
	run(m, fr, 0.01);
	pressTrigger(m, fr);
	report("vates", "note_1voct", m.voiceRateNow(),
	       std::fabs(m.voiceRateNow() - 2.f) < 1e-4f);
}

// ── the pattern inputs read the Rack window, and the hardware one on ask ─────
// A gate source resting at 0 V must mean "leave the pattern alone". Under the
// hardware's 0-5 V logic the same 0 V means invert, every step, which is what
// the menu option is for.
static void testPatternInputs() {
	Vates m;
	long fr = 0;
	m.params[Vates::TEMPO_PARAM].setValue(240.f);
	m.params[Vates::RHYTHM_PARAM].setValue(0.f);      // four on the floor
	m.params[Vates::GSW_PARAM].setValue(0.f);         // switch says invert...
	m.inputs[Vates::G_INPUT].channels = 1;            // ...but the jack decides
	m.inputs[Vates::G_INPUT].setVoltage(0.f);
	run(m, fr, 0.3);
	uint16_t start = m.gateWork;
	runSteps(m, fr, 16);
	report("vates", "pattern_in_0v_neutral", m.gateWork,
	       m.gateWork == start && start == 0x8888);

	m.inputs[Vates::G_INPUT].setVoltage(-5.f);        // invert
	runSteps(m, fr, 16);
	report("vates", "pattern_in_negative_inverts", m.gateWork,
	       m.gateWork == (uint16_t)~start);

	m.inputs[Vates::G_INPUT].setVoltage(5.f);         // randomize
	uint16_t before = m.gateWork;
	runSteps(m, fr, 16);
	report("vates", "pattern_in_positive_randomizes", m.gateWork,
	       m.gateWork != before);

	// the hardware window, for whoever wants it: 0 V inverts
	m.hardwareCvWindow = true;
	m.params[Vates::RHYTHM_PARAM].setValue(1.f);      // reload a clean pattern
	run(m, fr, 0.1);
	uint16_t clean = m.gateWork;
	m.inputs[Vates::G_INPUT].setVoltage(0.f);
	runSteps(m, fr, 16);
	report("vates", "pattern_in_hardware_window", m.gateWork,
	       m.gateWork == (uint16_t)~clean);
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
	m.params[Vates::RATE_PARAM].setValue(0.69f);    // one step per cycle
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
           testKnobRange, testCvRange, testDefaults, testUserKits, testClock,
           testPatternSwitches, testPatternInputs, testPitchTracking, testLfo, testLfoDirection, testToneAbuse,
           testAbuse)
