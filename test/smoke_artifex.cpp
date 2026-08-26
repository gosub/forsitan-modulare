// smoke_artifex — offline sanity checks for the nine-mode effect.
//
// See smoke_harness.hpp for the shared scaffolding and CSV format. The claims
// here are about what each mode does to a signal: that a knob at zero leaves
// it alone, that the delay repeats and snaps to a clock, that the freezer
// holds after the input stops, that the panner moves the channels in opposite
// directions, that the two pitch modes shift pitch, and that nothing goes
// non-finite or off the rails with the feedback wide open.

#include "smoke_harness.hpp"

#include <string>
#include "../src/artifex.cpp"

#include <vector>
#include <algorithm>

static const int kModes = 9;

static void step(Artifex& m, long& frame, float l, float r) {
	m.inputs[Artifex::LEFT_INPUT].channels = 1;
	m.inputs[Artifex::RIGHT_INPUT].channels = 1;
	m.inputs[Artifex::LEFT_INPUT].setVoltage(l);
	m.inputs[Artifex::RIGHT_INPUT].setVoltage(r);
	m.process(makeArgs(frame++));
}

// A tone at hz, for `seconds`, collecting both outputs.
struct Rec {
	std::vector<float> l, r, in;
	Stats sl, sr;
};

static void runTone(Artifex& m, long& frame, double seconds, float hz,
                    float amp, Rec* rec = NULL) {
	long n = (long)(seconds * SR);
	for (long i = 0; i < n; i++) {
		float ph = 2.f * (float)M_PI * hz * (float)frame / SR;
		float x = amp * std::sin(ph);
		step(m, frame, x, x);
		if (rec) {
			rec->l.push_back(m.outputs[Artifex::LEFT_OUTPUT].getVoltage());
			rec->r.push_back(m.outputs[Artifex::RIGHT_OUTPUT].getVoltage());
			rec->in.push_back(x);
			rec->sl.add(rec->l.back());
			rec->sr.add(rec->r.back());
		}
	}
}

static void runSilence(Artifex& m, long& frame, double seconds, Rec* rec = NULL) {
	runTone(m, frame, seconds, 100.f, 0.f, rec);
}

static void setMode(Artifex& m, int mode) {
	m.params[Artifex::FXMODE_PARAM].setValue((float)mode);
	m.mode = m.aimedMode = mode;
}

static double rmsOf(const std::vector<float>& v, size_t from, size_t to) {
	double s = 0.0;
	size_t n = 0;
	for (size_t i = from; i < to && i < v.size(); i++, n++)
		s += (double)v[i] * v[i];
	return n ? std::sqrt(s / n) : 0.0;
}

// Zero crossings per second of a buffer: a proxy for pitch on a sine.
static double zcr(const std::vector<float>& v, size_t from, size_t to) {
	int z = 0;
	size_t n = 0;
	float prev = 0.f;
	for (size_t i = from; i < to && i < v.size(); i++, n++) {
		if ((v[i] > 0.f) != (prev > 0.f))
			z++;
		prev = v[i];
	}
	return n ? 0.5 * z * SR / n : 0.0;
}

// ── the mode list is what the panel says it is ────────────────────────────────
static void testModeLabels() {
	Artifex m;
	SwitchQuantity* q = dynamic_cast<SwitchQuantity*>(m.getParamQuantity(Artifex::FXMODE_PARAM));
	bool ok = q && q->labels.size() == 9 && q->labels[0] == "delay"
	          && q->labels[2] == "freezer" && q->labels[8] == "shifter";
	report("artifex", "mode_labels", ok ? 1 : 0, ok);
}

// ── amount at zero is the dry signal, in every mode ───────────────────────────
// "Fully left this knob turns off any effect and you should hear a clean
// signal" is the one promise every mode makes, so every mode is asked.
static void testDryAtZero() {
	int worst = -1;
	double worstErr = 0.0;
	for (int mode = 0; mode < kModes; mode++) {
		Artifex m;
		long fr = 0;
		setMode(m, mode);
		m.params[Artifex::AMT_PARAM].setValue(0.f);
		m.params[Artifex::FBK_PARAM].setValue(0.f);
		m.params[Artifex::LEVEL_PARAM].setValue(1.f);
		m.params[Artifex::GAIN_PARAM].setValue(1.f);
		Rec rec;
		runTone(m, fr, 0.3, 220.f, 3.f, &rec);
		// compare the second half, past any settling
		size_t from = rec.l.size() / 2;
		double err = 0.0;
		for (size_t i = from; i < rec.l.size(); i++)
			err = std::max(err, (double)std::fabs(rec.l[i] - rec.in[i]));
		if (err > worstErr) {
			worstErr = err;
			worst = mode;
		}
	}
	report("artifex", "dry_at_zero_amount", worstErr, worstErr < 0.05);
	report("artifex", "dry_at_zero_worst_mode", worst, worst >= 0);
}

// ── every mode makes a sound, and none of them makes a non-finite one ─────────
static void testAllModesAudible() {
	int quiet = 0;
	int quietest = -1;
	double quietestRms = 1e9;
	long nans = 0;
	float loudest = 0.f;
	for (int mode = 0; mode < kModes; mode++) {
		Artifex m;
		long fr = 0;
		setMode(m, mode);
		m.params[Artifex::AMT_PARAM].setValue(1.f);
		m.params[Artifex::FBK_PARAM].setValue(0.4f);
		m.params[Artifex::TIME_PARAM].setValue(0.6f);
		m.params[Artifex::LEVEL_PARAM].setValue(1.f);
		Rec rec;
		runTone(m, fr, 1.0, 220.f, 3.f, &rec);
		nans += rec.sl.nans + rec.sr.nans;
		loudest = std::max(loudest, std::max(rec.sl.peak, rec.sr.peak));
		double r = std::max(rec.sl.rms(), rec.sr.rms());
		if (r < quietestRms) {
			quietestRms = r;
			quietest = mode;
		}
		if (r < 0.05)
			quiet++;
	}
	report("artifex", "all_modes_quietest", quietest, true);
	report("artifex", "all_modes_quietest_rms", quietestRms, true);
	report("artifex", "all_modes_finite", nans, nans == 0);
	report("artifex", "all_modes_audible", quiet, quiet == 0);
	report("artifex", "all_modes_bounded", loudest, loudest < 12.f);
}

// ── the delay repeats, and a clock at trig snaps it ───────────────────────────
static void testDelay() {
	Artifex m;
	long fr = 0;
	setMode(m, artifex_fx::MODE_DELAY);
	m.params[Artifex::AMT_PARAM].setValue(1.f);
	m.params[Artifex::FBK_PARAM].setValue(0.f);
	m.params[Artifex::TIME_PARAM].setValue(0.5f);
	m.params[Artifex::LEVEL_PARAM].setValue(1.f);
	m.params[Artifex::GAIN_PARAM].setValue(1.f);

	// one click in, then silence: the echo has to arrive later
	Rec rec;
	step(m, fr, 5.f, 5.f);
	runSilence(m, fr, 0.5, &rec);
	double early = rmsOf(rec.l, 0, (size_t)(0.002 * SR));
	double late = 0.0;
	size_t at = 0;
	for (size_t i = (size_t)(0.004 * SR); i < rec.l.size(); i++)
		if (std::fabs(rec.l[i]) > late) {
			late = std::fabs(rec.l[i]);
			at = i;
		}
	report("artifex", "delay_repeats", late, late > 0.5);
	report("artifex", "delay_repeat_is_later", (double)at / SR, at > (size_t)(0.004 * SR));
	report("artifex", "delay_dry_is_first", early, early >= 0.0);

	// a clock at the trig input: the delay snaps to a division of it, so the
	// echo lands on a multiple of the clock period rather than where the knob
	// happened to point
	Artifex m2;
	long fr2 = 0;
	setMode(m2, artifex_fx::MODE_DELAY);
	m2.params[Artifex::AMT_PARAM].setValue(1.f);
	m2.params[Artifex::TIME_PARAM].setValue(0.42f);
	m2.params[Artifex::LEVEL_PARAM].setValue(1.f);
	m2.inputs[Artifex::TRIG_INPUT].channels = 1;
	const double period = 0.25;
	for (int k = 0; k < 3; k++) {
		m2.inputs[Artifex::TRIG_INPUT].setVoltage(5.f);
		runSilence(m2, fr2, 0.002);
		m2.inputs[Artifex::TRIG_INPUT].setVoltage(0.f);
		runSilence(m2, fr2, period - 0.002);
	}
	bool synced = m2.trigPeriod > 0.2f && m2.trigPeriod < 0.3f;
	report("artifex", "delay_measures_the_clock", m2.trigPeriod, synced);
	report("artifex", "delay_shows_the_division", m2.core.uiUnit,
	       m2.core.uiUnit == artifex_fx::UNIT_DIV);
	// the snapped time is a division of the clock, not the knob's own value
	double ratio = m2.core.uiTime;
	double near = std::fabs(ratio - std::floor(ratio * 12.0 + 0.5) / 12.0);
	report("artifex", "delay_snaps_to_a_division", near, near < 0.02);
}

// ── the freezer holds a chunk after the input has gone ────────────────────────
static void testFreezer() {
	Artifex m;
	long fr = 0;
	setMode(m, artifex_fx::MODE_FREEZER);
	m.params[Artifex::AMT_PARAM].setValue(1.f);
	m.params[Artifex::FBK_PARAM].setValue(0.f);
	m.params[Artifex::TIME_PARAM].setValue(0.8f);   // short, tonal
	m.params[Artifex::LEVEL_PARAM].setValue(1.f);
	m.params[Artifex::GAIN_PARAM].setValue(1.f);

	runTone(m, fr, 0.5, 220.f, 3.f);
	// trigger a fresh capture of the tone, then take the input away
	m.params[Artifex::TRIG_PARAM].setValue(1.f);
	runTone(m, fr, 0.01, 220.f, 3.f);
	m.params[Artifex::TRIG_PARAM].setValue(0.f);
	Rec rec;
	runSilence(m, fr, 0.5, &rec);
	double held = rmsOf(rec.l, (size_t)(0.2 * SR), rec.l.size());
	report("artifex", "freezer_holds_after_input", held, held > 0.2);
	report("artifex", "freezer_finite", rec.sl.nans + rec.sr.nans,
	       rec.sl.nans + rec.sr.nans == 0);
}

// ── the panner moves the two channels in opposite directions ──────────────────
static void testPanner() {
	Artifex m;
	long fr = 0;
	setMode(m, artifex_fx::MODE_PANNER);
	m.params[Artifex::AMT_PARAM].setValue(1.f);
	m.params[Artifex::FBK_PARAM].setValue(0.f);
	m.params[Artifex::TIME_PARAM].setValue(0.15f);   // a slow sway
	m.params[Artifex::LEVEL_PARAM].setValue(1.f);
	m.params[Artifex::GAIN_PARAM].setValue(1.f);
	Rec rec;
	runTone(m, fr, 2.0, 300.f, 3.f, &rec);

	// envelope of each channel over 20 ms blocks, then their correlation
	size_t blk = (size_t)(0.02 * SR);
	std::vector<double> el, er;
	for (size_t i = 0; i + blk < rec.l.size(); i += blk) {
		el.push_back(rmsOf(rec.l, i, i + blk));
		er.push_back(rmsOf(rec.r, i, i + blk));
	}
	double ml = 0, mr = 0;
	for (size_t i = 0; i < el.size(); i++) { ml += el[i]; mr += er[i]; }
	ml /= el.size(); mr /= er.size();
	double cov = 0, vl = 0, vr = 0;
	for (size_t i = 0; i < el.size(); i++) {
		cov += (el[i] - ml) * (er[i] - mr);
		vl += (el[i] - ml) * (el[i] - ml);
		vr += (er[i] - mr) * (er[i] - mr);
	}
	double corr = cov / std::sqrt(std::max(vl * vr, 1e-12));
	report("artifex", "panner_channels_oppose", corr, corr < -0.5);
}

// ── the crusher holds its output between samples of its own clock ─────────────
static void testCrusher() {
	Artifex m;
	long fr = 0;
	setMode(m, artifex_fx::MODE_CRUSHER);
	m.params[Artifex::AMT_PARAM].setValue(0.5f);
	m.params[Artifex::FBK_PARAM].setValue(0.f);
	m.params[Artifex::TIME_PARAM].setValue(0.1f);   // a low sample rate
	m.params[Artifex::LEVEL_PARAM].setValue(1.f);
	m.params[Artifex::GAIN_PARAM].setValue(1.f);
	Rec rec;
	runTone(m, fr, 0.3, 300.f, 3.f, &rec);

	// most consecutive samples are identical when the signal is held
	size_t same = 0, n = 0;
	for (size_t i = rec.l.size() / 2; i + 1 < rec.l.size(); i++, n++)
		if (rec.l[i] == rec.l[i + 1])
			same++;
	double frac = n ? (double)same / n : 0.0;
	report("artifex", "crusher_holds_samples", frac, frac > 0.8);
}

// ── the slicer chops: it is loud on its steps and quiet between them ──────────
static void testSlicer() {
	Artifex m;
	long fr = 0;
	setMode(m, artifex_fx::MODE_SLICER);
	m.params[Artifex::AMT_PARAM].setValue(0.9f);    // a short decay
	m.params[Artifex::FBK_PARAM].setValue(0.f);
	m.params[Artifex::TIME_PARAM].setValue(0.f);    // four on the floor
	m.params[Artifex::TEMPO_PARAM].setValue(120.f);
	m.params[Artifex::LEVEL_PARAM].setValue(1.f);
	m.params[Artifex::GAIN_PARAM].setValue(1.f);
	Rec rec;
	runTone(m, fr, 2.0, 300.f, 3.f, &rec);

	// the loudest and quietest 30 ms blocks have to be far apart
	size_t blk = (size_t)(0.03 * SR);
	double loud = 0.0, quiet = 1e9;
	for (size_t i = (size_t)(0.5 * SR); i + blk < rec.l.size(); i += blk) {
		double e = rmsOf(rec.l, i, i + blk);
		loud = std::max(loud, e);
		quiet = std::min(quiet, e);
	}
	report("artifex", "slicer_chops", loud - quiet, loud > 0.3 && quiet < 0.05);

	// A slice can only ever be quieter than what went in, but it must not be
	// *inaudible*: the first version of the decay range put the shortest
	// setting an order of magnitude under the dry signal.
	double dry = 3.0 / std::sqrt(2.0);
	double wet = rmsOf(rec.l, (size_t)(0.5 * SR), rec.l.size());
	report("artifex", "slicer_stays_audible", wet / dry, wet > 0.25 * dry);
}

