#pragma once
#include <rack.hpp>

#include <cmath>
#include <cstdint>

// The modulation section vates and artifex share: a tempo generator, a
// sixteen-step pattern generator with its two editing switches, and an LFO
// that can phase-lock to the clock.
//
// The two modules come from the two sides of one piece of hardware, which is
// one board running one firmware — so this half of them really is the same
// machine. Everything here is inline, so both translation units can include
// it.

namespace forsitan_mod {

using namespace rack;

static const int kSteps = 16;

// ── the rhythm table ──────────────────────────────────────────────────────────
// 32 sixteen-step gate patterns, step 0 in the high bit. Sixteen written by
// hand — the ones a drummer would recognise — then the sixteen euclidean
// distributions E(1..16, 16), which fill in every density between them. The
// hardware loads its rhythms from a web app that rebuilds the firmware; this
// is the knob that replaces it.
inline uint16_t rhythmPattern(int i) {
	static const uint16_t classic[16] = {
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
	};
	i = clamp(i, 0, 31);
	if (i < 16)
		return classic[i];
	int k = i - 15;   // 1..16 onsets
	uint16_t p = 0;
	for (int s = 0; s < kSteps; s++)
		if ((s * k) % kSteps < k)
			p |= (uint16_t)(0x8000u >> s);
	return p;
}

// The CV sequence that goes with a rhythm: sixteen stepped levels, hashed
// from the pattern index so a rhythm always brings the same contour.
inline float rhythmCv(int pat, int step) {
	uint32_t h = (uint32_t)(pat + 1) * 2654435761u ^ (uint32_t)(step + 1) * 2246822519u;
	h ^= h >> 13;
	h *= 2654435761u;
	h ^= h >> 16;
	return (h % 16u) / 15.f * 10.f;
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
};

struct Modulation {
	// ── clock ────────────────────────────────────────────────────────────────
	double clockPhase = 0.0;              // 0..1 within a step
	float stepSeconds = 0.125f;
	float sinceExternal = 10.f;
	bool externalClock = false;
	int step = 0;
	// Position in steps, running and fractional: the synced LFO derives its
	// phase from this rather than free-running at a synced rate, so its saw
	// output really is the place in the bar and stays there.
	int barStep = 0;          // 0..31: two bars, so every division divides it
	double barPos = 0.0;

	// ── pattern ──────────────────────────────────────────────────────────────
	uint16_t gateWork = 0;
	float cvWork[kSteps] = {0.f};
	int loadedRhythm = -1;
	uint32_t patRng = 0x1234567u;
	float gateTimer = 0.f;
	float clkPulse = 0.f;

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
	float cv() const { return cvWork[clamp(step, 0, kSteps - 1)]; }
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
		bool extEdge = clkIn.process(in.clkVoltage, 0.1f, 1.f);
		if (extEdge && in.honourExternal) {
			if (externalClock && sinceExternal > 1e-4f && sinceExternal < 4.f)
				stepSeconds = sinceExternal;
			externalClock = true;
			sinceExternal = 0.f;
			stepped = true;
			clockPhase = 0.0;
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

		if (patResetIn.process(in.patResetVoltage, 0.1f, 1.f))
			resetSequence();

		// ── pattern generator ────────────────────────────────────────────────
		if (in.rhythm != loadedRhythm) {
			loadedRhythm = in.rhythm;
			gateWork = rhythmPattern(in.rhythm);
			for (int s = 0; s < kSteps; s++)
				cvWork[s] = rhythmCv(in.rhythm, s);
		}
		if (stepped) {
			step = (step + 1) % kSteps;
			barStep = (barStep + 1) % (2 * kSteps);

			// the two switches, each normalled to its own input: middle
			// leaves the sequence alone, up randomizes the step the sequence
			// is on, down inverts it — and both write into the working copy,
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
			if (in.cvMode == 2)
				cvWork[step] = (nextRandom() % 16u) / 15.f * 10.f;
			else if (in.cvMode == 0)
				cvWork[step] = 10.f - cvWork[step];

			if (gateWork & mask)
				gateTimer = 0.75f * stepSeconds;
			clkPulse = 1e-3f;
		}
		gateTimer = std::max(0.f, gateTimer - in.dt);
		clkPulse = std::max(0.f, clkPulse - in.dt);
		barPos = (double)barStep + clockPhase;

		// ── LFO ──────────────────────────────────────────────────────────────
		bool lfoReset = lfoResetIn.process(in.lfoResetVoltage, 0.1f, 1.f);
		if (in.lfoSynced) {
			// Steps per cycle, slowest first: clockwise has to speed the LFO up
			// here exactly as it does in free mode, or the knob reverses its
			// meaning as the switch flips. Sixteen steps is one pattern — one
			// bar — so at that division the saw output is the bar position.
			//
			// Synced means phase-locked, not merely a synced rate: the phase is
			// derived from the step clock, so the LFO cannot drift against the
			// pattern and its saw stays a usable phasor.
			static const float div[8] = {32.f, 16.f, 8.f, 4.f, 2.f, 1.f, 0.5f, 0.25f};
			int d = clamp((int)(in.lfoRateKnob * 7.999f + in.lfoRateMod * 4.f), 0, 7);
			// modulation moves the division rather than detuning the rate: a
			// phase derived from the clock has nothing to detune
			if (lfoReset)
				lfoOffset = barPos;
			double phase = (barPos - lfoOffset) / div[d];
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
		// Peak at phase 0, falling to the trough, rising back after — and
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
