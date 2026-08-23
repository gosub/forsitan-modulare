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

// ── the replayer runs its tape backwards below the centre ─────────────────────
static void testReplayer() {
	Artifex m;
	long fr = 0;
	setMode(m, artifex_fx::MODE_REPLAYER);
	m.params[Artifex::AMT_PARAM].setValue(1.f);     // locked: play, do not record
	m.params[Artifex::FBK_PARAM].setValue(0.f);
	m.params[Artifex::TIME_PARAM].setValue(0.75f);  // forwards
	m.params[Artifex::LEVEL_PARAM].setValue(1.f);
	m.params[Artifex::GAIN_PARAM].setValue(1.f);

	// fill the tape with a rising sweep, then lock it and listen
	m.params[Artifex::AMT_PARAM].setValue(0.f);
	m.params[Artifex::TIME_PARAM].setValue(0.75f);
	long n = (long)(1.2 * SR);
	for (long i = 0; i < n; i++) {
		float t = (float)i / SR;
		float x = 3.f * std::sin(2.f * (float)M_PI * (100.f + 400.f * t) * t);
		step(m, fr, x, x);
	}
	m.params[Artifex::AMT_PARAM].setValue(1.f);
	Rec fwd;
	runSilence(m, fr, 0.6, &fwd);
	double a = zcr(fwd.l, 0, fwd.l.size() / 3);
	double b = zcr(fwd.l, 2 * fwd.l.size() / 3, fwd.l.size());
	report("artifex", "replayer_plays_forward", b - a, b > a);

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

	// the same tape backwards: the sweep now falls
	m.params[Artifex::TIME_PARAM].setValue(0.25f);
	Rec back;
	runSilence(m, fr, 0.6, &back);
	double c = zcr(back.l, 0, back.l.size() / 3);
	double d = zcr(back.l, 2 * back.l.size() / 3, back.l.size());
	report("artifex", "replayer_plays_backward", c - d, d < c);
}

// ── the stereo knob pulls the channels apart ──────────────────────────────────
static void testStereo() {
	double width[2] = {0.0, 0.0};
	for (int k = 0; k < 2; k++) {
		Artifex m;
		long fr = 0;
		setMode(m, artifex_fx::MODE_FLANGER);
		m.params[Artifex::AMT_PARAM].setValue(1.f);
		m.params[Artifex::FBK_PARAM].setValue(0.3f);
		m.params[Artifex::TIME_PARAM].setValue(0.4f);
		m.params[Artifex::STEREO_PARAM].setValue(k == 0 ? 0.f : 1.f);
		m.params[Artifex::LEVEL_PARAM].setValue(1.f);
		m.params[Artifex::GAIN_PARAM].setValue(1.f);
		Rec rec;
		runTone(m, fr, 2.0, 300.f, 3.f, &rec);
		double sum = 0.0, diff = 0.0;
		for (size_t i = rec.l.size() / 2; i < rec.l.size(); i++) {
			double s = 0.5 * (rec.l[i] + rec.r[i]);
			double d = 0.5 * (rec.l[i] - rec.r[i]);
			sum += s * s;
			diff += d * d;
		}
		width[k] = std::sqrt(diff / std::max(sum, 1e-12));
	}
	report("artifex", "stereo_mono_at_zero", width[0], width[0] < 0.05);
	report("artifex", "stereo_widens", width[1], width[1] > width[0] + 0.1);
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
           testFreezer, testPanner, testCrusher, testSlicer, testPitch,
           testReplayer, testStereo, testTrigDeclick, testFilterPlacement, testFreezerLength, testDelayClockSync, testWrapClick,
           testClipContinuity, testDelaySweep, testCrusherRange, testCrusherAmount, testCrusherFeedback,
           testEnvelope,
           testModeSelect,
           testFeedbackSafety, testAbuse)
