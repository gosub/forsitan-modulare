// artifex_probe — measure and audition the nine effects.
//
//   artifex_probe sweep            every mode against a fixed test signal
//   artifex_probe mode <n>         one mode across the time and amount knobs
//   artifex_probe wav <dir>        write each mode as a stereo WAV to listen to
//   artifex_probe scene <dir> [hz] [speed] [amount]
//                                  render one replayer scenario to a WAV, so a
//                                  click heard in Rack can be compared against
//                                  the same settings driven offline
//   artifex_probe measure          the numbers the listening tests used to ask
//                                  a human for: overwrite times, the record
//                                  head's whine, what self-oscillates, CPU
//
// No Rack: the FX core is header-only, so this drives it straight.
#include "../src/artifex/core.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <utility>
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


// ── measurements ────────────────────────────────────────────────────────────
// Anything that produces a number rather than a verdict belongs here and not
// in a listening test: measured by hand it is measured once, against whatever
// the build happened to be that afternoon, and is stale by the next commit.

// Drive a core with a steady tone, returning the last output.
static float toneRun(Core& core, Ctl& ct, float sr, double& phase, float hz,
                     float amp, long n, float* outPeak = nullptr) {
	float last = 0.f;
	double peak = 0.0;
	for (long i = 0; i < n; i++) {
		phase += hz / sr;
		phase -= std::floor(phase);
		float x = amp * std::sin(2.f * (float)M_PI * (float)phase);
		float a = 0.f, b = 0.f;
		core.process(ct, x, x, a, b);
		last = a;
		peak = std::max(peak, (double)std::fabs(a));
	}
	if (outPeak) *outPeak = (float)peak;
	return last;
}

static Core* makeCore(float sr, float seconds, int mode) {
	Core* c = new Core();
	c->setRates(sr, seconds);
	c->enterMode(mode);
	c->lastMode = mode;
	return c;
}

// How long the replayer takes to overwrite what is on its tape. Record a tone,
// lock it, then overdub silence at `amt` and time how many laps it takes for
// the original to fall 40 dB.
static void measureOverwrite(float sr) {
	printf("\nreplayer: laps to erase the tape (original down 40 dB)\n");
	printf("  buffer |  amount 0.75  amount 0.50  amount 0.25\n");
	const float bufs[3] = {1.15f, 2.5f, 5.f};
	const float amts[3] = {0.75f, 0.5f, 0.25f};
	for (int b = 0; b < 3; b++) {
		printf("  %4.2f s |", bufs[b]);
		for (int a = 0; a < 3; a++) {
			Core* c = makeCore(sr, bufs[b], MODE_REPLAYER);
			Ctl ct;
			ct.mode = MODE_REPLAYER;
			ct.dt = 1.f / sr;
			ct.time = 5.f / 6.f;          // 1x
			ct.amount = 1.f;
			ct.feedback = 0.f;
			double ph = 0.0;
			long lap = c->tape[0].size();
			toneRun(*c, ct, sr, ph, 220.f, 2.f, 3 * lap);   // fill and lock
			float ref = 0.f;
			toneRun(*c, ct, sr, ph, 220.f, 2.f, lap, &ref);
			ct.amount = amts[a];                            // overdub silence
			int laps = 0;
			float pk = ref;
			for (; laps < 400 && pk > ref * 0.01f; laps++) {
				pk = 0.f;
				for (long i = 0; i < lap; i++) {
					float x = 0.f, o = 0.f, o2 = 0.f;
					c->process(ct, x, x, o, o2);
					pk = std::max(pk, std::fabs(o));
				}
			}
			double secs = laps * (double)lap / sr;
			if (laps >= 400)
				printf("      >400 laps");
			else
				printf("   %3d (%5.1fs)", laps, secs);
			delete c;
		}
		printf("\n");
	}
}