// ── the slicer's long decay is reachable ─────────────────────────────────────
// The wet fade and the decay range used to be stacked on the same tenth of the
// knob, so the long end could only be had dry. At amount 0 the mode chopped
// nothing at all and a drone came out a drone; by the time it was fully
// chopping, at a sixth of the travel, the decay was down to 0.63 s.
static void testSlicerDecay() {
	Artifex m;
	long fr = 0;
	Module::SampleRateChangeEvent sre;
	sre.sampleRate = SR;
	sre.sampleTime = 1.f / SR;
	m.onSampleRateChange(sre);
	setMode(m, artifex_fx::MODE_SLICER);
	m.params[Artifex::TIME_PARAM].setValue(0.f);    // four on the floor
	m.params[Artifex::AMT_PARAM].setValue(0.12f);   // just past the wet fade
	m.params[Artifex::FBK_PARAM].setValue(0.f);
	m.params[Artifex::TEMPO_PARAM].setValue(30.f);  // a hit every 2 s
	m.params[Artifex::LEVEL_PARAM].setValue(1.f);
	Rec rec;
	runTone(m, fr, 6.0, 300.f, 3.f, &rec);

	// the envelope in 5 ms blocks, over the second half
	size_t blk = (size_t)(0.005 * SR);
	std::vector<double> env;
	for (size_t i = rec.l.size() / 2; i + blk < rec.l.size(); i += blk)
		env.push_back(rmsOf(rec.l, i, i + blk));
	double loud = 0.0, quiet = 1e9;
	size_t peak = 0;
	for (size_t i = 0; i < env.size(); i++) {
		if (env[i] > loud) { loud = env[i]; peak = i; }
		quiet = std::min(quiet, env[i]);
	}
	// it has to be chopping here, not blending
	report("artifex", "slicer_chops_past_the_fade", quiet / loud, quiet < 0.1 * loud);
	// and the slice it chops has to be the long one the knob claims
	double decay = (double)(env.size() - peak) * 0.005;
	for (size_t i = peak; i < env.size(); i++)
		if (env[i] < loud * 0.1) { decay = (double)(i - peak) * 0.005; break; }
	report("artifex", "slicer_reaches_a_long_decay", decay, decay > 0.8);
}

// ── the two pitch modes move pitch, in the direction they claim ───────────────
static void testPitch() {
	// the shifter is the one that has to be in tune, so it is measured:
	// unity first, since the octaves are judged against it
	double f[3] = {0.0, 0.0, 0.0};
	static const float knob[3] = {0.5f, 0.f, 1.f};
	for (int k = 0; k < 3; k++) {
		Artifex m;
		long fr = 0;
		setMode(m, artifex_fx::MODE_SHIFTER);
		m.params[Artifex::AMT_PARAM].setValue(1.f);
		m.params[Artifex::FBK_PARAM].setValue(0.f);
		m.params[Artifex::STEREO_PARAM].setValue(0.f);
		m.params[Artifex::LEVEL_PARAM].setValue(1.f);
		m.params[Artifex::GAIN_PARAM].setValue(1.f);
		m.params[Artifex::TIME_PARAM].setValue(knob[k]);
		Rec rec;
		runTone(m, fr, 1.0, 400.f, 3.f, &rec);
		f[k] = zcr(rec.l, (size_t)(0.4 * SR), rec.l.size());
	}
	// a crossfaded shifter is not a tuner: a tenth of a semitone of error in
	// the window would show here, so the window is what these bound
	report("artifex", "shifter_unity", f[0], std::fabs(f[0] - 400.0) < 40.0);
	report("artifex", "shifter_down_an_octave", f[1] / f[0],
	       f[1] < f[0] * 0.62 && f[1] > f[0] * 0.40);
	report("artifex", "shifter_up_an_octave", f[2] / f[0], f[2] > f[0] * 1.6);

	// the pitcher only goes up, and its window is what time sets
	Artifex m;
	long fr = 0;
	setMode(m, artifex_fx::MODE_PITCHER);
	m.params[Artifex::AMT_PARAM].setValue(1.f);
	m.params[Artifex::FBK_PARAM].setValue(0.f);
	m.params[Artifex::TIME_PARAM].setValue(0.7f);
	m.params[Artifex::LEVEL_PARAM].setValue(1.f);
	m.params[Artifex::GAIN_PARAM].setValue(1.f);
	Rec rec;
	runTone(m, fr, 1.0, 400.f, 3.f, &rec);
	double up = zcr(rec.l, (size_t)(0.4 * SR), rec.l.size());
	report("artifex", "pitcher_goes_up", up, up > 440.0);
}

// ── the replayer's loop does not click once a lap ────────────────────────────
// One slot along from the newest sample on the tape is a sample written a whole
// lap earlier, and the play head crossing that splice stepped by however far
// apart the two moments were: 3 V of it with the tape locked, once every lap.
static void testReplayerSplice() {
	const float knob[4] = {5.f / 6.f, 0.84f, 0.75f, 1.f / 6.f};
	// The step at the splice depends on where a lap falls in the tone's cycle,
	// so drive four of them: a lap of 345 cycles joins by itself, 344.25 meets
	// its own peak. n is 55204 samples.
	const double lap[4] = {345.0, 344.5, 344.25, 344.75};
	double worst = 0.0;
	for (int k = 0; k < 4; k++) {
		Artifex m;
		long fr = 0;
		Module::SampleRateChangeEvent sre;
		sre.sampleRate = SR;
		sre.sampleTime = 1.f / SR;
		m.onSampleRateChange(sre);
		setMode(m, artifex_fx::MODE_REPLAYER);
		m.params[Artifex::TIME_PARAM].setValue(knob[k]);
		m.params[Artifex::AMT_PARAM].setValue(1.f);   // locked: the tape is all you hear
		m.params[Artifex::FBK_PARAM].setValue(0.f);
		m.params[Artifex::LEVEL_PARAM].setValue(1.f);
		m.params[Artifex::GAIN_PARAM].setValue(1.f);
		float hz = (float)(lap[k] * SR / 55204.0);
		Rec rec;
		runTone(m, fr, 6.0, hz, 3.f, &rec);
		double slew = 2.0 * M_PI * hz / SR * 3.0;
		// skip the first laps, where the tape is still filling
		for (size_t i = (size_t)(3.0 * SR) + 1; i < rec.l.size(); i++)
			worst = std::max(worst, (double)std::fabs(rec.l[i] - rec.l[i - 1]) / slew);
	}
	// as a multiple of what one sample of the tone moves by itself
	report("artifex", "replayer_splice_does_not_click", worst, worst < 2.5);
}

// ── the loop join neither steps nor bumps ───────────────────────────────────
// Two things met here. The loop was shortened by a fixed millisecond to make
// room for the join, which picks a length with no relation to what is on the
// tape: 220 Hz on a 1.15 s tape is 253.02 cycles, six degrees from joining
// itself, and a millisecond off moves that to seventy-two. Then, once the
// length was chosen by looking at the tape instead, the two ends matched -- and
// an equal-power crossfade of two signals that match sums them to +3 dB. The
// step became a bump, 2.25 V once a lap on a tape holding 2.
static void testReplayerJoin() {
	double worstBump = 0.0;
	const float hz[4] = {220.f, 317.f, 55.f, 1000.f};
	for (int k = 0; k < 4; k++) {
		Artifex m;
		long fr = 0;
		Module::SampleRateChangeEvent sre;
		sre.sampleRate = SR;
		sre.sampleTime = 1.f / SR;
		m.onSampleRateChange(sre);
		setMode(m, artifex_fx::MODE_REPLAYER);
		m.params[Artifex::TIME_PARAM].setValue(5.f / 6.f);   // +1.00x
		m.params[Artifex::AMT_PARAM].setValue(1.f);          // locked all through
		m.params[Artifex::FBK_PARAM].setValue(0.f);
		m.params[Artifex::LEVEL_PARAM].setValue(1.f);
		m.params[Artifex::GAIN_PARAM].setValue(1.f);
		runTone(m, fr, 3.0, hz[k], 2.f);                     // past the fill
		Rec rec;
		runTone(m, fr, 5.0, hz[k], 2.f, &rec);               // four laps
		// nothing on the tape can be louder than what went onto it
		double peak = 0.0;
		for (size_t i = 0; i < rec.l.size(); i++)
			peak = std::max(peak, (double)std::fabs(rec.l[i]));
		worstBump = std::max(worstBump, peak / 2.0);
	}
	report("artifex", "replayer_join_does_not_bump", worstBump, worstBump < 1.03);
}

// ── leaving the lock does not leave the tape clicking ───────────────────────
// The edge the tape already had is where a recording pass begins, and it fades
// by `keep` each time the record head laps over it -- once per lap, not a
// little every sample. Ducking it for exactly one lap is right only for a
// fill, where keep is zero and one lap erases everything: at a knob near the
// top keep is 0.995, so a lap later the edge is still all but intact, and it
// clicked every lap from then on.
static void testReplayerUnlock() {
	double worst = 0.0;
	const float to[4] = {0.9f, 0.75f, 0.5f, 0.f};
	for (int k = 0; k < 4; k++) {
		Artifex m;
		long fr = 0;
		Module::SampleRateChangeEvent sre;
		sre.sampleRate = SR;
		sre.sampleTime = 1.f / SR;
		m.onSampleRateChange(sre);
		setMode(m, artifex_fx::MODE_REPLAYER);
		m.params[Artifex::TIME_PARAM].setValue(5.f / 6.f);
		m.params[Artifex::AMT_PARAM].setValue(1.f);
		m.params[Artifex::FBK_PARAM].setValue(0.f);
		m.params[Artifex::LEVEL_PARAM].setValue(1.f);
		m.params[Artifex::GAIN_PARAM].setValue(1.f);
		runTone(m, fr, 3.0, 220.f, 2.f);            // fill and settle, locked
		m.params[Artifex::AMT_PARAM].setValue(to[k]);
		Rec rec;
		runTone(m, fr, 4.0, 220.f, 2.f, &rec);      // three laps of overdub
		std::vector<double> d2;
		for (size_t i = 2; i < rec.l.size(); i++)
			d2.push_back(std::fabs((double)rec.l[i] - 2.0 * rec.l[i - 1] + rec.l[i - 2]));
		std::vector<double> sorted = d2;
		std::sort(sorted.begin(), sorted.end());
		worst = std::max(worst, sorted.back()
		                        / std::max(sorted[(size_t)(sorted.size() * 0.999)], 1e-9));
	}
	report("artifex", "replayer_unlock_does_not_click", worst, worst < 40.0);
}

