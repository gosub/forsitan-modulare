// radix.hpp - the engine of radix: an integer machine on its own clock.
//
// Built from doc/design/radix.md and nothing else (see the lineage rule
// there). State is a 32-bit phase accumulator, a byte of XOR corruption, three
// free-running counters and an 8-bit shift register. Per engine tick:
//
//   SRC  -> modulator byte m
//   LAW  -> increment from the pitch rate and m
//   acc += increment, wrapping on purpose
//   TABLE[acc] -> 16-bit sample -> BITS -> GRIT -> held until the next tick
//
// The engine runs at `clock` Hz, not the host rate, and the host sees it
// through a zero-order hold with no filter: above the host rate ticks are
// dropped, below it they are repeated, and both alias. That is the sound.
//
// Where this deviates from the tables in the design doc, the doc's formulas
// were sketches that measured badly when written down properly:
//
//   ADD   the doc's `rate + m` moves a 32-bit increment by at most 255, which
//         is inaudible at any pitch. m is scaled to the rate instead:
//         +-50% around the pitch, so it stays near it and still moves.
//   SHIFT `rate << (m >> 5)` only ever goes up, so the RATE knob's pitch was
//         the bottom of the range. The shift is signed, -3..+4 octaves.
//   XOR   the corruption lands on the rate's top eight bits, wherever they
//         are, so it bends the pitch at every RATE instead of only at the
//         bottom.
//   MOD   `acc % (acc + m)` is `acc` unless the sum overflows, so it did
//         nothing, and wrapping the phase early at m/256 of a cycle (the
//         first rewrite) only ever read part of the table: pulse fell silent
//         and sine and saw carried up to 0.7 of DC. The table is read at the
//         accumulator times 1..9, modulo a cycle, instead: a hard sync, with
//         the accumulator as the master.
//
// SRC TABLE was `TABLE[param]`, which on the identity tables (SAW, BITS) is
// PARAM itself: `radix_probe measure` found those ten programs at a distance
// of exactly 0 from their PARAM twins. The table is walked instead, one step
// per cycle with PARAM as the stride: 0 holds still, 1 plays the table's
// shape as a slow figure, 128 alternates two values, odd strides scramble.
//
// COUNT uses the counters as a mask set by PARAM, and the counters step once
// per accumulator cycle, not per tick: per tick they are a fixed buzz at the
// clock rate, per cycle they walk the pitch in a figure that repeats only
// after 256 cycles.
//
// The caller hands over already-mapped parameters; the engine smooths nothing.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace radix {

// ---------------------------------------------------------------- constants

enum Src { SRC_PARAM, SRC_TABLE, SRC_SELF, SRC_COUNT, SRC_IN, NUM_SRC };
enum Law { LAW_ADD, LAW_MUL, LAW_SHIFT, LAW_XOR, LAW_MOD, NUM_LAW };
enum Table { TAB_SINE, TAB_SAW, TAB_PULSE, TAB_NOISE, TAB_BITS, TAB_TEXT, NUM_TABLE };

constexpr int kNumPrograms = NUM_SRC * NUM_LAW * NUM_TABLE;   // 150

constexpr float kClockMin = 100.f;
constexpr float kClockMax = 96000.f;
// The clock at which "Clock moves pitch" plays RATE's own pitch: the default
// CLOCK position, so switching the option on leaves an untouched patch alone.
constexpr float kRefClock = 32000.f;
// More ticks than this per host sample are dropped outright rather than run:
// 96 kHz into an 11 kHz host would otherwise cost nine ticks a sample for
// nothing the hold can show.
constexpr int kMaxTicks = 8;

inline const char* srcName(int i) {
    static const char* n[NUM_SRC] = {"param", "table", "self", "count", "in"};
    return n[i];
}
inline const char* lawName(int i) {
    static const char* n[NUM_LAW] = {"add", "mul", "shift", "xor", "mod"};
    return n[i];
}
inline const char* tableName(int i) {
    static const char* n[NUM_TABLE] = {"sine", "saw", "pulse", "noise", "bits", "text"};
    return n[i];
}

// ------------------------------------------------------------------- tables

struct Tables {
    int16_t sine[256];
    int16_t noise[256];
    int16_t text[256];

    Tables() {
        for (int i = 0; i < 256; i++)
            sine[i] = (int16_t)std::lrint(32767.0 * std::sin(6.283185307179586 * i / 256.0));
        // a fixed LCG, so NOISE is the same table on every machine and in
        // every patch
        uint32_t s = 0x2a2a2a2au;
        for (int i = 0; i < 256; i++) {
            s = s * 1664525u + 1013904223u;
            noise[i] = (int16_t)(s >> 16);
        }
        setText("forsitan radix");
    }

