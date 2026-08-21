#include "forsitan.hpp"
#include "sampler/kitloader.hpp"
#include "vates/bank.hpp"
#include "imber/imber_worker.hpp"
#include "shared/dsp.hpp"
#include "shared/modulation.hpp"
#include "position_switch.hpp"

#include <osdialog.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// vates — stereo sample player with a pattern generator underneath.
//
// After a Bastl Instruments hardware sampler, named and credited in
// doc/vates.md: you do not draw a rhythm, you modulate sample selection and
// let the rhythm fall out. The hardware's two modifier buttons (SHIFT and
// BANK, which give every knob two or three jobs) are unpacked here into real
// controls, since holding one thing while turning another is a gesture a
// mouse does badly.
//
// Six banks of eight samples are generated from a seed — drums, objects,
// grains, micro, tones, air (src/vates/bank.hpp) — one sample per kind the
// generator knows, so a knob position always means the same role. User kits
// follow, read from the kits folder shared with pellicula. See doc/vates.md.

namespace {

using forsitan_sampler::Sample;
using forsitan_sampler::Kit;

// Generated banks are always rendered at this rate and resampled on
// playback, exactly as a user's wav files are: the alternative is
// regenerating 48 samples every time the engine rate changes.
static const float kGenRate = 44100.f;

static const int kSteps = forsitan_mod::kSteps;

using forsitan_dsp::Svf;
using forsitan_dsp::Delay;
using forsitan_mod::rhythmPattern;
using forsitan_mod::rhythmCv;

}   // namespace

struct Vates : Module {
	enum ParamId {
		BANK_DOWN_PARAM,
		BANK_UP_PARAM,
		BANK_ATT_PARAM,
		SAMPLE_PARAM,
		SAMPLE_ATT_PARAM,
		MODE_PARAM,
		TRIG_PARAM,
		PITCH_PARAM,
		PITCH_ATT_PARAM,
		LENGTH_PARAM,
		LENGTH_ATT_PARAM,
		FILTER_PARAM,
		FX_PARAM,
		SYNC_PARAM,
		RATE_PARAM,
		LFO_ATT_PARAM,
		TEMPO_PARAM,
		RHYTHM_PARAM,
		GSW_PARAM,
		CSW_PARAM,
		LEVEL_PARAM,
		PWM_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		BANK_INPUT,
		SAMPLE_INPUT,
		TRIG_INPUT,
		FREE_INPUT,
		NOTE_INPUT,
		LENGTH_INPUT,
		FILTER_INPUT,
		FX_INPUT,
		LFO_INPUT,
		LFO_RESET_INPUT,
		CLK_INPUT,
		G_INPUT,
		C_INPUT,
		PAT_RESET_INPUT,
		RHYTHM_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		ENV_OUTPUT,
		TRI_OUTPUT,
		PULSE_OUTPUT,
		SAW_OUTPUT,
		CLK_OUTPUT,
		GATE_OUTPUT,
		CV_OUTPUT,
		LEFT_OUTPUT,
		RIGHT_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		LEFT_LIGHT,
		RIGHT_LIGHT,
		LIGHTS_LEN
	};

	// ── generated banks ──────────────────────────────────────────────────────
	struct GenJob {
		vates_bank::BankSet set;
		std::atomic<int> progress;
		std::atomic<bool> done;
		std::atomic<bool> failed;
		std::atomic<bool> abort;
		GenJob() : progress(0), done(false), failed(false), abort(false) {}
	};
	std::shared_ptr<GenJob> genJob;
	std::shared_ptr<vates_bank::BankSet> banks;
	uint64_t bankSeed = 0x5eedbaadull;
	bool pendingGen = true;

	// ── user kits ────────────────────────────────────────────────────────────
	struct KitJob {
		Kit kit;
		std::atomic<bool> done;
		std::atomic<bool> abort;
		KitJob() : done(false), abort(false) {}
	};
	std::shared_ptr<KitJob> kitJob;
	std::shared_ptr<Kit> userKit;
	std::vector<std::string> kitNames;
	std::string loadingKit;
	int loadedKitIndex = -1;

	// A user kit is paged into banks the size of a generated one, which is
	// how the hardware organises its own material: eight samples to a bank,
	// more banks for more sounds. It is also what keeps play mode usable —
	// crossings per LFO cycle are twice the samples the CV spans, so a bank
	// that grew to 64 would fire 128 times where the hardware fires 16.
	//
	// The table is built on the UI thread and read on the audio thread, so it
	// is plain ints and char arrays published behind one atomic count: no
	// allocation to race with, and a stale read is a clamped index for one
	// frame rather than a freed pointer.
	static const int kMaxUserPages = 256;
	static const int kMaxBanks = vates_bank::kNumBanks + kMaxUserPages;
	struct PageInfo {
		int kit = -1;      // index into kitNames
		int first = 0;     // first sample of the kit this page holds
		int count = 0;     // how many
		char name[40] = "";
	};
	PageInfo pages[kMaxBanks];
	std::atomic<int> bankTotal;
	int samplesPerBank = vates_bank::kSamplesPerBank;   // 0 = a kit is one bank

	// ── quantizer ────────────────────────────────────────────────────────────
	int rootNote = 0;                     // 0-11, C..B
	int scaleIndex = imber_dsp::kDefaultScale;
	bool honourExternalClock = true;
	// Which voltage window the pattern inputs read. The hardware's is 0-5 V
	// logic — below 1.6 V inverts — which in Rack means a gate resting at 0 V
	// inverts the pattern continuously until it goes high. The default here
	// is the Rack reading: zero is neutral, positive randomizes, negative
	// inverts.
	bool hardwareCvWindow = false;

	// ── voice ────────────────────────────────────────────────────────────────
	std::shared_ptr<void> voiceHold;      // keeps the bank or kit alive
	const std::vector<float>* voiceL = nullptr;
	const std::vector<float>* voiceR = nullptr;
	double voicePos = 0.0;
	float voiceRate = 1.f;                // samples per engine frame
	float voiceSrcRate = kGenRate;
	bool voiceActive = false;
	bool voiceReverse = false;
	bool voiceAttack = false;             // in the rising phase of a reverse hit
	float env = 0.f, envCoef = 0.f;
	bool envHold = false;                 // no-decay: play to the end
	float release = 1.f;

	// ── selection ────────────────────────────────────────────────────────────
	int bankIndex = 0;
	int bankBase = 0;          // what the buttons set; CV offsets it
	int sampleIndex = 0;
	int aimedSample = 0;
	int prevSampleKnob = -1;
	// What the display shows. Plain char buffers written by the audio thread
	// only when the text actually changes: the widget reads them from the UI
	// thread, and having it walk `banks` and the kit-name vector instead
	// would be a race against the swap that installs a freshly built set.
	char uiBankText[48] = "vates";
	char uiSampleText[48] = "";
	int uiBankShown = -1, uiSampleShown = -1, uiProgressShown = -1;
	float notePitch = 0.f;                // latched, quantized
	int uiBank = 0, uiSample = 0;

	// ── clock, LFO, pattern ──────────────────────────────────────────────────
	// The tempo generator, the pattern generator and the LFO are the section
	// artifex shares: on the hardware the two panels are one board.
	forsitan_mod::Modulation modul;