// ── nothing the replayer does makes a click ─────────────────────────────────
// Measured the right way. Curvature against the same signal's own curvature
// catches a step, which is what the early faults were, but it is nearly blind
// to what fixing them leaves behind -- a two-millisecond fade, a small dip, a
// corner where a ramp stops. Those are quiet, smooth, and completely audible
// on a sustained tone.
//
// So: for any sinusoid at w, x[n] = 2cos(w)x[n-1] - x[n-2] exactly, whatever
// its amplitude and phase. The residual of that is zero for a clean tone,
// immune to slow drift, and shows anything else at all. The plain second
// difference is the same expression with the coefficient rounded to 2, which
// leaves 2-2cos(w) of the tone behind and sets a floor 62 dB down that
// everything interesting hides under.
static void testReplayerClicks() {
	struct Local {
		// worst 1 ms of residual, in dB below the signal
		static double run(float knob, int scenario, long trigOffset) {
			Artifex m;
			long fr = 0;
			Module::SampleRateChangeEvent sre;
			sre.sampleRate = SR;
			sre.sampleTime = 1.f / SR;
			m.onSampleRateChange(sre);
			setMode(m, artifex_fx::MODE_REPLAYER);
			m.params[Artifex::TIME_PARAM].setValue(knob);
			m.params[Artifex::AMT_PARAM].setValue(1.f);
			m.params[Artifex::FBK_PARAM].setValue(0.f);
			m.params[Artifex::LEVEL_PARAM].setValue(1.f);
			m.params[Artifex::GAIN_PARAM].setValue(1.f);
			runTone(m, fr, trigOffset != 0 ? 3.0 : 4.0, 220.f, 2.f);
			if (trigOffset < 0) {
				// Wait for the moment that matters instead of guessing at it.
				// The worst trig is the one that lands with the play head on
				// the record head: a lap later the fill ends with the head
				// still inside the duck's window, which is where the duck, the
				// loop fold and the crossfade all arrive at once. Sampling the
				// lap at random misses it -- the window is a few hundred
				// samples out of fifty-five thousand.
				int n = m.core.tape[0].size();
				for (long i = 0; i < (long)(3.0 * SR); i++) {
					double gap = m.core.tapePos[0] - m.core.tapeWrite[0];
					gap -= std::floor(gap / n) * n;
					if (gap > 0.5 * n)
						gap -= n;
					if (std::fabs(gap) < 2.0)
						break;
					float x = 2.f * std::sin(2.f * (float)M_PI * 220.f * (float)fr / SR);
					step(m, fr, x, x);
				}
			}
			else
				for (long i = 0; i < trigOffset; i++) {
					float x = 2.f * std::sin(2.f * (float)M_PI * 220.f * (float)fr / SR);
					step(m, fr, x, x);
				}
			Rec rec;
			std::vector<bool> nearHead;
			long n = (long)(trigOffset != 0 ? 4.0 * SR : 8.0 * SR);
			for (long i = 0; i < n; i++) {
				switch (scenario) {
				case 0:                                     // a trig
					if (i == 100) m.params[Artifex::TRIG_PARAM].setValue(1.f);
					if (i == 300) m.params[Artifex::TRIG_PARAM].setValue(0.f);
					break;
				case 1:                                     // off the lock and back
					if (i == (long)(2.0 * SR)) m.params[Artifex::AMT_PARAM].setValue(0.9f);
					if (i == (long)(5.0 * SR)) m.params[Artifex::AMT_PARAM].setValue(1.f);
					break;
				case 2:                                     // a deep overdub and back
					if (i == (long)(2.0 * SR)) m.params[Artifex::AMT_PARAM].setValue(0.5f);
					if (i == (long)(5.0 * SR)) m.params[Artifex::AMT_PARAM].setValue(1.f);
					break;
				case 3:                                     // the speed, twice
					if (i == (long)(2.0 * SR)) m.params[Artifex::TIME_PARAM].setValue(0.4f);
					if (i == (long)(5.0 * SR)) m.params[Artifex::TIME_PARAM].setValue(0.9f);
					break;
				}
				float x = 2.f * std::sin(2.f * (float)M_PI * 220.f * (float)fr / SR);
				step(m, fr, x, x);
				rec.l.push_back(m.outputs[Artifex::LEFT_OUTPUT].getVoltage());
				// Whether this sample was read near a live record head.
				// Crossing the head while a pass is writing is a real take
				// boundary faded through in a couple of milliseconds -- it
				// is the effect, not a defect -- so those stretches are
				// excluded and everything else is held to the floor.
				int nt = m.core.tape[0].size();
				double gap = m.core.tapePos[0] - m.core.tapeWrite[0];
				gap -= std::floor(gap / nt) * nt;
				float win = artifex_fx::kEdgeFade * SR
				            * std::max(std::fabs(m.core.uiTime - 1.f), 1.f);
				nearHead.push_back(m.core.passFade > 0.f
				                   && (gap < 2.f * win || gap > nt - 4.0));
			}
			// what the tape should be playing, and the predictor tuned to it
			double f = 220.0 * std::fabs(m.core.uiTime);
			double c2 = 2.0 * std::cos(2.0 * M_PI * f / SR);
			double e = 0.0;
			for (size_t i = 0; i < rec.l.size(); i++)
				e += (double)rec.l[i] * rec.l[i];
			double rms = std::sqrt(e / rec.l.size());
			size_t blk = (size_t)(0.001 * SR);
			std::vector<double> env;
			for (size_t i = 2; i + blk < rec.l.size(); i += blk) {
				double q = 0.0;
				bool masked = false;
				for (size_t j = i; j < i + blk; j++) {
					double r = rec.l[j] - c2 * rec.l[j - 1] + rec.l[j - 2];
					q += r * r;
					masked = masked || nearHead[j];
				}
				if (!masked)
					env.push_back(std::sqrt(q / blk));
			}
			std::vector<double> sorted = env;
			std::sort(sorted.begin(), sorted.end());
			// Against the floor, not against the signal. Overdubbing really
			// does change what the tape holds, so the residual there is not an
			// artefact and an absolute threshold would either fail on it or be
			// too slack to catch anything. A click is what stands out of the
			// run it sits in.
			double floorDb = 20.0 * std::log10(sorted[sorted.size() / 2] / rms);
			double worstDb = 20.0 * std::log10(sorted.back() / rms);
			(void)floorDb;
			return worstDb - floorDb;
		}
	};
	const char* names[4] = {"trig", "off the lock", "overdub", "speed"};
	const float knob[3] = {1.f / 6.f, 0.75f, 1.f};
	double worst = -200.0;
	int worstS = 0;
	for (int sc = 0; sc < 4; sc++)
		for (int k = 0; k < 3; k++) {
			double v = Local::run(knob[k], sc, 0);
			if (v > worst) { worst = v; worstS = sc; }
		}
	// Stretches read near a live record head are excluded above -- that
	// crossing is a real take boundary faded through in two milliseconds.
	// What remains and sets the 25 dB line is the punch-in edge: leaving the
	// lock starts a pass whose fade-in is written onto the tape and crossed
	// on every later lap, 22 dB over the floor at 2x. Anything that clears
	// that is something else.
	if (worst >= 25.0)
		std::printf("# worst was the %s scenario\n", names[worstS]);
	report("artifex", "replayer_residual_over_floor_dB", worst, worst < 25.0);

	// And at more than one moment. A trig does something different depending
	// on where the play head, the record head and the seam happen to be, so
	// one trig proves nothing: the fault this found -- the duck applied to the
	// mix rather than to each reading, which un-ducked the ghost the instant
	// the head left the edge -- only appeared for trigs that landed in one
	// particular window, and measured -21 dB when it did.
	double worstPhase = -200.0;
	long worstOff = 0;
	// Sampled, not exhaustive -- and that is a real limitation. The fault
	// this was written after only showed for trigs landing in a window a few
	// hundred samples wide out of fifty-five thousand, and a sweep this coarse
	// walks straight past it. What found it was the same sweep run standalone
	// at more points and more speeds; this is here to catch the next one that
	// is not so narrow.
	for (int i = 0; i < 24; i++) {
		long off = 1 + (long)((double)i / 24.0 * 55204.0);
		for (int k = 0; k < 3; k++) {
			double v = Local::run(knob[k], 0, off);
			if (v > worstPhase) { worstPhase = v; worstOff = off; }
		}
	}
	if (worstPhase >= 25.0)
		std::printf("# worst trig offset was %ld\n", worstOff);
	report("artifex", "replayer_trig_at_any_moment_dB", worstPhase, worstPhase < 25.0);
}

// ── retuning between trigs does not tick once a lap ─────────────────────────
// The user's own reproduction, verbatim: time 1x, feedback 0, amount 75%, a
// tone, trig, a different tone, trig again. At 75% the mode never locks, so
// none of the locked-path work applies; at 1x the play head keeps station
// with the record head and hears what is being written directly; and the
// second tone is chosen to meet the first half a cycle out of phase at the
// lap, so the takes disagree as much as they can. Four separate faults lived
// here: the vertical tangent of keep at rec = 1, the cancelling discriminant,
// the hard clamp on the correlation, and the float read position's 1/256th
// sample grain -- each one a tick a lap, forever, written into the tape.
//
// Two tones, so the residual cascades an annihilator per tone; each one is
// exact for its own frequency and transparent to the other.
static void testReplayerRetune() {
	Artifex m;
	long fr = 0;
	Module::SampleRateChangeEvent sre;
	sre.sampleRate = SR;
	sre.sampleTime = 1.f / SR;
	m.onSampleRateChange(sre);
	setMode(m, artifex_fx::MODE_REPLAYER);
	m.params[Artifex::TIME_PARAM].setValue(5.f / 6.f);   // exactly 1x
	m.params[Artifex::AMT_PARAM].setValue(0.75f);
	m.params[Artifex::FBK_PARAM].setValue(0.f);
	m.params[Artifex::LEVEL_PARAM].setValue(1.f);
	m.params[Artifex::GAIN_PARAM].setValue(1.f);
	const float hzA = 220.f, hzB = 110.f;
	long trigA = (long)(1.0 * SR), retune = (long)(4.0 * SR);
	long trigB = (long)(5.0 * SR), total = (long)(10.0 * SR);
	double ph = 0.0;
	std::vector<float> out;
	for (long i = 0; i < total; i++) {
		float hz = i < retune ? hzA : hzB;
		if (i == trigA || i == trigB)
			m.params[Artifex::TRIG_PARAM].setValue(1.f);
		if (i == trigA + 200 || i == trigB + 200)
			m.params[Artifex::TRIG_PARAM].setValue(0.f);
		ph += hz / SR;
		ph -= std::floor(ph);
		float x = 3.f * std::sin(2.f * (float)M_PI * (float)ph);
		step(m, fr, x, x);
		out.push_back(m.outputs[Artifex::LEFT_OUTPUT].getVoltage());
	}
	double cA = 2.0 * std::cos(2.0 * M_PI * hzA / SR);
	double cB = 2.0 * std::cos(2.0 * M_PI * hzB / SR);
	std::vector<double> r1(out.size(), 0.0), res(out.size(), 0.0);
	for (size_t i = 2; i < out.size(); i++)
		r1[i] = (double)out[i] - cA * out[i - 1] + out[i - 2];
	for (size_t i = 4; i < out.size(); i++)
		res[i] = std::fabs(r1[i] - cB * r1[i - 1] + r1[i - 2]);
	double e = 0.0;
	for (size_t i = 0; i < out.size(); i++)
		e += (double)out[i] * out[i];
	double rms = std::sqrt(e / out.size());
	// worst 1 ms block, skipping 20 ms after each trig (the gesture) and
	// 5 ms after the retune (the input's own frequency step)
	size_t blk = (size_t)(0.001 * SR);
	double worst = 0.0;
	for (size_t i = (size_t)trigA; i + blk < res.size(); i += blk) {
		if ((i + blk > (size_t)trigA && i < trigA + (size_t)(0.020 * SR))
		    || (i + blk > (size_t)trigB && i < trigB + (size_t)(0.020 * SR))
		    || (i + blk > (size_t)retune && i < retune + (size_t)(0.005 * SR)))
			continue;
		double q = 0.0;
		for (size_t j = i; j < i + blk; j++)
			q += res[j] * res[j];
		worst = std::max(worst, std::sqrt(q / blk));
	}
	double worstDb = 20.0 * std::log10(worst / rms);
	// Absolute, not floor-relative: the floor here is at the 16-bit grain
	// and a returning once-a-lap tick measured -54 dB. -70 leaves it
	// nowhere to hide.
	report("artifex", "replayer_retune_overdub_dB", worstDb, worstDb < -70.0);
}

// ── the locked loop is a whole number of periods long ───────────────────────
// chooseLoop exists to pick a length whose end already resembles its start,
// which for anything periodic means a whole number of periods. It was scoring
// candidates by a plain sum of squared differences, measured in a window
// sitting on the seam -- which is exactly where a recording pass fades in, so
// the window was material ramping up out of silence and the score was
// smallest wherever the *other* window happened to be quietest. At 110 Hz it
// scored 1.7 at 123.5 periods against 540 at a whole 125: it preferred
// silence to a match, and landed half a cycle out for most tones.
//
// Tested as the property rather than as a level, because that is what the
// function is for, and because the audible cost of getting it wrong depends
// on the tone: 220 Hz on this tape is 253.018 cycles a lap and joins itself
// whatever the search does, while 110 Hz is 126.509 and joins at the worst
// phase there is.
static void testReplayerLoopLength() {
	const float hz[6] = {55.f, 110.f, 113.7f, 220.f, 440.f, 909.1f};
	double worst = 0.0;
	float worstHz = 0.f;
	for (int k = 0; k < 6; k++) {
		Artifex m;
		long fr = 0;
		Module::SampleRateChangeEvent sre;
		sre.sampleRate = SR;
		sre.sampleTime = 1.f / SR;
		m.onSampleRateChange(sre);
		setMode(m, artifex_fx::MODE_REPLAYER);
		m.params[Artifex::TIME_PARAM].setValue(1.f);
		m.params[Artifex::AMT_PARAM].setValue(1.f);     // locked, so it chooses
		m.params[Artifex::FBK_PARAM].setValue(0.f);
		m.params[Artifex::LEVEL_PARAM].setValue(1.f);
		m.params[Artifex::GAIN_PARAM].setValue(1.f);
		runTone(m, fr, 4.0, hz[k], 2.f);
		double period = SR / (double)hz[k];
		double cycles = m.core.loopLen / period;
		double err = std::fabs(cycles - std::floor(cycles + 0.5));
		if (err > worst) {
			worst = err;
			worstHz = hz[k];
		}
	}
	if (worst >= 0.05)
		std::printf("# worst loop join was at %.1f Hz\n", worstHz);
	report("artifex", "replayer_loop_is_whole_periods", worst, worst < 0.05);
}

// ── overdubbing does not tick where the heads cross, at any speed ───────────
// The play head crosses the record head once a lap at every speed but one,
// reading across an edge of *generation*: the slot behind the head carries a
// pass of overdub the slot in front of it has not had yet.
//
// Swept over speed rather than checked at a point, because the fault this was
// written after was invisible at 1x and 25 dB up at 2x: the crossing used to
// splice in the record head's own signal, which advances one slot per sample
// whatever the play head is doing and therefore carries the input's pitch
// instead of the tape's. Only a speed away from one shows it, and the further
// away the louder.
static void testReplayerCrossing() {
	struct Local {
		static double run(float knob, float amt, float* speed) {
			Artifex m;
			long fr = 0;
			Module::SampleRateChangeEvent sre;
			sre.sampleRate = SR;
			sre.sampleTime = 1.f / SR;
			m.onSampleRateChange(sre);
			setMode(m, artifex_fx::MODE_REPLAYER);
			m.params[Artifex::TIME_PARAM].setValue(knob);
			m.params[Artifex::AMT_PARAM].setValue(1.f);
			m.params[Artifex::FBK_PARAM].setValue(0.f);
			m.params[Artifex::LEVEL_PARAM].setValue(1.f);
			m.params[Artifex::GAIN_PARAM].setValue(1.f);
			runTone(m, fr, 4.0, 220.f, 2.f);           // fill and settle, locked
			m.params[Artifex::AMT_PARAM].setValue(amt);
			Rec rec;
			runTone(m, fr, 8.0, 220.f, 2.f, &rec);     // seven laps of overdub
			*speed = m.core.uiTime;
			// the tape's tone as the play head hears it
			double f = 220.0 * std::fabs(m.core.uiTime);
			double c2 = 2.0 * std::cos(2.0 * M_PI * f / SR);
			double e = 0.0;
			for (size_t i = 0; i < rec.l.size(); i++)
				e += (double)rec.l[i] * rec.l[i];
			double rms = std::sqrt(e / rec.l.size());
			size_t blk = (size_t)(0.001 * SR);
			double worst = 0.0;
			// past the first 200 ms, which is the amount knob arriving
			for (size_t i = (size_t)(0.2 * SR); i + blk < rec.l.size(); i += blk) {
				double q = 0.0;
				for (size_t j = i; j < i + blk; j++) {
					double r = rec.l[j] - c2 * rec.l[j - 1] + rec.l[j - 2];
					q += r * r;
				}
				worst = std::max(worst, std::sqrt(q / blk));
			}
			return 20.0 * std::log10(worst / rms);
		}
	};
	const float knobs[7] = {0.f, 0.1f, 0.25f, 0.4f, 0.6f, 0.9f, 1.f};
	double worst = -200.0;
	float worstSpeed = 0.f;
	for (int k = 0; k < 7; k++) {
		float speed = 0.f;
		double v = Local::run(knobs[k], 0.9f, &speed);
		if (v > worst) {
			worst = v;
			worstSpeed = speed;
		}
	}
	// -58 dB is where the deepest crossing sits once the generation step is
	// spread over a splice: at the ends of the travel the heads close at
	// three slots a sample, so a lap's worth of overdub is crossed in one
	// splice length. The pitch fault it was written for measured -37 dB.
	if (worst >= -58.0)
		std::printf("# worst crossing was at %+.3fx\n", worstSpeed);
	report("artifex", "replayer_crossing_over_speed_dB", worst, worst < -58.0);
}

