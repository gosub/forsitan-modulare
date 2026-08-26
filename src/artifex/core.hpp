#pragma once
#include <rack.hpp>

#include "../shared/dsp.hpp"
#include "../shared/modulation.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

// The FX core of artifex: nine modes around one filtered feedback loop.
//
// Every mode reads the same three controls — time, feedback, amount — plus
// the filter that sits inside the loop and the stereo detune that pulls the
// two channels apart. What they act on is the mode's business; that they mean
// rate, loop and wetness is the machine's. See doc/artifex.md.
//
// Header-only and free of Rack widgets, so test/artifex_probe can drive it.

namespace artifex_fx {

using namespace rack;
using forsitan_dsp::Delay;
using forsitan_dsp::Svf;

enum Mode {
	MODE_DELAY = 0,
	MODE_FLANGER,
	MODE_FREEZER,
	MODE_PANNER,
	MODE_CRUSHER,
	MODE_SLICER,
	MODE_PITCHER,
	MODE_REPLAYER,
	MODE_SHIFTER,
	MODE_COUNT
};

// What the right-hand display is reading.
enum TimeUnit {
	UNIT_MS = 0,
	UNIT_HZ,
	UNIT_DIV,        // a ratio of the clock period
	UNIT_STEPS,      // a length in clock steps
	UNIT_RHYTHM,     // which of the 32 patterns
	UNIT_SEMI,       // a pitch interval
	UNIT_SPEED,      // tape speed, signed
};

struct Ctl {
	int mode = 0;
	float time = 0.5f;        // 0..1, knob and CV already summed
	float amount = 0.5f;      // 0..1
	float feedback = 0.f;     // 0..1
	float filter = 0.f;       // -1..1, lowpass to the left
	float stereo = 0.f;       // 0..1
	bool trig = false;        // an edge this sample
	bool stepped = false;     // the clock advanced this sample
	int step = 0;             // which step it is on
	float stepSeconds = 0.125f;
	float trigPeriod = 0.f;   // measured period of the trig input, 0 = none
	bool limiter = true;
	float dt = 1.f / 44100.f;
};

// The longest delay the hardware has, and what every mode's time range is
// written against.
static const float kHardwareBuffer = 1.15f;

// Where the signal starts folding. Every buffer write and the output limiter
// alike run softClip over x / kClipVolts and scale back, so the path is exactly
// linear below this and asymptotic to it above. It is therefore also the level
// the input lamps and the envelope follower in src/artifex.cpp read against:
// three different numbers for the same ceiling would light the lamps after the
// sound had already been squashed.
static const float kClipVolts = 5.f;

// How long the slicer's envelope takes to open. Short enough to be a chop,
// long enough not to click.
static const float kSliceAttack = 0.002f;

// The delay's time glides towards the knob rather than following it. A knob in
// Rack arrives in steps -- a parameter moves once per UI frame -- and a delay
// that teleports its read position clicks on every one of them, which is a
// buzz at the frame rate and not a sweep. The glide is also the mode's whole
// character: a read pointer travelling through the tape at a speed other than
// one is exactly the pitch bend a tape echo makes when its motor changes
// speed, and 50 ms is slow enough to hear that and fast enough that the knob
// still feels connected.
static const float kDelayGlide = 0.050f;

// The flanger and the pitcher multiply a knob straight into a read position
// too, and need the same treatment for the same reason -- but not the bend.
// Twenty milliseconds bridges the 16.7 ms between two UI frames and is short
// enough that the knob still feels direct.
static const float kKnobGlide = 0.020f;

// How long the step a trig leaves behind takes to relax away. A trig resets a
// phase in the flanger, the panner, the pitcher and the shifter, and swaps the
// buffer under the playhead in the freezer and the replayer; either way the
// output moves somewhere it was not heading, and a step in a waveform is a
// click. Two milliseconds is long enough to carry the step and short enough
// that a triggered stereo throw still lands on the beat.
static const float kTrigDeclick = 0.002f;
// Every fade in the replayer: pass edges, the loop join, the ghost
// crossfade, the window over the record head. It is a splice length, not a
// declick length: a diagonal cut across quarter-inch tape overlaps for ten
// to thirty milliseconds at studio speeds, and that is what makes a splice
// sound like a splice on any material -- two milliseconds removes the click
// but leaves the transition audible as a transient on anything correlated.
static const float kEdgeFade = 0.010f;

// The tone control, at either of two slopes.
//
// Two poles is artifex's own: gentle, and narrow enough in range that the far
// ends of the travel still let the material through -- at the top of the
// highpass a 4 kHz component is down 2.9 dB and 10 kHz is untouched.
//
// Four is the one vates carries, Butterworth-damped with the resonance in the
// second section only, and reaching far enough past the material to take it
// away at either end. The two modules are two faces of one board, so this is
// the same filter arriving here; it is a choice rather than a replacement
// because the shallow one is the gentler tone control and some patches want
// that.
struct ToneFilter {
	Svf a, b;

	void reset() {
		a.reset();
		b.reset();
	}

	// `filt` is the smoothed knob, -1 to +1, open at the centre.
	float process(float x, float filt, float sr, bool fourPole) {
		// the wet path fades in over the first twentieth of the travel, which
		// is the stretch where the filter is transparent anyway
		float wet = clamp(std::fabs(filt) * 20.f, 0.f, 1.f);
		if (wet < 1e-4f)
			return x;
		bool lowpass = filt < 0.f;
		float mag = std::fabs(filt);
		float fc = fourPole
		           ? (lowpass ? 30.f * std::pow(667.f, 1.f - mag)
		                      : 25.f * std::pow(560.f, mag))
		           : (lowpass ? 80.f * std::pow(250.f, 1.f - mag)
		                      : 20.f * std::pow(200.f, mag));
		fc = clamp(fc, 20.f, 0.45f * sr);
		float g = std::tan((float)M_PI * fc / sr);
		float lp, hp, y;
		if (fourPole) {
			a.process(x, g, 1.f / 0.541f, lp, hp);
			y = lowpass ? lp : hp;
			b.process(y, g, 1.f / (1.8f + 1.2f * mag), lp, hp);
			y = lowpass ? lp : hp;
		}
		else {
			a.process(x, g, 1.4f, lp, hp);
			y = lowpass ? lp : hp;
		}
		return x + (y - x) * wet;
	}
};

// A one-pole glide that snaps the first time it is asked. The mode's state is
// reset on entering it, so arriving somewhere gives you the knob rather than a
// slide from wherever the last mode left this.
struct Glide {
	float v = 0.f;
	bool primed = false;
	void reset() { primed = false; }
	float operator()(float target, float dt, float tau) {
		if (!primed) {
			v = target;
			primed = true;
		}
		else {
			v += (target - v) * (1.f - std::exp(-dt / tau));
		}
		return v;
	}
};

struct Core {
	float sr = 44100.f;
	float bufSeconds = kHardwareBuffer;

	Delay tape[2];        // delay, freezer, replayer, pitcher
	Delay frz[2];         // the chunk the freezer is holding
	Delay flg[2];         // the flanger's short modulated line
	Delay shf[2];         // the shifter's window
	// Three places the filter can sit, one instance each so that moving it
	// never means filtering something twice.
	ToneFilter modeFilt[2];   // where the mode puts it: its wet path, or its loop
	ToneFilter outFilt[2];    // on the module's output, dry included
	ToneFilter fbLoopFilt[2]; // inside the delay's and the flanger's own feedback
	float declick[2] = {0.f, 0.f};   // the step a trig left, on its way out
	float declickZ1[2] = {0.f, 0.f}; // the last two samples actually emitted,
	float declickZ2[2] = {0.f, 0.f}; // so the step can be measured against them

	int lastMode = -1;
	float filtSm = 0.f;
	bool filtWasLow = false;
	// settings, from the context menu
	bool fourPole = false;      // 24 dB/oct and the wider range, as vates has
	bool filterDry = false;     // the filter moves to the output, dry included
	bool filterInLoop = false;  // the delay and the flanger feed back filtered
	float fbState[2] = {0.f, 0.f};