	// ── fx ───────────────────────────────────────────────────────────────────
	Svf filt[2], filtB[2];
	float filtSmooth = 0.f;
	bool filtWasLow = false;
	Delay dly[2], mod[2];
	float dlyFb[2] = {0.f, 0.f};
	float modPhase = 0.f;

	dsp::SchmittTrigger trigIn;
	dsp::BooleanTrigger trigButton, bankUpButton, bankDownButton;

	Vates() : bankTotal(vates_bank::kNumBanks) {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

		configButton(BANK_DOWN_PARAM, "Previous bank");
		configButton(BANK_UP_PARAM, "Next bank");
		configParam(BANK_ATT_PARAM, -1.f, 1.f, 0.f, "Bank CV", "%", 0.f, 100.f);
		configParam(SAMPLE_PARAM, 0.f, 1.f, 0.f, "Sample");
		configParam(SAMPLE_ATT_PARAM, -1.f, 1.f, 0.f, "Sample CV", "%", 0.f, 100.f);
		// The panel labels this switch "cue" above and "play" below, so its
		// up position — value 1 on a CKSS — is cue, and down is play. The
		// names here are indexed by value and must agree with the panel.
		configSwitch(MODE_PARAM, 0.f, 1.f, 0.f, "Sample modulation", {"play", "cue"});
		configButton(TRIG_PARAM, "Trigger");
		configParam(PITCH_PARAM, -2.f, 2.f, 0.f, "Pitch", " oct");
		configParam(PITCH_ATT_PARAM, -1.f, 1.f, 0.f, "Pitch CV", "%", 0.f, 100.f);
		// Centre is the shortest envelope, as on the hardware, but a fresh
		// module wants to make a sound rather than a click: the default sits
		// where a hit is a few hundred milliseconds long.
		configParam(LENGTH_PARAM, -1.f, 1.f, 0.6f, "Length");
		configParam(LENGTH_ATT_PARAM, -1.f, 1.f, 0.f, "Length CV", "%", 0.f, 100.f);
		configParam(FILTER_PARAM, -1.f, 1.f, 0.f, "Filter");
		configParam(FX_PARAM, -1.f, 1.f, 0.f, "FX");
		configSwitch(SYNC_PARAM, 0.f, 1.f, 1.f, "LFO clock", {"free", "sync"});
		configParam(RATE_PARAM, 0.f, 1.f, 0.5f, "LFO rate");
		configParam(LFO_ATT_PARAM, -1.f, 1.f, 0.f, "LFO rate CV", "%", 0.f, 100.f);
		configParam(TEMPO_PARAM, 30.f, 300.f, 120.f, "Tempo", " BPM");
		configParam(RHYTHM_PARAM, 0.f, 31.f, 0.f, "Rhythm");
		getParamQuantity(RHYTHM_PARAM)->snapEnabled = true;
		configSwitch(GSW_PARAM, 0.f, 2.f, 1.f, "Gate pattern", {"invert", "as is", "randomize"});
		configSwitch(CSW_PARAM, 0.f, 2.f, 1.f, "CV pattern", {"invert", "as is", "randomize"});
		configParam(LEVEL_PARAM, 0.f, 1.f, 0.8f, "Level");
		configParam(PWM_PARAM, 0.02f, 0.98f, 0.5f, "LFO pulse width", "%", 0.f, 100.f);

		configInput(BANK_INPUT, "Bank");
		configInput(SAMPLE_INPUT, "Sample");
		configInput(TRIG_INPUT, "Trigger");
		configInput(FREE_INPUT, "Free pitch (unquantized)");
		configInput(NOTE_INPUT, "Note pitch (quantized, latched at trigger)");
		configInput(LENGTH_INPUT, "Length");
		configInput(FILTER_INPUT, "Filter");
		configInput(FX_INPUT, "FX");
		configInput(LFO_INPUT, "LFO rate");
		configInput(LFO_RESET_INPUT, "LFO reset");
		configInput(CLK_INPUT, "Clock");
		configInput(G_INPUT, "Gate pattern modifier");
		configInput(C_INPUT, "CV pattern modifier");
		configInput(PAT_RESET_INPUT, "Pattern reset");
		configInput(RHYTHM_INPUT, "Rhythm select");

		configOutput(ENV_OUTPUT, "Envelope");
		configOutput(TRI_OUTPUT, "LFO triangle");
		configOutput(PULSE_OUTPUT, "LFO pulse");
		configOutput(SAW_OUTPUT, "LFO saw: the position in its cycle, and in the bar when synced to sixteen steps");
		configOutput(CLK_OUTPUT, "Clock");
		configOutput(GATE_OUTPUT, "Pattern gate");
		configOutput(CV_OUTPUT, "Pattern CV");
		configOutput(LEFT_OUTPUT, "Left");
		configOutput(RIGHT_OUTPUT, "Right");

		forsitan_sampler::loadSettingsOnce();
		bankSeed = (uint64_t)random::u32() | 1ull;
		allocDelays(44100.f);
	}

	// The delay lines are sized from the engine rate, not from a guess at it:
	// a fixed sample count is a different number of seconds at every rate,
	// and the delay would quietly stop being in tempo at the fast ones.
	void allocDelays(float sampleRate) {
		int n = (int)(3.4f * sampleRate) + 4;
		for (int i = 0; i < 2; i++) {
			dly[i].init(n);
			mod[i].init((int)(0.05f * sampleRate) + 4);
		}
	}

	void onSampleRateChange(const SampleRateChangeEvent& e) override {
		allocDelays(e.sampleRate);
	}

	~Vates() {
		if (genJob)
			genJob->abort.store(true);
		if (kitJob)
			kitJob->abort.store(true);
	}

	void onAdd(const AddEvent& e) override {
		refreshKits();
	}

	// Scans the kits folder and rebuilds the page table. UI thread only: it
	// reads directories.
	void refreshKits() {
		std::string folder = forsitan_sampler::getKitsFolder();
		kitNames = forsitan_sampler::listKits(folder);

		int n = vates_bank::kNumBanks;
		for (int k = 0; k < (int)kitNames.size() && n < kMaxBanks; k++) {
			int files = (int)forsitan_sampler::listKitFiles(
				folder + "/" + kitNames[k], 64).size();
			int per = samplesPerBank > 0 ? samplesPerBank : 64;
			int nPages = std::max(1, (files + per - 1) / per);
			for (int p = 0; p < nPages && n < kMaxBanks; p++, n++) {
				PageInfo& pi = pages[n];
				pi.kit = k;
				pi.first = p * per;
				pi.count = clamp(files - pi.first, 0, per);
				if (nPages > 1)
					snprintf(pi.name, sizeof pi.name, "%s %d/%d",
					         kitNames[k].c_str(), p + 1, nPages);
				else
					snprintf(pi.name, sizeof pi.name, "%s", kitNames[k].c_str());
			}
		}
		bankTotal.store(n);
		uiBankShown = -1;   // make the display pick the new names up
	}

	int bankCount() const {
		return std::max(bankTotal.load(), vates_bank::kNumBanks);
	}

	const PageInfo* page(int bank) const {
		if (bank < vates_bank::kNumBanks || bank >= kMaxBanks)
			return nullptr;
		return &pages[bank];
	}

	// ── background work ──────────────────────────────────────────────────────