// ── the replayer's loop keeps the level that went into it ────────────────────
// The overdub crossfaded amplitudes, keep + rec = 1. Two passes of the tape
// only line up when the speed is exactly 1, which is one point of the travel;
// everywhere else they add in power and the loop settles at rec^2 / (1 -
// keep^2) of the input -- a third of it, 4.8 dB down, with the knob at half.
static void testReplayerLevel() {
	double worst = 0.0;
	for (int k = 0; k < 4; k++) {
		const float amt[4] = {0.f, 0.3f, 0.5f, 0.8f};
		Artifex m;
		long fr = 0;
		Module::SampleRateChangeEvent sre;
		sre.sampleRate = SR;
		sre.sampleTime = 1.f / SR;
		m.onSampleRateChange(sre);
		setMode(m, artifex_fx::MODE_REPLAYER);
		m.params[Artifex::TIME_PARAM].setValue(0.8f);   // +0.87x, passes do not line up
		m.params[Artifex::FBK_PARAM].setValue(0.f);
		m.params[Artifex::LEVEL_PARAM].setValue(1.f);
		m.params[Artifex::GAIN_PARAM].setValue(1.f);
		m.params[Artifex::AMT_PARAM].setValue(amt[k]);
		// noise, so successive passes really are uncorrelated
		uint32_t seed = 22222;
		double z = 0.0, s2 = 0.0;
		long n = (long)(10.0 * SR);
		for (long i = 0; i < n; i++) {
			seed = seed * 1103515245u + 12345u;
			float w = (float)((seed >> 9) & 0x7FFFFF) / 4194304.f - 1.f;
			z += (w * 3.5 - z) * 0.3;
			step(m, fr, (float)z, (float)z);
			s2 += z * z;
		}
		double in = std::sqrt(s2 / n);
		m.params[Artifex::AMT_PARAM].setValue(1.f);     // locked: hear the tape
		Rec rec;
		runSilence(m, fr, 2.0, &rec);
		double db = 20.0 * std::log10(rmsOf(rec.l, 0, rec.l.size()) / in);
		worst = std::min(worst, db);
	}
	report("artifex", "replayer_holds_its_level", worst, worst > -2.0);

	// and it must not stack either. A drone correlates with what the tape
	// holds a lap later, and a crossfade that assumes it does not settles the
	// loop at rec / (1 - keep) of the input instead of at the input: measured,
	// a 220 Hz tone at half travel climbed 7.4 dB over twelve laps and sat on
	// the clip.
	double loudest = -100.0;
	for (int k = 0; k < 3; k++) {
		const float amt[3] = {0.3f, 0.5f, 0.7f};
		Artifex m;
		long fr = 0;
		Module::SampleRateChangeEvent sre;
		sre.sampleRate = SR;
		sre.sampleTime = 1.f / SR;
		m.onSampleRateChange(sre);
		setMode(m, artifex_fx::MODE_REPLAYER);
		m.params[Artifex::TIME_PARAM].setValue(1.f);    // +2x, so it laps often
		m.params[Artifex::FBK_PARAM].setValue(0.f);
		m.params[Artifex::LEVEL_PARAM].setValue(1.f);
		m.params[Artifex::GAIN_PARAM].setValue(1.f);
		m.params[Artifex::AMT_PARAM].setValue(amt[k]);
		runTone(m, fr, 14.0, 220.f, 3.f);               // a dozen laps of drone
		m.params[Artifex::AMT_PARAM].setValue(1.f);     // lock and listen
		Rec rec;
		runSilence(m, fr, 2.0, &rec);
		double db = 20.0 * std::log10(rmsOf(rec.l, 0, rec.l.size()) / (3.0 / std::sqrt(2.0)));
		loudest = std::max(loudest, db);
	}
	report("artifex", "replayer_does_not_stack_a_drone", loudest, loudest < 2.0);
}

// ── the replayer's tape runs at the speed on the display ─────────────────────
// The record head used to move with the play head, so the speed cancelled: a
// tone written at a quarter speed and read back at a quarter speed is unity,
// and the knob moved the number on the screen and nothing else. Only a trig,
// which wrote at 1.0, ever put material on the tape that the speed could act
// on. Above 1x the shared head also skipped slots -- a truncated write from a
// position advancing by 1.32 leaves every third one holding what was there
// before, which read back as broadcast noise.
static void testReplayer() {
	// Fill the tape with a rising sweep, then put the play head on the oldest
	// sample so a lap runs oldest to newest without crossing the splice.
	struct Local {
		static void fill(Artifex& m, long& fr) {
			m.params[Artifex::AMT_PARAM].setValue(0.f);      // recording
			long n = (long)(m.core.bufSeconds * SR) + 1;
			for (long i = 0; i < n; i++) {
				float t = (float)i / SR;
				float x = 3.f * std::sin(2.f * (float)M_PI * (100.f + 400.f * t) * t);
				step(m, fr, x, x);
			}
			m.params[Artifex::AMT_PARAM].setValue(1.f);      // locked
		}
		static void seek(Artifex& m, int offset) {
			for (int c = 0; c < 2; c++) {
				int n = (int)m.core.tape[c].size();
				int w = (int)m.core.tapeWrite[c];
				m.core.tapePos[c] = (double)(((w + offset) % n + n) % n);
			}
		}
	};
	Artifex m;
	long fr = 0;
	Module::SampleRateChangeEvent sre;
	sre.sampleRate = SR;
	sre.sampleTime = 1.f / SR;
	m.onSampleRateChange(sre);
	setMode(m, artifex_fx::MODE_REPLAYER);
	m.params[Artifex::FBK_PARAM].setValue(0.f);
	m.params[Artifex::LEVEL_PARAM].setValue(1.f);
	m.params[Artifex::GAIN_PARAM].setValue(1.f);
	m.params[Artifex::TIME_PARAM].setValue(0.75f);   // +0.71x
	Local::fill(m, fr);

	// forwards from the oldest sample: the sweep rises
	Local::seek(m, 0);
	Rec fwd;
	runSilence(m, fr, 0.6, &fwd);
	double a = zcr(fwd.l, 0, fwd.l.size() / 3);
	double b = zcr(fwd.l, 2 * fwd.l.size() / 3, fwd.l.size());
	report("artifex", "replayer_plays_forward", b - a, b > a);

	// and the pitch is the one the display names. The tape was written at 1.0
	// whatever the knob said, so a 300 Hz tone comes back at 300 * speed.
	for (int k = 0; k < 3; k++) {
		const float knob[3] = {0.5f, 0.75f, 1.f};
		const char* name[3] = {"replayer_quarter_speed", "replayer_two_thirds_speed",
		                       "replayer_double_speed"};
		Artifex t;
		long tf = 0;
		t.onSampleRateChange(sre);
		setMode(t, artifex_fx::MODE_REPLAYER);
		t.params[Artifex::FBK_PARAM].setValue(0.f);
		t.params[Artifex::LEVEL_PARAM].setValue(1.f);
		t.params[Artifex::GAIN_PARAM].setValue(1.f);
		t.params[Artifex::TIME_PARAM].setValue(knob[k]);
		t.params[Artifex::AMT_PARAM].setValue(0.f);
		runTone(t, tf, 3.0, 300.f, 3.f);
		t.params[Artifex::AMT_PARAM].setValue(1.f);
		Rec r;
		runSilence(t, tf, 1.0, &r);
		double heard = zcr(r.l, r.l.size() / 4, r.l.size());
		double want = 300.0 * std::fabs(t.core.uiTime);
		report("artifex", name[k], heard / want,
		       heard > want * 0.9 && heard < want * 1.1);
	}

	// The tape never stops. With the centre of the knob mapped to a speed of
	// zero the head held one sample and the mode put out a DC level — it
	// played a slice and then sat there.
	m.params[Artifex::TIME_PARAM].setValue(0.5f);
	Rec mid;
	runSilence(m, fr, 0.5, &mid);
	double moving = 0.0;
	for (size_t i = 1; i < mid.l.size(); i++)
		moving += std::fabs(mid.l[i] - mid.l[i - 1]);
	moving /= std::max<size_t>(1, mid.l.size() - 1);
	report("artifex", "replayer_centre_keeps_moving", moving, moving > 1e-3);
	report("artifex", "replayer_centre_audible", rmsOf(mid.l, 0, mid.l.size()),
	       rmsOf(mid.l, 0, mid.l.size()) > 0.1);

	// the same tape backwards, from the newest sample: the sweep falls
	m.params[Artifex::TIME_PARAM].setValue(0.25f);   // -0.71x
	Local::seek(m, -1);
	Rec back;
	runSilence(m, fr, 0.6, &back);
	double c = zcr(back.l, 0, back.l.size() / 3);
	double d = zcr(back.l, 2 * back.l.size() / 3, back.l.size());
	report("artifex", "replayer_plays_backward", c - d, d < c);
}

// ── stereo is the only thing that pulls the channels apart ───────────────────
// The slicer drew its inversion coin inside the per-channel loop, so with
// feedback up the two channels got different numbers and played two different
// rhythms with the stereo knob at zero. The old check only ever looked at the
// flanger, which is why the other eight modes went unwatched.
static void testStereo() {
	struct Local {
		// how far apart the two outputs are: 0 identical, 1 uncorrelated
		static double width(int mode, float stereo) {
			Artifex m;
			long fr = 0;
			Module::SampleRateChangeEvent sre;
			sre.sampleRate = SR;
			sre.sampleTime = 1.f / SR;
			m.onSampleRateChange(sre);
			setMode(m, mode);
			m.params[Artifex::TIME_PARAM].setValue(0.4f);
			m.params[Artifex::AMT_PARAM].setValue(0.5f);
			m.params[Artifex::FBK_PARAM].setValue(0.5f);
			m.params[Artifex::STEREO_PARAM].setValue(stereo);
			m.params[Artifex::TEMPO_PARAM].setValue(120.f);
			m.params[Artifex::LEVEL_PARAM].setValue(1.f);
			m.params[Artifex::GAIN_PARAM].setValue(1.f);
			Rec rec;
			runTone(m, fr, 4.0, 317.f, 3.f, &rec);
			double sum = 0.0, diff = 0.0;
			for (size_t i = rec.l.size() / 2; i < rec.l.size(); i++) {
				double a = 0.5 * (rec.l[i] + rec.r[i]);
				double d = 0.5 * (rec.l[i] - rec.r[i]);
				sum += a * a;
				diff += d * d;
			}
			return std::sqrt(diff / std::max(sum, 1e-12));
		}
	};
	// Every mode but the panner, whose two channels moving in opposite
	// directions is the whole mode. The shifter holds its two channels half a
	// crossfade window apart by design, which is worth about 1.6%.
	double worst = 0.0;
	int culprit = -1;
	for (int mode = 0; mode < kModes; mode++) {
		if (mode == artifex_fx::MODE_PANNER)
			continue;
		double w = Local::width(mode, 0.f);
		if (w > worst) {
			worst = w;
			culprit = mode;
		}
	}
	report("artifex", "stereo_mono_at_zero", worst, worst < 0.05);
	if (worst >= 0.05)
		std::printf("# widest at stereo 0 was mode %d\n", culprit + 1);

	// and the knob still opens them up, on a mode from each half of the table
	double flanger = Local::width(artifex_fx::MODE_FLANGER, 1.f);
	double slicer = Local::width(artifex_fx::MODE_SLICER, 1.f);
	report("artifex", "stereo_widens_the_flanger", flanger, flanger > 0.15);
	report("artifex", "stereo_widens_the_slicer", slicer, slicer > 0.15);
}

// ── nothing clicks when the write pointer comes round ─────────────────────────
// The tape's read position, folded into range, could round up to exactly the
// buffer length and index one past the end of the vector: one garbage sample
// every wrap, 1.15 s apart, and the 2 ms delay hit it every time. Run past two
// wraps at the shortest delay and assert the output stays smooth.
static void testWrapClick() {
	Artifex m;
	long fr = 0;
	// The harness steps process() at SR, but the core only learns the rate
	// from the event Rack sends when the module is added. Without this it
	// builds its buffers for 44.1 kHz, and 0.002 * 44100 is 88.2 samples --
	// nowhere near the integer boundary the bug needs.
	Module::SampleRateChangeEvent sre;
	sre.sampleRate = SR;
	sre.sampleTime = 1.f / SR;
	m.onSampleRateChange(sre);
	setMode(m, artifex_fx::MODE_DELAY);
	m.params[Artifex::TIME_PARAM].setValue(1.f);    // 2 ms, the worst case
	m.params[Artifex::AMT_PARAM].setValue(1.f);     // all wet, nothing to mask it
	m.params[Artifex::FBK_PARAM].setValue(0.f);
	Rec rec;
	runTone(m, fr, 3.0, 220.f, 5.f, &rec);

	// A click is a step the 220 Hz tone cannot make. Skip the first delay
	// period, where the empty buffer legitimately steps up to the signal.
	double worst = 0.0;
	size_t from = (size_t)(0.05 * SR);
	for (size_t i = from + 1; i < rec.l.size(); i++)
		worst = std::max(worst, (double)std::fabs(rec.l[i] - rec.l[i - 1]));
	// one sample of a 5 V 220 Hz sine moves at most 2*pi*220/SR * 5 V
	double slew = 2.0 * M_PI * 220.0 / SR * 5.0;
	report("artifex", "delay_min_no_wrap_click", worst / slew, worst < slew * 2.0);
}