	// per-mode state
	float modPhase[2] = {0.f, 0.f};      // flanger, panner
	Glide delayGl;            // delay: the time, which is also its pitch bend
	Glide flangeGl;           // flanger: the sweep depth, off the amount knob
	Glide pitchWinGl;         // pitcher: the window, off the time knob
	Glide pitchAmtGl;         // pitcher: the shift, off the amount knob
	int panDir = 1;
	float panFrom[2] = {0.f, 0.f};   // where the pan was when the trig landed
	float panLast[2] = {0.f, 0.f};   // and where it is now, to throw from next
	float panFade = 1.f;             // 0..1 across the throw
	double freezePos[2] = {0.0, 0.0};
	float freezeFrames = 0.f;    // the loop length now, which the knob moves
	float capturedFrames = 0.f;  // how much history the last freeze caught
	double recorded = 0.0;        // frames recorded since arriving in the mode
	bool frozen = false;
	bool wasSilentAmount = true;
	float crushHold[2] = {0.f, 0.f};
	float crushPhase[2] = {0.f, 0.f};
	float crushFbLp[2] = {0.f, 0.f};
	float crushDip = 0.f;
	float sliceEnv[2] = {0.f, 0.f};
	float sliceAtk[2] = {0.f, 0.f};
	float grainPhase[2] = {0.f, 0.f};
	float grainStretch = 0.f;
	float grainW[2] = {0.f, 0.f};      // the window this grain was started with
	float grainShift[2] = {0.f, 0.f};  // and the shift, latched with it
	double tapePos[2] = {0.0, 0.0};
	double tapeWrite[2] = {0.0, 0.0};
	double tapeSeam[2] = {0.0, 0.0};   // where the take on the tape begins
	double ghostPos[2] = {0.0, 0.0};   // the reading the head has just left
	int ghostLeft[2] = {-1, -1};       // samples of crossfade still to run
	double loopLen = 0.0;              // how much of the tape the locked loop plays
	// The last window's worth of pre-write tape values, so a reading just
	// behind the record head can be given the generation the material in
	// front of the head still has. Indexed by tape slot modulo its own
	// length, which is unambiguous because nothing ever looks back further
	// than the window.
	std::vector<float> shadow[2];
	float joinXY[2] = {0.f, 0.f};      // how alike the two crossfading readings
	float joinXX[2] = {0.f, 0.f};      // are, measured while the crossfade runs
	float joinYY[2] = {0.f, 0.f};
	float corrXY[2] = {0.f, 0.f};      // how much the tape and the input agree
	float corrXX[2] = {0.f, 0.f};
	float corrYY[2] = {0.f, 0.f};
	Glide speedGl;                     // the speed knob, with a motor behind it
	float amtSm[3] = {0.f, 0.f, 0.f};  // the amount knob, slewed three times
	float recHold[3] = {0.f, 0.f, 0.f};   // the record level, slewed three times
	float passFade = 0.f;              // and how far into its ramp it is
	bool wasRec = false;
	// A fill is counted in slots, not in seconds. The tape is a few samples
	// longer than bufSeconds -- a lap of it is 55204 samples where 1.15 s is
	// 55200 -- so a fill measured in time stopped 44 slots short and left that
	// much silence on the tape, a full-scale drop and return once a lap.
	int fillLeft = 0;
	float shiftPhase[2] = {0.f, 0.f};
	uint32_t rng = 0x9e3779b9u;

	// what the display reads
	float uiTime = 0.f;
	int uiUnit = UNIT_MS;

	void setRates(float sampleRate, float seconds) {
		sr = sampleRate;
		bufSeconds = seconds;
		int n = (int)(seconds * sr) + 4;
		for (int c = 0; c < 2; c++) {
			tape[c].init(n);
			frz[c].init(n);
			flg[c].init((int)(0.05f * sr) + 4);
			shf[c].init((int)(0.3f * sr) + 4);
			// The window never exceeds three splices: it scales with
			// |speed - 1|, and the speed reaches two either way.
			shadow[c].assign((int)(kEdgeFade * sr * 4.f) + 8, 0.f);
			modeFilt[c].reset();
			outFilt[c].reset();
			fbLoopFilt[c].reset();
			tapePos[c] = 0.0;
			tapeWrite[c] = 0.0;
			tapeSeam[c] = 0.0;
			ghostPos[c] = 0.0;
			ghostLeft[c] = -1;
			passFade = 0.f;
			for (int q = 0; q < 3; q++)
				recHold[q] = amtSm[q] = 0.f;
			corrXY[c] = corrXX[c] = corrYY[c] = 0.f;
			freezePos[c] = 0.0;
		}
		lastMode = -1;
	}

	uint32_t next() {
		rng ^= rng << 13;
		rng ^= rng >> 17;
		rng ^= rng << 5;
		return rng;
	}

	void enterMode(int mode) {
		for (int c = 0; c < 2; c++) {
			modeFilt[c].reset();
			outFilt[c].reset();
			fbLoopFilt[c].reset();
			fbState[c] = 0.f;
			sliceAtk[c] = 0.f;
			modPhase[c] = 0.f;
			crushPhase[c] = 0.f;
			crushHold[c] = 0.f;
			crushFbLp[c] = 0.f;
			sliceEnv[c] = 0.f;
			grainPhase[c] = 0.f;
			grainW[c] = 0.f;
			shiftPhase[c] = (float)c * 0.5f;
			tapePos[c] = 0.0;
			tapeWrite[c] = 0.0;
			tapeSeam[c] = 0.0;
			ghostPos[c] = 0.0;
			ghostLeft[c] = -1;
			passFade = 0.f;
			for (int q = 0; q < 3; q++)
				recHold[q] = amtSm[q] = 0.f;
			corrXY[c] = corrXX[c] = corrYY[c] = 0.f;
		}
		panDir = 1;
		panFade = 1.f;
		for (int c = 0; c < 2; c++) {
			declick[c] = 0.f;
			panFrom[c] = 0.f;
			panLast[c] = 0.f;
		}
		delayGl.reset();
		flangeGl.reset();
		pitchWinGl.reset();
		pitchAmtGl.reset();
		crushDip = 0.f;
		grainStretch = 0.f;
		// Arriving in the replayer loads the tape, which is what a trigger
		// does there too. Without it, landing on the mode with amount hard
		// right — the locked position — plays a buffer nobody has recorded
		// into yet, and the mode is silent for no reason a player can see.
		fillLeft = (mode == MODE_REPLAYER) ? tape[0].size() : 0;
		frozen = false;
		recorded = 0.0;
		capturedFrames = 0.f;
		// Arriving in the freezer captures a chunk, as on the hardware — but
		// it has to record one first. Leaving this true would freeze the
		// empty buffer on the first sample and hold silence for good.
		wasSilentAmount = false;
		lastMode = mode;
	}

	// The filter lives in the feedback path only: it colours what comes back
	// round without touching the dry signal. Open at the centre, a lowpass to
	// the left and a highpass to the right.
	//
	// It reads the smoothed knob, never the raw one, and its wet path fades
	// in over the first twentieth of the travel: crossing the centre swaps a
	// lowpass for a highpass, and no integrator state survives that. A step
	// here would be worse than elsewhere — it goes straight back into the
	// loop and comes round again.
	// Where the mode itself puts the filter: on its wet path if it has one,
	// inside the global loop if it does not. With the filter moved to the
	// output this goes quiet, so nothing is filtered twice.
	float loopFilter(int c, float x) {
		if (filterDry)
			return x;
		return modeFilt[c].process(x, filtSm, sr, fourPole);
	}

	// The delay and the flanger keep their own feedback line, and take it from
	// before the filter -- so a repeat is filtered once, on its way out, and
	// the tail does not darken pass by pass. Routing it through here is what
	// makes it a dub delay instead. Its own instance, so it works whether or
	// not the filter has moved to the output.
	float feedbackFilter(int c, float x) {
		if (!filterInLoop)
			return x;
		return fbLoopFilt[c].process(x, filtSm, sr, fourPole);
	}

	// The whole output, dry included. The global-feedback modes get the filter
	// inside their loop for free here, since fbState is taken after it.
	float outputFilter(int c, float x) {
		if (!filterDry)
			return x;
		return outFilt[c].process(x, filtSm, sr, fourPole);
	}