	// Build the six generated banks off the audio thread. startDetached, not
	// std::thread: this can be called from process(), and a worker that
	// inherits the audio thread's realtime policy gets SIGXCPU'd partway
	// through (see imber_worker.hpp).
	void startGeneration() {
		if (genJob)
			genJob->abort.store(true);
		std::shared_ptr<GenJob> j;
		try {
			j.reset(new GenJob());
		}
		catch (const std::exception& err) {
			WARN("vates: cannot allocate a bank job (%s)", err.what());
			return;
		}
		uint64_t seed = bankSeed;
		bool started = imber_worker::startDetached([j, seed]() {
			try {
				vates_bank::build(j->set, seed, kGenRate, &j->progress, &j->abort);
			}
			catch (...) {
				j->failed.store(true);
			}
			j->done.store(true);
		});
		if (!started) {
			WARN("vates: cannot start the bank worker");
			return;
		}
		genJob = j;
		pendingGen = false;
	}

	void startKitLoad(int kitIdx) {
		if (kitJob)
			kitJob->abort.store(true);
		if (kitIdx < 0 || kitIdx >= (int)kitNames.size()) {
			loadingKit.clear();
			return;
		}
		std::shared_ptr<KitJob> j;
		try {
			j.reset(new KitJob());
		}
		catch (const std::exception& err) {
			WARN("vates: cannot allocate a kit job (%s)", err.what());
			return;
		}
		std::string name = kitNames[kitIdx];
		std::string dir = forsitan_sampler::getKitsFolder() + "/" + name;
		j->kit.name = name;
		loadingKit = name;
		loadedKitIndex = kitIdx;
		bool started = imber_worker::startDetached([j, dir]() {
			std::vector<std::string> files = forsitan_sampler::listKitFiles(dir, 64);
			for (const std::string& f : files) {
				if (j->abort.load())
					break;
				Sample s;
				if (!forsitan_sampler::loadWav(f, s, /*mono=*/false))
					s = Sample();   // keep the index aligned with the file list
				j->kit.samples.push_back(std::move(s));
				j->kit.sampleNames.push_back(system::getStem(f));
			}
			j->done.store(true);
		});
		if (!started) {
			WARN("vates: cannot start the kit worker");
			return;
		}
		kitJob = j;
	}

	// ── selection helpers ────────────────────────────────────────────────────

	int samplesInBank(int b) const {
		if (b < vates_bank::kNumBanks)
			return banks ? vates_bank::kSamplesPerBank : 0;
		const PageInfo* pi = page(b);
		if (!pi || !userKit || loadedKitIndex != pi->kit)
			return 0;
		return clamp((int)userKit->samples.size() - pi->first, 0, pi->count);
	}

	// The knob alone spans the whole list end to end: its last position is
	// the last entry, never the first again. Wrapping belongs to modulation,
	// not to the hand.
	static int knobSelect(float knob, int count) {
		if (count <= 0)
			return 0;
		return clamp((int)(knob * count), 0, count - 1);
	}

	// The attenuverted CV offsets that index, and *this* wraps: modulation
	// past the last entry comes back to the first, which is what makes a slow
	// ramp into `sample` a sequence rather than a fade.
	//
	// Ten volts at full attenuverter is exactly one bank, and the epsilon is
	// the fencepost: without it a 0-10 V ramp reaches the first entry again
	// at its very top instead of resting on the last one, which is the same
	// off-by-one the knob used to have.
	static int cvSelect(int base, float cv, float att, int count) {
		if (count <= 0)
			return 0;
		int i = base + (int)std::floor(cv * 0.1f * att * (count - 1e-3f));
		i %= count;
		if (i < 0)
			i += count;
		return i;
	}

	float quantizePitch(float volts) const {
		const imber_dsp::ScaleDef& sc = imber_dsp::kScales[clamp(scaleIndex, 0, imber_dsp::kScaleCount - 1)];
		float st = volts * 12.f;
		int ref = (int)std::round(st);
		int best = ref;
		float bestDist = 1e9f;
		for (int s = ref - 6; s <= ref + 6; s++) {
			int pc = ((s - rootNote) % 12 + 12) % 12;
			for (int d = 0; d < sc.size; d++)
				if (sc.deg[d] == pc) {
					float dist = std::fabs(s - st);
					if (dist < bestDist) {
						bestDist = dist;
						best = s;
					}
					break;
				}
		}
		return best / 12.f;
	}

	// 0 = invert, 1 = leave alone, 2 = randomize. The switch is normalled to
	// the jack, as on the hardware: patch a cable and the voltage decides.
	int patternMode(int switchParam, int inputId) {
		if (!inputs[inputId].isConnected())
			return (int)std::round(params[switchParam].getValue());
		float v = inputs[inputId].getVoltage();
		if (hardwareCvWindow)
			return v > 3.2f ? 2 : (v < 1.6f ? 0 : 1);
		return v > 1.f ? 2 : (v < -1.f ? 0 : 1);
	}

	// ── the voice ────────────────────────────────────────────────────────────

	void trigger(float sr) {
		// a reverse hit that is still swelling is not interrupted, as on the
		// hardware: "during attack, samples don't retrigger"
		if (voiceActive && voiceAttack)
			return;

		const std::vector<float>* L = nullptr;
		const std::vector<float>* R = nullptr;
		std::shared_ptr<void> hold;
		float srcRate = kGenRate;
		int b = bankIndex;
		int s = sampleIndex;
		if (b < vates_bank::kNumBanks) {
			if (!banks)
				return;
			const vates_bank::Bank& bank = banks->banks[b];
			s = clamp(s, 0, vates_bank::kSamplesPerBank - 1);
			L = &bank.L[s];
			R = &bank.R[s];
			hold = banks;
		}
		else {
			const PageInfo* pi = page(b);
			if (!pi || !userKit || loadedKitIndex != pi->kit)
				return;
			int file = pi->first + s;
			if (s < 0 || s >= pi->count || file >= (int)userKit->samples.size())
				return;
			const Sample& smp = userKit->samples[file];
			if (smp.empty())
				return;
			L = &smp.l;
			R = smp.stereo() ? &smp.r : &smp.l;
			srcRate = smp.sampleRate;
			hold = userKit;
		}
		if (!L || L->size() < 2)
			return;

		voiceHold = hold;
		voiceL = L;
		voiceR = R;
		voiceSrcRate = srcRate;

		// length is latched here, direction included: modulation flips the
		// playback direction between hits and never inside one
		float len = clamp(params[LENGTH_PARAM].getValue()
		                  + inputs[LENGTH_INPUT].getVoltage() * 0.2f
		                    * params[LENGTH_ATT_PARAM].getValue(), -1.f, 1.f);
		voiceReverse = len < 0.f;
		float mag = std::fabs(len);
		float T = 0.005f * std::pow(1200.f, mag);   // 5 ms .. 6 s
		envHold = mag > 0.98f;

		if (!voiceReverse) {
			voicePos = 0.0;
			env = 1.f;
			voiceAttack = false;
			envCoef = std::exp(-1.f / std::max(T * 0.25f * sr, 1.f));
		}
		else {
			voicePos = (double)(L->size() - 2);
			env = 0.f;
			voiceAttack = true;
			// the swell reaches full level as the sample runs out, or in T,
			// whichever is shorter
			float playable = (float)L->size() / std::max(voiceRateNow(), 1e-6f) / sr;
			float A = std::min(T, std::max(playable, 0.002f));
			envCoef = std::exp(-1.f / std::max(A * 0.25f * sr, 1.f));
		}
		release = 1.f;

		if (inputs[NOTE_INPUT].isConnected())
			notePitch = quantizePitch(inputs[NOTE_INPUT].getVoltage()
			                          * params[PITCH_ATT_PARAM].getValue());
		else
			notePitch = 0.f;

		voiceActive = true;
		uiBank = bankIndex;
		uiSample = sampleIndex;
	}