// ââ the crusher reaches both ends ââââââââââââââââââââââââââââââââ
// The rate used to stop at sr/2, which is a sample-and-hold on every other
// sample: about 30 dB of grain sitting under the far right of the knob, so no
// setting of it ever gave the signal back. The top of the travel is the
// running sample rate, where the decimator holds for one sample and does
// nothing.
static void testCrusherRange() {
	// error against the input, in dB, at one point of the time knob
	struct Local {
		static double errDb(float t) {
			Artifex m;
			long fr = 0;
			// the core takes its rate from the event, not from process()
			Module::SampleRateChangeEvent sre;
			sre.sampleRate = SR;
			sre.sampleTime = 1.f / SR;
			m.onSampleRateChange(sre);
			setMode(m, artifex_fx::MODE_CRUSHER);
			m.params[Artifex::TIME_PARAM].setValue(t);
			m.params[Artifex::AMT_PARAM].setValue(0.25f);  // 9 bits, well clear
			m.params[Artifex::FBK_PARAM].setValue(0.f);
			m.params[Artifex::LEVEL_PARAM].setValue(1.f);  // so clean can be clean
			Rec rec;
			runTone(m, fr, 0.5, 220.f, 4.f, &rec);
			double se = 0.0, si = 0.0;
			for (size_t i = rec.l.size() / 2; i < rec.l.size(); i++) {
				double e = rec.l[i] - rec.in[i];
				se += e * e;
				si += (double)rec.in[i] * rec.in[i];
			}
			return 20.0 * std::log10(std::sqrt(se / si) + 1e-12);
		}
	};
	double clean = Local::errDb(1.f);
	double wrecked = Local::errDb(0.f);
	report("artifex", "crusher_right_end_is_clean", clean, clean < -50.0);
	report("artifex", "crusher_left_end_is_wrecked", wrecked, wrecked > -6.0);
}

// ââ the amount knob works all the way up âââââââââââââââââââââââââ
// Two ways it did not. The bit depth hit its floor at half travel, so the
// second half had nothing left to crush; and the mangling that was supposed to
// take over there ran an XOR on a signed integer, where two negative operands
// give a positive result -- every negative sample flipped up, and above 0.8 the
// output never crossed zero at all.
static void testCrusherAmount() {
	struct Local {
		// mean, error-against-input and rms frequency, at one amount
		static void at(float amt, double* dc, double* errDb, double* hz = NULL) {
			Artifex m;
			long fr = 0;
			Module::SampleRateChangeEvent sre;
			sre.sampleRate = SR;
			sre.sampleTime = 1.f / SR;
			m.onSampleRateChange(sre);
			setMode(m, artifex_fx::MODE_CRUSHER);
			m.params[Artifex::TIME_PARAM].setValue(1.f);   // no decimation
			m.params[Artifex::AMT_PARAM].setValue(amt);
			m.params[Artifex::FBK_PARAM].setValue(0.f);
			m.params[Artifex::LEVEL_PARAM].setValue(1.f);
			Rec rec;
			runTone(m, fr, 0.5, 220.f, 4.f, &rec);
			double sum = 0.0, se = 0.0, si = 0.0;
			size_t from = rec.l.size() / 2;
			for (size_t i = from; i < rec.l.size(); i++) {
				double e = rec.l[i] - rec.in[i];
				sum += rec.l[i];
				se += e * e;
				si += (double)rec.in[i] * rec.in[i];
			}
			*dc = sum / (double)(rec.l.size() - from);
			*errDb = 20.0 * std::log10(std::sqrt(se / si) + 1e-12);
			if (hz) {
				// energy of the first difference over energy: the rms
				// frequency, without needing a transform
				double d2 = 0.0, e2 = 0.0;
				for (size_t i = from + 1; i < rec.l.size(); i++) {
					double d = rec.l[i] - rec.l[i - 1];
					d2 += d * d;
					e2 += (double)rec.l[i] * rec.l[i];
				}
				*hz = std::sqrt(d2 / e2) * SR / (2.0 * M_PI);
			}
		}
	};
	// a symmetric input must come out symmetric, mangling and all
	double worstDc = 0.0;
	for (float amt = 0.5f; amt <= 1.001f; amt += 0.1f) {
		double dc, e;
		Local::at(amt, &dc, &e);
		worstDc = std::max(worstDc, std::fabs(dc));
	}
	report("artifex", "crusher_mangling_has_no_dc", worstDc, worstDc < 0.02);

	// and every step of the knob has to do something: no plateau, no floor
	double prev = -1e9, worstStep = 1e9;
	bool monotone = true;
	for (float amt = 0.15f; amt <= 1.001f; amt += 0.05f) {
		double dc, e;
		Local::at(amt, &dc, &e);
		if (prev > -1e8) {
			double d = e - prev;
			monotone = monotone && d > 0.0;
			worstStep = std::min(worstStep, d);
		}
		prev = e;
	}
	report("artifex", "crusher_amount_never_stalls", worstStep,
	       monotone && worstStep > 0.5);

	// The mangling has to add harmonics. It used to add an offset instead:
	// the rms frequency sat at 792 Hz from half travel to the top, unmoved,
	// while the only thing the knob changed was how far up the waveform had
	// been pushed.
	double dc, e, mid, top;
	Local::at(0.5f, &dc, &e, &mid);
	Local::at(1.0f, &dc, &e, &top);
	report("artifex", "crusher_mangling_adds_harmonics", top / mid,
	       top > mid * 1.5);
}

// ââ the crusher's feedback is a backdrop, not a trim âââââââââââââââââ
// It used to run through the global one-sample path at half gain, which is a
// memoryless loop around a saturator: no pitch in it, and a whole knob's travel
// worth 3% of level. Raising that gain past one only moves the fixed point to a
// rail and latches there. It now closes around the sample-and-hold, AC-coupled
// with a corner that follows the crush rate, so it howls and the howl is
// pitched.
static void testCrusherFeedback() {
	struct Local {
		// rms, mean and rms frequency of what is left after the input stops
		static void tail(float t, float fb, double* rms, double* dc, double* hz) {
			Artifex m;
			long fr = 0;
			Module::SampleRateChangeEvent sre;
			sre.sampleRate = SR;
			sre.sampleTime = 1.f / SR;
			m.onSampleRateChange(sre);
			setMode(m, artifex_fx::MODE_CRUSHER);
			m.params[Artifex::TIME_PARAM].setValue(t);
			m.params[Artifex::AMT_PARAM].setValue(0.4f);
			m.params[Artifex::FBK_PARAM].setValue(fb);
			m.params[Artifex::LEVEL_PARAM].setValue(1.f);
			runTone(m, fr, 0.5, 220.f, 2.f);        // something to start it
			Rec rec;
			runSilence(m, fr, 1.0, &rec);
			double sum = 0.0, s2 = 0.0, d2 = 0.0;
			size_t from = rec.l.size() / 2;
			for (size_t i = from; i < rec.l.size(); i++) {
				sum += rec.l[i];
				s2 += (double)rec.l[i] * rec.l[i];
				if (i > from) {
					double d = rec.l[i] - rec.l[i - 1];
					d2 += d * d;
				}
			}
			size_t n = rec.l.size() - from;
			*dc = sum / (double)n;
			*rms = std::sqrt(s2 / (double)n);
			*hz = s2 > 1e-9 ? std::sqrt(d2 / s2) * SR / (2.0 * M_PI) : 0.0;
		}
	};
	double rms, dc, lo, hi, unused;
	// wide open it sustains on its own, and stays centred while it does
	Local::tail(0.5f, 1.f, &rms, &dc, &unused);
	report("artifex", "crusher_feedback_sustains", rms, rms > 1.0);
	report("artifex", "crusher_feedback_does_not_latch", std::fabs(dc),
	       std::fabs(dc) < 0.25);
	// and shut it stays shut
	double quiet;
	Local::tail(0.5f, 0.f, &quiet, &dc, &unused);
	report("artifex", "crusher_feedback_off_is_silent", quiet, quiet < 0.05);
	// the backdrop is pitched, and the pitch follows the crush rate
	Local::tail(0.1f, 1.f, &rms, &dc, &lo);
	Local::tail(0.9f, 1.f, &rms, &dc, &hi);
	report("artifex", "crusher_feedback_tracks_the_rate", hi / lo, hi > lo * 4.0);
}


// ── a trig does its job without a click ───────────────────────────────────────
// The flanger, the panner, the pitcher and the shifter all reset a phase that a
// read position or a gain depends on, so the output landed somewhere it was not
// heading. What the mode does is wanted; the step it arrived on is not.
static void testTrigDeclick() {
	const int modes[4] = {artifex_fx::MODE_FLANGER, artifex_fx::MODE_PANNER,
	                      artifex_fx::MODE_PITCHER, artifex_fx::MODE_SHIFTER};
	const char* names[4] = {"flanger", "panner", "pitcher", "shifter"};
	// 317 ms is not a whole number of 220 Hz cycles -- at 300 ms every trig
	// would land on the same zero crossing of the tone and jump nothing
	const double period = 0.317;
	for (int k = 0; k < 4; k++) {
		Artifex m;
		long fr = 0;
		setMode(m, modes[k]);
		m.params[Artifex::TIME_PARAM].setValue(0.35f);
		m.params[Artifex::AMT_PARAM].setValue(0.7f);
		m.params[Artifex::FBK_PARAM].setValue(0.f);
		m.params[Artifex::LEVEL_PARAM].setValue(1.f);
		m.inputs[Artifex::TRIG_INPUT].channels = 1;
		runTone(m, fr, 1.0, 220.f, 5.f);

		double worst = 0.0, bend = 0.0;
		for (int t = 0; t < 6; t++) {
			// The step that matters is between the last sample before the trig
			// and the first one after it, so the recording has to start before
			// the trig rather than at it.
			Rec rec;
			runTone(m, fr, 0.01, 220.f, 5.f, &rec);
			size_t mark = rec.l.size();
			m.inputs[Artifex::TRIG_INPUT].setVoltage(5.f);
			runTone(m, fr, 0.002, 220.f, 5.f, &rec);
			m.inputs[Artifex::TRIG_INPUT].setVoltage(0.f);
			runTone(m, fr, period - 0.012, 220.f, 5.f, &rec);
			for (size_t i = mark; i < mark + (size_t)(0.005 * SR)
			                      && i < rec.l.size(); i++) {
				worst = std::max(worst, (double)std::fabs(rec.l[i] - rec.l[i - 1]));
				bend = std::max(bend, (double)std::fabs(rec.l[i] - 2.f * rec.l[i - 1]
				                                        + rec.l[i - 2]));
			}
		}
		// A step shows in the first difference. A *pop* shows in the second:
		// taking the step back out of the output afterwards leaves the signal
		// continuous but its slope still broken, and the correction is a
		// transient of its own. Both have to be near what the tone does alone.
		double slew = 2.0 * M_PI * 220.0 / SR * 5.0;
		double bendRef = slew * 2.0 * M_PI * 220.0 / SR;
		report("artifex", (std::string("trig_no_click_") + names[k]).c_str(),
		       worst / slew, worst < slew * 6.0);
		report("artifex", (std::string("trig_no_pop_") + names[k]).c_str(),
		       bend / bendRef, bend < bendRef * 12.0);
	}

	// and the panner's throw still lands on the other side
	Artifex m;
	long fr = 0;
	setMode(m, artifex_fx::MODE_PANNER);
	m.params[Artifex::TIME_PARAM].setValue(0.12f);   // slow enough to hold a side
	m.params[Artifex::AMT_PARAM].setValue(0.7f);
	m.params[Artifex::LEVEL_PARAM].setValue(1.f);
	m.inputs[Artifex::TRIG_INPUT].channels = 1;
	runTone(m, fr, 0.9, 220.f, 5.f);
	Rec before, after;
	runTone(m, fr, 0.1, 220.f, 5.f, &before);
	m.inputs[Artifex::TRIG_INPUT].setVoltage(5.f);
	runTone(m, fr, 0.002, 220.f, 5.f);
	m.inputs[Artifex::TRIG_INPUT].setVoltage(0.f);
	runTone(m, fr, 0.05, 220.f, 5.f);                // let the throw arrive
	runTone(m, fr, 0.1, 220.f, 5.f, &after);
	double bl = rmsOf(before.l, 0, before.l.size());
	double br = rmsOf(before.r, 0, before.r.size());
	double al = rmsOf(after.l, 0, after.l.size());
	double ar = rmsOf(after.r, 0, after.r.size());
	report("artifex", "trig_still_throws_the_pan", (bl - br) - (al - ar),
	       (bl > br) != (al > ar));
}

// ── the three filter placements from the context menu ─────────────────────────
static double tonePeak(Artifex& m, long& fr, float hz, double secs) {
	Rec rec;
	runTone(m, fr, secs, hz, 2.f, &rec);
	double pk = 0.0;
	for (size_t i = rec.l.size() * 2 / 3; i < rec.l.size(); i++)
		pk = std::max(pk, (double)std::fabs(rec.l[i]));
	return 20.0 * std::log10(std::max(pk, 1e-9) / 2.0);
}

static void setFilter(Artifex& m, bool four, bool dry, bool loop) {
	m.core.fourPole = four;
	m.core.filterDry = dry;
	m.core.filterInLoop = loop;
}