    // The string heard as a staircase of its own letters, repeated to fill
    // 256 steps, centred on its own mean letter and scaled so the letter
    // farthest from it reaches the rail. Spread over the printable range, a
    // lowercase word sat in the top fifth and carried +0.5 of DC; spread from
    // its own lowest to highest letter, the space in "forsitan radix" was the
    // bottom and the rest still sat high, at +0.6. The mean has no DC by
    // construction. An empty string, or one letter repeated, is silence.
    void setText(const std::string& str) {
        std::fill(text, text + 256, (int16_t)0);
        if (str.empty()) return;
        double mean = 0.0;
        for (int i = 0; i < 256; i++) mean += (unsigned char)str[i % str.size()];
        mean /= 256.0;
        double dev = 0.0;
        for (unsigned char c : str) dev = std::max(dev, std::fabs(c - mean));
        if (dev < 0.5) return;
        for (int i = 0; i < 256; i++) {
            double c = (unsigned char)str[i % str.size()];
            text[i] = (int16_t)std::lrint((c - mean) / dev * 32767.0);
        }
    }
};

// --------------------------------------------------------------- parameters

struct Params {
    float freq = 261.63f;   // Hz, with V/oct already applied
    float clock = kRefClock;// engine tick rate, Hz
    float param = 0.f;      // 0..255
    int bits = 16;          // 1..16
    float grit = 0.f;       // 0..1
    int src = SRC_PARAM;
    int law = LAW_ADD;
    int table = TAB_SINE;
    float in = 0.f;         // IN, normalized to -1..1
    bool clockMovesPitch = false;
};

// ------------------------------------------------------------------- engine

struct Engine {
    Tables tables;

    uint32_t acc = 0;
    uint8_t corrupt = 0;        // XOR law state, never cleared by a knob
    uint8_t c1 = 0, c3 = 0, c6 = 0;
    uint8_t reg = 0;            // shift register behind CV OUT
    int16_t held = 0;           // the zero-order hold
    float tickPhase = 0.f;
    float cv = 0.f;             // smoothed DAC, 0..1

    float sampleRate = 44100.f;
    uint32_t ticks = 0;         // engine ticks run, for the probe
    uint32_t cycles = 0;        // accumulator wraps, for the probe

    void setSampleRate(float sr) { sampleRate = sr; }

    void reset() {
        acc = 0; corrupt = 0;
        c1 = c3 = c6 = 0;
        reg = 0; held = 0;
        tickPhase = 0.f; cv = 0.f;
        ticks = cycles = 0;
    }

    // 16-bit sample of the current table at a phase
    int16_t read(int table, uint32_t phase) const {
        uint8_t i = phase >> 24;
        switch (table) {
            case TAB_SINE:  return tables.sine[i];
            case TAB_SAW:   return (int16_t)((phase >> 16) - 32768);
            case TAB_PULSE: return (phase & 0x80000000u) ? 32767 : -32768;
            case TAB_NOISE: return tables.noise[i];
            case TAB_BITS: {
                // the accumulator against itself one byte down
                uint8_t b = (uint8_t)(i ^ (phase >> 16));
                return (int16_t)((b << 8 | b) - 32768);
            }
            default:        return tables.text[i];
        }
    }

    // the same, as a modulator byte
    uint8_t readByte(int table, uint32_t phase) const {
        return (uint8_t)((read(table, phase) + 32768) >> 8);
    }

