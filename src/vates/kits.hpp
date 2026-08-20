#pragma once
// vates kit generators — the percussive half of the factory banks.
//
// Ported from pages64 (github.com/gosub/pages64, GPL-3.0-or-later, same
// author): the recipe and voice code of its four kit companions, 64Drums,
// 64Objects, 64Grains and 64Micro. There they are live voices struck by a
// grid of gates; here each one renders a single hit into a stereo buffer,
// once, when a bank is built.
//
// Two deliberate differences from the originals:
//
//   - pages64 lays a kit out as eight rows of one family, the column walking
//     the pitch. vates asks for one family at a time, at a column position of
//     its choosing, so a bank of eight is eight *kinds* of sound rather than
//     one kind in eight sizes.
//   - the "variety" extras are all enabled (their per-cell gates still keep
//     part of a bank clean), except those that only mean something across
//     repeated hits — pong panning, the drop cycle and the pitch cycle are
//     hit counters, and a rendered sample is always hit zero.
//
// Fixes to the originals have to be ported by hand; this is a copy, not a
// shared library.

#include "../imber/imber_dsp.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace vates_kits {

using imber_dsp::Rng;
using imber_dsp::clampf;

enum Kit { KIT_DRUMS, KIT_OBJECTS, KIT_GRAINS, KIT_MICRO, KIT_COUNT };

inline int familyCount(int kit) {
	switch (kit) {
		case KIT_MICRO: return 9;
		default:        return 8;
	}
}

inline const char* familyName(int kit, int fam) {
	static const char* drums[8]   = {"click", "open hat", "closed hat", "blip",
	                                 "clap", "snare", "tom", "kick"};
	static const char* objects[8] = {"woodblock", "tine", "glass", "marimba",
	                                 "vibraphone", "harp", "membrane", "bell"};
	static const char* grains[8]  = {"dust", "crackle", "glitch", "chirp",
	                                 "trainlet", "bubble", "hiss", "rumble"};
	static const char* micro[9]   = {"click", "tick", "crush", "data", "blip",
	                                 "zap", "ping", "thump", "fold"};
	int n = familyCount(kit);
	if (fam < 0 || fam >= n)
		fam = 0;
	switch (kit) {
		case KIT_DRUMS:   return drums[fam];
		case KIT_OBJECTS: return objects[fam];
		case KIT_GRAINS:  return grains[fam];
		default:          return micro[fam];
	}
}

// One-pole coefficient. The originals use the small-angle form
// clamp(2*pi*fc*dt, 0, 1), which reaches 1 at fc = fs/(2*pi) — 7018 Hz at
// 44.1 kHz — and a one-pole highpass whose coefficient is exactly 1 outputs
// silence, not treble. That is why the hat rows of 64Drums go quiet from
// column 4 up at 44.1 kHz unless their resonant-noise extra happens to be on.
// The exact form cannot reach 1, so it cannot do that.
inline float onePole(float fc, float dt) {
	return clampf(1.f - std::exp(-2.f * (float)M_PI * fc * dt), 0.f, 0.999f);
}

// ── shared tail ───────────────────────────────────────────────────────────────

// Trim the silence a decaying voice leaves behind, fade the last 2 ms so the
// cut never clicks, and normalize. Each sample is normalized on its own: a
// knob position has to arrive at a usable level whatever it holds.
inline void finishHit(std::vector<float>& L, std::vector<float>& R, float sr) {
	size_t n = L.size();
	float peak = 0.f;
	size_t last = 0;
	for (size_t i = 0; i < n; i++) {
		float a = std::max(std::fabs(L[i]), std::fabs(R[i]));
		if (a > peak)
			peak = a;
		if (a > 1e-4f)
			last = i;
	}
	size_t keep = std::min(n, last + (size_t)(0.005f * sr) + 2);
	L.resize(keep);
	R.resize(keep);
	if (peak > 1e-9f) {
		float g = 0.9f / peak;
		for (size_t i = 0; i < keep; i++) {
			L[i] *= g;
			R[i] *= g;
		}
	}
	int fade = (int)std::min((float)keep, 0.002f * sr);
	for (int i = 0; i < fade; i++) {
		float w = (float)i / fade;
		size_t k = keep - 1 - i;
		L[k] *= w;
		R[k] *= w;
	}
}

// ── 64Drums ───────────────────────────────────────────────────────────────────
// Sine with a pitch-drop envelope plus filtered noise, and five gated extras:
// fold, FM, ring mod, resonant noise band, rising pitch.