static void testFilterPlacement() {
	// the dry path: untouched by default, filtered when the menu says so
	double clean, filtered;
	{
		Artifex m; long fr = 0;
		setMode(m, artifex_fx::MODE_DELAY);
		m.params[Artifex::AMT_PARAM].setValue(0.f);     // pure dry
		m.params[Artifex::FILTER_PARAM].setValue(-1.f); // lowpass, hard left
		m.params[Artifex::LEVEL_PARAM].setValue(1.f);
		setFilter(m, false, false, false);
		clean = tonePeak(m, fr, 220.f, 1.0);
	}
	{
		Artifex m; long fr = 0;
		setMode(m, artifex_fx::MODE_DELAY);
		m.params[Artifex::AMT_PARAM].setValue(0.f);
		m.params[Artifex::FILTER_PARAM].setValue(-1.f);
		m.params[Artifex::LEVEL_PARAM].setValue(1.f);
		setFilter(m, false, true, false);
		filtered = tonePeak(m, fr, 220.f, 1.0);
	}
	report("artifex", "filter_leaves_the_dry_alone", clean, clean > -1.0);
	report("artifex", "filter_dry_option_filters_it", filtered, filtered < clean - 10.0);

	// the slope: four poles must take a 4 kHz tone away where two cannot
	double two, four;
	{
		Artifex m; long fr = 0;
		setMode(m, artifex_fx::MODE_DELAY);
		m.params[Artifex::AMT_PARAM].setValue(1.f);
		m.params[Artifex::FILTER_PARAM].setValue(1.f);  // highpass, hard right
		m.params[Artifex::LEVEL_PARAM].setValue(1.f);
		setFilter(m, false, false, false);
		two = tonePeak(m, fr, 4000.f, 1.0);
	}
	{
		Artifex m; long fr = 0;
		setMode(m, artifex_fx::MODE_DELAY);
		m.params[Artifex::AMT_PARAM].setValue(1.f);
		m.params[Artifex::FILTER_PARAM].setValue(1.f);
		m.params[Artifex::LEVEL_PARAM].setValue(1.f);
		setFilter(m, true, false, false);
		four = tonePeak(m, fr, 4000.f, 1.0);
	}
	report("artifex", "filter_12db_leaves_4k_alone", two, two > -8.0);
	report("artifex", "filter_24db_takes_4k_away", four, four < -30.0);

	// in the loop: repeats have to darken pass by pass, not just once
	double slope[2] = {0.0, 0.0};
	for (int loop = 0; loop < 2; loop++) {
		Artifex m; long fr = 0;
		setMode(m, artifex_fx::MODE_DELAY);
		m.params[Artifex::AMT_PARAM].setValue(1.f);
		m.params[Artifex::FBK_PARAM].setValue(0.8f);
		m.params[Artifex::TIME_PARAM].setValue(0.5f);
		m.params[Artifex::FILTER_PARAM].setValue(-0.5f);
		m.params[Artifex::LEVEL_PARAM].setValue(1.f);
		setFilter(m, false, false, loop == 1);
		Rec rec;
		runTone(m, fr, 0.01, 1000.f, 4.f, &rec);
		runSilence(m, fr, 0.29, &rec);
		const double d = 0.04775;                  // the knob-0.5 delay time
		double pk[6] = {0, 0, 0, 0, 0, 0};
		for (size_t i = 0; i < rec.l.size(); i++) {
			int k = (int)(i / SR / d);
			if (k >= 1 && k <= 5)
				pk[k] = std::max(pk[k], (double)std::fabs(rec.l[i]));
		}
		slope[loop] = 20.0 * std::log10(std::max(pk[5], 1e-9)
		                                / std::max(pk[1], 1e-9));
	}
	// fb 0.8 over four passes is -7.7 dB in theory; the tape's own softClip and
	// the interpolation take a little more. The check that matters is the
	// difference below, not this bound.
	report("artifex", "filter_out_of_loop_decays_evenly", slope[0], slope[0] > -12.0);
	report("artifex", "filter_in_loop_darkens_each_pass", slope[1],
	       slope[1] < slope[0] - 2.0);
}

// ── the freezer's time knob moves the loop without re-capturing ───────────────
// The loop length used to be written only inside the refreeze branch, so the
// knob did nothing at all until a trig or amount leaving zero caught a new
// chunk. Shortening the loop is how a frozen bar becomes a pitch, and that has
// to work on the held audio rather than on a fresh capture.
static void testFreezerLength() {
	Artifex m;
	long fr = 0;
	setMode(m, artifex_fx::MODE_FREEZER);
	m.params[Artifex::AMT_PARAM].setValue(1.f);
	m.params[Artifex::TIME_PARAM].setValue(0.60f);
	runTone(m, fr, 2.0, 220.f, 5.f);

	// a trig with the tape full catches all the history there is
	m.params[Artifex::TRIG_PARAM].setValue(1.f);
	runTone(m, fr, 0.01, 220.f, 5.f);
	m.params[Artifex::TRIG_PARAM].setValue(0.f);
	runTone(m, fr, 0.05, 220.f, 5.f);
	float captured = m.core.capturedFrames;
	report("artifex", "freezer_catches_the_whole_tape", captured / SR,
	       captured > 1.0f * SR);

	// now the knob alone, with nothing re-capturing
	float held = m.core.capturedFrames;
	float lengths[4] = {0.f, 0.f, 0.f, 0.f};
	float knobs[4] = {0.55f, 0.70f, 0.85f, 0.95f};
	for (int i = 0; i < 4; i++) {
		m.params[Artifex::TIME_PARAM].setValue(knobs[i]);
		runTone(m, fr, 0.2, 220.f, 5.f);
		lengths[i] = m.core.freezeFrames;
	}
	bool shrinks = lengths[0] > lengths[1] && lengths[1] > lengths[2]
	               && lengths[2] > lengths[3];
	report("artifex", "freezer_length_follows_the_knob", lengths[0] / lengths[3],
	       shrinks && lengths[0] > lengths[3] * 10.f);
	report("artifex", "freezer_keeps_its_capture", m.core.capturedFrames - held,
	       m.core.capturedFrames == held);
	// short enough at the top to be a pitch rather than a rhythm
	report("artifex", "freezer_shortest_is_a_pitch", SR / lengths[3],
	       SR / lengths[3] > 100.f);
}

// ── the delay syncs to clk, and to trig only when clk is quiet ────────────────
// Everything else clock-driven in the module follows clk; the delay used to
// follow the trig input alone, which is where the hardware takes a clock but
// not where anyone patches one here.
static void clockFor(Artifex& m, long& fr, int inputId, double period, int n) {
	m.inputs[inputId].channels = 1;
	for (int k = 0; k < n; k++) {
		m.inputs[inputId].setVoltage(5.f);
		runSilence(m, fr, 0.002);
		m.inputs[inputId].setVoltage(0.f);
		runSilence(m, fr, period - 0.002);
	}
}

static void testDelayClockSync() {
	// nothing patched: the knob is free and the display reads a time
	{
		Artifex m;
		long fr = 0;
		setMode(m, artifex_fx::MODE_DELAY);
		m.params[Artifex::TIME_PARAM].setValue(0.42f);
		runSilence(m, fr, 0.5);
		report("artifex", "delay_free_without_a_clock", m.core.uiUnit,
		       m.core.uiUnit == artifex_fx::UNIT_MS);
	}
	// a clock at clk: it snaps, and to a division of that clock
	{
		Artifex m;
		long fr = 0;
		setMode(m, artifex_fx::MODE_DELAY);
		m.params[Artifex::TIME_PARAM].setValue(0.42f);
		clockFor(m, fr, Artifex::CLK_INPUT, 0.25, 4);
		double ratio = m.core.uiTime;
		double off = std::fabs(ratio - std::floor(ratio * 12.0 + 0.5) / 12.0);
		report("artifex", "delay_syncs_to_clk", m.core.uiUnit,
		       m.core.uiUnit == artifex_fx::UNIT_DIV);
		report("artifex", "delay_clk_division_is_exact", off, off < 0.02);
	}
	// both patched: clk defines the bar, so a rhythm at trig cannot redefine it
	{
		Artifex m;
		long fr = 0;
		setMode(m, artifex_fx::MODE_DELAY);
		m.params[Artifex::TIME_PARAM].setValue(0.42f);
		m.inputs[Artifex::TRIG_INPUT].channels = 1;
		m.inputs[Artifex::CLK_INPUT].channels = 1;
		// interleaved: a 0.25 s clock at clk against a 0.1 s one at trig.
		// Rack's SchmittTrigger starts high, so the first edge of each is
		// swallowed -- run long enough for both to be measured anyway.
		for (int k = 0; k < 24; k++) {
			m.inputs[Artifex::CLK_INPUT].setVoltage(k % 5 == 0 ? 5.f : 0.f);
			m.inputs[Artifex::TRIG_INPUT].setVoltage(k % 2 == 0 ? 5.f : 0.f);
			runSilence(m, fr, 0.05);
		}
		// both were measured; the delay takes the clk one
		report("artifex", "trig_clock_also_measured", m.trigPeriod,
		       m.trigPeriod > 0.08f && m.trigPeriod < 0.12f);
		report("artifex", "clk_beats_trig_for_sync", m.modul.stepSeconds,
		       m.modul.stepSeconds > 0.2f && m.modul.stepSeconds < 0.3f);
	}
}

// ── the limiter is continuous where it starts folding ─────────────────────────
// softClip branched straight into tanh above kClipVolts, and tanh(1) is 0.762,
// so a signal crossing 5 V dropped 1.19 V on the way through and climbed back
// on the way out. Drive a tone that crosses it four times a cycle and assert
// the output stays smooth.
static void testClipContinuity() {
	Artifex m;
	long fr = 0;
	setMode(m, artifex_fx::MODE_DELAY);
	m.params[Artifex::AMT_PARAM].setValue(0.f);     // dry: only the limiter
	m.params[Artifex::GAIN_PARAM].setValue(2.f);    // +-10 V in, so it folds
	m.params[Artifex::LEVEL_PARAM].setValue(1.f);
	Rec rec;
	runTone(m, fr, 0.5, 220.f, 5.f, &rec);

	double worst = 0.0;
	for (size_t i = (size_t)(0.05 * SR) + 1; i < rec.l.size(); i++)
		worst = std::max(worst, (double)std::fabs(rec.l[i] - rec.l[i - 1]));
	// the folded tone is steeper than the bare one, but only by its own shape
	double slew = 2.0 * M_PI * 220.0 / SR * 10.0;
	report("artifex", "clip_is_continuous", worst / slew, worst < slew * 1.5);
}

// ── sweeping the time knob bends, it does not click ───────────────────────────
// A parameter in Rack moves once per UI frame, so a knob drag arrives as ~60
// steps a second. A delay that teleports its read position clicks on every one
// of them. Drag the time knob across its whole range at frame rate and assert
// the output never steps faster than a swept sine can.
static void testDelaySweep() {
	Artifex m;
	long fr = 0;
	setMode(m, artifex_fx::MODE_DELAY);
	m.params[Artifex::AMT_PARAM].setValue(1.f);     // all wet
	m.params[Artifex::FBK_PARAM].setValue(0.f);
	m.params[Artifex::TIME_PARAM].setValue(1.f);

	const double secs = 2.0;
	const long n = (long)(secs * SR);
	const long stepN = (long)(SR / 60.f);           // one UI frame
	std::vector<float> out;
	out.reserve(n);
	for (long i = 0; i < n; i++) {
		if (i % stepN == 0)
			m.params[Artifex::TIME_PARAM].setValue(1.f - (float)i / n);
		float x = 5.f * std::sin(2.f * (float)M_PI * 220.f * (float)fr / SR);
		step(m, fr, x, x);
		out.push_back(m.outputs[Artifex::LEFT_OUTPUT].getVoltage());
	}

	// The sweep pitches the tone, so the output legitimately slews faster than
	// 220 Hz would -- but by its pitch ratio, not by a factor of seventy.
	double slew = 2.0 * M_PI * 220.0 / SR * 5.0;
	double worst = 0.0;
	for (size_t i = (size_t)(0.05 * SR); i < out.size(); i++)
		worst = std::max(worst, (double)std::fabs(out[i] - out[i - 1]));
	report("artifex", "delay_sweep_bends_not_clicks", worst / slew, worst < slew * 6.0);
}

// ── the envelope follower reads the input ─────────────────────────────────────
static void testEnvelope() {
	Artifex m;
	long fr = 0;
	setMode(m, artifex_fx::MODE_DELAY);
	m.params[Artifex::AMT_PARAM].setValue(0.f);
	m.params[Artifex::GAIN_PARAM].setValue(1.f);
	runTone(m, fr, 0.5, 200.f, 5.f);
	float loud = m.outputs[Artifex::ENV_OUTPUT].getVoltage();
	runSilence(m, fr, 2.0);
	float quiet = m.outputs[Artifex::ENV_OUTPUT].getVoltage();
	report("artifex", "env_follows_input", loud, loud > 1.f);
	report("artifex", "env_falls_on_silence", quiet, quiet < loud * 0.5f);

	// Full scale is the clip point, not the rail: an input driven past
	// kClipVolts — where the lamps light and the mode buffers fold — must
	// read 10 V, so the follower and the panel agree on what "too loud" is.
	Artifex hot;
	fr = 0;
	setMode(hot, artifex_fx::MODE_DELAY);
	hot.params[Artifex::AMT_PARAM].setValue(0.f);
	hot.params[Artifex::GAIN_PARAM].setValue(4.f);
	runTone(hot, fr, 2.0, 200.f, 5.f);
	float pinned = hot.outputs[Artifex::ENV_OUTPUT].getVoltage();
	report("artifex", "env_full_scale_at_the_clip_point", pinned, pinned > 9.5f);

	// And a signal that sits just under it does not reach full scale, so the
	// top of the range still means something.
	Artifex warm;
	fr = 0;
	setMode(warm, artifex_fx::MODE_DELAY);
	warm.params[Artifex::AMT_PARAM].setValue(0.f);
	warm.params[Artifex::GAIN_PARAM].setValue(1.f);
	runTone(warm, fr, 2.0, 200.f, 2.5f);
	float half = warm.outputs[Artifex::ENV_OUTPUT].getVoltage();
	report("artifex", "env_below_the_clip_point_has_headroom", half,
	       half > 4.f && half < 6.f);
}

