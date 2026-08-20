#pragma once
// vates factory banks — six banks of eight samples, built from one seed.
//
// Four banks are percussive and come from the ported pages64 kit engines
// (kits.hpp); two are tonal and come from imber's sustained generators, which
// have no counterpart there — every pages64 kit is struck, granular or
// clicked, and none of them sustains or holds a chord.
//
// Two rules shape a bank:
//
//   - every kind the generator knows appears at least once, and the slots are
//     handed out so no kind appears twice before every kind appears once. A
//     bank is therefore eight roles, and a knob position always means the
//     same sort of sound.
//   - everything is rendered at C. imber's generators take their pitch from
//     rng.tune, so they are handed a tuning whose scale is the single degree
//     0 with root C; the pages64 recipes are pitched by their own family
//     register and the column position, which vates draws at random.

#include "kits.hpp"
#include "../imber/imber_gen.hpp"

#include <atomic>
#include <string>
#include <vector>

namespace vates_bank {

using imber_dsp::Rng;
using imber_dsp::clampf;

static const int kNumBanks       = 6;
static const int kSamplesPerBank = 8;

// ── tonal families (imber generators) ─────────────────────────────────────────

enum TonalFamily {
	TON_DRONE_PURE, TON_DRONE_DETUNED, TON_DRONE_FM, TON_DRONE_SUB,
	TON_PAD_SLOW, TON_PAD_CLUSTER, TON_CHORD_STAB,
	TON_STRETCHED, TON_PAD_TAPE, TON_AIR_FILTERED, TON_AIR_WASH,
	TON_AIR_VOWEL, TON_AIR_COMB,
	TON_COUNT
};

static const int kToneFamilies[] = {
	TON_DRONE_PURE, TON_DRONE_DETUNED, TON_DRONE_FM, TON_DRONE_SUB,
	TON_PAD_SLOW, TON_PAD_CLUSTER, TON_CHORD_STAB
};
static const int kAirFamilies[] = {
	TON_STRETCHED, TON_PAD_TAPE, TON_AIR_FILTERED, TON_AIR_WASH,
	TON_AIR_VOWEL, TON_AIR_COMB
};

inline int tonalFamilyCount() { return TON_COUNT; }

inline const char* tonalFamilyName(int fam) {
	static const char* names[TON_COUNT] = {
		"drone pure", "drone detuned", "drone FM", "drone sub",
		"pad slow", "pad cluster", "chord stab",
		"stretched", "pad tape", "air filtered", "air wash",
		"air vowel", "air comb"
	};
	if (fam < 0 || fam >= TON_COUNT)
		fam = 0;
	return names[fam];
}

// Everything generated is a C, so the pitch knob and the quantizer mean what
// they say. A single-degree scale makes imber's pickFreq() land on the root
// in whatever octave it rolls; the chord voices keep a minor-seventh stack so
// a chord stab is still a chord.
inline imber_dsp::Tuning rootCTuning() {
	static const int deg[1] = {0};
	imber_dsp::Tuning t;
	t.root = imber_dsp::kRootOctave;   // C
	t.deg  = deg;
	t.nDeg = 1;
	t.chord[0] = 0; t.chord[1] = 3; t.chord[2] = 7; t.chord[3] = 10;
	return t;
}

// A mono buffer into a stereo pair: a fixed pan, and for the sustained
// families an allpass chain on one side so the two channels decorrelate
// without the envelope moving.
inline void spread(const std::vector<float>& mono, Rng& rng, float sr,
                   bool decorrelate, std::vector<float>& L, std::vector<float>& R) {
	size_t n = mono.size();
	L.assign(n, 0.f);
	R.assign(n, 0.f);
	float pan = 0.5f + rng.bipolar() * 0.25f;
	if (!decorrelate) {
		for (size_t i = 0; i < n; i++) {
			L[i] = mono[i] * (1.f - pan);
			R[i] = mono[i] * pan;
		}
		return;
	}
	// two Schroeder allpasses, prime-ish lengths, on the right channel only
	int d1 = (int)(sr * rng.range(0.0043f, 0.0071f));
	int d2 = (int)(sr * rng.range(0.0111f, 0.0173f));
	std::vector<float> a1(std::max(2, d1), 0.f), a2(std::max(2, d2), 0.f);
	int i1 = 0, i2 = 0;
	const float g = 0.68f;
	for (size_t i = 0; i < n; i++) {
		float x = mono[i];
		float v = a1[i1];
		float y = -g * x + v;
		a1[i1] = x + g * y;
		if (++i1 >= (int)a1.size()) i1 = 0;
		float v2 = a2[i2];
		float y2 = -g * y + v2;
		a2[i2] = y + g * y2;
		if (++i2 >= (int)a2.size()) i2 = 0;
		// only part of the right channel is the allpass: all of it makes a
		// pair that measures wider than mid, which folds down badly in mono
		L[i] = x * (1.f - pan);
		R[i] = (0.4f * x + 0.6f * y2) * pan;
	}
}

// Peak-normalize a stereo pair together, so panning cannot leave a sample
// quiet and decorrelation cannot push it over the rails.
inline void normalizeStereo(std::vector<float>& L, std::vector<float>& R, float target) {
	float peak = 0.f;
	for (size_t i = 0; i < L.size(); i++)
		peak = std::max(peak, std::max(std::fabs(L[i]), std::fabs(R[i])));
	if (peak < 1e-9f)
		return;
	float g = target / peak;
	for (size_t i = 0; i < L.size(); i++) {
		L[i] *= g;
		R[i] *= g;
	}
}

// Drop the silence in front of a render — imber's generators are loops and
// may start anywhere — and, for material that scatters several events through
// a buffer, keep only the first one: a sample player wants a hit, not a
// phrase with rests in it.
inline void trimToFirstEvent(std::vector<float>& b, float sr, bool cutAtGap) {
	if (b.empty())
		return;
	float peak = 0.f;
	for (size_t i = 0; i < b.size(); i++)
		peak = std::max(peak, std::fabs(b[i]));
	if (peak < 1e-6f)
		return;
	const float thresh = 0.02f * peak;
	size_t start = 0;
	while (start < b.size() && std::fabs(b[start]) < thresh)
		start++;
	size_t end = b.size();
	if (cutAtGap) {
		const size_t gap = (size_t)(0.2f * sr);
		size_t quiet = 0;
		for (size_t i = start; i < b.size(); i++) {
			quiet = std::fabs(b[i]) < thresh ? quiet + 1 : 0;
			if (quiet >= gap) {
				end = i;
				break;
			}
		}
	}
	if (start > 0 || end < b.size())
		b = std::vector<float>(b.begin() + start, b.begin() + end);
}

// Render one tonal one-shot. imber's generators finish loops — they fade both
// edges — so the tail work is done here instead: the head keeps a 3 ms fade
// (a drone starting mid-cycle would click), the sample is trimmed to a usable
// length, and the tail fades out.
inline void renderTonal(int fam, Rng& rng, float sr,
                        std::vector<float>& L, std::vector<float>& R) {
	rng.tune = rootCTuning();
	std::vector<float> b;
	switch (fam) {
		case TON_DRONE_PURE:    imber_gen::genDronePure(rng, sr, b); break;
		case TON_DRONE_DETUNED: imber_gen::genDroneDetuned(rng, sr, b); break;
		case TON_DRONE_FM:      imber_gen::genDroneFm(rng, sr, b); break;
		case TON_DRONE_SUB:     imber_gen::genDroneSub(rng, sr, b); break;
		case TON_PAD_SLOW:      imber_gen::genPadSlow(rng, sr, b); break;
		case TON_PAD_CLUSTER:   imber_gen::genPadCluster(rng, sr, b); break;
		case TON_CHORD_STAB:    imber_gen::genFragChordStab(rng, sr, b); break;
		case TON_STRETCHED:     imber_gen::genDroneStretched(rng, sr, b); break;
		case TON_PAD_TAPE:      imber_gen::genAmbientTapePad(rng, sr, b); break;
		case TON_AIR_FILTERED:  imber_gen::genDroneFiltNoise(rng, sr, b); break;
		case TON_AIR_WASH:      imber_gen::genAmbientWash(rng, sr, b); break;
		case TON_AIR_VOWEL:     imber_gen::genVowelDrone(rng, sr, b); break;
		default:                imber_gen::genDroneComb(rng, sr, b); break;
	}
	imber_dsp::dirtify(b, rng, sr, rng.range(0.f, 0.25f));

	// the stab generator scatters one to three hits through a loop-length
	// buffer: keep the first hit alone
	trimToFirstEvent(b, sr, fam == TON_CHORD_STAB);

	size_t maxN = (size_t)(sr * 2.5f);
	if (b.size() > maxN)
		b.resize(maxN);
	imber_dsp::safetyClip(b);

	int head = (int)(0.003f * sr);
	for (int i = 0; i < head && i < (int)b.size(); i++)
		b[i] *= (float)i / head;
	int tail = (int)std::min((float)b.size(), 0.05f * sr);
	for (int i = 0; i < tail; i++)
		b[b.size() - 1 - i] *= (float)i / tail;

	spread(b, rng, sr, fam != TON_CHORD_STAB, L, R);
	normalizeStereo(L, R, 0.9f);
}

// ── a bank ────────────────────────────────────────────────────────────────────

struct Bank {
	std::string name;
	std::string sampleNames[kSamplesPerBank];
	std::vector<float> L[kSamplesPerBank];
	std::vector<float> R[kSamplesPerBank];
};

struct BankSet {
	Bank banks[kNumBanks];
	uint64_t seed = 0;
	float sr = 44100.f;
	bool ready = false;
};

inline const char* bankName(int b) {
	static const char* names[kNumBanks] =
		{"drums", "objects", "grains", "micro", "tones", "air"};
	return names[(b < 0 || b >= kNumBanks) ? 0 : b];
}

// Slot -> family: a shuffled family list dealt round-robin, so with eight
// families every slot is a different one and with fewer the repeats are as
// even as they can be.
inline void dealFamilies(const int* families, int nFam, Rng& rng, int* out) {
	std::vector<int> pool(families, families + nFam);
	for (int i = nFam - 1; i > 0; i--)
		std::swap(pool[i], pool[rng.irange(0, i)]);
	for (int s = 0; s < kSamplesPerBank; s++) {
		if (s % nFam == 0 && s > 0)   // reshuffle each full pass
			for (int i = nFam - 1; i > 0; i--)
				std::swap(pool[i], pool[rng.irange(0, i)]);
		out[s] = pool[s % nFam];
	}
}

// Build one bank. `progress` counts finished samples across the whole set.
inline void buildBank(Bank& bank, int b, uint64_t seed, float sr,
                      std::atomic<int>* progress, std::atomic<bool>* abort) {
	bank.name = bankName(b);
	Rng rng;
	rng.seed(seed ^ ((uint64_t)(b + 1) * 0x9e3779b97f4a7c15ull));

	int fams[kSamplesPerBank];
	if (b < 4) {
		int nFam = vates_kits::familyCount(b);
		std::vector<int> ids(nFam);
		for (int i = 0; i < nFam; i++)
			ids[i] = i;
		dealFamilies(ids.data(), nFam, rng, fams);
	}
	else if (b == 4)
		dealFamilies(kToneFamilies, (int)(sizeof(kToneFamilies) / sizeof(int)), rng, fams);
	else
		dealFamilies(kAirFamilies, (int)(sizeof(kAirFamilies) / sizeof(int)), rng, fams);

	for (int s = 0; s < kSamplesPerBank; s++) {
		if (abort && abort->load())
			return;
		// each slot gets its own stream, so one slot's draws never shift
		// another's, and its own column position in the family's register
		Rng srng;
		srng.seed(seed ^ ((uint64_t)(b * 97 + s + 1) * 0xda942042e4dd58b5ull));
		if (b < 4) {
			float spread = srng.uniform();
			vates_kits::render(b, fams[s], spread, srng, sr, bank.L[s], bank.R[s]);
			bank.sampleNames[s] = vates_kits::familyName(b, fams[s]);
		}
		else {
			renderTonal(fams[s], srng, sr, bank.L[s], bank.R[s]);
			bank.sampleNames[s] = tonalFamilyName(fams[s]);
		}
		if (progress)
			progress->fetch_add(1);
	}
}

inline void build(BankSet& set, uint64_t seed, float sr,
                  std::atomic<int>* progress, std::atomic<bool>* abort) {
	set.seed = seed;
	set.sr = sr;
	set.ready = false;
	for (int b = 0; b < kNumBanks; b++) {
		if (abort && abort->load())
			return;
		buildBank(set.banks[b], b, seed, sr, progress, abort);
	}
	set.ready = true;
}

static const int kTotalSamples = kNumBanks * kSamplesPerBank;

}   // namespace vates_bank