// The replayer's record head drops each sample into one slot while the play
// head reads between two. The interpolation error is periodic in the read
// position's fractional part, which advances by frac(|speed|) each sample, so
// the mismatch is a tone at frac(|speed|) x sample rate folded about Nyquist
// -- above 1x that is |speed - 1| x sr, but below it the tone has aliased and
// |speed - 1| names its image rather than the tone. Measured as the energy at
// that frequency against the tone the tape is carrying.
static void measureRecordHead(float sr) {
	printf("\nreplayer: the record head's whine, amount 0.5, 220 Hz in\n");
	printf("   knob |  speed  |  whine Hz |  level vs signal\n");
	const float knobs[6] = {0.15f, 0.20f, 0.50f, 0.80f, 0.85f, 0.95f};
	for (int k = 0; k < 6; k++) {
		Core* c = makeCore(sr, 1.15f, MODE_REPLAYER);
		Ctl ct;
		ct.mode = MODE_REPLAYER;
		ct.dt = 1.f / sr;
		ct.time = knobs[k];
		ct.amount = 1.f;
		ct.feedback = 0.f;
		double ph = 0.0;
		long lap = c->tape[0].size();
		toneRun(*c, ct, sr, ph, 220.f, 2.f, 3 * lap);
		float speed = c->uiTime;
		ct.amount = 0.5f;
		// collect a second of overdub and pick out the mismatch frequency
		long n = (long)sr;
		std::vector<float> y;
		y.reserve(n);
		for (long i = 0; i < n; i++) {
			ph += 220.0 / sr;
			ph -= std::floor(ph);
			float x = 2.f * std::sin(2.f * (float)M_PI * (float)ph);
			float a = 0.f, b = 0.f;
			c->process(ct, x, x, a, b);
			y.push_back(a);
		}
		float frac = std::fabs(speed) - std::floor(std::fabs(speed));
		float whine = frac * sr;
		if (whine > 0.5f * sr)
			whine = sr - whine;                     // it aliased
		double re = 0, im = 0, tot = 0;
		bool has = whine > 20.f && whine < 0.49f * sr;
		if (has) {
			double w = 2.0 * M_PI * whine / sr;
			for (long i = 0; i < n; i++) {
				re += y[i] * std::cos(w * i);
				im += y[i] * std::sin(w * i);
			}
		}
		for (long i = 0; i < n; i++)
			tot += (double)y[i] * y[i];
		double mag = 2.0 * std::sqrt(re * re + im * im) / n;
		double rms = std::sqrt(tot / n);
		printf("   %4.2f  | %+6.3fx |", knobs[k], speed);
		if (has)
			printf("  %8.0f |  %+6.1f dB\n", whine,
			       20.0 * std::log10(std::max(mag, 1e-12) / std::max(rms, 1e-12)));
		else
			printf("       n/a |  (at or above Nyquist)\n");
		delete c;
	}
}

// Envelope steps in the replayer's loop, which is the artifact a residual
// detector cannot see. Everything else here looks for curvature over three
// samples, so it finds sharp transients and is deaf to a slow disturbance --
// and the seam is slow: it reads as a lump once a lap rather than a tick.
// Reported by ear as "not a high-pitch click, not a smooth tone", and it
// measured -93 dB by residual while stepping the envelope by a third.
//
// The composite is periodic at the input frequency (the dry tone, plus the
// tape an octave up at 2x), so a peak per input period is an envelope with no
// beat in it. What shows up is the tape's own seam: at 220 Hz the lap is
// 253.018 cycles and the take joins itself, at 110 Hz it is 126.509 and lands
// half a cycle out, so the overdub adds on one side and cancels on the other.
static void measureLoopEnvelope(float sr) {
	printf("\nreplayer: envelope steps around the loop, +2x, amount 0.9\n");
	printf("  the seam is only crossfaded when locked, so overdubbing lets it\n");
	printf("  through once a play lap -- worst step, and where it sits\n");
	printf("   Hz   cycles/lap   worst step   per lap\n");
	const float hz[6] = {55.f, 110.f, 113.7f, 220.f, 440.f, 909.1f};
	for (int k = 0; k < 6; k++) {
		Core* core = makeCore(sr, kHardwareBuffer, MODE_REPLAYER);
		Ctl ct;
		ct.mode = MODE_REPLAYER;
		ct.dt = 1.f / sr;
		ct.time = 1.f;                       // +2x
		ct.amount = 1.f;
		ct.feedback = 0.f;
		double ph = 0.0;
		std::vector<float> y;
		long fill = (long)(4.0 * sr), hold = (long)(12.0 * sr);
		for (long i = 0; i < fill + hold; i++) {
			if (i == fill)
				ct.amount = 0.9f;
			ph += hz[k] / sr;
			ph -= std::floor(ph);
			float x = 2.f * std::sin(2.f * (float)M_PI * (float)ph);
			float a = 0.f, b = 0.f;
			core->process(ct, x, x, a, b);
			if (i >= fill + (long)(0.5 * sr))
				y.push_back(a);
		}
		size_t per = (size_t)(sr / hz[k]);
		std::vector<double> env;
		for (size_t i = 0; i + per < y.size(); i += per / 4) {
			double p = 0.0;
			for (size_t j = i; j < i + per; j++)
				p = std::max(p, (double)std::fabs(y[j]));
			env.push_back(p);
		}
		double worst = 0.0;
		for (size_t j = 1; j < env.size(); j++)
			worst = std::max(worst,
			                 std::fabs(env[j] - env[j - 1]) / std::max(env[j - 1], 1e-9));
		int nt = core->tape[0].size();
		printf("  %6.1f  %10.3f   %8.1f%%   %7.3f s\n", hz[k],
		       (double)hz[k] * nt / sr, 100.0 * worst, nt / 2.0 / sr);
		delete core;
	}
}