	// Linear below kClipVolts, saturating above it -- and continuous at the
	// join, which the obvious form is not. tanh(1) is 0.762, so branching
	// straight into tanh dropped 0.238 -- 1.19 V -- the instant a signal
	// crossed the boundary, and climbed back the instant it recrossed. Every
	// mode sat on that step, because 5 V is where ordinary material sits: at
	// the flanger's normal level it read as a click twice per cycle of a tone,
	// and it was the largest discontinuity anywhere in the module.
	//
	// Starting the curve where the line ends matches the value and the slope
	// at the join, since d/dx tanh(0) is 1, so nothing happens there at all.
	// The asymptote moves from 1 to 2: kClipVolts is still where folding
	// begins, and a fully folded signal now reaches the rail rather than
	// stopping at nominal.
	static float softClip(float x) {
		if (x > 1.f)
			return 1.f + std::tanh(x - 1.f);
		if (x < -1.f)
			return -1.f - std::tanh(-x - 1.f);
		return x;
	}

	// How long the locked loop should be. Shortening the tape by a fixed
	// amount to make room for the join picks a length with no relation to
	// what is on it, and the phase it lands on is then whatever it is: a
	// 220 Hz tone on a 1.15 s tape is 253.02 cycles, six degrees from
	// joining itself, and taking a millisecond off moves that to seventy-two.
	// The step at the splice is traded for a bigger one a millisecond wide.
	//
	// So look instead: the loop wants a length whose end already resembles
	// its start, which for anything periodic is a whole number of periods.
	// One pass over about a thousand candidates when the tape locks, coarse
	// enough to be a few tens of microseconds.
	double chooseLoop(int n, float join) const {
		const int W = 128, stride = 2;
		int hi = n - (int)join - W;
		int lo = std::max(W + 1, hi - 1200);
		if (hi <= lo)
			return (double)n - join;
		// Measured a splice past the seam, not at it. A pass fades in over
		// exactly that distance, so a window sitting on the seam is looking
		// at material ramping up out of silence: the sum of squares it
		// minimises is then smallest wherever the *other* window happens to
		// be quietest, which has nothing to do with the phase the loop joins
		// on. It picked a length half a cycle out for most tones -- 110 Hz
		// scored 1.7 at 123.5 periods against 540 at a whole 125, the metric
		// preferring silence to a match. Only the offset matters, so moving
		// both windows into the take costs nothing.
		int s = (((int)tapeSeam[0] + (int)join) % n + n) % n;
		double best = 1e30;
		int bestL = hi;
		for (int L = lo; L <= hi; L++) {
			// Normalised. A plain sum of squares is smallest wherever the
			// two windows are quietest, so on material that breathes at all
			// it picks a lull rather than a match. Dividing by the energy in
			// the two windows asks how alike they are instead of how loud.
			double cost = 0.0, energy = 0.0;
			for (int k = 0; k < W; k += stride) {
				int ia = s + k;
				if (ia >= n) ia -= n;
				int ib = s + L + k;
				while (ib >= n) ib -= n;
				double a = tape[0].buf[ia], b = tape[0].buf[ib];
				double d = a - b;
				cost += d * d;
				energy += a * a + b * b;
			}
			cost /= energy + 1e-9;
			if (cost < best) {
				best = cost;
				bestL = L;
			}
		}
		// A whole number of samples is not a whole number of periods of
		// anything in particular: 220 Hz is 218.18 samples, so the closest
		// integer loop still leaves the two ends a fraction of a sample out,
		// and the crossfade glides across that difference once a lap. The
		// play head reads between slots anyway, so the loop does not have to
		// be an integer either -- fit a parabola through the cost at the best
		// candidate and its neighbours and take the minimum.
		double cm = matchCost(n, bestL - 1), c0 = matchCost(n, bestL);
		double cp = matchCost(n, bestL + 1);
		double denom = cm - 2.0 * c0 + cp;
		double frac = denom > 1e-20 ? 0.5 * (cm - cp) / denom : 0.0;
		return (double)bestL + std::max(-0.5, std::min(0.5, frac));
	}

	// how badly a loop of this length would join, over a short window
	double matchCost(int n, int L) const {
		const int W = 128, stride = 2;
		int s = (((int)tapeSeam[0] + (int)(kEdgeFade * sr)) % n + n) % n;
		double cost = 0.0, energy = 0.0;
		for (int k = 0; k < W; k += stride) {
			int ia = s + k;
			if (ia >= n) ia -= n;
			int ib = s + L + k;
			while (ib >= n) ib -= n;
			double a = tape[0].buf[ia], b = tape[0].buf[ib];
			double d = a - b;
			cost += d * d;
			energy += a * a + b * b;
		}
		return cost / (energy + 1e-9);
	}

	// Two channels pulled apart by the stereo knob: left slows, right speeds.
	static float detune(int c, float stereo) {
		float d = stereo * 0.35f;
		return c == 0 ? 1.f - d : 1.f + d;
	}

	void process(const Ctl& ct, float inL, float inR, float& outL, float& outR) {
		if (ct.mode != lastMode)
			enterMode(ct.mode);

		// the filter knob, smoothed, with the states cleared as it crosses
		// the centre — where the wet path above is faded out, so the clearing
		// is inaudible
		filtSm += (clamp(ct.filter, -1.f, 1.f) - filtSm)
		          * (1.f - std::exp(-ct.dt / 0.010f));
		bool low = filtSm < 0.f;
		if (low != filtWasLow) {
			for (int c = 0; c < 2; c++) {
				modeFilt[c].reset();
				outFilt[c].reset();
				fbLoopFilt[c].reset();
			}
			filtWasLow = low;
		}

		float in[2] = {inL, inR};
		float out[2] = {0.f, 0.f};
		float amt = clamp(ct.amount, 0.f, 1.f);
		float fb = clamp(ct.feedback, 0.f, 1.f);
		float t = clamp(ct.time, 0.f, 1.f);

		switch (ct.mode) {
		case MODE_DELAY:      doDelay(ct, in, out, t, amt, fb); break;
		case MODE_FLANGER:    doFlanger(ct, in, out, t, amt, fb); break;
		case MODE_FREEZER:    doFreezer(ct, in, out, t, amt, fb); break;
		case MODE_PANNER:     doPanner(ct, in, out, t, amt, fb); break;
		case MODE_CRUSHER:    doCrusher(ct, in, out, t, amt, fb); break;
		case MODE_SLICER:     doSlicer(ct, in, out, t, amt, fb); break;
		case MODE_PITCHER:    doPitcher(ct, in, out, t, amt, fb); break;
		case MODE_REPLAYER:   doReplayer(ct, in, out, t, amt, fb); break;
		default:              doShifter(ct, in, out, t, amt, fb); break;
		}

		// Take the step a trig leaves out of the output. What the mode did is
		// wanted -- a new chunk, a reset modulator, a thrown pan -- but the
		// jump it arrives on is not: the signal lands somewhere it was not
		// heading, and that is a click rather than a gesture.
		//
		// The step is measured against where the last two emitted samples were
		// going, not against the last one alone. A straight line through them
		// predicts an ordinary waveform to within the second difference, which
		// for a 220 Hz tone at 5 V is four millivolts -- so on a trig that
		// happens not to jump anything, almost nothing is subtracted.
		float declickDecay = std::exp(-ct.dt / kTrigDeclick);

		for (int c = 0; c < 2; c++) {
			if (!std::isfinite(out[c]))
				out[c] = 0.f;
			if (ct.trig)
				declick[c] = out[c] - (2.f * declickZ1[c] - declickZ2[c]);
			out[c] -= declick[c];
			declick[c] *= declickDecay;
			declickZ2[c] = declickZ1[c];
			declickZ1[c] = out[c];
			out[c] = outputFilter(c, out[c]);
			if (ct.limiter)
				out[c] = softClip(out[c] / kClipVolts) * kClipVolts;
			else
				out[c] = clamp(out[c], -20.f, 20.f);
			fbState[c] = out[c];
		}
		outL = out[0];
		outR = out[1];
	}

