// vespae.cpp - VCV Rack 2 module
// vespae (Latin: "of the wasp") is an emulation of the EDP Wasp / Doepfer
// A-124 filter: a 12 dB/oct state-variable filter built, to save money in
// 1978, out of CD4069 CMOS inverters instead of op-amps and run from a single
// unipolar supply. That compromise is the whole point - the inverters are
// sloppy, asymmetric amplifiers with a switching threshold that is not quite
// mid-supply, the OTAs saturate and slam into the rails, and a diode pair
// across the resonance network clamps the feedback once it gets loud. The
// result is the filter's famous erratic, dirty voice.
//
// The topology and component values follow the circuit analysis in
//   L. Köper, M. Holters, F. Esqueda, J. D. Parker, "A Virtual Analog Model
//   of the EDP Wasp VCF", Proc. DAFx-22, Vienna, 2022,
// which models the Doepfer A-124 version (R13 = 1M, +12 V). This is a
// cheap real-time caricature of that paper, not its white-box state-space
// model: the topology, the resonance network, the damping terms and the
// nonlinearity *shapes* are the circuit's, but they are solved with a
// zero-delay-feedback SVF and gain-scheduled saturators rather than a Newton
// solve. See doc/vespae.md for what is and is not modelled.
//
// The hardware brings out only BP and a single LP/HP jack whose pot
// crossfades the two. We keep the four filter nodes on their own jacks and
// add that pot as a fifth output, with a CV input for it as on the A-124-2.
//
// Two trimpots are mods rather than emulation: BIAS walks the CD4069's
// switching point further off mid-supply, HISS raises the inverter noise
// inside the loop. See doc/vespae.md for what each actually does.
//
// Controls:
//   Knobs : CUTOFF, RES, DRIVE, GRIT, MIX
//   Trims : FM (attenuverter), TRACK, BIAS, HISS
//   In    : IN (audio), V/OCT, FM, RES CV, MIX CV
//   Out   : LP, BP, HP, NOTCH, MIX
//   Lights: one level LED per output

#include "forsitan.hpp"
// the ChowDSP variable oversampler already vendored for guttur
#include "guttur/VariableOversampling.hpp"