// What the filter and the stereo knob actually do in each mode. The suite
// asserts the filter's reach and its placement options, and that stereo is
// mono at zero, but not that every mode answers both knobs -- which is the
// one part of the shared loop a listening test was still being asked for.
static void measureSharedLoop(float sr) {
	printf("\nthe shared loop, per mode: what each knob is worth\n");
	printf("  mode      | filter L..R | stereo width | feedback\n");
	for (int m = 0; m < MODE_COUNT; m++) {
		auto bright = [&](float filt, float stereo, float fb) {
			Core* c = makeCore(sr, kHardwareBuffer, m);
			Ctl ct;
			ct.mode = m;
			ct.dt = 1.f / sr;
			ct.time = 0.35f;
			// Fully wet: the dry is never filtered, so any of it in the mix
			// dilutes the reading rather than telling you about the filter.
			ct.amount = 1.f;
			ct.feedback = fb;
			ct.filter = filt;
			ct.stereo = stereo;
			ct.stepSeconds = 0.125f;
			double ph = 0.0, lo = 0.0, loE = 0.0, hiE = 0.0, sE = 0.0, dE = 0.0;
			double k = 1.0 - std::exp(-2.0 * M_PI * 1500.0 / sr);
			uint32_t rng = 99991u;
			for (long i = 0; i < (long)(4.0 * sr); i++) {
				// noise bursts on a beat: something for every mode to bite on
				rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
				float n = ((float)(rng >> 8) / 8388608.f - 1.f);
				ph += 2.0 / sr;
				if (ph >= 1.0) ph -= 1.0;
				float env = ph < 0.1 ? 1.f - (float)(ph / 0.1) : 0.f;
				float x = n * 2.f * env;
				ct.stepped = (i % (long)(0.125 * sr)) == 0;
				if (ct.stepped)
					ct.step = (ct.step + 1) % 16;
				float a = 0.f, b = 0.f;
				c->process(ct, x, x, a, b);
				if (i > (long)(1.0 * sr)) {
					lo += (a - lo) * k;
					loE += lo * lo;
					hiE += (a - lo) * (a - lo);
					double sm = 0.5 * (a + b), df = 0.5 * (a - b);
					sE += sm * sm; dE += df * df;
				}
			}
			delete c;
			double br = 10.0 * std::log10((hiE + 1e-12) / (loE + 1e-12));
			double wd = std::sqrt(dE / std::max(sE, 1e-12));
			return std::make_pair(br, wd);
		};
		// Four modes put the filter inside the global loop rather than on a
		// wet path, so it does nothing there until the loop is carrying
		// something: measured with feedback well up, as the mode asks.
		const float fb = 0.8f;
		double dark = bright(-1.f, 0.f, fb).first;
		double open = bright(0.f, 0.f, fb).first;
		double thin = bright(1.f, 0.f, fb).first;
		double w0 = bright(0.f, 0.f, fb).second;
		double w1 = bright(0.f, 1.f, fb).second;
		double fb0 = bright(0.f, 0.f, 0.f).first;
		printf("  %-9s | %5.1f dB     | %.3f -> %.3f | %+5.1f dB\n",
		       kNames[m], thin - dark, w0, w1, open - fb0);
		(void)open;
	}
	printf("  (filter L..R is how far the knob moves the brightness, fully\n");
	printf("   wet; stereo width is 0 for mono; feedback is what turning it\n");
	printf("   up adds. The replayer reads nought for the filter because its\n");
	printf("   feedback goes into the tape, and a locked tape is not\n");
	printf("   recording -- that is the mode working, not the filter failing.)\n");
}

// Which modes keep going with no input and the feedback wide open.
static void measureSelfOscillation(float sr) {
	printf("\nself-oscillation: feedback 1.0, limiter off, silence in\n");
	printf("  mode      | after 2 s | after 10 s | verdict\n");
	for (int m = 0; m < MODE_COUNT; m++) {
		Core* c = makeCore(sr, 1.15f, m);
		Ctl ct;
		ct.mode = m;
		ct.dt = 1.f / sr;
		ct.time = 0.5f;
		ct.amount = 1.f;
		ct.feedback = 1.f;
		ct.limiter = false;
		double ph = 0.0;
		// a second of tone to excite it, then silence for good
		toneRun(*c, ct, sr, ph, 220.f, 2.f, (long)sr);
		auto quietRun = [&](long n) {
			double pk = 0;
			for (long i = 0; i < n; i++) {
				float a = 0.f, b = 0.f;
				c->process(ct, 0.f, 0.f, a, b);
				if (std::isfinite(a))
					pk = std::max(pk, (double)std::fabs(a));
			}
			return pk;
		};
		double at2 = quietRun((long)(2.0 * sr));
		double at10 = quietRun((long)(8.0 * sr));
		const char* verdict = at10 > 0.05 ? (at10 >= at2 * 0.9 ? "sustains" : "decaying")
		                                  : "needs input";
		printf("  %-9s | %9.3f | %10.3f | %s\n", kNames[m], at2, at10, verdict);
		delete c;
	}
}