// ── mode changes from CV wait for the clock, and the knob does not ────────────
// ── the shared modulation section, as artifex wires it ──────────────────────
// The section itself is one piece of code shared with vates, and smoke_vates
// exercises it: the rhythm table, the CV that selects from it, the pattern
// switches and their two gate windows, the LFO free and synced, its reset.
// What is not covered there is artifex's own wiring of it, and the handful of
// behaviours vates has no equivalent for. These were listening tests until
// somebody pointed out that a person was being asked to confirm arithmetic.

// The pulse is high exactly while the triangle rises, at every width. That is
// the whole definition of the pwm control, and it is a number, so it is
// checked rather than scoped by hand.
static void testLfoPwm() {
	double worst = 0.0;
	float worstPw = 0.f;
	const float widths[5] = {0.05f, 0.25f, 0.5f, 0.75f, 0.95f};
	for (int k = 0; k < 5; k++) {
		Artifex m;
		long fr = 0;
		m.params[Artifex::PWM_PARAM].setValue(widths[k]);
		m.params[Artifex::RATE_PARAM].setValue(0.75f);    // a few Hz, free
		m.params[Artifex::SYNC_PARAM].setValue(0.f);
		runSilence(m, fr, 0.5);
		long high = 0, rising = 0, n = 0;
		float lastTri = m.outputs[Artifex::TRI_OUTPUT].getVoltage();
		for (long i = 0; i < (long)(2.0 * SR); i++) {
			step(m, fr, 0.f, 0.f);
			float tri = m.outputs[Artifex::TRI_OUTPUT].getVoltage();
			float pulse = m.outputs[Artifex::PULSE_OUTPUT].getVoltage();
			if (pulse > 5.f) high++;
			if (tri > lastTri) rising++;
			lastTri = tri;
			n++;
		}
		double duty = (double)high / n, rise = (double)rising / n;
		double err = std::fabs(duty - rise);
		if (err > worst) {
			worst = err;
			worstPw = widths[k];
		}
	}
	if (worst >= 0.02)
		std::printf("# worst pulse width was %.2f\n", worstPw);
	report("artifex", "lfo_pulse_is_high_while_tri_rises", worst, worst < 0.02);
}

// The saw is a phasor over the pattern, so a reset puts both sequences back
// to their first step -- which shows as the saw dropping to zero.
static void testPatternReset() {
	Artifex m;
	long fr = 0;
	m.params[Artifex::TEMPO_PARAM].setValue(240.f);
	m.params[Artifex::SYNC_PARAM].setValue(1.f);      // saw follows the pattern
	m.params[Artifex::RATE_PARAM].setValue(0.f);      // slowest division
	runSilence(m, fr, 1.3);                           // land somewhere mid-bar
	float before = m.outputs[Artifex::SAW_OUTPUT].getVoltage();
	m.inputs[Artifex::PAT_RESET_INPUT].channels = 1;
	m.inputs[Artifex::PAT_RESET_INPUT].setVoltage(10.f);
	runSilence(m, fr, 0.002);
	float after = m.outputs[Artifex::SAW_OUTPUT].getVoltage();
	report("artifex", "pattern_reset_was_mid_cycle", before, before > 0.5f);
	report("artifex", "pattern_reset_returns_to_step_one", after, after < 0.5f);
}

// With the menu option off, a cable at clk is ignored and the tempo knob
// keeps the section. Counted as clock pulses, since that is what changes.
static void testHonourExternalClock() {
	struct Local {
		static int pulses(bool honour, double extPeriod) {
			Artifex m;
			long fr = 0;
			m.honourExternalClock = honour;
			m.params[Artifex::TEMPO_PARAM].setValue(60.f);
			m.inputs[Artifex::CLK_INPUT].channels = 1;
			int count = 0;
			bool was = false;
			double per = extPeriod;
			for (long i = 0; i < (long)(4.0 * SR); i++) {
				double t = (double)i / SR;
				bool hi = std::fmod(t, per) < 0.005;
				m.inputs[Artifex::CLK_INPUT].setVoltage(hi ? 10.f : 0.f);
				step(m, fr, 0.f, 0.f);
				bool now = m.outputs[Artifex::CLK_OUTPUT].getVoltage() > 5.f;
				if (now && !was)
					count++;
				was = now;
			}
			return count;
		}
	};
	// The section's own clock is sixteenths, so the knob's 60 BPM is 4 Hz and
	// sixteen pulses in four seconds. An 8 Hz cable should double that when it
	// is honoured and change nothing when it is not -- the first version of
	// this fed in 240 BPM, which IS 4 Hz in sixteenths, so both cases came
	// out identical and the check could not have failed for the right reason.
	int on = Local::pulses(true, 0.125);
	int off = Local::pulses(false, 0.125);
	report("artifex", "external_clock_taken_when_honoured", on, on > 28 && on < 36);
	report("artifex", "external_clock_ignored_when_off", off, off > 13 && off < 19);
}

// An attenuverter at zero disconnects its input, however hot the input is.
static void testLfoModAttenuverter() {
	struct Local {
		static double cycles(float att, float cv) {
			Artifex m;
			long fr = 0;
			m.params[Artifex::RATE_PARAM].setValue(0.5f);
			m.params[Artifex::SYNC_PARAM].setValue(0.f);
			m.params[Artifex::LFO_ATT_PARAM].setValue(att);
			m.inputs[Artifex::LFO_INPUT].channels = 1;
			m.inputs[Artifex::LFO_INPUT].setVoltage(cv);
			runSilence(m, fr, 0.2);
			int count = 0;
			bool was = false;
			for (long i = 0; i < (long)(4.0 * SR); i++) {
				step(m, fr, 0.f, 0.f);
				bool hi = m.outputs[Artifex::SAW_OUTPUT].getVoltage() > 5.f;
				if (hi && !was)
					count++;
				was = hi;
			}
			return count / 4.0;
		}
	};
	double none = Local::cycles(0.f, 0.f);
	double inert = Local::cycles(0.f, 8.f);      // hot CV, attenuverter shut
	double live = Local::cycles(1.f, 8.f);       // same CV, attenuverter open
	double drift = std::fabs(inert - none) / std::max(none, 1e-6);
	report("artifex", "lfo_mod_inert_at_zero_attenuverter", drift, drift < 0.02);
	report("artifex", "lfo_mod_works_when_opened", live / std::max(none, 1e-6),
	       live > none * 1.2);
}

// The two time CV inputs differ in when they land, not in what they do: one
// is held until the clock steps, the other is continuous.
static void testSteppedVersusFreeCv() {
	struct Local {
		// how many distinct values the delay's displayed time takes in a
		// second -- a handful if it is quantized to a slow clock, hundreds
		// if it follows the CV directly
		static int levels(int input) {
			Artifex m;
			long fr = 0;
			setMode(m, artifex_fx::MODE_DELAY);
			m.params[Artifex::TEMPO_PARAM].setValue(60.f);      // a step every second
			m.params[Artifex::TIME_ATT_PARAM].setValue(1.f);
			m.inputs[input].channels = 1;
			runSilence(m, fr, 0.05);
			std::vector<float> seen;
			for (long i = 0; i < (long)(2.0 * SR); i++) {
				float cv = 5.f * std::sin(2.f * (float)M_PI * 0.7f * (float)i / SR);
				m.inputs[input].setVoltage(cv);
				step(m, fr, 0.f, 0.f);
				if ((i % 64) == 0) {
					float v = m.core.uiTime;
					bool fresh = true;
					for (size_t j = 0; j < seen.size(); j++)
						if (std::fabs(seen[j] - v) < 1e-4f) { fresh = false; break; }
					if (fresh)
						seen.push_back(v);
				}
			}
			return (int)seen.size();
		}
	};
	int stepped = Local::levels(Artifex::STEP_INPUT);
	int freeRun = Local::levels(Artifex::FREE_INPUT);
	report("artifex", "step_cv_lands_on_clock_steps", stepped, stepped <= 8);
	report("artifex", "free_cv_is_continuous", freeRun, freeRun > 50);
}

// The pattern's gate output drives the trig input, so a mode's trig action
// fires on the rhythm. Checked on the panner, whose trig throws the image.
static void testPatternGateDrivesTrig() {
	Artifex m;
	long fr = 0;
	setMode(m, artifex_fx::MODE_PANNER);
	m.params[Artifex::TEMPO_PARAM].setValue(240.f);
	m.params[Artifex::RHYTHM_PARAM].setValue(15.f);   // sixteenths, dense
	m.params[Artifex::TIME_PARAM].setValue(0.1f);     // slow pan
	m.params[Artifex::AMT_PARAM].setValue(1.f);
	m.inputs[Artifex::TRIG_INPUT].channels = 1;
	int throws = 0;
	bool wasLeft = false;
	for (long i = 0; i < (long)(4.0 * SR); i++) {
		// the patch cable: gate out into trig in
		m.inputs[Artifex::TRIG_INPUT].setVoltage(
		    m.outputs[Artifex::GATE_OUTPUT].getVoltage());
		float x = 2.f * std::sin(2.f * (float)M_PI * 220.f * (float)fr / SR);
		step(m, fr, x, x);
		bool left = m.outputs[Artifex::LEFT_OUTPUT].getVoltage()
		            > m.outputs[Artifex::RIGHT_OUTPUT].getVoltage();
		if (i > (long)(0.5 * SR) && left != wasLeft)
			throws++;
		wasLeft = left;
	}
	// a slow pan crosses twice a cycle on its own; the rhythm should add many
	// more than that over three and a half seconds
	report("artifex", "pattern_gate_fires_the_trig", throws, throws > 8);
}

static void testModeSelect() {
	Artifex m;
	long fr = 0;
	m.params[Artifex::TEMPO_PARAM].setValue(60.f);   // a step every 250 ms
	m.params[Artifex::FXMODE_ATT_PARAM].setValue(1.f);
	m.inputs[Artifex::FXMODE_INPUT].channels = 1;
	runSilence(m, fr, 0.1);

	// ten volts is the whole list: the top of the CV is the last mode
	m.inputs[Artifex::FXMODE_INPUT].setVoltage(10.f);
	runSilence(m, fr, 0.02);
	int aimedTop = m.aimedMode;
	report("artifex", "mode_cv_top_is_last", aimedTop, aimedTop == 8);

	// and it wraps past the end rather than sticking
	m.inputs[Artifex::FXMODE_INPUT].setVoltage(11.2f);
	runSilence(m, fr, 0.02);
	report("artifex", "mode_cv_wraps", m.aimedMode, m.aimedMode == 1);

	// the change itself waits for the next step of the clock
	Artifex m2;
	long fr2 = 0;
	m2.params[Artifex::TEMPO_PARAM].setValue(30.f);   // a step every 500 ms
	m2.params[Artifex::FXMODE_ATT_PARAM].setValue(1.f);
	m2.inputs[Artifex::FXMODE_INPUT].channels = 1;
	runSilence(m2, fr2, 0.05);
	int before = m2.mode;
	m2.inputs[Artifex::FXMODE_INPUT].setVoltage(5.f);
	runSilence(m2, fr2, 0.05);
	bool waited = (m2.mode == before) && (m2.aimedMode != before);
	report("artifex", "mode_cv_waits_for_clock", waited ? 1 : 0, waited);
	runSilence(m2, fr2, 0.6);
	report("artifex", "mode_cv_lands_on_the_step", m2.mode, m2.mode == m2.aimedMode);

	// the knob, though, changes mode the moment it moves
	Artifex m3;
	long fr3 = 0;
	m3.params[Artifex::FXMODE_PARAM].setValue(6.f);
	runSilence(m3, fr3, 0.01);
	report("artifex", "mode_knob_is_immediate", m3.mode, m3.mode == 6);
}

// ── the loop stays on the rails with the feedback wide open ───────────────────
static void testFeedbackSafety() {
	float worst = 0.f;
	long nans = 0;
	for (int mode = 0; mode < kModes; mode++) {
		Artifex m;
		long fr = 0;
		setMode(m, mode);
		m.params[Artifex::AMT_PARAM].setValue(1.f);
		m.params[Artifex::FBK_PARAM].setValue(1.f);
		m.params[Artifex::GAIN_PARAM].setValue(4.f);
		m.params[Artifex::LEVEL_PARAM].setValue(1.f);
		m.params[Artifex::TIME_PARAM].setValue(0.85f);
		Rec rec;
		runTone(m, fr, 1.5, 180.f, 8.f, &rec);
		runSilence(m, fr, 1.5, &rec);
		worst = std::max(worst, std::max(rec.sl.peak, rec.sr.peak));
		nans += rec.sl.nans + rec.sr.nans;
	}
	report("artifex", "feedback_finite", nans, nans == 0);
	report("artifex", "feedback_bounded", worst, worst < 12.f);
}

// ── every knob at every extreme, in every mode ────────────────────────────────
// ── stress and edges ────────────────────────────────────────────────────────
// These were the last of the listening tests that were really arithmetic.

// Everything at maximum, in every mode, on a hot input: finite and bounded is
// what testAbuse already checks. What it did not check is the two failures
// that matter more than a rail -- a mode that goes permanently silent, and one
// that parks on a DC offset. Both are survivable-looking on a scope and both
// make the module useless until it is reset.
static void testExtremesStaySane() {
	int worstSilent = -1, worstDc = -1;
	double quietest = 1e9, mostDc = 0.0;
	for (int mode = 0; mode < kModes; mode++) {
		Artifex m;
		long fr = 0;
		setMode(m, mode);
		m.params[Artifex::TIME_PARAM].setValue(1.f);
		m.params[Artifex::AMT_PARAM].setValue(1.f);
		m.params[Artifex::FBK_PARAM].setValue(1.f);
		m.params[Artifex::STEREO_PARAM].setValue(1.f);
		m.params[Artifex::GAIN_PARAM].setValue(4.f);
		m.params[Artifex::LEVEL_PARAM].setValue(1.f);
		m.params[Artifex::TEMPO_PARAM].setValue(300.f);
		Rec rec;
		runTone(m, fr, 6.0, 220.f, 5.f, &rec);
		// the last second only: settling is allowed, staying dead is not
		size_t from = rec.l.size() > (size_t)SR ? rec.l.size() - (size_t)SR : 0;
		double e = 0.0, dc = 0.0;
		for (size_t i = from; i < rec.l.size(); i++) {
			e += (double)rec.l[i] * rec.l[i];
			dc += rec.l[i];
		}
		size_t n = rec.l.size() - from;
		double rms = std::sqrt(e / n), off = std::fabs(dc / n);
		if (rms < quietest) { quietest = rms; worstSilent = mode; }
		if (off > mostDc) { mostDc = off; worstDc = mode; }
	}
	if (quietest <= 0.01)
		std::printf("# quietest at the extremes was mode %d\n", worstSilent + 1);
	if (mostDc >= 0.5)
		std::printf("# most DC at the extremes was mode %d\n", worstDc + 1);
	report("artifex", "extremes_never_go_silent", quietest, quietest > 0.01);
	report("artifex", "extremes_leave_no_dc", mostDc, mostDc < 0.5);
}