	float voiceRateNow() {
		float pitch = params[PITCH_PARAM].getValue() + notePitch;
		if (inputs[FREE_INPUT].isConnected())
			pitch += inputs[FREE_INPUT].getVoltage() * params[PITCH_ATT_PARAM].getValue();
		return std::pow(2.f, clamp(pitch, -6.f, 6.f));
	}

	// ── process ──────────────────────────────────────────────────────────────

	void process(const ProcessArgs& args) override {
		const float sr = args.sampleRate;

		// finished background work
		if (pendingGen && !genJob)
			startGeneration();
		if (genJob && genJob->done.load()) {
			if (!genJob->failed.load() && genJob->set.ready) {
				std::shared_ptr<vates_bank::BankSet> fresh;
				try {
					fresh.reset(new vates_bank::BankSet());
				}
				catch (...) {
					fresh.reset();
				}
				if (fresh) {
					fresh->swapFrom(genJob->set);
					banks = fresh;
				}
			}
			genJob.reset();
		}
		if (kitJob && kitJob->done.load()) {
			std::shared_ptr<Kit> fresh;
			try {
				fresh.reset(new Kit());
			}
			catch (...) {
				fresh.reset();
			}
			if (fresh) {
				fresh->name = kitJob->kit.name;
				fresh->samples.swap(kitJob->kit.samples);
				fresh->sampleNames.swap(kitJob->kit.sampleNames);
				userKit = fresh;
			}
			loadingKit.clear();
			kitJob.reset();
		}

		// ── bank and sample selection ────────────────────────────────────────
		int nBanks = bankCount();
		// The bank is stepped, not swept: two buttons as on the hardware,
		// where BANK is a button and only the samples sit under a knob.
		bool bankStepped = false;
		if (bankUpButton.process(params[BANK_UP_PARAM].getValue() > 0.5f)) {
			bankBase++;
			bankStepped = true;
		}
		if (bankDownButton.process(params[BANK_DOWN_PARAM].getValue() > 0.5f)) {
			bankBase--;
			bankStepped = true;
		}
		bankBase %= std::max(nBanks, 1);
		if (bankBase < 0)
			bankBase += nBanks;
		int newBank = cvSelect(bankBase, inputs[BANK_INPUT].getVoltage(),
		                       params[BANK_ATT_PARAM].getValue(), nBanks);
		bool bankChanged = (newBank != bankIndex);
		if (bankChanged) {
			bankIndex = newBank;
			const PageInfo* pi = page(bankIndex);
			// pages of one kit share its load: turning past a page boundary
			// changes the window, not the files
			if (pi && pi->kit >= 0 && pi->kit != loadedKitIndex)
				startKitLoad(pi->kit);
		}
		uiBank = bankIndex;

		int nSamples = samplesInBank(bankIndex);
		int sampleKnob = knobSelect(params[SAMPLE_PARAM].getValue(), nSamples);
		int sel = cvSelect(sampleKnob, inputs[SAMPLE_INPUT].getVoltage(),
		                   params[SAMPLE_ATT_PARAM].getValue(), nSamples);
		bool play = params[MODE_PARAM].getValue() < 0.5f;

		// play mode fires when *modulation* crosses into another sample. A
		// hand on the sample knob, or a bank change moving the ground under
		// the index, is browsing, not playing: it must not fire, or the
		// module screams while you are looking for a sound.
		bool knobMoved = (sampleKnob != prevSampleKnob) || bankChanged
		                 || bankStepped || prevSampleKnob < 0;
		bool crossed = (sel != aimedSample) && !knobMoved;
		prevSampleKnob = sampleKnob;
		aimedSample = sel;
		if (!voiceActive)
			uiSample = sel;

		// ── clock, pattern generator and LFO ─────────────────────────────────
		// all three live in forsitan_mod::Modulation, shared with artifex; the knob
		// spans the 32 rhythms and the CV offsets it, wrapping, on the same
		// ten-volts-is-the-whole-list scale as bank and sample
		forsitan_mod::ModIn min;
		min.dt = args.sampleTime;
		min.bpm = params[TEMPO_PARAM].getValue();
		min.clkVoltage = inputs[CLK_INPUT].getVoltage();
		min.honourExternal = honourExternalClock;
		min.patResetVoltage = inputs[PAT_RESET_INPUT].getVoltage();
		min.rhythm = (int)std::round(params[RHYTHM_PARAM].getValue());
		if (inputs[RHYTHM_INPUT].isConnected())
			min.rhythm = forsitan_mod::rhythmSelect(min.rhythm, inputs[RHYTHM_INPUT].getVoltage(), 1.f);
		min.gateMode = patternMode(GSW_PARAM, G_INPUT);
		min.cvMode = patternMode(CSW_PARAM, C_INPUT);
		min.lfoRateKnob = params[RATE_PARAM].getValue();
		min.lfoRateMod = inputs[LFO_INPUT].getVoltage() * 0.2f * params[LFO_ATT_PARAM].getValue();
		min.lfoSynced = params[SYNC_PARAM].getValue() > 0.5f;
		min.lfoResetVoltage = inputs[LFO_RESET_INPUT].getVoltage();
		min.pulseWidth = params[PWM_PARAM].getValue();
		modul.process(min);

		float stepSeconds = modul.stepSeconds;
		float tri = modul.tri;

		// ── triggers ─────────────────────────────────────────────────────────
		bool fire = false;
		if (trigIn.process(inputs[TRIG_INPUT].getVoltage(), 0.1f, 1.f))
			fire = true;
		if (trigButton.process(params[TRIG_PARAM].getValue() > 0.5f))
			fire = true;
		if (play && crossed && nSamples > 0)
			fire = true;
		if (fire) {
			sampleIndex = aimedSample;
			trigger(sr);
		}

		// ── playback ─────────────────────────────────────────────────────────
		float outL = 0.f, outR = 0.f;
		if (voiceActive && voiceL) {
			size_t n = voiceL->size();
			double p = voicePos;
			if (p < 0.0 || p >= (double)(n - 1)) {
				voiceActive = false;
			}
			else {
				size_t i0 = (size_t)p;
				float fr = (float)(p - i0);
				size_t i1 = std::min(i0 + 1, n - 1);
				outL = (*voiceL)[i0] + ((*voiceL)[i1] - (*voiceL)[i0]) * fr;
				outR = (*voiceR)[i0] + ((*voiceR)[i1] - (*voiceR)[i0]) * fr;

				float rate = voiceRateNow() * voiceSrcRate / sr;
				voicePos += voiceReverse ? -rate : rate;

				if (voiceAttack) {
					env += (1.f - env) * (1.f - envCoef);
					if (env > 0.995f) {
						env = 1.f;
						voiceAttack = false;
					}
				}
				else if (!envHold && !voiceReverse)
					env *= envCoef;

				// a reversed hit stops when it reaches the head of the
				// sample; fade the last few ms so the stop does not click
				if (voiceReverse) {
					float togo = (float)voicePos / std::max(rate, 1e-6f) / sr;
					if (togo < 0.003f)
						release = clamp(togo / 0.003f, 0.f, 1.f);
				}
				float a = env * release;
				outL *= a;
				outR *= a;
				if (!envHold && env < 1e-4f && !voiceAttack)
					voiceActive = false;
			}
		}
		if (!voiceActive) {
			env = 0.f;
			voiceHold.reset();
		}

		// ── filter ───────────────────────────────────────────────────────────
		// The knob is smoothed before it reaches any coefficient: a mouse
		// drag steps a parameter once per frame, and a four-pole filter that
		// jumps its cutoff in one sample clicks.
		float fTarget = clamp(params[FILTER_PARAM].getValue()
		                      + inputs[FILTER_INPUT].getVoltage() * 0.2f, -1.f, 1.f);
		filtSmooth += (fTarget - filtSmooth)
		              * (1.f - std::exp(-args.sampleTime / 0.010f));
		float fParam = filtSmooth;
		float mag = std::fabs(fParam);
		bool lowpass = fParam < 0.f;

		// Crossing the centre swaps a lowpass for a highpass and moves the
		// cutoff from 20 kHz to 25 Hz. No integrator state survives that, so
		// both sections are cleared on the way through — which costs nothing,
		// because the wet path is faded out here and nobody hears it.
		if (lowpass != filtWasLow) {
			for (int c = 0; c < 2; c++) {
				filt[c].reset();
				filtB[c].reset();
			}
			filtWasLow = lowpass;
		}

		// the wet path fades in over the first twentieth of the travel, which
		// is the stretch where the filter is transparent anyway
		float fWet = clamp(mag * 20.f, 0.f, 1.f);
		if (fWet > 1e-4f) {
			// A DJ filter has to be able to take the track away at either
			// end. The lowpass floor sits under the kick and the highpass
			// ceiling above the air, so the far end of the travel is silence
			// rather than a thump or a hi-hat you can still hear.
			float fc = lowpass ? 30.f * std::pow(667.f, 1.f - mag)
			                   : 25.f * std::pow(560.f, mag);
			fc = clamp(fc, 20.f, 0.45f * sr);
			float g = std::tan((float)M_PI * fc / sr);
			// Four poles: at two, the bass never quite leaves as the highpass
			// climbs. The pair is Butterworth-damped, and the resonance goes
			// into the second section only — raising the Q of both would
			// square the peak instead of tilting it.
			float k1 = 1.f / 0.541f;
			float k2 = 1.f / (1.8f + 1.2f * mag);
			float lp, hp;
			for (int c = 0; c < 2; c++) {
				float dry = (c == 0) ? outL : outR;
				float x = dry;
				filt[c].process(x, g, k1, lp, hp);
				x = lowpass ? lp : hp;
				filtB[c].process(x, g, k2, lp, hp);
				x = lowpass ? lp : hp;
				x = dry + (x - dry) * fWet;
				if (c == 0)
					outL = x;
				else
					outR = x;
			}
		}

		// ── fx ───────────────────────────────────────────────────────────────
		float fxParam = clamp(params[FX_PARAM].getValue()
		                      + inputs[FX_INPUT].getVoltage() * 0.2f, -1.f, 1.f);
		if (fxParam < -0.01f) {
			// Tempo-synced delay at three eighths of a note, as the hardware
			// states — a dotted quarter, a beat and a half — with the right
			// channel a plain beat against it, so the two run a 3:2 cross
			// rhythm and the cross-feedback below throws it side to side.
			float amt = -fxParam;
			float beat = stepSeconds * 4.f;
			float maxT = (float)(dly[0].size() - 4) / sr;
			float t = beat * 1.5f;
			// A dotted quarter does not fit the buffer at every tempo. Halving
			// the division keeps the delay in tempo, where clamping it to
			// whatever fits would leave it in no tempo at all.
			while (t > maxT && t > 0.02f)
				t *= 0.5f;
			t = clamp(t, 0.005f, maxT) * sr;
			float wetL = dly[0].read(t);
			float wetR = dly[1].read(t * 0.667f);
			float fb = 0.25f + 0.35f * amt;
			dly[0].write(outL + wetR * fb);
			dly[1].write(outR + wetL * fb);
			outL += wetL * amt * 0.8f;
			outR += wetR * amt * 0.8f;
		}
		else if (fxParam > 0.01f) {
			// chorus into flanger: the further up, the shorter the delay and
			// the more feedback, and the wet path soft-clips
			float amt = fxParam;
			modPhase += 0.35f * args.sampleTime;
			modPhase -= std::floor(modPhase);
			float m1 = std::sin(2.f * (float)M_PI * modPhase);
			float m2 = std::sin(2.f * (float)M_PI * (modPhase + 0.25f));
			float base = (8.f - 6.5f * amt) * 0.001f * sr;
			float depth = (2.5f - 1.8f * amt) * 0.001f * sr;
			float wetL = mod[0].read(base + depth * m1);
			float wetR = mod[1].read(base + depth * m2);
			float fb = 0.7f * amt;
			mod[0].write(std::tanh(outL + wetL * fb));
			mod[1].write(std::tanh(outR + wetR * fb));
			float mix = 0.9f * amt;
			outL = outL * (1.f - 0.5f * mix) + wetL * mix;
			outR = outR * (1.f - 0.5f * mix) + wetR * mix;
			outL = std::tanh(outL * (1.f + amt));
			outR = std::tanh(outR * (1.f + amt));
		}

		// ── outputs ──────────────────────────────────────────────────────────
		float level = params[LEVEL_PARAM].getValue();
		outL *= level * 5.f;
		outR *= level * 5.f;
		outputs[LEFT_OUTPUT].setVoltage(clamp(outL, -10.f, 10.f));
		outputs[RIGHT_OUTPUT].setVoltage(clamp(outR, -10.f, 10.f));
		lights[LEFT_LIGHT].setBrightnessSmooth(std::fabs(outL) * 0.2f, args.sampleTime);
		lights[RIGHT_LIGHT].setBrightnessSmooth(std::fabs(outR) * 0.2f, args.sampleTime);

		outputs[ENV_OUTPUT].setVoltage(clamp(env * release * 10.f, 0.f, 10.f));
		outputs[TRI_OUTPUT].setVoltage(tri * 10.f);
		outputs[PULSE_OUTPUT].setVoltage(modul.lfoRising ? 10.f : 0.f);
		// the saw is the cycle's own position, rising from the reset point:
		// synced to sixteen steps it sweeps a whole bank once a bar
		outputs[SAW_OUTPUT].setVoltage(modul.lfoPhase * 10.f);
		outputs[CLK_OUTPUT].setVoltage(modul.clock() ? 10.f : 0.f);
		outputs[GATE_OUTPUT].setVoltage(modul.gate() ? 10.f : 0.f);
		outputs[CV_OUTPUT].setVoltage(modul.cv());

		refreshDisplayText();
	}

