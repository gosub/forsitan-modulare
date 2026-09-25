#pragma once
#include <rack.hpp>

#include <cmath>
#include <cstdint>

// The modulation section vates and artifex share: a tempo generator, a
// sixteen-step pattern generator with its two editing switches, and an LFO
// that can phase-lock to the clock.
//
// The two modules come from the two sides of one piece of hardware, which is
// one board running one firmware - so this half of them really is the same
// machine. Everything here is inline, so both translation units can include
// it.

namespace forsitan_mod {

using namespace rack;

static const int kSteps = 16;
// Width of the clock output pulse, and the window inside which a free-run
// step and an arriving external edge are the same beat rather than two.
static const float kClockGuard = 1e-3f;

// ── the rhythm table ──────────────────────────────────────────────────────────
// 32 sixteen-step gate patterns, step 0 in the high bit. Twenty-two written by
// hand - the ones a drummer would recognise - then the euclidean distributions
// E(k, 16), which fill in the densities between them. The hardware loads its
// rhythms from a web app that rebuilds the firmware; this is the knob that
// replaces it.
//
// The euclidean half skips six densities, because at 16 steps those *are* the
// hand-written patterns: E(1) is the downbeat alone, E(2) half notes, E(4)
// four on the floor, E(6) tresillo, E(8) eighths, E(16) sixteenths. Running
// the series straight from 1 to 16 spent six of the knob's thirty-two
// positions repeating patterns it already had, note for note.
inline uint16_t rhythmPattern(int i) {
	static const uint16_t hand[22] = {
		0x8888,   // four on the floor
		0x0808,   // backbeat
		0x2222,   // offbeat eighths
		0xAAAA,   // eighths
		0xFFFF,   // sixteenths
		0x9292,   // tresillo
		0x9228,   // son clave
		0x9128,   // rumba clave
		0x9224,   // bossa
		0xA94A,   // cascara
		0x9999,   // shuffle
		0xB2B2,   // gallop
		0x8000,   // downbeat only
		0x8080,   // half notes
		0x9249,   // dotted eighths
		0x9632,   // syncopated
		0x8100,   // charleston
		0x9222,   // bo diddley
		0x9294,   // dembow
		0x922A,   // amen
		0x2A2A,   // montuno
		0xB4B4,   // funk
	};
	// the onset counts the hand-written half does not already contain
	static const int density[10] = {3, 5, 7, 9, 10, 11, 12, 13, 14, 15};
	i = clamp(i, 0, 31);
	if (i < 22)
		return hand[i];
	int k = density[i - 22];
	uint16_t p = 0;
	for (int s = 0; s < kSteps; s++)
		if ((s * k) % kSteps < k)
			p |= (uint16_t)(0x8000u >> s);
	return p;
}

// ── the CV sequence ───────────────────────────────────────────────────────────
// A rungler, as the hardware's is. One *bit* per step, and the output is a
// three-bit word gathered from the bits at the current step, three on and five
// on - Sequencer::UpdateCvOutput - indexed into eight unevenly spaced levels.
//
// The shape of it is the point. Sixteen independent levels, which is what this
// was, make the CV switch a per-step edit: invert one step and one step of the
// output changes. Here a single bit is read by three different steps, so
// flipping it moves the output at three places in the bar, and the sequence
// folds back on itself as it evolves instead of wandering. That is the whole
// character of the circuit, and it is why the switch is worth having.
static const uint32_t kRunglerLevels[8] = {0, 320, 480, 600, 720, 800, 880, 1020};

inline float runglerVolts(int i) {
	return (float)kRunglerLevels[i & 7] / 1023.f * 10.f;
}

// The bits a rhythm starts from. The hardware seeds them at random once and
// they evolve from there; seeding from the rhythm index instead keeps the one
// thing our own version bought - that a rhythm brings its own contour back
// with it - without giving up the rungler's dynamics.
// ── the 2.16.2 behaviour ──────────────────────────────────────────────────────
// Both modules keep the pattern CV and the synced LFO divisions they had before
// 2.16.3 behind a context-menu setting, so a patch saved then still sounds as
// it did: the rungler above gives a different sequence from the same rhythm,
// and the fifteen divisions below put a different one under the same knob. A
// patch that predates the setting opens with it on; a new module has it off.

// The CV sequence as it was: sixteen stepped levels, hashed from the pattern
// index so a rhythm always brings the same contour.
inline float legacyRhythmCv(int pat, int step) {
	uint32_t h = (uint32_t)(pat + 1) * 2654435761u ^ (uint32_t)(step + 1) * 2246822519u;
	h ^= h >> 13;
	h *= 2654435761u;
	h ^= h >> 16;
	return (h % 16u) / 15.f * 10.f;
}

// The synced LFO's eight divisions as they were, in steps per cycle.
static const float kLegacyLfoDiv[8] = {32.f, 16.f, 8.f, 4.f, 2.f, 1.f, 0.5f, 0.25f};

inline uint16_t rhythmCvBits(int pat) {
	uint32_t h = (uint32_t)(pat + 1) * 2654435761u;
	h ^= h >> 13;
	h *= 2246822519u;
	h ^= h >> 16;
	return (uint16_t)(h & 0xFFFFu);
}

// Which of the 32 rhythms a knob and a CV input select together: ten volts
// covers the whole table and wraps, the same rule the module's other list
// knobs follow.
inline int rhythmSelect(int base, float cv, float att) {
	int i = base + (int)std::floor(cv * 0.1f * att * (32.f - 1e-3f));
	i %= 32;
	if (i < 0)
		i += 32;
	return i;
}

// The synced LFO's divisions, in steps per cycle, slowest first. This is
// kBaseLfoRatios read the other way up: fifteen divisions from 256 steps a
// cycle out to four cycles a step, thirds included. Eight of them reaching only
// 32 steps was two bars at the slow end where the hardware gives sixteen, and a
// modulation source that takes sixteen bars to come round is a different
// instrument from one that takes two.
// How many steps the running counter the synced LFO derives its phase from
// takes to come round. Every division has to divide it exactly, or the LFO
// jumps when the counter wraps - and two modules on one cable, having wrapped
// at different moments, then disagree for good. That is 48 bars: the LCM of
// 256, the slowest division, and 12, the coarsest of the thirds. It was two
// bars, which was enough while every division was a power of two.
static const int kBarCycle = 48 * kSteps;

// Steps per cycle, as exact rationals. A float cannot hold a third, and the
// phase is a division by this: stored as 0.33333334 the counter's 768 steps
// come to 2303.9998 cycles rather than 2304, so the LFO steps a fraction of a
// cycle every time the counter wraps. Kept as a pair it divides exactly, and
// every entry lands on a whole number of cycles at kBarCycle.
static const int kLfoDivCount = 15;
static const int kLfoDivNum[kLfoDivCount] = {
	256, 128, 64, 32, 16, 12, 8, 6, 4, 3, 2, 1, 1, 1, 1};
static const int kLfoDivDen[kLfoDivCount] = {
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 3, 4};

// The same as a number, for a display or a test.
inline float lfoDivision(int i) {
	i = clamp(i, 0, kLfoDivCount - 1);
	return (float)kLfoDivNum[i] / (float)kLfoDivDen[i];
}

// The rate knob position that selects one of them, for tests and presets: the
// middle of that division's band, so it does not sit on a boundary.
inline float lfoDivisionKnob(int i) {
	return ((float)clamp(i, 0, kLfoDivCount - 1) + 0.5f) / (float)kLfoDivCount;
}

// ── the section ───────────────────────────────────────────────────────────────

// Everything the host module has already read from its own panel. The
// section itself owns no params: it is fed plain numbers, which is what
// makes it testable without a Rack engine behind it.
struct ModIn {
	float dt = 1.f / 44100.f;
	float bpm = 120.f;
	float clkVoltage = 0.f;
	bool honourExternal = true;
	float patResetVoltage = 0.f;
	int rhythm = 0;               // 0..31
	int gateMode = 1;             // 0 invert, 1 leave alone, 2 randomize
	int cvMode = 1;
	float lfoRateKnob = 0.5f;     // 0..1
	float lfoRateMod = 0.f;       // -1..1, already attenuverted
	bool lfoSynced = true;
	float lfoResetVoltage = 0.f;
	float pulseWidth = 0.5f;      // 0..1, the fraction of the cycle spent rising
	bool legacy = false;          // the 2.16.2 pattern CV and LFO divisions
};

struct Modulation {
	// ── clock ────────────────────────────────────────────────────────────────
	double clockPhase = 0.0;              // 0..1 within a step
	float stepSeconds = 0.125f;
	// The tempo, as opposed to the step. An external clock's stepSeconds is
	// the last interval it measured, which under swing alternates long and
	// short on every step; anything timed from it - a delay, a loop length -
	// moved on every step and clicked on every one (#22). This is the mean of
	// the last eight measured intervals, which a swing of two or four steps
	// averages out exactly. With the internal clock it is stepSeconds.
	float tempoSeconds = 0.125f;
	static constexpr int kTempoHist = 8;
	float tempoHist[kTempoHist] = {};
	int tempoHistN = 0, tempoHistI = 0;
	float sinceExternal = 10.f;
	bool externalClock = false;
	int step = 0;
	// Position in steps, running and fractional: the synced LFO derives its
	// phase from this rather than free-running at a synced rate, so its saw
	// output really is the place in the bar and stays there.
	int barStep = 0;          // 0..kBarCycle-1, see the note on kBarCycle
	double barPos = 0.0;