namespace vespae {

// ── Doepfer A-124 component values (DAFx-22, Table 1) ────────────────────────
static constexpr float kR3   = 27e3f;      // summing-amp feedback resistor
static constexpr float kR4   = 27e3f;      // resonance leg shunted by D1/D2
static constexpr float kR13  = 1e6f;
static constexpr float kR14  = 1e3f;
static constexpr float kR15  = 100e3f;
static constexpr float kRres = 50e3f;      // resonance pot
static constexpr float kC7   = 0.22e-6f;
// R2 * C2: the LP feedback leg has C2 across R2, which adds a damping term
// proportional to the cutoff frequency. This is why the Wasp's maximum Q
// falls as the filter opens (Q ~ 94 at 400 Hz, ~ 6 at 10 kHz).
static constexpr float kR2C2 = 27e3f * 100e-12f;

// 1N4148 pair across R4, Shockley parameters fitted in the paper
static constexpr float kIs  = 2.52e-9f;
static constexpr float kNVt = 1.752f * 25.85e-3f;

// OTA saturation: i = a * ibias * tanh(b * v_ota / 2Vt), with the input
// divider v_ota = -16.26e-3 * v_IC1 and b = 0.9408, Vt = 25 mV. Collapsed
// into one "tanh argument per volt at the summing-amp output".
static constexpr float kOtaPerVolt = 0.3059f;

// Rail knees, normalised so +-1 is the supply rail. The fitted OTA rail
// parameters put the transitions at 0.61 V and 10.81 V on a 12 V supply,
// i.e. asymmetrically about the 6 V virtual ground: a fifth of the swing
// of headroom at the top, a tenth at the bottom.
static constexpr float kRoomHi = 0.20f;
static constexpr float kRoomLo = 0.10f;
// The CD4069 switching point sits below mid-supply, so the summing stage
// carries a standing offset. It is what makes the clipping lopsided.
static constexpr float kInvBias = 0.06f;
// The diode pair is biased by the HP node through the rest of the star, and
// taken literally it holds the resonance down at a tenth of the rail - where
// the paper's own state-space plots (Fig. 11) show it reaching them. This
// trim backs the clamp off to where the hardware actually sits.
static constexpr float kDiodeTrim = 0.5f;

// ── the two panel mods, neither of them on the hardware ─────────────────────
// How far the bias trimpot can walk the inverter threshold, and the range of
// the hiss trimpot. kHissMin is also the idle dither that lets the filter
// find its way into self-oscillation from silence.
static constexpr float kBiasRange = 0.50f;
static constexpr float kHissMin = 1e-5f;
static constexpr float kHissMax = 3e-2f;

// ── cheap saturators ────────────────────────────────────────────────────────
// Padé tanh, exact enough (<0.4 %) and bounded once the argument is clamped.
static inline float ftanh(float x) {
    x = clamp(x, -3.f, 3.f);
    const float x2 = x * x;
    return x * (27.f + x2) / (27.f + 9.f * x2);
}

// tanh(u)/u for u >= 0: the instantaneous gain of a tanh saturator, used to
// schedule the integrator gains instead of iterating the implicit solve.
static inline float satGain(float u) {
    if (u < 1e-3f) return 1.f;
    return ftanh(u) / u;
}

// Asymmetric soft clip, asymptotic at the supply rails and *strictly*
// increasing: a clipper with a genuinely flat top lets the integrator states
// latch against a rail with no gradient left to walk back down, and the
// filter dies silently. Unit slope at each knee, so it is C1 there too.
// Smaller rooms mean the signal runs linear for longer and then turns hard.
static inline float railClip(float x, float roomHi, float roomLo) {
    const float hi = 1.f - roomHi;
    if (x > hi) {
        const float t = x - hi;
        return hi + roomHi * t / (roomHi + t);
    }
    const float lo = -1.f + roomLo;
    if (x < lo) {
        const float t = lo - x;
        return lo - roomLo * t / (roomLo + t);
    }
    return x;
}

// ── the filter core ─────────────────────────────────────────────────────────
// Signals are normalised so +-1 is the supply rail. Node names follow the
// paper: hp is the summing inverter IC1, bp the first integrator IC3, lp the
// second IC5. All three outputs are inverting, as in the circuit.
struct Core {
    // TPT integrator states
    float s1 = 0.f, s2 = 0.f;
    // one-pole state of the resonance network's shelf
    float shelfZ = 0.f;
    // last solved node voltages: they schedule the OTA gains and bias the diodes
    float hpZ = 0.f, bpZ = 0.f;
    float hp = 0.f, bp = 0.f, lp = 0.f;

    // control-rate coefficients
    float g = 0.01f;          // prewarped integrator gain, tan(pi fc / fs)
    float kd = 0.f;           // R2*C2*wc, the cutoff-dependent damping
    float negK = 0.f;         // damping cancelled at the top of the RES knob
    float Ra = 0.f, Rb = 0.f, Rc = 0.f;   // resonance star network
    float otaK = 1.5f;        // tanh knee per unit of normalised node voltage
    float diodeBias = 1.f;    // volts across D1/D2 per unit of normalised hp
    float invBias = kInvBias; // CD4069 threshold offset from mid-supply
    float roomHi = kRoomHi, roomLo = kRoomLo;
    float Tos = 1.f / 96000.f;

    void reset() { s1 = s2 = shelfZ = hpZ = bpZ = hp = bp = lp = 0.f; }

    // Delta-to-Y of the resonance triangle (paper eqs. 21-23). Only the pot
    // setting enters here, so this runs at control rate.
    void setRes(float rho) {
        const float L1 = rho * kRres + kR14;
        const float L2 = (1.f - rho) * kRres;
        const float L3 = kR13 + kR15;
        const float sum = kRres + kR13 + kR14 + kR15;
        Ra = L1 * L3 / sum;
        Rb = L2 * L3 / sum;
        Rc = L1 * L2 / sum;
    }

    // Re{H1(jwc)} + R2 C2 wc with the diodes off: the small-signal damping,
    // i.e. 1/Q (paper eq. 32).
    float smallSignalDamping(float wc) const {
        const float S    = Ra + Rc + kR4;
        const float D    = Rb * S + Ra * (Rc + kR4);
        const float k0   = kR3 / S;
        const float kinf = kR3 * Rb / D;
        const float wp   = S / (kC7 * D);
        const float r    = wc / wp;
        return (k0 + kinf * r * r) / (1.f + r * r) + kR2C2 * wc;
    }