	// The global loop: the module's own output, filtered, back into its input.
	float loopIn(int c, float x, const Ctl& ct, float fb) {
		return x + loopFilter(c, fbState[c]) * fb * 0.95f;
	}

	// ── 1. delay ─────────────────────────────────────────────────────────────
	// Longest to the left, 2 ms to the right. Feedback is taken before the
	// filter, as on the hardware, so the filter colours the repeats rather
	// than swallowing them.
	void doDelay(const Ctl& ct, float* in, float* out, float t, float amt, float fb) {
		float maxT = bufSeconds - 0.01f;
		float base = maxT * std::pow(0.002f / maxT, t);
		uiUnit = UNIT_MS;

		// A clock at the trig input snaps the time to its nearest division,
		// exactly as the hardware does when it detects one there.
		if (ct.trigPeriod > 1e-4f) {
			static const float div[22] = {
				1.f / 256, 1.f / 128, 1.f / 64, 1.f / 32, 1.f / 16, 1.f / 12,
				1.f / 8, 1.f / 6, 1.f / 4, 1.f / 3, 1.f / 2, 2.f / 3,
				1.f, 3.f / 2, 2.f, 3.f, 4.f, 6.f, 8.f, 12.f, 16.f, 32.f};
			float best = base;
			float bestErr = 1e9f;
			for (int i = 0; i < 22; i++) {
				float cand = ct.trigPeriod * div[i];
				if (cand < 0.002f || cand > maxT)
					continue;
				float err = std::fabs(std::log(cand / base));
				if (err < bestErr) {
					bestErr = err;
					best = cand;
				}
			}
			base = best;
			uiUnit = UNIT_DIV;
			uiTime = base / std::max(ct.trigPeriod, 1e-6f);
		}
		if (uiUnit == UNIT_MS)
			uiTime = base * 1000.f;

		// The display reads the knob; the tape follows it. Arriving in the
		// mode snaps, so the first repeat is the time you asked for.
		float glided = delayGl(base, ct.dt, kDelayGlide);

		for (int c = 0; c < 2; c++) {
			float d = clamp(glided * detune(c, ct.stereo), 0.002f, maxT) * sr;
			float wet = tape[c].read(d);
			tape[c].write(softClip((in[c] + feedbackFilter(c, wet) * fb * 0.98f)
			                       / kClipVolts) * kClipVolts);
			float heard = loopFilter(c, wet);
			out[c] = in[c] * (1.f - amt) + heard * amt;
		}
	}

	// ── 2. flanger ───────────────────────────────────────────────────────────
	// A short delay swept by a sine: chorus around the middle, flanger with
	// feedback, pitch modulation at the extremes.
	void doFlanger(const Ctl& ct, float* in, float* out, float t, float amt, float fb) {
		float hz = 0.02f * std::pow(500.f, t);
		uiUnit = UNIT_HZ;
		uiTime = hz;
		// The modulator's rate is integrated into a phase, so a step in it is
		// harmless. The depth is not: it multiplies the read position, and a
		// knob that moves once per frame moves the tap in jumps.
		float dep = flangeGl(amt, ct.dt, kKnobGlide);
		// As in the panner: a reset to phase zero is a reset to the *centre* of
		// the sweep, and the tap has to jump there from wherever it was --
		// which in a delay line is a click nothing downstream can take back
		// out. Mirroring the phase leaves sin() where it is and reverses which
		// way it travels, so a trig turns the sweep round with no jump at all.
		if (ct.trig)
			for (int c = 0; c < 2; c++)
				modPhase[c] = 0.5f - modPhase[c] - std::floor(0.5f - modPhase[c]);
		for (int c = 0; c < 2; c++) {
			float f = hz * detune(c, ct.stereo);
			modPhase[c] += f * ct.dt;
			modPhase[c] -= std::floor(modPhase[c]);
			float m = std::sin(2.f * (float)M_PI * modPhase[c]);
			float base = 5.5f * 0.001f * sr;
			float depth = (5.f * dep) * 0.001f * sr;
			float wet = flg[c].read(base + depth * m);
			flg[c].write(softClip((in[c] + feedbackFilter(c, wet) * fb * 0.95f)
			                      / kClipVolts) * kClipVolts);
			float heard = loopFilter(c, wet);
			out[c] = in[c] * (1.f - 0.5f * amt) + heard * amt;
		}
	}

	// ── 3. freezer ───────────────────────────────────────────────────────────
	// Captures a chunk and loops it. Long times are divisions of the clock, so
	// the freeze is rhythmic; short ones shrink until the loop is a pitch.
	void doFreezer(const Ctl& ct, float* in, float* out, float t, float amt, float fb) {
		float len;
		if (t < 0.5f) {
			static const float steps[8] = {16.f, 12.f, 8.f, 6.f, 4.f, 3.f, 2.f, 1.f};
			int i = clamp((int)(t * 2.f * 8.f), 0, 7);
			len = ct.stepSeconds * steps[i];
			uiUnit = UNIT_STEPS;
			uiTime = steps[i];
		}
		else {
			len = 0.25f * std::pow(0.008f, (t - 0.5f) * 2.f);
			uiUnit = UNIT_MS;
			uiTime = len * 1000.f;
		}
		len = clamp(len, 0.002f, bufSeconds - 0.02f);

		// the record head never stops: what freezing does is take a copy, so
		// the next freeze still catches whatever has just gone past
		for (int c = 0; c < 2; c++)
			tape[c].write(in[c]);
		recorded += 1.0;
		bool ready = recorded >= (double)(len * sr);

		// three ways to catch a new chunk: arriving in the mode, a trigger,
		// and amount leaving zero -- none of which may capture a buffer that
		// has not been filled yet
		bool refreeze = ct.trig && ready;
		if (amt > 0.02f && wasSilentAmount && ready)
			refreeze = true;
		wasSilentAmount = (amt <= 0.02f);
		if (!frozen && ready)
			refreeze = true;

		// A freeze catches everything it can, not just the length asked for,
		// so the time knob can go on choosing how much of it loops. Catching
		// only `len` meant the knob did nothing at all until something
		// re-captured, and the way to hear a frozen bar shrink into a pitch
		// is to shorten the loop, not to freeze a shorter one.
		if (refreeze) {
			float maxFrames = (bufSeconds - 0.02f) * sr;
			capturedFrames = clamp((float)recorded, len * sr, maxFrames);
			int frames = (int)capturedFrames;
			for (int c = 0; c < 2; c++) {
				// the last `frames` samples ending at the freeze moment, in
				// order, as two spans rather than a modulo per sample
				int n = tape[c].size();
				int from = ((tape[c].w - frames) % n + n) % n;
				int first = std::min(frames, n - from);
				std::copy(tape[c].buf.begin() + from,
				          tape[c].buf.begin() + from + first,
				          frz[c].buf.begin());
				std::copy(tape[c].buf.begin(),
				          tape[c].buf.begin() + (frames - first),
				          frz[c].buf.begin() + first);
				freezePos[c] = 0.0;
			}
			frozen = true;
		}

		// The loop is the last `freezeFrames` before the freeze moment: the
		// captured chunk ends there, so shortening keeps the audio nearest to
		// it -- what you had just heard -- rather than the oldest of it.
		// Lengthening is limited by how much history the freeze actually got.
		freezeFrames = clamp(len * sr, 4.f, std::max(capturedFrames, 4.f));
		float base = capturedFrames - freezeFrames;

		for (int c = 0; c < 2; c++) {
			float rate = detune(c, ct.stereo);
			// the knob can shorten the loop under the playhead, so wrap it
			// round rather than assuming one subtraction is enough
			if (freezePos[c] >= (double)freezeFrames || freezePos[c] < 0.0)
				freezePos[c] -= std::floor(freezePos[c] / freezeFrames) * freezeFrames;
			float wet = frz[c].at(base + (float)freezePos[c]);
			// feedback here bleeds new audio into the frozen buffer rather
			// than running the global loop, thickening what is held
			if (fb > 0.001f)
				frz[c].poke((int)(base + (float)freezePos[c]), wet + in[c] * fb * 0.5f);
			freezePos[c] += rate;
			float heard = loopFilter(c, wet);
			out[c] = in[c] * (1.f - amt) + heard * amt;
		}
	}