inline void renderDrum(Rng& rng, int family, float spread, float sr,
                       std::vector<float>& L, std::vector<float>& R) {
	auto rnd = [&]() { return rng.uniform(); };
	float jitter = 0.85f + 0.3f * rnd();

	float f0 = 0.f, decay = 0.1f, pitchAmt = 0.f, pitchRate = 0.f;
	float sineAmt = 0.f, noiseAmt = 0.f, lpFc = 0.f, hpFc = 0.f;
	auto base = [&](float a, float b, float c, float d, float e, float f, float g, float h) {
		f0 = a; decay = b; pitchAmt = c; pitchRate = d;
		sineAmt = e; noiseAmt = f; lpFc = g; hpFc = h;
	};
	switch (family) {
		case 7:   // kick
			base((40.f + 18.f * spread) * jitter, 0.25f + 0.15f * rnd(),
			     3.f, 25.f, 1.f, 0.04f, 400.f, 0.f);
			break;
		case 6:   // tom
			base((85.f + 80.f * spread) * jitter, 0.18f + 0.12f * rnd(),
			     1.f, 18.f, 1.f, 0.08f, 800.f, 0.f);
			break;
		case 5: { // snare
			float dec = 0.12f + 0.08f * rnd();
			float lp  = 5000.f + 3000.f * rnd();
			base((165.f + 60.f * spread) * jitter, dec, 0.5f, 30.f, 0.5f, 0.8f, lp, 300.f);
			break;
		}
		case 4: { // clap
			float dec = 0.08f + 0.07f * rnd();
			float lp  = 3500.f + 2500.f * rnd();
			base(0.f, dec, 0.f, 0.f, 0.f, 1.f, lp, 600.f + 400.f * spread);
			break;
		}
		case 3:   // perc blip
			base((380.f + 520.f * spread) * jitter, 0.05f + 0.05f * rnd(),
			     0.5f, 40.f, 1.f, 0.05f, 2000.f, 0.f);
			break;
		case 2:   // closed hat
			base(0.f, 0.03f + 0.03f * rnd(), 0.f, 0.f, 0.f, 1.f,
			     12000.f, 6000.f + 2500.f * spread);
			break;
		case 1:   // open hat
			base(0.f, 0.2f + 0.2f * rnd(), 0.f, 0.f, 0.f, 1.f,
			     12000.f, 6000.f + 2500.f * spread);
			break;
		default:  // click
			base((2000.f + 1800.f * spread) * jitter, 0.015f + 0.015f * rnd(),
			     0.f, 0.f, 0.3f, 0.7f, 14000.f, 8000.f);
			break;
	}

	float pan = 0.5f + (rnd() - 0.5f) * 0.6f;

	float g, a;
	g = rnd(); a = rnd();
	float shape   = (g < 0.5f)  ? 0.15f + 0.85f * a : 0.f;
	float fmRatio = 0.5f + 5.5f * rnd();
	g = rnd(); a = rnd();
	float fmAmt   = (g < 0.4f)  ? 1.f + 7.f * a * a : 0.f;
	float rmRatio = 1.25f + 6.f * rnd();
	g = rnd(); a = rnd();
	float rmAmt   = (g < 0.35f) ? 0.4f + 0.6f * a : 0.f;
	g = rnd(); a = rnd();
	float reso    = (g < 0.4f)  ? 0.3f + 0.7f * a : 0.f;
	bool  rise    = rnd() < 0.35f;
	float bpFc    = std::sqrt(std::max(hpFc, 150.f) * std::max(lpFc, 300.f));

	const float dt = 1.f / sr;
	size_t n = (size_t)clampf(7.f * decay * sr, 0.02f * sr, 4.f * sr);
	L.assign(n, 0.f);
	R.assign(n, 0.f);

	float phase = 0.f, pitchEnv = 1.f, env = 1.f;
	float lp = 0.f, hpLp = 0.f, fmPhase = 0.f, rmPhase = 0.f;
	float svfLow = 0.f, svfBand = 0.f;

	for (size_t i = 0; i < n; i++) {
		float out = 0.f;
		if (sineAmt > 0.f) {
			float pAmt = pitchAmt;
			if (rise && pitchAmt > 0.f)
				pAmt = -0.6f * std::min(pitchAmt, 1.2f);
			float freq = f0 * (1.f + pAmt * pitchEnv * pitchEnv);
			phase += freq * dt;
			phase -= (int)phase;
			float ph = 2.f * (float)M_PI * phase;
			if (fmAmt > 0.f) {
				fmPhase += freq * fmRatio * dt;
				fmPhase -= (int)fmPhase;
				ph += fmAmt * env * std::sin(2.f * (float)M_PI * fmPhase);
			}
			float s = std::sin(ph);
			if (shape > 0.f)
				s = std::sin(ph + 4.f * shape * (0.25f + 0.75f * env) * s);
			if (rmAmt > 0.f) {
				rmPhase += f0 * rmRatio * dt;
				rmPhase -= (int)rmPhase;
				float d = rmAmt * env;
				s *= 1.f - d + d * std::sin(2.f * (float)M_PI * rmPhase);
			}
			out += sineAmt * s;
		}
		if (noiseAmt > 0.f) {
			float nz = rng.bipolar();
			float raw = nz;
			if (hpFc > 0.f) {
				hpLp += onePole(hpFc, dt) * (nz - hpLp);
				nz -= hpLp;
			}
			lp += onePole(lpFc, dt) * (nz - lp);
			float shaped = lp;
			if (reso > 0.f) {
				float fq = std::min(2.f * std::sin((float)M_PI * bpFc * dt), 0.7f);
				float q  = std::max(1.2f - reso, 0.2f);
				svfLow += fq * svfBand;
				float high = raw - svfLow - q * svfBand;
				svfBand += fq * high;
				shaped = shaped + (svfBand * q - shaped) * reso;
			}
			out += noiseAmt * shaped;
		}
		out *= env;

		env      -= env * dt / decay;
		pitchEnv -= pitchEnv * dt * pitchRate;

		L[i] = out * (1.f - pan);
		R[i] = out * pan;
	}
	finishHit(L, R, sr);
}