// What each mode costs, as a multiple of real time.
static void measureCpu(float sr) {
	printf("\nCPU: process() cost, 10 s of audio per mode\n");
	printf("  mode      | 1.15 s buffer |  5 s buffer\n");
	for (int m = 0; m < MODE_COUNT; m++) {
		printf("  %-9s |", kNames[m]);
		for (int b = 0; b < 2; b++) {
			Core* c = makeCore(sr, b ? 5.f : 1.15f, m);
			Ctl ct;
			ct.mode = m;
			ct.dt = 1.f / sr;
			ct.time = 0.5f;
			ct.amount = 0.7f;
			ct.feedback = 0.4f;
			ct.stereo = 0.5f;
			double ph = 0.0;
			long n = (long)(10.0 * sr);
			clock_t t0 = clock();
			toneRun(*c, ct, sr, ph, 220.f, 2.f, n);
			double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
			printf("  %11.2f%%", 100.0 * secs / 10.0);
			delete c;
		}
		printf("\n");
	}
	printf("  (one core, percentage of real time for a stereo voice)\n");
}


// Render one replayer scenario to a file. The point is to settle whether a
// click heard in Rack is in the DSP or in the patch around it: this drives the
// same core with nothing else in the signal path -- no host, no blocks, no
// other modules -- so if the file is clean and Rack is not, the difference is
// upstream of the core.
static void renderScene(const std::string& dir, float sr, float hz,
                        float timeKnob, float amount) {
	Core core;
	core.setRates(sr, kHardwareBuffer);
	core.enterMode(MODE_REPLAYER);
	core.lastMode = MODE_REPLAYER;
	Ctl ct;
	ct.mode = MODE_REPLAYER;
	ct.dt = 1.f / sr;
	ct.time = timeKnob;
	ct.amount = 1.f;              // locked while it fills
	ct.feedback = 0.f;
	std::vector<float> L, R;
	double ph = 0.0;
	long fill = (long)(4.0 * sr), hold = (long)(16.0 * sr);
	// the amount knob arrives the way a mouse delivers it, once a frame
	long framePeriod = (long)(sr / 60.f);
	long dragN = (long)(0.25 * sr);
	for (long i = 0; i < fill + hold; i++) {
		if (i >= fill) {
			long k = i - fill;
			if (k < dragN && (k % framePeriod) == 0)
				ct.amount = 1.f - (1.f - amount) * ((float)k / (float)dragN);
			else if (k == dragN)
				ct.amount = amount;
		}
		ph += hz / sr;
		ph -= std::floor(ph);
		float x = 2.f * std::sin(2.f * (float)M_PI * (float)ph);
		float a = 0.f, b = 0.f;
		core.process(ct, x, x, a, b);
		if (i >= fill) {
			L.push_back(a);
			R.push_back(b);
		}
	}
	char name[256];
	std::snprintf(name, sizeof name, "%s/replayer-%.0fhz-%+.2fx-amt%.0f.wav",
	              dir.c_str(), hz, core.uiTime, amount * 100.f);
	writeWav(name, L, R, sr);
	printf("  %s   (%.1f s, tape %d, %.3f cycles a lap)\n", name,
	       L.size() / sr, core.tape[0].size(),
	       (double)hz * core.tape[0].size() / sr);
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

	if (what == "scene") {
		std::string dir = argc > 2 ? argv[2] : ".";
		float hz = argc > 3 ? (float)atof(argv[3]) : 0.f;
		float knob = argc > 4 ? (float)atof(argv[4]) : 1.f;
		float amt = argc > 5 ? (float)atof(argv[5]) : 0.9f;
		printf("replayer scenes (locked, then amount dragged to %.0f%%):\n",
		       amt * 100.f);
		if (hz > 0.f)
			renderScene(dir, sr, hz, knob, amt);
		else {
			// the reported pair: one tone that joins its own lap and one
			// that lands half a cycle out
			renderScene(dir, sr, 110.f, knob, amt);
			renderScene(dir, sr, 220.f, knob, amt);
		}
		return 0;
	}

	if (what == "measure") {
		measureOverwrite(sr);
		measureRecordHead(sr);
		measureLoopEnvelope(sr);
		measureSharedLoop(sr);
		measureSelfOscillation(sr);
		measureCpu(sr);
		return 0;
	}

	printf("usage: artifex_probe [sweep|mode <n>|wav <dir>|measure|"
	       "scene <dir> [hz] [timeknob] [amount]]\n");
	return 2;
}