	// ── 4. panner ────────────────────────────────────────────────────────────
	// Amplitude modulation in opposite phase, from a sway to a hard
	// alternation, and up into ring modulation as the rate leaves the LFO band.
	void doPanner(const Ctl& ct, float* in, float* out, float t, float amt, float fb) {
		float hz = 0.05f * std::pow(40000.f, t);
		uiUnit = UNIT_HZ;
		uiTime = hz;
		float depth = clamp(amt * 2.f, 0.f, 1.f);
		float hard = clamp(amt * 2.f - 1.f, 0.f, 1.f);
		// A trig throws the pan to the other side. It used to do that by
		// setting the phase to zero, and zero is the *centre* of the sweep --
		// so a pan sitting at one side, which is where it sits for most of its
		// cycle once amount hardens the sine towards a square, jumped half the
		// signal's amplitude in one sample to arrive in the middle. That is a
		// click, and taking the step back out of the output afterwards only
		// turns it into a thump: the correction is a transient of its own.
		//
		// So the phase goes to the *peak* on the side being thrown to rather
		// than to the centre, and the modulator glides there instead of
		// jumping. The glide is what makes it a throw: a gain moving smoothly
		// across in a few tens of milliseconds is a sound crossing the image,
		// where the same distance in one sample is only a click. It is capped
		// at a quarter of the modulator's own period so that up at ring
		// modulation rates it stays out of the way.
		if (ct.trig) {
			panDir = -panDir;
			float peak = (panDir > 0) ? 0.25f : 0.75f;
			for (int c = 0; c < 2; c++) {
				modPhase[c] = peak;
				panFrom[c] = panLast[c];
			}
			panFade = 0.f;
		}
		float throwTime = std::min(0.025f, 0.25f / std::max(hz, 0.01f));
		panFade = std::min(1.f, panFade + ct.dt / throwTime);
		for (int c = 0; c < 2; c++) {
			float f = hz * detune(c, ct.stereo);
			modPhase[c] += f * ct.dt * (float)panDir;
			modPhase[c] -= std::floor(modPhase[c]);
			float m = std::sin(2.f * (float)M_PI * modPhase[c]);
			// clipping the sine towards a square as amount goes up
			m = std::tanh(m * (1.f + hard * 30.f)) / std::tanh(1.f + hard * 30.f);
			m = panFrom[c] + (m - panFrom[c]) * panFade;
			panLast[c] = m;
			float s = (c == 0) ? m : -m;
			float x = loopIn(c, in[c], ct, fb);
			out[c] = x * (1.f - depth * 0.5f * (1.f - s));
		}
	}

	// ── 5. crusher ───────────────────────────────────────────────────────────
	// Downsampling, then bit mangling: the top of the amount knob XORs the
	// sample with a shift of itself, which is where it stops sounding like a
	// bitcrusher and starts sounding broken.
	void doCrusher(const Ctl& ct, float* in, float* out, float t, float amt, float fb) {
		// The top of the knob is the running sample rate, not a fixed number
		// of kHz: there the decimator holds for exactly one sample and passes
		// the signal through untouched. Stopping short of that — at sr/2, say
		// — leaves a sample-and-hold on every other sample, which is a good
		// 30 dB of grain that no knob position can get rid of.
		float rate = 200.f * std::pow(sr / 200.f, t);
		uiUnit = UNIT_HZ;
		uiTime = rate;
		if (ct.trig)
			crushDip = 1.f;
		crushDip *= std::exp(-ct.dt / 0.15f);
		rate *= std::pow(0.05f, crushDip);
		// The knob is bit depth in the bottom half and mangling in the top,
		// but the depth only starts once the wet fade below has finished, and
		// it starts at twelve bits rather than sixteen. Sixteen down to two
		// across half a knob spent its first quarter between -100 and -60 dB,
		// which is nothing you can hear, and then did the whole audible job in
		// the second quarter.
		float crush = clamp((amt - 0.1f) / 0.4f, 0.f, 1.f);
		float bits = 12.f - 9.f * crush;
		float xorAmt = clamp(amt * 2.f - 1.f, 0.f, 1.f);
		// an integer count, so the mangling below can wrap inside it
		int levels = (int)(std::pow(2.f, bits) + 0.5f);
		// amount owns the crushing itself, but the rule that a knob at zero
		// leaves the signal alone holds here too: the first tenth of the
		// travel fades the decimated path in
		float wetMix = clamp(amt * 10.f, 0.f, 1.f);

		for (int c = 0; c < 2; c++) {
			float f = clamp(rate * detune(c, ct.stereo), 20.f, sr);
			crushPhase[c] += f * ct.dt;
			if (crushPhase[c] >= 1.f) {
				crushPhase[c] -= std::floor(crushPhase[c]);
				// The loop runs around the sample-and-hold rather than
				// through the global one-sample path, and it runs AC.
				// Straight, at a gain under one it is only a gain: a
				// memoryless loop around a saturator has no pitch to it, and
				// the knob did nothing but turn the mode up. Over one, it
				// stops being memoryless the wrong way -- the fixed point
				// moves to a rail and it latches there, 4.6 V of DC and
				// silence. Taking the hold's own period as the loop delay
				// and blocking DC leaves it nothing to latch onto: it
				// oscillates instead, near a third of the crush rate, so the
				// backdrop is pitched and follows the time knob.
				float tick = 1.f / std::max(f, 1.f);
				float corner = clamp(f * 0.012f, 4.f, 400.f);
				float hpC = clamp(1.f - std::exp(-2.f * (float)M_PI * corner * tick),
				                  0.f, 1.f);
				float y = fbState[c];
				crushFbLp[c] += (y - crushFbLp[c]) * hpC;
				float x = in[c] + loopFilter(c, y - crushFbLp[c]) * fb * 1.25f;
				// soft, not clamped: a hard clip with feedback into it turns
				// every mode setting into the same full-scale square
				float s = softClip(x / kClipVolts);
				int k = (int)(std::fabs(s) * (float)levels + 0.5f);
				float q = (float)k / (float)levels;
				if (xorAmt > 0.001f) {
					// Gray-code the quantizer's own level index: the mangling
					// has to live in the bits the crusher left, or it does
					// nothing. Run on a fixed 15-bit word instead it either
					// dies -- past the middle of the knob there are two bits
					// left and XOR-ing the ones below them changes nothing the
					// quantizer keeps -- or, taken on the signed integer, it
					// flips every negative sample positive, which is where the
					// 2.2 V of DC on a 4 V sine came from. Here the sign is
					// carried outside and the index wraps inside the range, so
					// the result is odd and bounded at any depth.
					int g = (k ^ (k >> 1)) % (levels + 1);
					// blended as a level, not as an index: with eight levels
					// left, rounding the blend back to an integer makes the
					// second half of the knob a staircase of three plateaus
					q += ((float)g / (float)levels - q) * xorAmt;
				}
				q = clamp(q, 0.f, 1.f);
				crushHold[c] = (s < 0.f ? -q : q) * 5.f;
			}
			out[c] = in[c] * (1.f - wetMix) + crushHold[c] * wetMix;
		}
	}