    void process(float u) {
        // ── D1/D2 across R4. They are open for small signals, which is why
        // the Wasp is so eager to resonate; once the HP node clears a diode
        // drop they shunt R4, damping shoots up and the resonance self-limits.
        const float vd = std::fabs(hpZ) * diodeBias;
        float shunt = 0.f;                          // R4 * G_diode
        if (vd > 0.15f) {
            const float e = std::exp(std::min(vd / kNVt, 30.f));
            shunt = std::min(kR4 * 2.f * kIs * (e - 1.f) / vd, 60.f);
        }
        const float R4e = kR4 / (1.f + shunt);

        // ── resonance network as a one-pole shelf: k0 at DC, kinf at HF
        const float S    = Ra + Rc + R4e;
        const float D    = Rb * S + Ra * (Rc + R4e);
        const float invD = 1.f / D;
        const float k0   = kR3 / S;
        const float kinf = kR3 * Rb * invD;
        const float wp   = S * invD / kC7;
        const float a    = wp * Tos / (1.f + wp * Tos);

        // split into an instantaneous coefficient (goes into the ZDF solve)
        // and a state term (a known constant this sample)
        const float kInst = kinf + (k0 - kinf) * a;
        const float cTerm = (k0 - kinf) * (1.f - a) * shelfZ;
        const float K     = kInst + kd - negK;

        // ── OTA transconductance saturation, scheduled on the last sample
        const float g1 = g * satGain(otaK * std::fabs(hpZ));
        const float g2 = g * satGain(otaK * std::fabs(bpZ));

        // ── zero-delay-feedback solve of hp = -(u + K*bp + lp)
        const float denom = 1.f + K * g1 + g1 * g2;
        hp = -(u + cTerm + (K + g2) * s1 + s2) / denom;

        // summing inverter: finite headroom, threshold below mid-supply
        hp = railClip(hp - invBias, roomHi, roomLo);

        // integrators, each output clamped by its inverter's rails; feeding
        // the clamped value back into the state is what bounds the whole loop
        bp = railClip(g1 * hp + s1, roomHi, roomLo);
        s1 = bp + g1 * hp;
        lp = railClip(g2 * bp + s2, roomHi, roomLo);
        s2 = lp + g2 * bp;

        shelfZ += a * (bp - shelfZ);
        hpZ = hp;
        bpZ = bp;

        if (!std::isfinite(hp) || !std::isfinite(lp)) reset();
    }
};

// one-pole DC blocker, standing in for the circuit's coupling capacitors
struct DCBlock {
    float xz = 0.f, yz = 0.f, r = 0.9995f;
    void setRate(float sr) { r = 1.f - 2.f * (float)M_PI * 10.f / sr; }
    void reset() { xz = yz = 0.f; }
    float process(float x) {
        yz = x - xz + r * yz;
        xz = x;
        return yz;
    }
};

} // namespace vespae

struct Vespae : Module {
    enum ParamId {
        CUTOFF_PARAM,
        RES_PARAM,
        DRIVE_PARAM,
        GRIT_PARAM,
        MIX_PARAM,
        FM_PARAM,
        TRACK_PARAM,
        BIAS_PARAM,
        HISS_PARAM,
        PARAMS_LEN
    };
    enum InputId {
        AUDIO_INPUT,
        VOCT_INPUT,
        FM_INPUT,
        RES_CV_INPUT,
        MIX_CV_INPUT,
        INPUTS_LEN
    };
    // LP..NOTCH are taken straight off the filter's nodes; MIX is the A-124's
    // own output, a pot crossfading the LP and HP nodes.
    enum OutputId {
        LP_OUTPUT,
        HP_OUTPUT,
        BP_OUTPUT,
        NOTCH_OUTPUT,
        MIX_OUTPUT,
        OUTPUTS_LEN
    };
    enum LightId {
        LP_LIGHT,
        HP_LIGHT,
        BP_LIGHT,
        NOTCH_LIGHT,
        MIX_LIGHT,
        LIGHTS_LEN
    };
    static const int kOuts = 5;

    vespae::Core core;
    vespae::DCBlock dcBlock[kOuts];
    VariableOversampling<> upsampler;
    AAFilter<4> decim[kOuts];
    float osOut[kOuts][16] = {};
    float levelEnv[kOuts] = {};
    float acX = 0.f, acY = 0.f, acR = 0.9974f;   // ~20 Hz input coupling
    uint32_t noise = 0x1234567u;
    float noiseAmt = vespae::kHissMin;