    // one tick of the machine
    void tick(const Params& p, uint32_t rate) {
        const uint8_t param = (uint8_t)std::min(std::max(p.param, 0.f), 255.f);

        // ---- SRC
        uint8_t m;
        switch (p.src) {
            case SRC_PARAM: m = param; break;
            case SRC_TABLE: m = readByte(p.table, (uint32_t)(uint8_t)(c1 * param) << 24); break;
            // PARAM is where the feedback reads, ahead of the phase
            case SRC_SELF:  m = readByte(p.table, acc + ((uint32_t)param << 24)); break;
            case SRC_COUNT: m = (uint8_t)((c1 ^ c3 ^ c6) & param); break;
            default: {
                // PARAM is the depth: at 0 the input is not heard, m sits at
                // 128, which is the plain pitch under ADD
                float x = std::min(std::max(p.in, -1.f), 1.f) * (float)param / 255.f;
                m = (uint8_t)std::lrint((x + 1.f) * 127.5f);
            }
        }

        // ---- LAW
        uint32_t inc;
        bool wrapped = false;
        switch (p.law) {
            case LAW_ADD:
                // +-50% of the rate, m = 128 is the pitch itself
                inc = rate + (uint32_t)(((int64_t)m - 128) * (int64_t)(rate >> 8));
                break;
            case LAW_MUL:
                // harmonics 1..32 of the rate, overflowing past the clock
                inc = rate * (1u + (m >> 3));
                break;
            case LAW_SHIFT: {
                int s = (m >> 5) - 3;
                inc = s >= 0 ? rate << s : rate >> -s;
                break;
            }
            case LAW_XOR: {
                corrupt ^= m;
                int top = 31;
                while (top > 7 && !(rate >> top)) top--;
                inc = rate ^ ((uint32_t)corrupt << (top - 7));
                break;
            }
            default:
                inc = rate;
        }

        uint32_t prev = acc;
        acc += inc;
        if (acc < prev) wrapped = true;

        if (wrapped) {
            cycles++;
            c1 += 1; c3 += 3; c6 += 6;
            // rungler: a low bit of the sample against the oldest bit. The
            // sign would be the obvious choice and locks: just before a wrap
            // most tables are at the same point of their shape, the data bit
            // is the same every cycle, and an all-zero register stays there.
            uint8_t bit = (uint8_t)(((held >> 8) & 1) ^ (reg >> 7));
            reg = (uint8_t)(reg << 1 | bit);
        }

        // ---- TABLE, BITS, GRIT
        uint32_t phase = acc;
        if (p.law == LAW_MOD)
            // the slave runs at 1 + m/32 of the master, reset by its wrap
            phase = (uint32_t)(((uint64_t)acc * (32u + m)) >> 5);
        int32_t s = read(p.table, phase);
        int bits = std::min(std::max(p.bits, 1), 16);
        if (bits < 16) s &= ~((1 << (16 - bits)) - 1);
        held = (int16_t)s;
        ticks++;
    }

    // GRIT: the accumulator's middle bits XORed into the sample's low ones.
    // The amount is continuous by crossfading the two nearest bit counts.
    //
    // Every bit is worth about 7 dB, so a knob spread evenly over bit counts
    // is a knob whose first half does nothing: 0 to 12 bits measured under
    // -52 dB up to 0.6 of the travel and only -23 dB at the top
    // (`radix_probe grit`). The knob runs over 8 to 16 bits instead, faded in
    // over its first 5%, which is about 5 dB a tenth from -46 dB up to the
    // whole sample corrupted. The XOR source is the accumulator's own bits,
    // so the grit stays locked to the pitch.
    float grit(int16_t s, float amount) const {
        float g = std::min(std::max(amount, 0.f), 1.f);
        if (g <= 0.f) return (float)s;
        float k = 8.f + 8.f * g;
        int k0 = std::min((int)k, 15);
        float f = k - (float)k0;
        uint16_t noise = (uint16_t)((acc >> 16) ^ (acc >> 5));
        auto at = [&](int n) {
            uint16_t mask = (uint16_t)((1u << n) - 1u);
            return (float)(int16_t)((uint16_t)s ^ (noise & mask));
        };
        float a = at(k0);
        float b = at(k0 + 1);
        float y = a + (b - a) * f;
        float fade = std::min(1.f, g / 0.05f);
        return (float)s + (y - (float)s) * fade;
    }

    // One host sample. Returns the audio, -1..1; the CV comes out in `cvOut`,
    // 0..1.
    float process(const Params& p, float& cvOut) {
        float clock = std::min(std::max(p.clock, kClockMin), kClockMax);
        // pitch as a fraction of a cycle per tick: RATE owns the pitch unless
        // the clock is allowed to move it, as on the hardware
        double cyclesPerTick = (double)p.freq / (p.clockMovesPitch ? kRefClock : clock);
        cyclesPerTick -= std::floor(cyclesPerTick);
        uint32_t rate = (uint32_t)(cyclesPerTick * 4294967296.0);

        tickPhase += clock / sampleRate;
        int n = 0;
        while (tickPhase >= 1.f) {
            tickPhase -= 1.f;
            if (n++ < kMaxTicks) tick(p, rate);
        }

        // 0.1 ms smoothing on the stepped DAC, as bulla does
        float dac = (float)(4 * (reg & 1) + 2 * ((reg >> 1) & 1) + ((reg >> 2) & 1)) / 7.f;
        float lag = std::min(1.f, 1.f / (0.0001f * sampleRate));
        cv += (dac - cv) * lag;
        cvOut = cv;

        return grit(held, p.grit) / 32768.f;
    }
};

}  // namespace radix