	// ── 6. slicer ────────────────────────────────────────────────────────────
	// The rhythm table applied to amplitude: time picks the pattern, amount is
	// the decay, feedback is the chance of a step flipping.
	void doSlicer(const Ctl& ct, float* in, float* out, float t, float amt, float fb) {
		int pat = clamp((int)(t * 31.999f), 0, 31);
		uiUnit = UNIT_RHYTHM;
		uiTime = (float)(pat + 1);
		// A slice is only ever quieter than what went in, so the range stops
		// at 60 ms rather than at a click, the tail is half the stated decay
		// rather than a third of it, and short slices get some of their
		// loudness back: chopping a drone into a rhythm should not also turn
		// the volume down.
		//
		// The decay starts where the wet fade below finishes rather than at
		// zero. Stacked on top of each other, the long end of the range was
		// unreachable: the second the chopping was fully there, at a sixth of
		// the travel, the decay had already come down to 0.63 s, and the 1 s
		// the knob claims sat at the one position where the mode is dry.
		float slice = clamp((amt - 0.1f) / 0.9f, 0.f, 1.f);
		float decay = 1.f * std::pow(0.06f, slice);
		float coef = std::exp(-ct.dt / std::max(decay * 0.5f, 1e-4f));
		float wetMix = clamp(amt * 10.f, 0.f, 1.f);
		float makeup = 1.f + 0.6f * slice;

		// One coin for the step, not one per channel. Drawn inside the loop
		// below the two channels got different numbers, so feedback alone
		// pulled them apart into two different rhythms with the stereo knob
		// at zero -- and stereo is the only thing allowed to do that.
		bool flip = false;
		if (ct.stepped && fb > 0.001f)
			flip = (next() % 1000u) < (uint32_t)(fb * 700.f);

		for (int c = 0; c < 2; c++) {
			if (ct.stepped) {
				// stereo gives the two channels different rhythms
				int p = pat + (c == 1 ? (int)(ct.stereo * 8.f + 0.5f) : 0);
				bool hit = (forsitan_mod::rhythmPattern(p & 31)
				            & (uint16_t)(0x8000u >> (ct.step & 15))) != 0;
				if (flip)
					hit = !hit;
				if (hit)
					sliceAtk[c] = kSliceAttack;
			}
			if (ct.trig)
				sliceAtk[c] = kSliceAttack;
			// a couple of milliseconds of attack: an envelope that jumps
			// straight to one clicks on every step
			if (sliceAtk[c] > 0.f) {
				sliceAtk[c] -= ct.dt;
				sliceEnv[c] = std::min(1.f, sliceEnv[c] + ct.dt / kSliceAttack);
			}
			else
				sliceEnv[c] *= coef;
			float x = loopIn(c, in[c], ct, fb * 0.5f);
			float wet = x * sliceEnv[c] * makeup;
			out[c] = x * (1.f - wetMix) + wet * wetMix;
		}
	}

	// ── 7. pitcher ───────────────────────────────────────────────────────────
	// Pitch up by sweeping a delay tap with a ramp: crude on purpose, with the
	// transient duplication that comes with a single window.
	void doPitcher(const Ctl& ct, float* in, float* out, float t, float amt, float fb) {
		float window = 0.3f * std::pow(0.01f, t);
		uiUnit = UNIT_MS;
		uiTime = window * 1000.f;
		if (ct.trig)
			grainStretch = 1.f;
		grainStretch *= std::exp(-ct.dt / 0.25f);
		// Both knobs scale the tap position here, so both jump it when they
		// move. The ramp restarting is the mode's own crudeness and stays;
		// the knobs clicking on top of it was never part of that.
		window = pitchWinGl(window, ct.dt, kKnobGlide);
		window *= 1.f + 3.f * grainStretch;
		float shift = pitchAmtGl(amt, ct.dt, kKnobGlide);

		for (int c = 0; c < 2; c++) {
			float w = clamp(window * detune(c, ct.stereo), 0.002f, 0.4f);
			float x = loopIn(c, in[c], ct, fb);
			tape[c].write(x);
			if (grainW[c] <= 0.f) {
				grainW[c] = w;
				grainShift[c] = shift;
			}
			grainPhase[c] += ct.dt / grainW[c];
			// A grain reads a ramp whose length and reach are both scaled by
			// the window, so changing either mid-grain moves the tap under the
			// playhead -- a jump if it is sudden, a chirp if it is smoothed.
			// Both are taken at the boundary instead, where the crossfade
			// below has the grain at zero and nothing can be heard changing.
			// A trig quadrupling the window is exactly that kind of change.
			if (grainPhase[c] >= 1.f) {
				grainPhase[c] -= std::floor(grainPhase[c]);
				grainW[c] = w;
				grainShift[c] = shift;
			}
			// the tap walks from a window back towards now: a falling delay
			// raises the pitch, and the ramp restarting is the duplication
			float d = (1.f - grainPhase[c]) * grainW[c] * grainShift[c] * sr + 2.f;
			float wet = tape[c].read(d);
			// fade the ends of the ramp so the wrap is a click and not a bang
			float e = std::min(grainPhase[c], 1.f - grainPhase[c]) * 20.f;
			wet *= clamp(e, 0.f, 1.f);
			float heard = loopFilter(c, wet);
			out[c] = in[c] * (1.f - amt) + heard * amt * 1.4f;
		}
	}

