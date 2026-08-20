// artifex_probe — measure and audition the nine effects.
//
//   artifex_probe sweep            every mode against a fixed test signal
//   artifex_probe mode <n>         one mode across the time and amount knobs
//   artifex_probe wav <dir>        write each mode as a stereo WAV to listen to
//
// No Rack: the FX core is header-only, so this drives it straight.
#include "../src/artifex/core.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace artifex_fx;

static const char* kNames[MODE_COUNT] = {
	"delay", "flanger", "freezer", "panner", "crusher",
	"slicer", "pitcher", "replayer", "shifter",
};

// A test signal with something for every mode to bite on: a plucked tone on
// the beat, a noise burst off it, and a slow sweep underneath.
static void source(std::vector<float>& L, std::vector<float>& R, float sr, float seconds) {
	long n = (long)(seconds * sr);
	L.assign(n, 0.f);
	R.assign(n, 0.f);
	uint32_t rng = 12345u;
	float env = 0.f, nenv = 0.f;
	float beat = 0.5f;
	for (long i = 0; i < n; i++) {
		float t = (float)i / sr;
		float ph = std::fmod(t, beat) / beat;
		if (ph < 1e-3f)
			env = 1.f;
		if (std::fabs(ph - 0.5f) < 1e-3f)
			nenv = 1.f;
		env *= std::exp(-1.f / (0.25f * sr));
		nenv *= std::exp(-1.f / (0.05f * sr));
		rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
		float noise = ((rng >> 9) & 0xffff) / 32768.f - 1.f;
		float tone = std::sin(2.f * (float)M_PI * 220.f * t)
		             + 0.4f * std::sin(2.f * (float)M_PI * 660.f * t);
		float slow = 0.35f * std::sin(2.f * (float)M_PI * (80.f + 40.f * std::sin(t)) * t);
		float x = 2.2f * (tone * env * 0.6f + noise * nenv * 0.5f + slow);
		L[i] = x;
		R[i] = x;
	}
}

struct Result {
	float peak = 0.f;
	double rms = 0.0;
	double width = 0.0;
	long nans = 0;
	double diff = 0.0;      // how far the output is from the dry input
};

static Result run(int mode, float time, float amount, float feedback,
                  float stereo, float sr, float seconds,
                  std::vector<float>* outL = NULL, std::vector<float>* outR = NULL) {
	std::vector<float> L, R;
	source(L, R, sr, seconds);
	Core core;
	core.setRates(sr, kHardwareBuffer);
	Ctl ct;
	ct.mode = mode;
	ct.time = time;
	ct.amount = amount;
	ct.feedback = feedback;
	ct.stereo = stereo;
	ct.dt = 1.f / sr;
	ct.stepSeconds = 0.125f;

	Result r;
	double sum = 0.0, sumDiff = 0.0, sumS = 0.0, sumD = 0.0;
	long n = (long)L.size();
	double stepPhase = 0.0;
	for (long i = 0; i < n; i++) {
		stepPhase += ct.dt / ct.stepSeconds;
		ct.stepped = false;
		if (stepPhase >= 1.0) {
			stepPhase -= 1.0;
			ct.stepped = true;
			ct.step = (ct.step + 1) % 16;
		}
		float a = 0.f, b = 0.f;
		core.process(ct, L[i], R[i], a, b);
		if (!std::isfinite(a) || !std::isfinite(b)) {
			r.nans++;
			a = b = 0.f;
		}
		if (outL) outL->push_back(a);
		if (outR) outR->push_back(b);
		r.peak = std::max(r.peak, std::max(std::fabs(a), std::fabs(b)));
		sum += (double)a * a + (double)b * b;
		sumDiff += (double)(a - L[i]) * (a - L[i]);
		double s = 0.5 * (a + b), d = 0.5 * (a - b);
		sumS += s * s;
		sumD += d * d;
	}
	r.rms = std::sqrt(sum / std::max(1L, 2 * n));
	r.width = std::sqrt(sumD / std::max(sumS, 1e-12));
	r.diff = std::sqrt(sumDiff / std::max(1L, n));
	return r;
}

