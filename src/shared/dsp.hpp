#pragma once
#include <rack.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

// Small DSP blocks shared by vates and artifex. Both are header-only and
// self-contained: every function is inline, so the two translation units that
// include this file do not collide (see tools/release/check_symbols.py and
// the ODR note in CLAUDE.md).

namespace forsitan_dsp {

using namespace rack;

// TPT state-variable filter, one per channel. Used as a lowpass on one side
// of a bipolar filter knob and a highpass on the other.
struct Svf {
	float ic1 = 0.f, ic2 = 0.f;
	void reset() { ic1 = ic2 = 0.f; }
	// returns lowpass in lp and highpass in hp
	void process(float x, float g, float k, float& lp, float& hp) {
		float a1 = 1.f / (1.f + g * (g + k));
		float a2 = g * a1;
		float v3 = x - ic2;
		float v1 = a1 * ic1 + a2 * v3;
		float v2 = ic2 + g * v1;
		ic1 = 2.f * v1 - ic1;
		ic2 = 2.f * v2 - ic2;
		lp = v2;
		hp = x - k * v1 - v2;
		if (!std::isfinite(ic1) || !std::isfinite(ic2))
			reset();
	}
};

// One delay line with a fractional read.
struct Delay {
	std::vector<float> buf;
	int w = 0;
	void init(int n) {
		buf.assign(std::max(2, n), 0.f);
		w = 0;
	}
	void clear() {
		std::fill(buf.begin(), buf.end(), 0.f);
	}
	int size() const { return (int)buf.size(); }
	void write(float x) {
		if (buf.empty())
			return;
		buf[w] = x;
		if (++w >= (int)buf.size())
			w = 0;
	}
	float read(float delaySamples) const {
		if (buf.empty())
			return 0.f;
		int n = (int)buf.size();
		float d = clamp(delaySamples, 1.f, (float)(n - 2));
		float rp = (float)w - d;
		while (rp < 0.f)
			rp += n;
		return lerp(rp, n);
	}
	// absolute read, for buffers used as tape rather than as a delay
	float at(float pos) const {
		if (buf.empty())
			return 0.f;
		int n = (int)buf.size();
		float p = pos - std::floor(pos / n) * n;
		if (p < 0.f)
			p = 0.f;
		return lerp(p, n);
	}

	// Interpolated read at a position already folded into [0, n) -- or so the
	// arithmetic that folded it believes. A position a hair below zero, once
	// n is added to it, rounds up to exactly n: floats near 55204 (the tape at
	// 48 kHz) are 1/256 apart, so anything within 1/512 of the wrap lands on
	// the boundary itself. Truncating that gave an index one past the end of
	// the vector, which is undefined behaviour and in practice read whatever
	// the heap held next -- one garbage sample every time the write pointer
	// came round, 1.15 s apart at the default buffer. The 2 ms delay hit it on
	// every wrap, since 0.002f * sr is a hair over 96 samples.
	//
	// A position that rounds to n is the wrap itself, so it folds to zero.
	float lerp(float pos, int n) const {
		int i0 = (int)pos;
		float fr = pos - (float)i0;
		if (i0 >= n) {
			i0 -= n;
			fr = 0.f;
		}
		if (i0 < 0)
			i0 = 0;
		int i1 = i0 + 1 < n ? i0 + 1 : 0;
		return buf[i0] + (buf[i1] - buf[i0]) * fr;
	}
	void poke(int i, float x) {
		if (buf.empty())
			return;
		int n = (int)buf.size();
		i %= n;
		if (i < 0)
			i += n;
		buf[i] = x;
	}
};

}   // namespace forsitan_dsp