	// ── 8. replayer ──────────────────────────────────────────────────────────
	// A tape loop. Time is the speed and the sign of it; amount decides whether
	// the tape is locked or being written over.
	//
	// There are exactly two ways this mode can make a discontinuity -- change
	// what is on the tape, or move the play head -- and each carries its own
	// fade rather than a patch bolted on after the fact. Enumerating edges and
	// ducking them one at a time does not converge: every pass over it found
	// another one that had not been thought of.
	//
	//   * A recording pass ramps in and out over a couple of milliseconds. The
	//     tape is therefore continuous where a pass began and where it ended,
	//     whatever the knob did, and the only edge a pass makes is the record
	//     head itself -- one position, known, which the play head ducks across
	//     while the pass runs and which is gone the moment it stops.
	//   * Any jump in the play head leaves a ghost reading on from where the
	//     head was, and the output crossfades from it to the new position.
	//     Nothing has to know why the head moved: the loop wrapping, a pass
	//     ending, a speed the fold lands differently on, all of it is the same
	//     event and gets the same treatment.
	void doReplayer(const Ctl& ct, float* in, float* out, float t, float amt, float fb) {
		// The tape never stops. A knob whose centre is exactly zero puts a
		// dead spot in the middle of its travel — the head holds one sample
		// and the mode outputs a DC level — so the centre is the *slowest*
		// speed instead, a quarter, and which side of it you are on is the
		// direction. Two octaves down at the centre, two up at the ends.
		float u = (t - 0.5f) * 2.f;              // -1 .. +1
		float mag = 0.25f * std::pow(8.f, std::fabs(u));
		float speed = u < 0.f ? -mag : mag;
		// A tape has a motor: the speed arrives over a few tens of
		// milliseconds rather than in one sample. Stepping it is a break in
		// the slope of the read position, audible on its own, and it lands the
		// loop's fold somewhere new at the same instant.
		speed = speedGl(speed, ct.dt, kKnobGlide);
		uiUnit = UNIT_SPEED;
		uiTime = speed;

		int n = tape[0].size();
		float fade = std::max(kEdgeFade * sr, 1.f);

		// The knob itself, slewed. It is a dry/wet mix as well as the record
		// level, and a mix that steps is a click whatever the tape is doing --
		// measured, moving amount off the lock in one go stepped the output
		// from 2.00 V to 1.00 V in a sample, which is the loudest thing the
		// mode ever did and had nothing to do with the tape at all. A knob
		// arriving from a mouse drag moves once a block; from a menu or a CV
		// it can move all at once.
		// Three one-poles, not one. A linear ramp has a corner where it stops;
		// a single pole has one where it starts, since its slope goes from
		// nothing to everything the instant the target moves. Each pole in
		// series buys one more order: two leave the slope continuous, three
		// leave the curvature continuous, and curvature is what a click is.
		float k = clamp(4.f / fade, 0.f, 1.f);
		amtSm[0] += (amt - amtSm[0]) * k;
		amtSm[1] += (amtSm[0] - amtSm[1]) * k;
		amtSm[2] += (amtSm[1] - amtSm[2]) * k;
		amt = amtSm[2];

		// A trig lays down a whole new take. Where it starts is where the tape's
		// seam will be -- the one place the loop is not continuous, because it is
		// where the end of the take meets its beginning.
		if (ct.trig) {
			fillLeft = n;
			for (int c = 0; c < 2; c++)
				tapeSeam[c] = tapeWrite[c];
		}
		bool filling = fillLeft > 0;
		if (filling)
			fillLeft--;

		// The knob drives the record level and what survives is derived from it.
		// Both orders hold the loop at unity, but deriving the record level
		// instead makes its slope at the top of the travel vertical: a hair off
		// fully locked would already be recording at -17 dB, loud enough to hear
		// the input arrive on a loop you thought was held. This way the same
		// position records at -40 dB, and the last fiftieth locks outright.
		float recTarget = filling ? 1.f : (1.f - amt);
		if (!filling && recTarget < 0.02f)
			recTarget = 0.f;
		// The record level, slewed like the knob and for the same reason --
		// with the play head sitting on the record head at 1x it hears what
		// is being written directly, so a corner in the record level is a
		// corner in the output, and a fill ending steps its target from one
		// back to whatever the knob says, once a lap after every trig.
		//
		// Slewed as an angle, not as a level. The overdub law below keeps
		// keep = sqrt(1 - rec^2) at the fill's level, and that square root
		// has a vertical tangent at rec = 1: however smoothly rec leaves the
		// top, keep departs zero at unbounded slope -- measured 14 mV/V in
		// the first sample of every fill end, written straight into the tape
		// at the seam and read back once a lap from then on. On the circle
		// there is no cliff: rec = sin(th) arrives at one with zero slope
		// and keep = cos(th) leaves zero with zero slope, so a smooth angle
		// makes both gains smooth to every order.
		float thTarget = std::asin(clamp(recTarget, 0.f, 1.f));
		if (recTarget > 0.f) {
			recHold[0] += (thTarget - recHold[0]) * k;
			recHold[1] += (recHold[0] - recHold[1]) * k;
			recHold[2] += (recHold[1] - recHold[2]) * k;
		}
		passFade += clamp((recTarget > 0.f ? 1.f : 0.f) - passFade,
		                  -1.f / fade, 1.f / fade);
		// Eased. The ramp has to reach nought and one exactly, so it is
		// linear underneath, but a linear ramp starts and stops with a corner
		// -- and a corner in what is being written is a corner in the tape,
		// sitting there to be crossed once a lap for as long as it lasts. It
		// measured 60 dB down, which is quiet and completely audible on a
		// sustained tone.
		float pass = passFade * passFade * (3.f - 2.f * passFade);
		float sinTh = std::sin(recHold[2]);
		float cosTh = std::cos(recHold[2]);
		float rec = sinTh * pass;
		// 1 - rec^2 with no subtraction anywhere near 1: as the angle closes
		// on ninety degrees sin rounds to 1.0f in steps of one ulp, and a
		// keep built on 1 - rec^2 descends its last decade in sqrt-of-ulp
		// chunks -- a 3e-4 stair once a fill, right at the seam. cos^2 of
		// the same angle is the same number computed where float has plenty
		// of room.
		float oneMinusRec2 = cosTh * cosTh
		                     + sinTh * sinTh * (1.f - pass) * (1.f + pass);
		bool recording = passFade > 0.f;


		// The loop's length is chosen when a pass ends, by looking at the tape:
		// a fixed shortening picks a length with no relation to what is on it,
		// and the phase it lands on is then whatever it is.
		// A pass ending is what puts the seam on the tape, so that is when
		// the seam's position is taken -- not only at a trig, as it was.
		// The edge is a boundary in when the tape was last written, and it
		// sits wherever the record head stopped: everything behind it has
		// the pass on it, everything in front does not. A trig is only one
		// way to make one; the amount knob makes one every time it crosses
		// the lock.
		//
		// Leaving it at the last trig meant the crossfade was laid over a
		// place with no edge while the real edge was crossed raw, once a
		// lap, for as long as the loop was held. Measured with a punch-in
		// over different material, the loop wrapped 2400 slots away from
		// where the tape actually broke, and the break read -63 dB against
		// a -80 dB floor.
		if (wasRec && !recording) {
			for (int c = 0; c < 2; c++)
				tapeSeam[c] = tapeWrite[c];
			loopLen = chooseLoop(n, fade);
		}
		else if (!recording && loopLen <= 0.0)
			loopLen = chooseLoop(n, fade);
		wasRec = recording;

		float corrRate = 1.f - std::exp(-ct.dt / 0.100f);

		for (int c = 0; c < 2; c++) {
			float sp = speed * detune(c, ct.stereo);
			float x = in[c] + loopFilter(c, fbState[c]) * fb * 0.9f;

			// ── the record head, in real time whatever the play head is doing.
			// Sharing one moving position between them cancels the speed exactly:
			// material laid down at a quarter speed and read back at a quarter
			// speed is unity at every setting of the knob.
			int w = (int)tapeWrite[c];
			float old = tape[c].at((float)w);
			corrXY[c] += (old * x - corrXY[c]) * corrRate;
			corrXX[c] += (old * old - corrXX[c]) * corrRate;
			corrYY[c] += (x * x - corrYY[c]) * corrRate;
			// What survives depends on whether the tape and the input agree.
			// Solving for a loop that settles at the level that went in gives
			// keep^2 + 2*rho*rec*keep + rec^2 - 1 = 0, whose root is
			// sqrt(1 - rec^2) when they do not -- which holds real material at
			// unity -- and 1 - rec when they do, which is the only thing that
			// stops a drone from stacking on itself. At rec of one it is zero,
			// so a fill replaces outright without needing to be a special case.
			// The correlation crosses zero all the time -- it ripples at
			// signal rate around whatever its mean is -- and a hard clamp
			// there is a corner: keep's slope breaks every time it lands,
			// and while recording the break is written into the tape
			// (measured 4e-4 at the end of every fill, replayed once a lap).
			// The smooth positive part has no corner anywhere; at zero it
			// reads one hundredth instead of nothing, which moves the
			// overdub level by less than that.
			float r = corrXY[c] / (std::sqrt(corrXX[c] * corrYY[c]) + 1e-9f);
			float rho = std::min(0.5f * (r + std::sqrt(r * r + 0.0004f)), 1.f);
			// The discriminant written as rho^2 rec^2 - rec^2 + 1 cancels
			// catastrophically at rec = 1: the true value is rho^2, around
			// 1e-6, but it is reached by adding 1 to a number a hair below
			// -1, and float32 near 1 moves in steps of 6e-8. keep came out
			// with a few percent of noise on it, a new value every sample --
			// times the tape that was a -74 dB crackle written into every
			// fill. oneMinusRec2 above carries the difference already
			// computed where nothing cancels.
			float disc = oneMinusRec2 + rho * rho * rec * rec;
			float keep = clamp(-rho * rec + std::sqrt(std::max(0.f, disc)),
			                   0.f, 1.f);
			if (recording) {
				int m = (int)shadow[c].size();
				if (m > 0)
					shadow[c][((w % m) + m) % m] = old;
				tape[c].poke(w, softClip((old * keep + x * rec) / kClipVolts)
				                * kClipVolts);
			}
			tapeWrite[c] += 1.0;
			if (tapeWrite[c] >= n)
				tapeWrite[c] -= n;

			// The record head leaves one edge in the tape, and it is an edge
			// of *generation*: the slot it has just written carries one more
			// pass of overdub than the slot in front of it, and the two
			// differ by exactly what a pass adds. The play head crosses that
			// step once a lap at any speed but one, and sits on it forever at
			// one.
			//
			// Substituting the head's own live signal there was wrong, and
			// wrong in a way only a speed other than 1x shows: the head
			// advances one slot per sample whatever the play head is doing,
			// so its signal carries the input's pitch, not the tape's. At 2x
			// that spliced an octave-down fragment in for the length of the
			// window -- the output's slope halved, once a lap. At 1x the two
			// rates coincide, which is why it measured clean there.
			//
			// What is wanted is the same tape position one generation older,
			// which is a read at the play head's own position and therefore
			// at the play head's own pitch. `shadow` keeps the last window's
			// worth of pre-write values for exactly that: approaching the
			// head from behind, the reading fades from the tape to its own
			// previous generation, so it meets the older material waiting on
			// the far side of the edge with nothing left to step over.
			//
			// The window is measured in tape but heard in time. The heads
			// close at |speed - 1| slots per sample, so a window of that
			// times the splice length is always crossed in one splice of
			// wall time, at every speed, sampled the same number of times.
			//
			// It therefore vanishes at exactly 1x -- and that is right, not
			// the flaw the old duck had there. At 1x the heads keep station
			// and the play head never crosses the edge at all: whichever
			// side it is on, it stays, and the honest reading is the one the
			// tape holds. Forcing a window open there instead made a play
			// head parked just behind the record head present the generation
			// before the one it is sitting on, for good -- which at 1x is an
			// overdub you can never hear.
			float win = kEdgeFade * sr * std::fabs(sp - 1.f);
			int shadowLen = (int)shadow[c].size();
			// The tape as it would read if this lap's overdub had not
			// happened yet: every slot the head has already rewritten put
			// back to the value it held before. Reconstructed slot by slot
			// rather than read from a parallel buffer, because a fractional
			// read straddling the head takes one sample from each side --
			// substituting only the whole-sample value left a one-sample
			// spike at the crossing.
			auto older = [&](double pos) -> float {
				double f = std::floor(pos);
				int i0 = (int)(((long long)f % n + n) % n);
				int i1 = i0 + 1 < n ? i0 + 1 : 0;
				float fr = (float)(pos - f);
				auto pick = [&](int slot) {
					int ds = slot - w;
					ds -= (int)std::floor((double)ds / n + 0.5) * n;
					if (ds <= 0 && ds > 2 - shadowLen)
						return shadow[c][((slot % shadowLen) + shadowLen)
						                 % shadowLen];
					return tape[c].buf[slot];
				};
				float a = pick(i0);
				return a + (pick(i1) - a) * fr;
			};
			auto readTape = [&](double pos) -> float {
				float raw = tape[c].at(pos);   // double: see Delay::at
				if (!recording || shadowLen < 4 || win <= 0.f)
					return raw;
				// Signed distance from the head. A slot in front of it still
				// holds the older generation and needs nothing; one behind
				// it has been rewritten this lap and is a generation newer
				// than its neighbour across the edge.
				double d = pos - (double)w;
				d -= std::floor(d / (double)n + 0.5) * (double)n;
				if (d >= 1.0 || d <= -(double)win)
					return raw;
				// One at the far edge of the window, nought at the head --
				// and held at nought across the straddling sample, which
				// costs no corner because the ease is flat there anyway.
				float u = clamp((float)(-d) / win, 0.f, 1.f);
				u = u * u * (3.f - 2.f * u);
				float before = older(pos);
				return before + (raw - before) * u;
			};

			// ── the play head, which the speed alone moves.
			// One reduction, not two: bringing it into the tape and then
			// folding it into the loop fight each other, and the position
			// swings by the difference between them every sample -- which
			// reads as a jump every sample, so the ghost below re-arms before
			// it can fade and the crossfade never finishes.
			double p = tapePos[c];
			double before = p;
			if (!recording) {
				// locked, it runs a loop a little shorter than the tape so the
				// crossfade below has material to fade into
				double loop = loopLen > 0.0 ? loopLen : (double)n - fade;
				double tau = p - tapeSeam[c];
				tau -= std::floor(tau / loop) * loop;
				p = tapeSeam[c] + tau;
			}
			else
				p -= std::floor(p / (double)n) * (double)n;
			// a jump is any move a whole number of laps cannot explain
			double moved = p - before;
			moved -= std::floor(moved / (double)n + 0.5) * (double)n;
			if (std::fabs(moved) > 0.5) {
				ghostPos[c] = before;
				ghostLeft[c] = (int)fade;
				joinXY[c] = joinXX[c] = joinYY[c] = 0.f;
			}
			tapePos[c] = p;

			// Applied per reading, not to the mix: during a crossfade the two
			// taps are in different places, and only one of them may be near
			// the record head.
			float wet = readTape(p);
			if (ghostLeft[c] >= 0) {
				// Weighted by how alike the two readings are. "Equal power" only
				// conserves power for signals that are unrelated; two that match
				// add in amplitude instead, and sin against cos sums them to
				// +3 dB. Dividing by sqrt(1 + rho*sin(2*theta)) is equal power at
				// a correlation of zero and sums to one at a correlation of one.
				//
				// rho is a running measurement of the two readings themselves,
				// not one number for the whole fade: across a splice-length
				// crossfade the relationship changes -- at a loop fold the two
				// taps start out identical, since the splice begins as the
				// tail's own continuation, and drift apart as it glides toward
				// the head -- and a single average is wrong at both ends. The
				// measurement assumes nothing about the material. It converges
				// in an eighth of the fade, and starting it at zero is safe
				// because the correction only has leverage mid-fade, where
				// sin and cos are comparable; at the edges g is one whatever
				// rho says.
				//
				// Counted in samples so that it lands on both ends exactly, and
				// eased: sin and cos are a fine pair of weights but cos reaches
				// zero with a slope of -1, so a fade driven by a linear ramp
				// stops with a corner in it.
				float u = (float)ghostLeft[c] / fade;        // 1 down to 0
				float wgt = u * u * (3.f - 2.f * u);
				float bs = readTape(ghostPos[c]);
				float jr = clamp(8.f / fade, 0.f, 1.f);
				joinXY[c] += (wet * bs - joinXY[c]) * jr;
				joinXX[c] += (wet * wet - joinXX[c]) * jr;
				joinYY[c] += (bs * bs - joinYY[c]) * jr;
				float rj = joinXY[c]
				           / (std::sqrt(joinXX[c] * joinYY[c]) + 1e-9f);
				// the same smooth positive part as the overdub law, and for
				// the same reason: a hard clamp is a corner
				float rhoJ = std::min(0.5f * (rj + std::sqrt(rj * rj + 0.0004f)),
				                      1.f);
				float th = 0.5f * (float)M_PI * (1.f - wgt);
				float sa = std::sin(th), cb = std::cos(th);
				float g = 1.f / std::sqrt(1.f + rhoJ * 2.f * sa * cb);
				wet = (wet * sa + bs * cb) * g;
				ghostPos[c] += sp;
				ghostLeft[c]--;
			}
			tapePos[c] += sp;
			out[c] = in[c] * (1.f - amt) + wet * amt;
		}
	}