	// ── ui helpers ───────────────────────────────────────────────────────────

	// Called from process(), and only writes when something changed: the
	// generation percentage, the bank, or the sample.
	void refreshDisplayText() {
		int progress = -1;
		if (genJob)
			progress = 100 * genJob->progress.load() / vates_bank::kTotalSamples;
		int n = samplesInBank(bankIndex);
		int s = n > 0 ? clamp(uiSample, 0, n - 1) : -1;
		if (progress == uiProgressShown && bankIndex == uiBankShown && s == uiSampleShown)
			return;
		uiProgressShown = progress;
		uiBankShown = bankIndex;
		uiSampleShown = s;

		if (progress >= 0)
			snprintf(uiBankText, sizeof uiBankText, "building %d%%", progress);
		else if (bankIndex < vates_bank::kNumBanks)
			snprintf(uiBankText, sizeof uiBankText, "%s", vates_bank::bankName(bankIndex));
		else if (!loadingKit.empty())
			snprintf(uiBankText, sizeof uiBankText, "%s ...", loadingKit.c_str());
		else {
			const PageInfo* pi = page(bankIndex);
			snprintf(uiBankText, sizeof uiBankText, "%s",
			         (pi && pi->kit >= 0) ? pi->name : "-");
		}

		if (s < 0)
			snprintf(uiSampleText, sizeof uiSampleText, "-");
		else if (bankIndex < vates_bank::kNumBanks && banks)
			snprintf(uiSampleText, sizeof uiSampleText, "%d %s", s + 1,
			         banks->banks[bankIndex].sampleNames[s].c_str());
		else {
			const PageInfo* pi = page(bankIndex);
			int file = pi ? pi->first + s : s;
			if (userKit && file < (int)userKit->sampleNames.size())
				snprintf(uiSampleText, sizeof uiSampleText, "%d %s", s + 1,
				         userKit->sampleNames[file].c_str());
			else
				snprintf(uiSampleText, sizeof uiSampleText, "%d/%d", s + 1, n);
		}
	}