static void line(const char* label, const Result& r) {
	printf("  %-26s peak %6.3f  rms %6.3f  width %4.2f  vs dry %6.3f%s%s\n",
	       label, r.peak, r.rms, r.width, r.diff,
	       r.nans ? "   <-- NAN" : "",
	       r.rms < 0.02 ? "   <-- QUIET" : "");
}

static void writeWav(const std::string& path, const std::vector<float>& L,
                     const std::vector<float>& R, float sr) {
	FILE* f = fopen(path.c_str(), "wb");
	if (!f) {
		printf("cannot write %s\n", path.c_str());
		return;
	}
	uint32_t n = (uint32_t)L.size(), bytes = n * 4;
	auto w32 = [&](uint32_t v) { fputc(v & 255, f); fputc((v >> 8) & 255, f);
	                             fputc((v >> 16) & 255, f); fputc((v >> 24) & 255, f); };
	auto w16 = [&](uint16_t v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); };
	fwrite("RIFF", 1, 4, f); w32(36 + bytes); fwrite("WAVE", 1, 4, f);
	fwrite("fmt ", 1, 4, f); w32(16); w16(1); w16(2); w32((uint32_t)sr);
	w32((uint32_t)sr * 4); w16(4); w16(16);
	fwrite("data", 1, 4, f); w32(bytes);
	for (uint32_t i = 0; i < n; i++) {
		w16((uint16_t)(int16_t)lrintf(std::max(-1.f, std::min(1.f, L[i] * 0.2f)) * 32767.f));
		w16((uint16_t)(int16_t)lrintf(std::max(-1.f, std::min(1.f, R[i] * 0.2f)) * 32767.f));
	}
	fclose(f);
}

int main(int argc, char** argv) {
	const float sr = 48000.f;
	std::string what = argc > 1 ? argv[1] : "sweep";

	if (what == "sweep") {
		printf("artifex: every mode, time 0.5, amount 1.0, feedback 0.35, stereo 0.5\n");
		for (int m = 0; m < MODE_COUNT; m++) {
			char label[64];
			snprintf(label, sizeof label, "%d %s", m + 1, kNames[m]);
			line(label, run(m, 0.5f, 1.f, 0.35f, 0.5f, sr, 4.f));
		}
		printf("\ndry check: amount 0 has to give the input back\n");
		for (int m = 0; m < MODE_COUNT; m++) {
			char label[64];
			snprintf(label, sizeof label, "%d %s", m + 1, kNames[m]);
			line(label, run(m, 0.5f, 0.f, 0.f, 0.f, sr, 1.f));
		}
		return 0;
	}

	if (what == "mode" && argc > 2) {
		int m = atoi(argv[2]) - 1;
		if (m < 0 || m >= MODE_COUNT) {
			printf("mode 1..9\n");
			return 2;
		}
		printf("artifex mode %d %s: time across, amount down\n", m + 1, kNames[m]);
		for (int a = 0; a <= 4; a++)
			for (int t = 0; t <= 4; t++) {
				char label[64];
				snprintf(label, sizeof label, "amount %.2f time %.2f", a * 0.25f, t * 0.25f);
				line(label, run(m, t * 0.25f, a * 0.25f, 0.3f, 0.4f, sr, 3.f));
			}
		return 0;
	}

	if (what == "wav") {
		std::string dir = argc > 2 ? argv[2] : ".";
		for (int m = 0; m < MODE_COUNT; m++) {
			std::vector<float> L, R;
			run(m, 0.5f, 1.f, 0.35f, 0.5f, sr, 6.f, &L, &R);
			char path[512];
			snprintf(path, sizeof path, "%s/%d_%s.wav", dir.c_str(), m + 1, kNames[m]);
			writeWav(path, L, R, sr);
		}
		std::vector<float> L, R;
		run(0, 0.5f, 0.f, 0.f, 0.f, sr, 6.f, &L, &R);
		writeWav(dir + "/0_dry.wav", L, R, sr);
		printf("wrote %d files to %s\n", MODE_COUNT + 1, dir.c_str());
		return 0;
	}

	printf("usage: artifex_probe [sweep|mode <n>|wav <dir>]\n");
	return 2;
}