    int osIndex = 1;             // 2^osIndex, default 2x
    int lastOsIndex = -1;
    float lastSampleRate = 0.f;
    // GRIT and DRIVE only move when a hand does; their pow() calls are the
    // most expensive thing in the control block, so cache them on the knob.
    float lastGrit = -1.f, lastDrive = -1.f, lastHiss = -1.f;
    float supplyC = 4.9f, otaKC = 1.5f, roomHiC = 0.2f, roomLoC = 0.1f;
    float driveGainC = 1.f;

    Vespae() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        configParam(CUTOFF_PARAM, 0.f, 1.f, 0.6f, "Cutoff", " Hz", 1024.f, 20.f);
        configParam(RES_PARAM,    0.f, 1.f, 0.3f, "Resonance", "%", 0.f, 100.f);
        configParam(DRIVE_PARAM,  0.f, 1.f, 0.5f, "Drive", " dB", 0.f, 43.2f, -21.6f);
        configParam(GRIT_PARAM,   0.f, 1.f, 0.5f, "Grit (supply headroom)", "%", 0.f, 100.f);
        configParam(MIX_PARAM,    0.f, 1.f, 0.f,  "Mix (lowpass to highpass)", "%", 0.f, 100.f);
        configParam(FM_PARAM,    -1.f, 1.f, 0.f,  "FM amount", "%", 0.f, 100.f);
        configParam(TRACK_PARAM,  0.f, 1.f, 1.f,  "V/oct tracking", "%", 0.f, 100.f);
        configParam(BIAS_PARAM,   0.f, 1.f, 0.f,  "Inverter bias (mod: lopsided clipping)", "%", 0.f, 100.f);
        configParam(HISS_PARAM,   0.f, 1.f, 0.f,  "Inverter hiss (mod: noise in the loop)", "%", 0.f, 100.f);
        configInput(AUDIO_INPUT,  "Audio");
        configInput(VOCT_INPUT,   "1V/oct cutoff");
        configInput(FM_INPUT,     "Cutoff FM");
        configInput(RES_CV_INPUT, "Resonance CV");
        configInput(MIX_CV_INPUT, "Mix CV");
        configOutput(LP_OUTPUT,    "Lowpass");
        configOutput(HP_OUTPUT,    "Highpass");
        configOutput(BP_OUTPUT,    "Bandpass");
        configOutput(NOTCH_OUTPUT, "Notch");
        configOutput(MIX_OUTPUT,   "Lowpass/highpass mix");
        configLight(LP_LIGHT,    "Lowpass level");
        configLight(HP_LIGHT,    "Highpass level");
        configLight(BP_LIGHT,    "Bandpass level");
        configLight(NOTCH_LIGHT, "Notch level");
        configLight(MIX_LIGHT,   "Mix level");
        configBypass(AUDIO_INPUT, LP_OUTPUT);
        configBypass(AUDIO_INPUT, MIX_OUTPUT);
    }

    void onReset() override {
        core.reset();
        for (int i = 0; i < kOuts; i++) { dcBlock[i].reset(); levelEnv[i] = 0.f; }
        acX = acY = 0.f;
        lastOsIndex = -1;
    }

    void onSampleRateChange() override { lastOsIndex = -1; }

    json_t* dataToJson() override {
        json_t* root = json_object();
        json_object_set_new(root, "oversampling", json_integer(osIndex));
        return root;
    }
    void dataFromJson(json_t* root) override {
        if (json_t* j = json_object_get(root, "oversampling"))
            osIndex = clamp((int)json_integer_value(j), 0, 4);
        lastOsIndex = -1;
    }

    // xorshift, for the ignition dither that lets the filter self-oscillate
    float dither() {
        noise ^= noise << 13; noise ^= noise >> 17; noise ^= noise << 5;
        return ((float)(noise & 0xffffff) / 8388608.f - 1.f);
    }

    void process(const ProcessArgs& args) override {
        const float sr = args.sampleRate;

        if (osIndex != lastOsIndex || sr != lastSampleRate) {
            upsampler.setOversamplingIndex(osIndex);
            upsampler.reset(sr);
            for (int i = 0; i < kOuts; i++) {
                decim[i].reset(sr, 1 << osIndex);
                dcBlock[i].setRate(sr * (1 << osIndex));
            }
            acR = 1.f - 2.f * (float)M_PI * 20.f / sr;
            lastOsIndex = osIndex;
            lastSampleRate = sr;
            core.reset();
        }

        const int ratio = 1 << osIndex;
        const float fsOs = sr * (float)ratio;
        core.Tos = 1.f / fsOs;

        // ── cutoff ──────────────────────────────────────────────────────────
        float octaves = 10.f * params[CUTOFF_PARAM].getValue();
        octaves += params[TRACK_PARAM].getValue() * inputs[VOCT_INPUT].getVoltage();
        octaves += params[FM_PARAM].getValue() * inputs[FM_INPUT].getVoltage();
        float fc = 20.f * dsp::exp2_taylor5(clamp(octaves, -2.f, 14.f));
        fc = clamp(fc, 8.f, 0.45f * fsOs);
        core.g  = std::min(std::tan((float)M_PI * fc / fsOs), 4.f);
        const float wc = 2.f * (float)M_PI * fc;
        core.kd = vespae::kR2C2 * wc;

        // ── resonance ───────────────────────────────────────────────────────
        float res = params[RES_PARAM].getValue();
        if (inputs[RES_CV_INPUT].isConnected())
            res = clamp(res + inputs[RES_CV_INPUT].getVoltage() * 0.1f, 0.f, 1.f);
        core.setRes(res);
        // The circuit's own damping never quite reaches zero, and the A-124
        // manual is explicit that the filter cannot self-oscillate. Over the
        // last tenth of the knob we cancel that damping anyway, so it tips
        // into oscillation: a deliberate addition, bounded by the diode clamp.
        // A fixed excess rather than a proportional one: the circuit's own
        // damping is smallest around 400 Hz, so scaling it there would leave
        // the least push exactly where the filter is most resonant, and the
        // self-oscillation would dip in the middle of the range.
        const float top = clamp((res - 0.90f) * 10.f, 0.f, 1.f);
        core.negK = top * (core.smallSignalDamping(wc) + 0.006f);

        // ── grit: the supply rails, from a roomy 12 V down to a mean 2 V ────
        // Lower supply => the OTA tanh knee sits far above the rails, so hard
        // clipping wins over soft compression, and the diodes take longer to
        // clamp the resonance. That is the whole 5 V-EDP vs 12 V-Doepfer axis.
        const float grit = params[GRIT_PARAM].getValue();
        if (grit != lastGrit) {
            lastGrit = grit;
            supplyC = 12.f * std::pow(1.f / 6.f, grit);
            otaKC   = vespae::kOtaPerVolt * supplyC;
            // ...and how squarely the inverters hit those rails: a roomy
            // supply bends over early and gently, a mean one runs linear
            // and then slams.
            const float soft = 2.f / (1.f + 3.f * grit);
            roomHiC = clamp(vespae::kRoomHi * soft, 0.04f, 0.45f);
            roomLoC = clamp(vespae::kRoomLo * soft, 0.02f, 0.30f);
        }
        core.otaK   = otaKC;
        core.roomHi = roomHiC;
        core.roomLo = roomLoC;
        // The diode branch is in series with the rest of the star, so only
        // part of the HP swing lands across the pair - and less of it the
        // higher the resonance, which is why the Wasp gets wilder up there.
        core.diodeBias = supplyC * vespae::kDiodeTrim
                       * vespae::kR4 / (vespae::kR4 + core.Ra + core.Rc);

        // ── input: level pot, then the C1/R1 AC coupling ────────────────────
        // ── the two mods ────────────────────────────────────────────────────
        // Neither is on the A-124. bias walks the CD4069's switching point
        // further off mid-supply, so the two halves of the waveform clip at
        // very different levels and the rasp turns even-harmonic. hiss is the
        // inverter noise, which does almost nothing to a loud signal but
        // wanders the operating point of a loop that is close to oscillating.
        core.invBias = vespae::kInvBias
                     + vespae::kBiasRange * params[BIAS_PARAM].getValue();
        const float hiss = params[HISS_PARAM].getValue();
        if (hiss != lastHiss) {
            lastHiss = hiss;
            noiseAmt = vespae::kHissMin
                     * std::pow(vespae::kHissMax / vespae::kHissMin, hiss);
        }

        const float drive = params[DRIVE_PARAM].getValue();
        if (drive != lastDrive) {
            lastDrive = drive;
            driveGainC = std::pow(12.f, 2.f * drive - 1.f);
        }
        const float driveGain = driveGainC;
        const float raw = inputs[AUDIO_INPUT].getVoltage();
        acY = acR * (acY + raw - acX);
        acX = raw;
        const float u = acY * driveGain * 0.2f;

        // ── mix: the A-124's own output stage ───────────────────────────────
        // A plain pot with the LP node on one end and the HP node on the
        // other, wiper to the jack. Not just a notch at the centre: the null
        // slides down from above the cutoff to below it as the knob turns,
        // which is the manual's "asymmetrical / symmetrical / asymmetrical
        // notch". Being a convex blend of two bounded nodes it cannot exceed
        // either of them, so unlike the notch summer it needs no clipping.
        float mix = params[MIX_PARAM].getValue();
        if (inputs[MIX_CV_INPUT].isConnected())
            mix = clamp(mix + inputs[MIX_CV_INPUT].getVoltage() * 0.1f, 0.f, 1.f);

        // ── run the core at the oversampled rate ────────────────────────────
        upsampler.upsample(u);
        const float* osIn = upsampler.getOSBuffer();
        for (int k = 0; k < ratio; k++) {
            core.process(osIn[k] + noiseAmt * dither());
            osOut[0][k] = dcBlock[0].process(core.lp);
            osOut[1][k] = dcBlock[1].process(core.hp);
            osOut[2][k] = dcBlock[2].process(core.bp);
            // the notch summer is another inverter on the same supply, so it
            // clips too - without that, HP and LP hitting a rail together
            // would hand back twice the swing of any other output
            osOut[3][k] = dcBlock[3].process(
                vespae::railClip(core.hp + core.lp, core.roomHi, core.roomLo));
            osOut[4][k] = dcBlock[4].process(
                core.lp + mix * (core.hp - core.lp));
        }

        for (int i = 0; i < kOuts; i++) {
            float y = 0.f;
            for (int k = 0; k < ratio; k++) y = decim[i].process(osOut[i][k]);
            if (!std::isfinite(y)) { y = 0.f; decim[i].reset(sr, ratio); }
            outputs[LP_OUTPUT + i].setVoltage(clamp(5.f * y, -10.f, 10.f));
            levelEnv[i] += (std::fabs(y) - levelEnv[i]) * 0.002f;
            lights[LP_LIGHT + i].setBrightness(clamp(levelEnv[i] * 1.4f, 0.f, 1.f));
        }
    }
};