	// ── names for the display pickers (UI thread) ────────────────────────────

	std::string bankName(int b) {
		if (b < vates_bank::kNumBanks)
			return vates_bank::bankName(b);
		const PageInfo* pi = page(b);
		return (pi && pi->kit >= 0) ? pi->name : "-";
	}

	std::string sampleName(int b, int s) {
		if (b < vates_bank::kNumBanks && banks)
			return string::f("%d  %s", s + 1,
			                 banks->banks[b].sampleNames[s].c_str());
		const PageInfo* pi = page(b);
		int file = pi ? pi->first + s : s;
		if (userKit && file < (int)userKit->sampleNames.size())
			return string::f("%d  %s", s + 1, userKit->sampleNames[file].c_str());
		return string::f("%d", s + 1);
	}

	// ── persistence ──────────────────────────────────────────────────────────

	void onReset(const ResetEvent& e) override {
		Module::onReset(e);
		rootNote = 0;
		scaleIndex = imber_dsp::kDefaultScale;
		honourExternalClock = true;
		hardwareCvWindow = false;
		bankSeed = (uint64_t)random::u32() | 1ull;
		pendingGen = true;
		bankBase = 0;
		voiceActive = false;
		modul.resetSequence();
		modul.loadedRhythm = -1;
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "bankSeed", json_integer((json_int_t)bankSeed));
		json_object_set_new(root, "root", json_integer(rootNote));
		json_object_set_new(root, "scale", json_integer(scaleIndex));
		json_object_set_new(root, "externalClock", json_boolean(honourExternalClock));
		json_object_set_new(root, "samplesPerBank", json_integer(samplesPerBank));
		json_object_set_new(root, "bank", json_integer(bankBase));
		json_object_set_new(root, "hardwareCvWindow", json_boolean(hardwareCvWindow));
		return root;
	}

	void dataFromJson(json_t* root) override {
		if (json_t* j = json_object_get(root, "bankSeed")) {
			bankSeed = (uint64_t)json_integer_value(j);
			pendingGen = true;
		}
		if (json_t* j = json_object_get(root, "root"))
			rootNote = clamp((int)json_integer_value(j), 0, 11);
		if (json_t* j = json_object_get(root, "scale"))
			scaleIndex = clamp((int)json_integer_value(j), 0, imber_dsp::kScaleCount - 1);
		if (json_t* j = json_object_get(root, "externalClock"))
			honourExternalClock = json_boolean_value(j);
		if (json_t* j = json_object_get(root, "samplesPerBank"))
			samplesPerBank = clamp((int)json_integer_value(j), 0, 64);
		if (json_t* j = json_object_get(root, "bank"))
			bankBase = std::max(0, (int)json_integer_value(j));
		if (json_t* j = json_object_get(root, "hardwareCvWindow"))
			hardwareCvWindow = json_boolean_value(j);
	}
};

// ── displays ──────────────────────────────────────────────────────────────────
// Two of them, bank on the left and sample on the right. Kits are the user's
// own and generated banks are new with every seed, so the panel cannot label
// what a selection holds — these can, and a right-click on either one lists
// what is there and jumps straight to it.
struct VatesDisplay : Widget {
	Vates* module = nullptr;
	bool isBank = true;

	void draw(const DrawArgs& args) override {
		Rect r = box.zeroPos();
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, r.pos.x, r.pos.y, r.size.x, r.size.y, 2.f);
		nvgFillColor(args.vg, nvgRGB(0x11, 0x11, 0x11));
		nvgFill(args.vg);
		nvgStrokeColor(args.vg, nvgRGB(0x55, 0x55, 0x55));
		nvgStrokeWidth(args.vg, 0.8f);
		nvgStroke(args.vg);
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1)
			return;
		std::shared_ptr<Font> font =
			APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
		if (!font)
			return;
		Rect r = box.zeroPos();
		nvgFontFaceId(args.vg, font->handle);
		nvgFontSize(args.vg, 11.f);
		nvgFillColor(args.vg, nvgRGB(0xff, 0xd5, 0x00));
		nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
		nvgScissor(args.vg, r.pos.x + 2.f, r.pos.y, r.size.x - 4.f, r.size.y);
		const char* txt = !module ? (isBank ? "vates" : "")
		                          : (isBank ? module->uiBankText : module->uiSampleText);
		nvgText(args.vg, r.pos.x + 5.f, r.getCenter().y, txt, NULL);
		nvgResetScissor(args.vg);
	}

	// right-click: the list of what this display selects from
	void onButton(const ButtonEvent& e) override {
		if (e.action != GLFW_PRESS || e.button != GLFW_MOUSE_BUTTON_RIGHT
		    || !module) {
			Widget::onButton(e);
			return;
		}
		e.consume(this);
		Menu* menu = createMenu();
		Vates* m = module;
		if (isBank) {
			menu->addChild(createMenuLabel("Bank"));
			int n = m->bankCount();
			for (int b = 0; b < n; b++)
				menu->addChild(createCheckMenuItem(m->bankName(b), "",
					[=]() { return m->bankBase == b; },
					[=]() { m->bankBase = b; }));
		}
		else {
			menu->addChild(createMenuLabel("Sample"));
			int n = m->samplesInBank(m->bankIndex);
			if (n <= 0) {
				menu->addChild(createMenuLabel("(nothing loaded)"));
				return;
			}
			int b = m->bankIndex;
			for (int i = 0; i < n; i++)
				menu->addChild(createCheckMenuItem(m->sampleName(b, i), "",
					[=]() { return m->aimedSample == i; },
					[=]() {
						// the knob is still the selector: move it there
						APP->engine->setParamValue(m, Vates::SAMPLE_PARAM,
						                           (i + 0.5f) / n);
					}));
		}
	}
};