	// ── 9. shifter ───────────────────────────────────────────────────────────
	// Two taps crossfaded, so there is no stutter — and with feedback a small
	// shift walks the tail away in pitch, one interval per pass.
	void doShifter(const Ctl& ct, float* in, float* out, float t, float amt, float fb) {
		float semis = (t - 0.5f) * 24.f;         // an octave either way
		uiUnit = UNIT_SEMI;
		uiTime = semis;
		// "Resync" means putting the two channels back into their half-a-window
		// relationship, not putting both at a fixed phase -- the second jumps
		// the left channel's read position for no reason, and at phase zero its
		// near tap is crossfaded out entirely, so the jump is to whatever the
		// far tap happens to be holding. Squaring the right one up to the left
		// leaves the left untouched and moves the right only by however far it
		// had actually drifted.
		if (ct.trig) {
			shiftPhase[1] = shiftPhase[0] + 0.5f;
			shiftPhase[1] -= std::floor(shiftPhase[1]);
		}
		float window = 0.08f;                    // seconds of crossfade window

		for (int c = 0; c < 2; c++) {
			float s = semis + (c == 1 ? ct.stereo * 3.f : -ct.stereo * 3.f);
			float ratio = std::pow(2.f, s / 12.f);
			float x = loopIn(c, in[c], ct, fb);
			shf[c].write(x);
			// The pitch a moving tap gives is 1 - d'(t), so the ramp has to
			// run at (1 - ratio) / window: a delay that shortens raises the
			// pitch. Two taps half a window apart, crossfaded, hide the ramp
			// resetting.
			float rate = (1.f - ratio) / window;
			shiftPhase[c] += rate * ct.dt;
			shiftPhase[c] -= std::floor(shiftPhase[c]);
			float wet = 0.f;
			for (int k = 0; k < 2; k++) {
				float ph = shiftPhase[c] + 0.5f * k;
				ph -= std::floor(ph);
				float d = ph * window * sr + 2.f;
				float g = 0.5f - 0.5f * std::cos(2.f * (float)M_PI * ph);
				wet += shf[c].read(d) * g;
			}
			float heard = loopFilter(c, wet);
			out[c] = in[c] * (1.f - amt) + heard * amt;
		}
	}
};

}   // namespace artifex_fx