	// ── pattern ──────────────────────────────────────────────────────────────
	uint16_t gateWork = 0;
	uint16_t cvBits = 0;      // one bit a step, read three at a time
	float cvLegacy[kSteps] = {0.f};   // the 2.16.2 sequence, one level a step
	bool legacy = false;      // which of the two cv() reads, and which LFO table
	int loadedRhythm = -1;
	uint32_t patRng = 0x1234567u;
	float gateTimer = 0.f;
	float clkPulse = 0.f;
	float sinceStep = 10.f;   // seconds since the last step, from either clock

	// ── lfo ──────────────────────────────────────────────────────────────────
	double lfoOffset = 0.0;
	float lfoPhase = 0.f;
	float tri = 0.f;
	bool lfoRising = false;

	// ── this block's results ─────────────────────────────────────────────────
	bool stepped = false;

	dsp::SchmittTrigger clkIn, lfoResetIn, patResetIn;

	bool gate() const { return gateTimer > 0.f; }
	bool clock() const { return clkPulse > 0.f; }
	bool cvBitAt(int s) const {
		return (cvBits & (uint16_t)(0x8000u >> (s & 15))) != 0;
	}
	float cv() const {
		int s = clamp(step, 0, kSteps - 1);
		if (legacy)
			return cvLegacy[s];
		int i = (cvBitAt(s) ? 1 : 0) | (cvBitAt(s + 3) ? 2 : 0)
		        | (cvBitAt(s + 5) ? 4 : 0);
		return runglerVolts(i);
	}
	// the whole rhythm as it stands, for a display or for a slicer reading it
	uint16_t pattern() const { return gateWork; }
	bool gateAt(int s) const { return (gateWork & (uint16_t)(0x8000u >> (s & 15))) != 0; }