// ── 64Objects ─────────────────────────────────────────────────────────────────
// Modal percussion: a bank of damped resonators as complex phasors, the
// harp family a Karplus-Strong string. Extras: mode beating, rattle, flam,
// felt mute.

static const int OBJ_MODES = 8;
static const int KS_BUF    = 4096;
static const int KS_MASK   = KS_BUF - 1;

inline void renderObject(Rng& rng, int family, float spread, float sr,
                         std::vector<float>& L, std::vector<float>& R) {
	auto rnd = [&]() { return rng.uniform(); };
	float jitter = 0.85f + 0.3f * rnd();

	bool  string = false;
	float f0 = 0.f;
	int   nModes = 0;
	float modeRatio[OBJ_MODES] = {0.f};
	float modeT60[OBJ_MODES]   = {0.f};
	float modeGain[OBJ_MODES]  = {0.f};
	float ksT60 = 1.f, ksBright = 0.5f, pluckPos = 0.2f, pluckBright = 0.4f;

	auto modal = [&](std::initializer_list<float> ratios, float freq, float t60, float gamma) {
		f0 = freq;
		nModes = (int)ratios.size();
		float hard = rnd();
		float pos  = 0.08f + 0.42f * rnd();
		int k = 0;
		for (float r : ratios)
			modeRatio[k++] = r * (0.99f + 0.02f * rnd());
		float sum = 0.f;
		for (k = 0; k < nModes; k++) {
			float r = modeRatio[k];
			modeT60[k] = t60 * std::pow(std::max(r, 0.5f), -gamma);
			float gg = std::fabs(std::sin((k + 1) * (float)M_PI * pos));
			gg /= 1.f + std::max(r - 1.f, 0.f) * 1.5f * (1.f - hard);
			modeGain[k] = gg;
			sum += gg;
		}
		for (k = 0; k < nModes; k++)
			modeGain[k] /= std::max(sum, 0.1f);
	};

	switch (family) {
		case 0:   // woodblock
			modal({1.f, 2.42f, 3.93f}, (750.f + 650.f * spread) * jitter,
			      0.05f + 0.05f * rnd(), 1.3f);
			break;
		case 1:   // music-box tine
			modal({1.f, 6.267f, 17.547f}, (900.f + 900.f * spread) * jitter,
			      0.6f + 0.8f * rnd(), 0.6f);
			break;
		case 2:   // glass / bowl
			modal({1.f, 2.32f, 4.25f, 6.63f}, (500.f + 700.f * spread) * jitter,
			      1.2f + 1.8f * rnd(), 0.35f);
			break;
		case 3:   // marimba bar
			modal({1.f, 2.756f, 5.404f, 8.933f}, (130.f + 260.f * spread) * jitter,
			      0.2f + 0.15f * rnd(), 1.1f);
			break;
		case 4:   // vibraphone bar
			modal({1.f, 2.756f, 5.404f, 8.933f}, (175.f + 350.f * spread) * jitter,
			      2.0f + 3.0f * rnd(), 0.45f);
			break;
		case 5:   // harp / pluck
			string      = true;
			f0          = (110.f + 330.f * spread) * jitter;
			ksT60       = 1.0f + 1.8f * rnd();
			ksBright    = 0.25f + 0.6f * rnd();
			pluckPos    = 0.12f + 0.3f * rnd();
			pluckBright = 0.15f + 0.55f * rnd();
			break;
		case 6:   // membrane
			modal({1.f, 1.594f, 2.136f, 2.296f, 2.653f, 2.918f},
			      (75.f + 105.f * spread) * jitter, 0.2f + 0.25f * rnd(), 0.9f);
			break;
		default:  // bell
			modal({0.5f, 1.f, 1.2f, 1.5f, 2.f, 2.5f, 2.67f},
			      (210.f + 210.f * spread) * jitter, 2.5f + 4.0f * rnd(), 0.5f);
			break;
	}

	float pan = 0.5f + (rnd() - 0.5f) * 0.6f;

	float g, a;
	g = rnd(); a = rnd();
	float beatHz    = (g < 0.4f)  ? 0.6f + 2.4f * a : 0.f;
	g = rnd(); a = rnd();
	float rattleAmt = (g < 0.35f) ? 0.35f + 0.45f * a : 0.f;
	g = rnd(); a = rnd();
	float flamGain  = (g < 0.35f) ? 0.35f + 0.4f * a : 0.f;
	float flamS     = 0.012f + 0.03f * rnd();
	bool  mute      = rnd() < 0.3f;

	float decayScale = mute ? 0.18f : 1.f;

	// voice state
	int   nVoiceModes = 0;
	float rotRe[OBJ_MODES] = {0.f}, rotIm[OBJ_MODES] = {0.f};
	float re[OBJ_MODES] = {0.f}, im[OBJ_MODES] = {0.f}, exRe[OBJ_MODES] = {0.f};
	std::vector<float> ks;
	int   len = 0, wr = 0, feed = 0;
	float frac = 0.f, loopLp = 0.f, loss = 0.f, bright = 0.f, feedAmp = 0.f;
	long  life = 0;

	if (!string) {
		int dup = beatHz > 0.f ? std::min(nModes, OBJ_MODES - nModes) : 0;
		nVoiceModes = nModes + dup;
		float maxT60 = 0.f;
		for (int k = 0; k < nVoiceModes; k++) {
			int   src  = k < nModes ? k : k - nModes;
			float freq = f0 * modeRatio[src] + (k >= nModes ? beatHz : 0.f);
			float gain = modeGain[src] * (k >= nModes ? 0.7f : 1.f);
			if (mute)
				gain /= 1.f + std::max(modeRatio[src] - 1.f, 0.f);
			if (freq > 0.45f * sr)
				gain = 0.f;
			float t60 = std::max(modeT60[src] * decayScale, 0.005f);
			maxT60 = std::max(maxT60, gain > 0.f ? t60 : 0.f);
			float r = std::pow(10.f, -3.f / (t60 * sr));
			float w = 2.f * (float)M_PI * freq / sr;
			rotRe[k] = r * std::cos(w);
			rotIm[k] = r * std::sin(w);
			exRe[k]  = gain;
			re[k]    = gain;
		}
		life = (long)(sr * std::max(maxT60, 0.01f));
	}
	else {
		ks.assign(KS_BUF, 0.f);
		float f = clampf(f0, sr / (KS_BUF - 2.f), 0.25f * sr);
		float D = sr / f - 0.5f;
		len  = (int)D + 1;
		frac = len - D;
		float t60 = std::max(ksT60 * decayScale, 0.02f);
		loss   = std::min(std::pow(10.f, -3.f / (f * t60)), 0.99995f);
		bright = ksBright;
		float lp = 0.f;
		for (int k = 0; k < len; k++) {
			float nz = rng.bipolar();
			lp += pluckBright * (nz - lp);
			ks[k] = lp * 0.9f;
		}
		int off = std::max(1, (int)(pluckPos * len));
		for (int k = len - 1; k >= 0; k--) {
			int j = k - off;
			if (j < 0) j += len;
			ks[k] -= 0.8f * ks[j];
		}
		wr   = len;
		life = (long)(sr * t60 * 1.5f);
	}

	int flamWait = flamGain > 0.f ? std::max(1, (int)(sr * flamS)) : 0;
	float flamAmt = flamGain;

	size_t n = (size_t)clampf((float)life, 0.02f * sr, 6.f * sr);
	L.assign(n, 0.f);
	R.assign(n, 0.f);

	for (size_t i = 0; i < n; i++) {
		if (flamWait > 0 && --flamWait == 0) {
			if (!string)
				for (int k = 0; k < nVoiceModes; k++)
					re[k] += flamAmt * exRe[k];
			else {
				feed    = len;
				feedAmp = flamAmt * 0.4f;
			}
		}

		float s = 0.f;
		if (!string) {
			for (int k = 0; k < nVoiceModes; k++) {
				float nr = rotRe[k] * re[k] - rotIm[k] * im[k];
				float ni = rotRe[k] * im[k] + rotIm[k] * re[k];
				re[k] = nr;
				im[k] = ni;
				s += nr;
			}
		}
		else {
			int   i0 = (wr - len) & KS_MASK;
			float s0 = ks[i0];
			float s1 = ks[(i0 + 1) & KS_MASK];
			s = s0 + frac * (s1 - s0);
			loopLp += bright * (s - loopLp);
			float fb = loss * loopLp;
			if (feed > 0) {
				fb += feedAmp * rng.bipolar();
				feed--;
			}
			ks[wr] = fb;
			wr = (wr + 1) & KS_MASK;
		}

		if (rattleAmt > 0.f)
			s = s + (std::tanh(5.f * s) - s) * rattleAmt;

		L[i] = s * (1.f - pan);
		R[i] = s * pan;
	}
	finishHit(L, R, sr);
}