struct VespaeWidget : ModuleWidget {
    VespaeWidget(Vespae* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/vespae.svg")));

// @layout:begin vespae 60.96 128.5
// @elem SCREW_TL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_TR ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BR ScrewSilver 3.5 screw "" 0.0
// @elem FM_PARAM Trimpot 3.03 param "" 0.0
// @elem TRACK_PARAM Trimpot 3.03 param "" 0.0
// @elem CUTOFF_PARAM RoundBigBlackKnob 7.62 param "" 0.0
// @elem BIAS_PARAM Trimpot 3.03 param "" 0.0
// @elem HISS_PARAM Trimpot 3.03 param "" 0.0
// @elem RES_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem DRIVE_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem GRIT_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem MIX_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem AUDIO_INPUT PJ301MPort 4.01 input "" 0.0
// @elem VOCT_INPUT PJ301MPort 4.01 input "" 0.0
// @elem FM_INPUT PJ301MPort 4.01 input "" 0.0
// @elem RES_CV_INPUT PJ301MPort 4.01 input "" 0.0
// @elem MIX_CV_INPUT PJ301MPort 4.01 input "" 0.0
// @elem LP_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem BP_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem HP_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem NOTCH_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem MIX_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem LP_LIGHT SmallLight 1.0 light "" 0.0
// @elem BP_LIGHT SmallLight 1.0 light "" 0.0
// @elem HP_LIGHT SmallLight 1.0 light "" 0.0
// @elem NOTCH_LIGHT SmallLight 1.0 light "" 0.0
// @elem MIX_LIGHT SmallLight 1.0 light "" 0.0
// @elem LABEL_FM label 0.0 label "fm" 0.0 11.00 28.50
// @elem LABEL_TRACK label 0.0 label "trk" 0.0 11.00 39.60
// @elem LABEL_CUTOFF label 0.0 label "cutoff" 0.0 30.48 38.50
// @elem LABEL_BIAS label 0.0 label "bias" 0.0 49.96 28.50
// @elem LABEL_HISS label 0.0 label "hiss" 0.0 49.96 39.60
// @elem LABEL_RES label 0.0 label "res" 0.0 10.20 62.50
// @elem LABEL_DRIVE label 0.0 label "drive" 0.0 23.70 62.50
// @elem LABEL_GRIT label 0.0 label "grit" 0.0 37.20 62.50
// @elem LABEL_MIX label 0.0 label "mix" 0.0 50.70 62.50
// @elem LABEL_IN label 0.0 label "in" 0.0 8.48 84.50
// @elem LABEL_VOCT label 0.0 label "v/oct" 0.0 19.48 84.50
// @elem LABEL_FMIN label 0.0 label "fm" 0.0 30.48 84.50
// @elem LABEL_RESCV label 0.0 label "res" 0.0 41.48 84.50
// @elem LABEL_MIXCV label 0.0 label "mix" 0.0 52.48 84.50
// @elem BOX_LP panel_box 7.0 box "" 0.0 14.98 95.50
// @elem BOX_BP panel_box 7.0 box "" 0.0 30.48 95.50
// @elem BOX_HP panel_box 7.0 box "" 0.0 45.98 95.50
// @elem BOX_NOTCH panel_box 7.0 box "" 0.0 22.73 111.00
// @elem BOX_MIX panel_box 7.0 box "" 0.0 38.23 111.00
// @elem LABEL_LP label 0.0 label "lp" 0.0 14.98 101.00
// @elem LABEL_BP label 0.0 label "bp" 0.0 30.48 101.00
// @elem LABEL_HP label 0.0 label "hp" 0.0 45.98 101.00
// @elem LABEL_NOTCH label 0.0 label "notch" 0.0 22.73 116.50
// @elem LABEL_MIXOUT label 0.0 label "mix" 0.0 38.23 116.50
// @elem LOGO forsitan_logo 0.0 logo "" 0.0 30.48 122.50

        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 0.00f)))); // SCREW_TL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(53.34f, 0.00f)))); // SCREW_TR
        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 123.42f)))); // SCREW_BL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(53.34f, 123.42f)))); // SCREW_BR
        addParam(createParamCentered<Trimpot>(mm2px(Vec(11.00f, 22.00f)), module, Vespae::FM_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(11.00f, 33.10f)), module, Vespae::TRACK_PARAM));
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(30.48f, 27.00f)), module, Vespae::CUTOFF_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(49.96f, 22.00f)), module, Vespae::BIAS_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(49.96f, 33.10f)), module, Vespae::HISS_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.20f, 54.00f)), module, Vespae::RES_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(23.70f, 54.00f)), module, Vespae::DRIVE_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(37.20f, 54.00f)), module, Vespae::GRIT_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(50.70f, 54.00f)), module, Vespae::MIX_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(8.48f, 77.00f)), module, Vespae::AUDIO_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(19.48f, 77.00f)), module, Vespae::VOCT_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(30.48f, 77.00f)), module, Vespae::FM_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(41.48f, 77.00f)), module, Vespae::RES_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(52.48f, 77.00f)), module, Vespae::MIX_CV_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(14.98f, 93.50f)), module, Vespae::LP_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(30.48f, 93.50f)), module, Vespae::BP_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(45.98f, 93.50f)), module, Vespae::HP_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(22.73f, 109.00f)), module, Vespae::NOTCH_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(38.23f, 109.00f)), module, Vespae::MIX_OUTPUT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(19.98f, 90.50f)), module, Vespae::LP_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(35.48f, 90.50f)), module, Vespae::BP_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(50.98f, 90.50f)), module, Vespae::HP_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(27.73f, 106.00f)), module, Vespae::NOTCH_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(43.23f, 106.00f)), module, Vespae::MIX_LIGHT));
        // @layout:end
    }

    void appendContextMenu(Menu* menu) override {
        Vespae* m = dynamic_cast<Vespae*>(module);
        if (!m) return;
        menu->addChild(new MenuSeparator);
        menu->addChild(createIndexSubmenuItem("Oversampling",
            {"1× (rawest, lightest)", "2× (default)", "4×", "8×", "16× (cleanest)"},
            [m]() { return m->osIndex; },
            [m](int i) { m->osIndex = i; }));
    }
};

Model* modelVespae = createModel<Vespae, VespaeWidget>("vespae");