	void resetSequence() {
		step = 0;
		barStep = 0;
		clockPhase = 0.0;
		lfoOffset = 0.0;
	}

	uint32_t nextRandom() {
		patRng ^= patRng << 13;
		patRng ^= patRng >> 17;
		patRng ^= patRng << 5;
		return patRng;
	}

	void process(const ModIn& in) {
		// ── clock ────────────────────────────────────────────────────────────
		stepped = false;
		sinceExternal += in.dt;
		sinceStep += in.dt;
		bool extEdge = clkIn.process(in.clkVoltage, 0.1f, 1.f);
		if (extEdge && in.honourExternal) {
			if (externalClock && sinceExternal > 1e-4f && sinceExternal < 4.f) {
				stepSeconds = sinceExternal;
				tempoHist[tempoHistI] = stepSeconds;
				tempoHistI = (tempoHistI + 1) % kTempoHist;
				tempoHistN = std::min(tempoHistN + 1, kTempoHist);
			}
			else if (!externalClock)
				tempoHistN = tempoHistI = 0;   // adopting a clock: nothing measured yet
			externalClock = true;
			sinceExternal = 0.f;
			clockPhase = 0.0;
			// A module still free-running at the same tempo as the one about
			// to clock it wraps its own phase a sample or two before that
			// clock arrives down the cable. Stepping again here would advance
			// it twice for the one beat and leave it a step ahead of the
			// leader for good, which is the opposite of what sharing a clock
			// is for. Adopt the external clock, but only step if this is not
			// the step we just took. The guard is the width of our own clock
			// pulse: a clock faster than that has no distinguishable output
			// anyway.
			if (sinceStep > kClockGuard)
				stepped = true;
		}
		if (externalClock && sinceExternal > 2.f)
			externalClock = false;   // the external clock stopped; take over again
		if (!externalClock) {
			stepSeconds = 60.f / std::max(in.bpm, 1.f) / 4.f;   // sixteenths
			clockPhase += in.dt / stepSeconds;
			if (clockPhase >= 1.0) {
				clockPhase -= 1.0;
				stepped = true;
			}
		}
		else
			clockPhase = std::min(1.0, clockPhase + in.dt / std::max(stepSeconds, 1e-4f));
		if (externalClock && tempoHistN > 0) {
			float sum = 0.f;
			for (int i = 0; i < tempoHistN; i++)
				sum += tempoHist[i];
			tempoSeconds = sum / tempoHistN;
		}
		else
			tempoSeconds = stepSeconds;

		if (patResetIn.process(in.patResetVoltage, 0.1f, 1.f))
			resetSequence();

		// ── pattern generator ────────────────────────────────────────────────
		if (in.rhythm != loadedRhythm) {
			loadedRhythm = in.rhythm;
			gateWork = rhythmPattern(in.rhythm);
			cvBits = rhythmCvBits(in.rhythm);
			for (int s = 0; s < kSteps; s++)
				cvLegacy[s] = legacyRhythmCv(in.rhythm, s);
		}
		legacy = in.legacy;
		if (stepped) {
			step = (step + 1) % kSteps;
			barStep = (barStep + 1) % kBarCycle;

			// the two switches, each normalled to its own input: middle
			// leaves the sequence alone, up randomizes the step the sequence
			// is on, down inverts it - and both write into the working copy,
			// so a flick changes the pattern for good
			uint16_t mask = (uint16_t)(0x8000u >> step);
			if (in.gateMode == 2) {
				if (nextRandom() & 1)
					gateWork |= mask;
				else
					gateWork &= (uint16_t)~mask;
			}
			else if (in.gateMode == 0)
				gateWork ^= mask;
			// One bit, at the step the sequence is on - and three steps of
			// the output move with it, which is the rungler working.
			//
			// Both sequences are edited, whichever is being read, so that
			// flipping the setting mid-patch finds the other one where the
			// switches have taken it too.
			if (in.cvMode == 2) {
				if (nextRandom() & 1)
					cvBits |= mask;
				else
					cvBits &= (uint16_t)~mask;
				cvLegacy[step] = (nextRandom() % 16u) / 15.f * 10.f;
			}
			else if (in.cvMode == 0) {
				cvBits ^= mask;
				cvLegacy[step] = 10.f - cvLegacy[step];
			}

			if (gateWork & mask)
				gateTimer = 0.75f * stepSeconds;
			clkPulse = kClockGuard;
		}
		if (stepped)
			sinceStep = 0.f;
		gateTimer = std::max(0.f, gateTimer - in.dt);
		clkPulse = std::max(0.f, clkPulse - in.dt);
		barPos = (double)barStep + clockPhase;

		// ── LFO ──────────────────────────────────────────────────────────────
		bool lfoReset = lfoResetIn.process(in.lfoResetVoltage, 0.1f, 1.f);
		if (in.lfoSynced) {
			// Steps per cycle, slowest first: clockwise has to speed the LFO up
			// here exactly as it does in free mode, or the knob reverses its
			// meaning as the switch flips. Sixteen steps is one pattern - one
			// bar - so at that division the saw output is the bar position.
			//
			// Synced means phase-locked, not merely a synced rate: the phase is
			// derived from the step clock, so the LFO cannot drift against the
			// pattern and its saw stays a usable phasor.
			// modulation moves the division rather than detuning the rate: a
			// phase derived from the clock has nothing to detune
			if (lfoReset)
				lfoOffset = barPos;
			double phase;
			if (in.legacy) {
				// kBarCycle is a multiple of 32 steps, so these wrap cleanly too
				int d = clamp((int)(in.lfoRateKnob * 7.999f + in.lfoRateMod * 4.f), 0, 7);
				phase = (barPos - lfoOffset) / kLegacyLfoDiv[d];
			}
			else {
				int d = clamp((int)(in.lfoRateKnob * (kLfoDivCount - 0.001f)
				                    + in.lfoRateMod * (kLfoDivCount / 2)),
				              0, kLfoDivCount - 1);
				phase = (barPos - lfoOffset) * (double)kLfoDivDen[d]
				        / (double)kLfoDivNum[d];
			}
			phase -= std::floor(phase);
			lfoPhase = (float)phase;
		}
		else {
			float lfoHz = 0.01f * std::pow(2000.f, clamp(in.lfoRateKnob, 0.f, 1.f));
			lfoHz *= std::pow(4.f, clamp(in.lfoRateMod, -1.f, 1.f));
			lfoHz = clamp(lfoHz, 0.002f, 400.f);
			if (lfoReset)
				lfoPhase = 0.f;
			lfoPhase += lfoHz * in.dt;
			lfoPhase -= std::floor(lfoPhase);
		}
		// Peak at phase 0, falling to the trough, rising back after - and
		// pulse is high exactly while the triangle rises, so the width knob
		// skews the triangle and the pulse follows it. That is the same
		// relationship the hardware gets by patching its pulse output back
		// into its own rate input; here it is a control. At the default half
		// it is the plain symmetric triangle with a square beside it.
		float w = clamp(in.pulseWidth, 0.02f, 0.98f);
		float fall = 1.f - w;
		if (lfoPhase < fall) {
			tri = 1.f - lfoPhase / fall;
			lfoRising = false;
		}
		else {
			tri = (lfoPhase - fall) / w;
			lfoRising = true;
		}
	}
};

}   // namespace forsitan_mod