// ── 64Grains ──────────────────────────────────────────────────────────────────
// A cloud of windowed sine or noise grains scheduled over tens to hundreds of
// milliseconds. Extras: reverse swell, accelerando, pan sweep, glide.

inline void renderGrainCloud(Rng& rng, int family, float spread, float sr,
                             std::vector<float>& L, std::vector<float>& R) {
	auto rnd = [&]() { return rng.uniform(); };
	float jitter = 0.85f + 0.3f * rnd();

	bool  noise = false, regular = false;
	float f0 = 0.f, cloudLen = 0.2f, grainLen = 0.01f;
	float pitchScat = 0.f, glide = 0.f, lpFc = 0.f, hpFc = 0.f, crush = 0.f;
	float attack = 0.3f, scatter = 0.5f;
	int   count = 8;

	switch (family) {
		case 0:   // dust
			noise = true;
			count = 2 + (int)(4.f * rnd());
			cloudLen = 0.12f + 0.25f * rnd();
			grainLen = 0.0008f + 0.0015f * rnd();
			lpFc = 6000.f + 7000.f * rnd();
			hpFc = 1000.f + 1500.f * spread;
			attack = 0.15f;
			scatter = 0.7f;
			break;
		case 1:   // crackle
			noise = true;
			count = 16 + (int)(30.f * rnd());
			cloudLen = 0.06f + 0.15f * rnd();
			grainLen = 0.0004f + 0.0009f * rnd();
			lpFc = 9000.f + 5000.f * rnd();
			hpFc = 2500.f + 2000.f * spread;
			attack = 0.1f;
			scatter = 0.9f;
			break;
		case 2:   // glitch
			f0 = (300.f + 1300.f * spread) * jitter;
			count = 4 + (int)(7.f * rnd());
			cloudLen = 0.08f + 0.18f * rnd();
			grainLen = 0.004f + 0.008f * rnd();
			pitchScat = 14.f + 10.f * rnd();
			crush = 0.4f + 0.6f * rnd();
			attack = 0.05f;
			break;
		case 3:   // chirp / glisson
			f0 = (350.f + 1050.f * spread) * jitter;
			count = 3 + (int)(6.f * rnd());
			cloudLen = 0.1f + 0.25f * rnd();
			grainLen = 0.008f + 0.017f * rnd();
			pitchScat = 4.f + 6.f * rnd();
			glide = (rnd() < 0.5f ? -1.f : 1.f) * (0.8f + 1.5f * rnd());
			attack = 0.3f;
			break;
		case 4:   // trainlet
			noise = true;
			regular = true;
			f0 = (55.f + 165.f * spread) * jitter;
			cloudLen = 0.12f + 0.2f * rnd();
			grainLen = 0.0012f + 0.0015f * rnd();
			lpFc = 3000.f + 5000.f * rnd();
			attack = 0.1f;
			break;
		case 5:   // bubble
			f0 = (280.f + 620.f * spread) * jitter;
			count = 2 + (int)(4.f * rnd());
			cloudLen = 0.15f + 0.2f * rnd();
			grainLen = 0.015f + 0.03f * rnd();
			pitchScat = 3.f + 4.f * rnd();
			glide = 0.7f + 1.3f * rnd();
			attack = 0.5f;
			break;
		case 6:   // hiss
			noise = true;
			count = 3 + (int)(6.f * rnd());
			cloudLen = 0.1f + 0.25f * rnd();
			grainLen = 0.012f + 0.025f * rnd();
			lpFc = 9000.f + 5000.f * rnd();
			hpFc = 2500.f + 3500.f * spread + 1000.f * rnd();
			attack = 0.4f;
			break;
		default:  // rumble
			noise = true;
			count = 5 + (int)(10.f * rnd());
			cloudLen = 0.2f + 0.3f * rnd();
			grainLen = 0.025f + 0.045f * rnd();
			lpFc = 80.f + 170.f * spread + 50.f * rnd();
			attack = 0.4f;
			break;
	}
	if (regular)
		count = (int)clampf(cloudLen * f0, 4.f, 48.f);

	float pan = 0.5f + (rnd() - 0.5f) * 0.6f;

	float g, a;
	bool  rev = rnd() < 0.45f;
	g = rnd(); a = rnd();
	float accel  = (g < 0.45f) ? std::exp2((a - 0.5f) * 3.f) : 1.f;
	g = rnd(); a = rnd();
	float sweep  = (g < 0.45f) ? (a - 0.5f) * 2.2f : 0.f;
	g = rnd(); a = rnd();
	float glideX = (g < 0.4f)  ? 0.5f + 1.2f * a : 0.f;

	const float dt = 1.f / sr;
	size_t n = (size_t)clampf((cloudLen + grainLen * 3.f + 0.05f) * sr, 0.05f * sr, 2.5f * sr);
	L.assign(n, 0.f);
	R.assign(n, 0.f);

	// Grains are rendered one at a time straight into the buffer: offline
	// there is no need for the live version's pool of 96.
	float t = 0.f, next = 0.f;
	int spawned = 0;
	while (spawned < count && t < cloudLen + grainLen) {
		if (t < next) {
			t += dt;
			continue;
		}
		float pos = clampf(t / cloudLen, 0.f, 1.f);
		float dur = grainLen * (0.6f + 0.8f * rnd());
		float amp = 0.75f / std::pow((float)count, 0.35f);
		amp *= rev ? (0.25f + 0.75f * pos * pos) : (1.f - 0.35f * pos);

		float freq = 0.f, fMul = 1.f, lpC = 0.f, hpC = 0.f;
		float crushAmt = 0.f;
		int   holdDiv = 1;
		if (!noise) {
			freq = f0 * std::exp2(pitchScat * (rnd() - 0.5f) / 12.f);
			float gld = glide;
			if (glideX > 0.f)
				gld += glideX * (rnd() - 0.5f) * 2.f;
			fMul = std::exp2(gld * dt / std::max(dur, 1e-5f));
			if (crush > 0.f) {
				crushAmt = crush;
				holdDiv  = 1 + (int)(crush * 0.004f / dt);
			}
		}
		else {
			lpC = onePole(lpFc, dt);
			hpC = hpFc > 0.f ? onePole(hpFc, dt) : 0.f;
		}

		float p = pan + scatter * (rnd() - 0.5f) * 0.8f;
		if (sweep != 0.f)
			p += sweep * (pos - 0.5f);
		p = clampf(p, 0.f, 1.f);
		float gl = 1.f - p, gr = p;

		size_t start = (size_t)(t * sr);
		size_t gn    = (size_t)(dur * sr);
		float phase = 0.f, lp = 0.f, hp = 0.f, hold = 0.f;
		int   holdN = 1;
		for (size_t k = 0; k < gn && start + k < n; k++) {
			float x = (float)k / gn;
			float env = x < attack ? x / std::max(attack, 1e-4f)
			                       : (1.f - x) / std::max(1.f - attack, 1e-4f);
			float s;
			if (noise) {
				float nz = rng.bipolar();
				lp += lpC * (nz - lp);
				s = lp;
				if (hpC > 0.f) {
					hp += hpC * (s - hp);
					s -= hp;
				}
			}
			else {
				s = std::sin(2.f * (float)M_PI * phase);
				phase += freq * dt;
				phase -= (int)phase;
				freq  *= fMul;
				if (crushAmt > 0.f) {
					if (--holdN <= 0) {
						hold  = s;
						holdN = holdDiv;
					}
					float q = 20.f - 14.f * crushAmt;
					s = std::round(hold * q) / q;
				}
			}
			s *= env * amp;
			L[start + k] += s * gl;
			R[start + k] += s * gr;
		}

		float iv = regular ? 1.f / std::max(f0, 1.f)
		                   : (cloudLen / count) * (0.4f + 1.2f * rnd());
		if (accel != 1.f)
			iv *= std::pow(accel, (float)spawned / count);
		next = t + iv;
		spawned++;
		t += dt;
	}
	finishHit(L, R, sr);
}