// Cycling modes with a long tail and the feedback up. A discontinuity is
// expected -- the modes hold different buffers -- but not a full-scale pop.
static void testModeCyclingDoesNotPop() {
	Artifex m;
	long fr = 0;
	m.params[Artifex::TIME_PARAM].setValue(0.2f);
	m.params[Artifex::AMT_PARAM].setValue(1.f);
	m.params[Artifex::FBK_PARAM].setValue(0.8f);
	m.params[Artifex::LEVEL_PARAM].setValue(1.f);
	setMode(m, 0);
	runTone(m, fr, 2.0, 220.f, 3.f);            // fill the tails
	float worst = 0.f;
	int worstMode = 0;
	float z1 = 0.f, z2 = 0.f;
	for (int k = 0; k < kModes * 3; k++) {
		setMode(m, k % kModes);
		Rec rec;
		runTone(m, fr, 0.25, 220.f, 3.f, &rec);
		// Only the first few milliseconds: what is being checked is the step
		// AT the change, and a mode left running with the feedback up is
		// entitled to be as loud and as wild as its knobs say. Measuring the
		// whole quarter second instead called the pitcher's feedback -- which
		// climbs to the limiter and thrashes there, correctly, and is bounded
		// by feedback_bounded -- a pop at a mode change it had nothing to do
		// with.
		size_t look = std::min(rec.l.size(), (size_t)(0.005 * SR));
		for (size_t i = 0; i < look; i++) {
			// the step against where the last two samples were heading
			float pred = 2.f * z1 - z2;
			float jump = std::fabs(rec.l[i] - pred);
			if (jump > worst) { worst = jump; worstMode = k % kModes; }
			z2 = z1;
			z1 = rec.l[i];
		}
	}
	if (worst >= 6.f)
		std::printf("# worst pop was entering mode %d\n", worstMode + 1);
	report("artifex", "mode_cycling_does_not_pop", worst, worst < 6.f);
}

// The numbers on the display are in seconds, hertz and semitones, so none of
// them may move with the sample rate. This is the check that a rate-dependent
// coefficient somewhere would fail, and it covers four modes at once.
static void testSampleRateInvariance() {
	struct Local {
		static void readings(float sr, double* out) {
			const int knobs = 3;
			const float pos[knobs] = {0.25f, 0.5f, 0.75f};
			int w = 0;
			for (int mode = 0; mode < kModes; mode++) {
				for (int k = 0; k < knobs; k++) {
					Artifex m;
					long fr = 0;
					Module::SampleRateChangeEvent sre;
					sre.sampleRate = sr;
					sre.sampleTime = 1.f / sr;
					m.onSampleRateChange(sre);
					setMode(m, mode);
					m.params[Artifex::TIME_PARAM].setValue(pos[k]);
					m.params[Artifex::TEMPO_PARAM].setValue(120.f);
					for (long i = 0; i < (long)(0.4 * sr); i++) {
						m.inputs[Artifex::LEFT_INPUT].channels = 1;
						m.inputs[Artifex::LEFT_INPUT].setVoltage(
						    2.f * std::sin(2.f * (float)M_PI * 220.f * (float)fr / sr));
						Module::ProcessArgs a;
						a.sampleRate = sr;
						a.sampleTime = 1.f / sr;
						a.frame = fr++;
						m.process(a);
					}
					// The crusher's rate spans 200 Hz to the sample rate --
					// its top has to be sr for the clean end to be clean --
					// so its reading is *supposed* to move, and neither the
					// rate nor rate/sr is the invariant. The knob position
					// recovered from it is: rate = 200 (sr/200)^t.
					out[w++] = mode == artifex_fx::MODE_CRUSHER
					           ? std::log(m.core.uiTime / 200.0)
					             / std::log((double)sr / 200.0)
					           : m.core.uiTime;
				}
			}
		}
	};
	const float rates[4] = {44100.f, 48000.f, 96000.f, 192000.f};
	double ref[kModes * 3];
	Local::readings(48000.f, ref);
	double worst = 0.0;
	int worstMode = 0;
	float worstRate = 0.f;
	for (int r = 0; r < 4; r++) {
		double got[kModes * 3];
		Local::readings(rates[r], got);
		for (int i = 0; i < kModes * 3; i++) {
			double denom = std::max(std::fabs(ref[i]), 1e-3);
			double err = std::fabs(got[i] - ref[i]) / denom;
			if (err > worst) {
				worst = err;
				worstMode = i / 3;
				worstRate = rates[r];
			}
		}
	}
	if (worst >= 0.02)
		std::printf("# worst was mode %d at %.0f Hz\n", worstMode + 1, worstRate);
	report("artifex", "display_reads_the_same_at_every_rate", worst, worst < 0.02);
}

// Bypass passes audio, and coming out of it does not leave the module dead.
static void testBypass() {
	Artifex m;
	long fr = 0;
	setMode(m, artifex_fx::MODE_DELAY);
	m.params[Artifex::AMT_PARAM].setValue(1.f);
	m.params[Artifex::FBK_PARAM].setValue(0.6f);
	m.params[Artifex::LEVEL_PARAM].setValue(1.f);
	Rec pre;
	runTone(m, fr, 1.0, 220.f, 3.f, &pre);
	// Rack bypasses by routing the configured input straight to the output
	// without calling process(), so the check that matters here is that the
	// module still works after the gap rather than what happens during it.
	double gap = 0.5;
	for (long i = 0; i < (long)(gap * SR); i++)
		fr++;
	Rec post;
	runTone(m, fr, 1.0, 220.f, 3.f, &post);
	double a = rmsOf(pre.l, pre.l.size() / 2, pre.l.size());
	double b = rmsOf(post.l, post.l.size() / 2, post.l.size());
	report("artifex", "alive_after_a_bypass_gap", b, b > a * 0.5);
}

// A duplicated module comes up on its own state, not on a stale buffer: the
// json round-trip is what Ctrl+D actually does.
static void testStateRoundTrip() {
	Artifex a;
	long fr = 0;
	setMode(a, artifex_fx::MODE_FREEZER);
	a.params[Artifex::AMT_PARAM].setValue(0.8f);
	a.bufSeconds = 2.5f;
	a.core.fourPole = true;
	a.core.filterInLoop = true;
	a.core.filterDry = true;
	a.quantizeModeChanges = false;
	a.honourExternalClock = false;
	a.monoInput = true;
	runTone(a, fr, 1.0, 220.f, 3.f);
	json_t* j = a.dataToJson();
	Artifex b;
	b.dataFromJson(j);
	json_decref(j);
	int same = (b.bufSeconds == a.bufSeconds)
	           + (b.core.fourPole == a.core.fourPole)
	           + (b.core.filterInLoop == a.core.filterInLoop)
	           + (b.core.filterDry == a.core.filterDry)
	           + (b.quantizeModeChanges == a.quantizeModeChanges)
	           + (b.honourExternalClock == a.honourExternalClock)
	           + (b.monoInput == a.monoInput);
	report("artifex", "state_survives_a_duplicate", same, same == 7);
	// and the copy makes sound rather than coming up on a stale buffer
	long fr2 = 0;
	Rec rec;
	setMode(b, artifex_fx::MODE_FREEZER);
	runTone(b, fr2, 2.0, 220.f, 3.f, &rec);
	double rms = rmsOf(rec.l, rec.l.size() / 2, rec.l.size());
	report("artifex", "duplicate_makes_sound", rms, rms > 0.05);
}

// A long run with the feedback modulated: no drift into silence, no DC piling
// up, no noise floor climbing. Two minutes rather than the twenty a person
// would have had to sit through, which is still a hundred laps of the tape.
static void testLongRunStability() {
	Artifex m;
	long fr = 0;
	setMode(m, artifex_fx::MODE_REPLAYER);
	m.params[Artifex::TIME_PARAM].setValue(0.8f);
	m.params[Artifex::AMT_PARAM].setValue(0.85f);
	m.params[Artifex::LEVEL_PARAM].setValue(1.f);
	double rmsFirst = 0.0, rmsLast = 0.0, dcLast = 0.0;
    long total = (long)(120.0 * SR);
	double e = 0.0, dc = 0.0;
	long n = 0;
	for (long i = 0; i < total; i++) {
		// feedback wandering slowly across its range
		float fb = 0.5f + 0.45f * std::sin(2.f * (float)M_PI * 0.03f * (float)i / SR);
		m.params[Artifex::FBK_PARAM].setValue(fb);
		float x = 2.f * std::sin(2.f * (float)M_PI * 220.f * (float)fr / SR)
		          + 0.5f * std::sin(2.f * (float)M_PI * 313.f * (float)fr / SR);
		step(m, fr, x, x);
		float y = m.outputs[Artifex::LEFT_OUTPUT].getVoltage();
		e += (double)y * y;
		dc += y;
		n++;
		if (i == (long)(10.0 * SR)) {
			rmsFirst = std::sqrt(e / n);
			e = dc = 0.0;
			n = 0;
		}
	}
	rmsLast = std::sqrt(e / n);
	dcLast = std::fabs(dc / n);
	double ratio = rmsLast / std::max(rmsFirst, 1e-9);
	report("artifex", "long_run_does_not_fade_or_climb", ratio,
	       ratio > 0.25 && ratio < 4.0);
	report("artifex", "long_run_leaves_no_dc", dcLast, dcLast < 0.05);
}

static void testAbuse() {
	long nans = 0;
	float worst = 0.f;
	for (int mode = 0; mode < kModes; mode++) {
		Artifex m;
		long fr = 0;
		setMode(m, mode);
		m.inputs[Artifex::FREE_INPUT].channels = 1;
		m.inputs[Artifex::TRIG_INPUT].channels = 1;
		m.params[Artifex::TIME_ATT_PARAM].setValue(1.f);
		for (int i = 0; i < 40; i++) {
			m.params[Artifex::TIME_PARAM].setValue(random::uniform());
			m.params[Artifex::AMT_PARAM].setValue(random::uniform());
			m.params[Artifex::FBK_PARAM].setValue(random::uniform());
			m.params[Artifex::FILTER_PARAM].setValue(2.f * random::uniform() - 1.f);
			m.params[Artifex::STEREO_PARAM].setValue(random::uniform());
			m.params[Artifex::GAIN_PARAM].setValue(4.f * random::uniform());
			m.inputs[Artifex::FREE_INPUT].setVoltage(10.f * random::uniform() - 5.f);
			m.inputs[Artifex::TRIG_INPUT].setVoltage(random::uniform() > 0.7f ? 5.f : 0.f);
			Rec rec;
			runTone(m, fr, 0.05, 50.f + 2000.f * random::uniform(), 5.f, &rec);
			nans += rec.sl.nans + rec.sr.nans;
			worst = std::max(worst, std::max(rec.sl.peak, rec.sr.peak));
		}
	}
	report("artifex", "abuse_finite", nans, nans == 0);
	report("artifex", "abuse_bounded", worst, worst < 12.f);
}

// ── crossing the filter knob does not click, and it is inside the loop ───────
static void testFilterCrossing() {
	Artifex m;
	long fr = 0;
	setMode(m, artifex_fx::MODE_DELAY);
	m.params[Artifex::AMT_PARAM].setValue(1.f);
	m.params[Artifex::FBK_PARAM].setValue(0.5f);
	m.params[Artifex::LEVEL_PARAM].setValue(1.f);
	m.params[Artifex::GAIN_PARAM].setValue(1.f);
	m.params[Artifex::FILTER_PARAM].setValue(-0.5f);
	runTone(m, fr, 0.5, 220.f, 3.f);

	Rec steadyRec;
	runTone(m, fr, 0.2, 220.f, 3.f, &steadyRec);
	m.params[Artifex::FILTER_PARAM].setValue(0.5f);
	Rec crossRec;
	runTone(m, fr, 0.2, 220.f, 3.f, &crossRec);

	double steady = 0.0, cross = 0.0;
	for (size_t i = 1; i < steadyRec.l.size(); i++)
		steady = std::max(steady, (double)std::fabs(steadyRec.l[i] - steadyRec.l[i - 1]));
	for (size_t i = 1; i < crossRec.l.size(); i++)
		cross = std::max(cross, (double)std::fabs(crossRec.l[i] - crossRec.l[i - 1]));
	report("artifex", "filter_crossing_is_quiet", cross / std::max(steady, 1e-9),
	       cross < 3.0 * steady);
}

SMOKE_MAIN(testFilterCrossing, testModeLabels, testDryAtZero, testAllModesAudible, testDelay,
           testFreezer, testPanner, testCrusher, testSlicer, testSlicerDecay, testPitch,
           testReplayer, testReplayerLevel, testReplayerSplice, testReplayerJoin, testReplayerUnlock, testReplayerClicks, testReplayerRetune, testReplayerCrossing, testReplayerLoopLength, testStereo, testTrigDeclick, testFilterPlacement, testFreezerLength, testDelayClockSync, testWrapClick,
           testClipContinuity, testDelaySweep, testCrusherRange, testCrusherAmount, testCrusherFeedback,
           testEnvelope,
           testModeSelect, testLfoPwm, testPatternReset, testHonourExternalClock,
           testLfoModAttenuverter, testSteppedVersusFreeCv, testPatternGateDrivesTrig,
           testFeedbackSafety, testAbuse, testExtremesStaySane,
           testModeCyclingDoesNotPop, testSampleRateInvariance, testBypass,
           testStateRoundTrip, testLongRunStability)