struct VatesWidget : ModuleWidget {
	VatesWidget(Vates* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/vates.svg")));

// @layout:begin vates 132.08 128.5
// @elem SCREW_TL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_TR ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BR ScrewSilver 3.5 screw "" 0.0
// @elem BANK_DOWN_PARAM VCVButton 4.15 param "" 0.0
// @elem BANK_UP_PARAM VCVButton 4.15 param "" 0.0
// @elem SAMPLE_PARAM RoundBigBlackKnob 7.62 param "" 0.0
// @elem PITCH_PARAM RoundBigBlackKnob 7.62 param "" 0.0
// @elem LENGTH_PARAM RoundBigBlackKnob 7.62 param "" 0.0
// @elem BANK_ATT_PARAM Trimpot 3.03 param "" 0.0
// @elem BANK_INPUT PJ301MPort 4.01 input "" 0.0
// @elem SAMPLE_ATT_PARAM Trimpot 3.03 param "" 0.0
// @elem SAMPLE_INPUT PJ301MPort 4.01 input "" 0.0
// @elem PITCH_ATT_PARAM Trimpot 3.03 param "" 0.0
// @elem FREE_INPUT PJ301MPort 4.01 input "" 0.0
// @elem NOTE_INPUT PJ301MPort 4.01 input "" 0.0
// @elem LENGTH_ATT_PARAM Trimpot 3.03 param "" 0.0
// @elem LENGTH_INPUT PJ301MPort 4.01 input "" 0.0
// @elem MODE_PARAM CKSS 2.3 param "" 0.0
// @elem TRIG_PARAM TL1105 2.6 param "" 0.0
// @elem TRIG_INPUT PJ301MPort 4.01 input "" 0.0
// @elem FILTER_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem FILTER_INPUT PJ301MPort 4.01 input "" 0.0
// @elem FX_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem FX_INPUT PJ301MPort 4.01 input "" 0.0
// @elem ENV_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem SYNC_PARAM CKSS 2.3 param "" 0.0
// @elem RATE_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem LFO_ATT_PARAM Trimpot 3.03 param "" 0.0
// @elem LFO_INPUT PJ301MPort 4.01 input "" 0.0
// @elem LFO_RESET_INPUT PJ301MPort 4.01 input "" 0.0
// @elem TRI_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem PULSE_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem SAW_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem PWM_PARAM Trimpot 3.03 param "" 0.0
// @elem TEMPO_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem CLK_INPUT PJ301MPort 4.01 input "" 0.0
// @elem RHYTHM_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem GSW_PARAM CKSSThreePos 2.3 param "" 0.0
// @elem G_INPUT PJ301MPort 4.01 input "" 0.0
// @elem CSW_PARAM CKSSThreePos 2.3 param "" 0.0
// @elem C_INPUT PJ301MPort 4.01 input "" 0.0
// @elem PAT_RESET_INPUT PJ301MPort 4.01 input "" 0.0
// @elem RHYTHM_INPUT PJ301MPort 4.01 input "" 0.0
// @elem GATE_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem CV_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem CLK_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem LEVEL_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem LEFT_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem RIGHT_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem LEFT_LIGHT SmallLight 1.0 light "" 0.0
// @elem RIGHT_LIGHT SmallLight 1.0 light "" 0.0
// @elem LABEL_BANK_DOWN label 0.0 label "-" 0.0 11.00 37.00
// @elem LABEL_BANK_UP label 0.0 label "+" 0.0 22.00 37.00
// @elem LABEL_BANK label 0.0 label "bank" 0.0 16.50 41.80
// @elem LABEL_SAMPLE label 0.0 label "sample" 0.0 49.50 40.50
// @elem LABEL_PITCH label 0.0 label "pitch" 0.0 82.50 40.50
// @elem LABEL_LENGTH label 0.0 label "length" 0.0 115.50 40.50
// @elem LABEL_FREE label 0.0 label "free" 0.0 82.50 54.50
// @elem LABEL_NOTE label 0.0 label "note" 0.0 93.00 54.50
// @elem LABEL_CUE label 0.0 label "cue" 0.0 26.50 57.90
// @elem LABEL_MODE label 0.0 label "play" 0.0 26.50 72.50
// @elem LABEL_TRIG label 0.0 label "trig" 0.0 12.25 72.50
// @elem LABEL_FILTER label 0.0 label "filter" 0.0 49.50 73.50
// @elem LABEL_FX label 0.0 label "fx" 0.0 82.50 73.50
// @elem BOX_ENV panel_box 7.0 box "" 0.0 82.00 120.00
// @elem LABEL_ENV label 0.0 label "env" 0.0 82.00 125.50
// @elem LABEL_SYNC label 0.0 label "sync" 0.0 8.00 77.30
// @elem LABEL_FREERUN label 0.0 label "free" 0.0 8.00 92.50
// @elem LABEL_RATE label 0.0 label "rate" 0.0 21.00 91.00
// @elem LABEL_LFO_RESET label 0.0 label "reset" 0.0 66.04 90.00
// @elem BOX_SAW panel_box 7.0 box "" 0.0 98.00 84.50
// @elem BOX_TRI panel_box 7.0 box "" 0.0 82.00 84.50
// @elem LABEL_TRI label 0.0 label "tri" 0.0 82.00 90.00
// @elem BOX_PULSE panel_box 7.0 box "" 0.0 114.00 84.50
// @elem LABEL_PULSE label 0.0 label "pulse" 0.0 114.00 90.00
// @elem LABEL_SAW label 0.0 label "saw" 0.0 98.00 90.00
// @elem LABEL_PWM label 0.0 label "pwm" 0.0 126.50 90.00
// @elem LABEL_TEMPO label 0.0 label "tempo" 0.0 110.00 73.50
// @elem LABEL_CLK label 0.0 label "clk" 0.0 121.00 73.50
// @elem LABEL_RHYTHM label 0.0 label "rhythm" 0.0 22.01 126.50
// @elem LABEL_G label 0.0 label "gate ptrn" 0.0 20.75 107.50
// @elem LABEL_C label 0.0 label "cv ptrn" 0.0 42.75 107.50
// @elem LABEL_PAT_RESET label 0.0 label "reset" 0.0 66.04 107.50
// @elem BOX_GATE panel_box 7.0 box "" 0.0 82.00 102.00
// @elem LABEL_GATE label 0.0 label "gate" 0.0 82.00 107.50
// @elem BOX_CV panel_box 7.0 box "" 0.0 98.00 102.00
// @elem LABEL_CV label 0.0 label "cv" 0.0 98.00 107.50
// @elem BOX_CLK_OUT panel_box 7.0 box "" 0.0 114.00 102.00
// @elem LABEL_CLK_OUT label 0.0 label "clk" 0.0 114.00 107.50
// @elem LABEL_LEVEL label 0.0 label "level" 0.0 44.03 126.50
// @elem BOX_LEFT panel_box 7.0 box "" 0.0 98.00 120.00
// @elem LABEL_LEFT label 0.0 label "L" 0.0 98.00 125.50
// @elem BOX_RIGHT panel_box 7.0 box "" 0.0 114.00 120.00
// @elem LABEL_RIGHT label 0.0 label "R" 0.0 114.00 125.50
// @elem LOGO forsitan_logo 0.0 logo "" 0.0 66.04 122.50

        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 0.00f)))); // SCREW_TL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(124.46f, 0.00f)))); // SCREW_TR
        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 123.42f)))); // SCREW_BL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(124.46f, 123.42f)))); // SCREW_BR
        addParam(createParamCentered<VCVButton>(mm2px(Vec(11.00f, 29.00f)), module, Vates::BANK_DOWN_PARAM));
        addParam(createParamCentered<VCVButton>(mm2px(Vec(22.00f, 29.00f)), module, Vates::BANK_UP_PARAM));
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(49.50f, 29.00f)), module, Vates::SAMPLE_PARAM));
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(82.50f, 29.00f)), module, Vates::PITCH_PARAM));
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(115.50f, 29.00f)), module, Vates::LENGTH_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(11.50f, 47.00f)), module, Vates::BANK_ATT_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(21.00f, 47.00f)), module, Vates::BANK_INPUT));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(44.50f, 47.00f)), module, Vates::SAMPLE_ATT_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(54.00f, 47.00f)), module, Vates::SAMPLE_INPUT));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(72.00f, 47.00f)), module, Vates::PITCH_ATT_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(82.50f, 47.00f)), module, Vates::FREE_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(93.00f, 47.00f)), module, Vates::NOTE_INPUT));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(110.50f, 47.00f)), module, Vates::LENGTH_ATT_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(120.00f, 47.00f)), module, Vates::LENGTH_INPUT));
        addParam(createParamCentered<CKSS>(mm2px(Vec(26.50f, 64.00f)), module, Vates::MODE_PARAM));
        addParam(createParamCentered<TL1105>(mm2px(Vec(7.50f, 65.00f)), module, Vates::TRIG_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(17.00f, 65.00f)), module, Vates::TRIG_INPUT));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(49.50f, 65.00f)), module, Vates::FILTER_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(60.50f, 65.00f)), module, Vates::FILTER_INPUT));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(82.50f, 65.00f)), module, Vates::FX_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(93.50f, 65.00f)), module, Vates::FX_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(82.00f, 118.00f)), module, Vates::ENV_OUTPUT));
        addParam(createParamCentered<CKSS>(mm2px(Vec(8.00f, 84.00f)), module, Vates::SYNC_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(21.00f, 82.50f)), module, Vates::RATE_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(33.00f, 82.50f)), module, Vates::LFO_ATT_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(44.00f, 82.50f)), module, Vates::LFO_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(66.04f, 82.50f)), module, Vates::LFO_RESET_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(82.00f, 82.50f)), module, Vates::TRI_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(114.00f, 82.50f)), module, Vates::PULSE_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(98.00f, 82.50f)), module, Vates::SAW_OUTPUT));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(126.50f, 82.50f)), module, Vates::PWM_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(110.00f, 65.00f)), module, Vates::TEMPO_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(121.00f, 65.00f)), module, Vates::CLK_INPUT));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(16.51f, 118.00f)), module, Vates::RHYTHM_PARAM));
        addParam(createParamCentered<CKSSThreePos>(mm2px(Vec(16.00f, 98.00f)), module, Vates::GSW_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(25.50f, 100.00f)), module, Vates::G_INPUT));
        addParam(createParamCentered<CKSSThreePos>(mm2px(Vec(38.00f, 98.00f)), module, Vates::CSW_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(47.50f, 100.00f)), module, Vates::C_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(66.04f, 100.00f)), module, Vates::PAT_RESET_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(27.51f, 118.00f)), module, Vates::RHYTHM_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(82.00f, 100.00f)), module, Vates::GATE_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(98.00f, 100.00f)), module, Vates::CV_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(114.00f, 100.00f)), module, Vates::CLK_OUTPUT));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(44.03f, 118.00f)), module, Vates::LEVEL_PARAM));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(98.00f, 118.00f)), module, Vates::LEFT_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(114.00f, 118.00f)), module, Vates::RIGHT_OUTPUT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(103.00f, 115.00f)), module, Vates::LEFT_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(119.00f, 115.00f)), module, Vates::RIGHT_LIGHT));
        // @layout:end

		VatesDisplay* bankDisp = new VatesDisplay;
		bankDisp->module = module;
		bankDisp->isBank = true;
		bankDisp->box.pos = mm2px(Vec(8.f, 9.5f));
		bankDisp->box.size = mm2px(Vec(53.f, 8.f));
		addChild(bankDisp);

		VatesDisplay* sampleDisp = new VatesDisplay;
		sampleDisp->module = module;
		sampleDisp->isBank = false;
		sampleDisp->box.pos = mm2px(Vec(71.08f, 9.5f));
		sampleDisp->box.size = mm2px(Vec(53.f, 8.f));
		addChild(sampleDisp);
	}

	void appendContextMenu(Menu* menu) override {
		Vates* m = getModule<Vates>();
		if (!m)
			return;

		menu->addChild(new MenuSeparator);

		std::vector<std::string> noteNames;
		for (int i = 0; i < 12; i++)
			noteNames.push_back(imber_dsp::noteName(i));
		menu->addChild(createIndexSubmenuItem("Root", noteNames,
			[=]() { return m->rootNote; },
			[=](int v) { m->rootNote = v; }));

		std::vector<std::string> scaleNames;
		for (int i = 0; i < imber_dsp::kScaleCount; i++)
			scaleNames.push_back(imber_dsp::kScales[i].name);
		menu->addChild(createIndexSubmenuItem("Scale", scaleNames,
			[=]() { return m->scaleIndex; },
			[=](int v) { m->scaleIndex = v; }));

		menu->addChild(createMenuItem("Reroll kit", "", [=]() {
			m->bankSeed = (uint64_t)random::u32() | 1ull;
			m->pendingGen = true;
		}));

		menu->addChild(createBoolPtrMenuItem("External clock takes over", "",
			&m->honourExternalClock));

		menu->addChild(createIndexSubmenuItem("Pattern input window",
			{"0V neutral, +1V randomize, -1V invert", "Hardware: 1.6-3.2V neutral"},
			[=]() { return m->hardwareCvWindow ? 1 : 0; },
			[=](int v) { m->hardwareCvWindow = (v == 1); }));

		menu->addChild(new MenuSeparator);
		std::string folder = forsitan_sampler::getKitsFolder();
		menu->addChild(createMenuLabel(folder.empty() ? "Kits folder: (not set)"
		                                              : "Kits folder: " + folder));
		menu->addChild(createMenuItem("Set kits folder…", "", [=]() {
			char* p = osdialog_file(OSDIALOG_OPEN_DIR,
			                        folder.empty() ? NULL : folder.c_str(), NULL, NULL);
			if (p) {
				forsitan_sampler::setKitsFolder(p);
				std::free(p);
				m->refreshKits();
			}
		}));
		menu->addChild(createMenuItem("Rescan kits", string::f("%d found", (int)m->kitNames.size()),
			[=]() { m->refreshKits(); }));

		// How a user kit is cut into banks. Eight is a generated bank and the
		// hardware's own default; it is also what holds play-mode density
		// steady, since a bank twice the size fires twice as often.
		static const int perOptions[4] = {8, 16, 32, 0};
		int cur = 0;
		for (int i = 0; i < 4; i++)
			if (perOptions[i] == m->samplesPerBank)
				cur = i;
		menu->addChild(createIndexSubmenuItem("Samples per bank",
			{"8", "16", "32", "whole kit"},
			[=]() { return cur; },
			[=](int v) {
				m->samplesPerBank = perOptions[clamp(v, 0, 3)];
				m->refreshKits();
			}));
	}
};


Model* modelVates = createModel<Vates, VatesWidget>("vates");