// ── 64Micro ───────────────────────────────────────────────────────────────────
// Single designed micro-sounds, 0.2 to 20 ms: clicks, ticks, crush fragments,
// data bursts, blips, zaps, pings, sub thumps and folds. The pong/drop/alt
// cycles are per-hit and mean nothing in a rendered sample; the doubler does,
// and stays.

enum { MIC_CLICK, MIC_TICK, MIC_CRUSH, MIC_DATA, MIC_BLIP,
       MIC_ZAP, MIC_PING, MIC_THUMP, MIC_FOLD };
enum { WIN_RECT, WIN_EXP, WIN_HANN };

inline void renderMicro(Rng& rng, int family, float spread, float sr,
                        std::vector<float>& L, std::vector<float>& R) {
	auto rnd = [&]() { return rng.uniform(); };
	float jitter = 0.85f + 0.3f * rnd();

	float f0 = 0.f, f1 = 0.f, dur = 0.005f, winK = 5.f, lpFc = 0.f, t60 = 0.f;
	float crushHz = 0.f, levels = 8.f, foldK = 0.f;
	int   win = WIN_RECT;
	bool  dual = false;

	switch (family) {
		case MIC_CLICK:
			f0   = (2000.f + 6000.f * spread) * jitter;
			dur  = 0.0003f + 0.0022f * rnd();
			win  = rnd() < 0.4f ? WIN_RECT : WIN_EXP;
			winK = 5.f + 4.f * rnd();
			break;
		case MIC_TICK:
			lpFc = (2500.f + 9000.f * spread) * jitter;
			dur  = 0.0004f + 0.002f * rnd();
			win  = WIN_EXP;
			winK = 6.f + 3.f * rnd();
			break;
		case MIC_CRUSH:
			f0      = (250.f + 1400.f * spread) * jitter;
			crushHz = 900.f + 3500.f * rnd();
			levels  = 3.f + 10.f * rnd();
			dur     = 0.003f + 0.007f * rnd();
			win     = WIN_RECT;
			break;
		case MIC_DATA:
			f0  = (300.f + 2700.f * spread) * jitter;
			dur = 0.006f + 0.04f * rnd();
			win = WIN_RECT;
			break;
		case MIC_BLIP:
			f0   = (300.f + 2700.f * spread) * jitter;
			dual = rnd() < 0.35f;
			f1   = f0 * (1.2f + 1.3f * rnd());
			dur  = 0.004f + 0.016f * rnd();
			win  = WIN_RECT;
			break;
		case MIC_ZAP: {
			f0 = (1200.f + 4000.f * spread) * jitter;
			float rise = rnd();
			float amt  = rnd();
			f1   = rise < 0.25f ? f0 * (3.f + 4.f * amt) : f0 * (0.05f + 0.1f * amt);
			dur  = 0.004f + 0.012f * rnd();
			win  = WIN_EXP;
			winK = 4.f + 3.f * rnd();
			break;
		}
		case MIC_PING:
			f0  = (400.f + 3000.f * spread) * jitter;
			t60 = 0.01f + 0.05f * rnd();
			dur = std::min(t60 * 2.f, 0.06f);
			win = WIN_RECT;
			break;
		case MIC_THUMP:
			f0  = (40.f + 50.f * spread) * jitter;
			f1  = f0 * 0.8f;
			dur = 0.02f + 0.04f * rnd();
			win = WIN_RECT;
			break;
		default:   // fold
			f0    = (300.f + 2200.f * spread) * jitter;
			foldK = 2.f + 6.f * rnd();
			dur   = 0.002f + 0.008f * rnd();
			win   = WIN_EXP;
			winK  = 4.f + 4.f * rnd();
			break;
	}
	uint32_t nseed = (uint32_t)(rng.next() | 1);
	float pan = 0.5f + (rnd() - 0.5f) * 0.6f;

	float g, a;
	g = rnd(); a = rnd();
	(void)((g < 0.4f) ? 2 + (int)(a * 2.999f) : 1);   // altN: hit cycle, unused
	for (int k = 1; k < 4; k++)
		(void)((rnd() < 0.5f ? -1.f : 1.f) * (1.f + 11.f * rnd()));
	(void)(rnd() < 0.4f);                              // pong: hit cycle, unused
	g = rnd(); a = rnd();
	(void)((g < 0.35f) ? 2 + (int)(a * 2.999f) : 0);   // dropN: hit cycle, unused
	g = rnd(); a = rnd();
	float dblGain = (g < 0.35f) ? 0.4f + 0.4f * a : 0.f;
	float dblS    = 0.008f + 0.03f * rnd();

	const float dt = 1.f / sr;
	float total = dur + (dblGain > 0.f ? dblS + dur : 0.f) + 0.005f;
	size_t n = (size_t)clampf(total * sr, 0.002f * sr, 0.3f * sr);
	L.assign(n, 0.f);
	R.assign(n, 0.f);

	// one event, rendered at `start`, at `gain`
	auto event = [&](size_t start, float gain) {
		float phase = 0.f, phase2 = 0.f, freq = f0, freq2 = f1, fMul = 1.f;
		float re = 1.f, im = 0.f, rotRe = 0.f, rotIm = 0.f;
		uint32_t lfsr = nseed;
		float lp = 0.f, lpC = lpFc > 0.f ? onePole(lpFc, dt) : 0.f;
		float hold = 0.f;
		int   holdN = 1, holdDiv = 1, bitN = 0, bitDiv = 1;
		if (family == MIC_ZAP || family == MIC_THUMP)
			fMul = std::pow(std::max(f1, 1.f) / std::max(f0, 1.f),
			                1.f / std::max(dur * sr, 1.f));
		if (family == MIC_PING) {
			float r = std::pow(10.f, -3.f / (t60 * sr));
			float w = 2.f * (float)M_PI * std::min(f0, 0.45f * sr) / sr;
			rotRe = r * std::cos(w);
			rotIm = r * std::sin(w);
		}
		if (family == MIC_CRUSH)
			holdDiv = std::max(1, (int)(sr / crushHz));
		if (family == MIC_DATA)
			bitDiv = std::max(1, (int)(sr / std::max(f0, 30.f)));

		size_t en = (size_t)(dur * sr);
		for (size_t k = 0; k < en && start + k < n; k++) {
			float t = (float)k / sr;
			float x = t / dur;
			float w;
			switch (win) {
				case WIN_EXP:  w = std::exp(-x * winK); break;
				case WIN_HANN: w = 0.5f - 0.5f * std::cos(2.f * (float)M_PI * x); break;
				default: {
					float e = std::min(t, dur - t) / 0.0003f;
					w = e >= 1.f ? 1.f : 0.5f - 0.5f * std::cos((float)M_PI * clampf(e, 0.f, 1.f));
					break;
				}
			}
			float s = 0.f;
			switch (family) {
				case MIC_CLICK:
				case MIC_BLIP:
					s = std::sin(2.f * (float)M_PI * phase);
					phase += freq * dt;
					if (dual) {
						s = 0.6f * s + 0.6f * std::sin(2.f * (float)M_PI * phase2);
						phase2 += freq2 * dt;
					}
					break;
				case MIC_TICK: {
					lfsr ^= lfsr << 13; lfsr ^= lfsr >> 17; lfsr ^= lfsr << 5;
					float nz = ((lfsr >> 8) / 8388608.f) - 1.f;
					lp += lpC * (nz - lp);
					s = lp * 2.f;
					break;
				}
				case MIC_CRUSH: {
					float raw = std::sin(2.f * (float)M_PI * phase);
					phase += freq * dt;
					if (--holdN <= 0) { hold = raw; holdN = holdDiv; }
					s = std::round(hold * levels) / levels;
					break;
				}
				case MIC_DATA:
					if (--bitN <= 0) {
						lfsr ^= lfsr << 13; lfsr ^= lfsr >> 17; lfsr ^= lfsr << 5;
						bitN = bitDiv;
					}
					s = (lfsr & 1) ? 0.8f : -0.8f;
					break;
				case MIC_ZAP:
				case MIC_THUMP:
					s = std::sin(2.f * (float)M_PI * phase);
					phase += freq * dt;
					freq  *= fMul;
					break;
				case MIC_PING: {
					float nr = rotRe * re - rotIm * im;
					float ni = rotRe * im + rotIm * re;
					re = nr; im = ni;
					s = nr;
					break;
				}
				default:   // fold
					s = std::sin(foldK * std::sin(2.f * (float)M_PI * phase));
					phase += freq * dt;
					break;
			}
			s *= w * gain;
			L[start + k] += s * (1.f - pan);
			R[start + k] += s * pan;
		}
	};

	event(0, 1.f);
	if (dblGain > 0.f)
		event((size_t)(dblS * sr), dblGain);
	finishHit(L, R, sr);
}

// ── one entry point ───────────────────────────────────────────────────────────

inline void render(int kit, int family, float spread, Rng& rng, float sr,
                   std::vector<float>& L, std::vector<float>& R) {
	switch (kit) {
		case KIT_DRUMS:   renderDrum(rng, family, spread, sr, L, R); break;
		case KIT_OBJECTS: renderObject(rng, family, spread, sr, L, R); break;
		case KIT_GRAINS:  renderGrainCloud(rng, family, spread, sr, L, R); break;
		default:          renderMicro(rng, family, spread, sr, L, R); break;
	}
}

}   // namespace vates_kits
