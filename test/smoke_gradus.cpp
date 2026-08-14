// smoke_gradus — offline sanity checks for the gradus module.
// See smoke_harness.hpp for the shared scaffolding and CSV format.
//
// gradus has no DSP to measure: it is a rule about what happens to one
// number when triggers arrive. So these checks are the rules themselves,
// one per line of the manual, plus the clip settings, the buttons and the
// patch round-trip.

#include "smoke_harness.hpp"
#include "../src/gradus.cpp"

static const char* MOD = "gradus";

// Which side of a row a trigger arrives on.
enum Side { PLUS, MINUS };

static void setRow(Gradus& m, int row, float knob, int mode) {
    m.params[Gradus::STEP1_PARAM + row].setValue(knob);
    m.params[Gradus::MODE1_PARAM + row].setValue((float) mode);
}

static void setClip(Gradus& m, int clip) {
    m.params[Gradus::CLIP_PARAM].setValue((float) clip);
}

// One sample with the listed (row, side) inputs high, then everything low.
// The Schmitt trigger starts in its high state, so an idle sample always
// runs first: that is what arms the edge detectors.
static void fire(Gradus& m, long& frame, std::vector<std::pair<int, Side>> hits) {
    for (int i = 0; i < Gradus::ROWS; i++) {
        m.inputs[Gradus::PLUS1_INPUT + i].setVoltage(0.f);
        m.inputs[Gradus::MINUS1_INPUT + i].setVoltage(0.f);
    }
    m.process(makeArgs(frame++));
    for (size_t k = 0; k < hits.size(); k++) {
        int base = (hits[k].second == PLUS) ? Gradus::PLUS1_INPUT : Gradus::MINUS1_INPUT;
        m.inputs[base + hits[k].first].setVoltage(5.f);
    }
    m.process(makeArgs(frame++));
    for (size_t k = 0; k < hits.size(); k++) {
        int base = (hits[k].second == PLUS) ? Gradus::PLUS1_INPUT : Gradus::MINUS1_INPUT;
        m.inputs[base + hits[k].first].setVoltage(0.f);
    }
}

// One press and release of a row's button.
static void press(Gradus& m, long& frame, int row, Side side) {
    int base = (side == PLUS) ? Gradus::PLUS1_PARAM : Gradus::MINUS1_PARAM;
    m.params[base + row].setValue(0.f);
    m.process(makeArgs(frame++));
    m.params[base + row].setValue(1.f);
    m.process(makeArgs(frame++));
    m.params[base + row].setValue(0.f);
}

static float out(Gradus& m) {
    return m.outputs[Gradus::CV_OUTPUT].getVoltage();
}

static bool near(float a, float b) {
    return std::fabs(a - b) < 1e-5f;
}

// Nothing arrives, nothing moves: the output holds where it was left.
static void testHold() {
    Gradus m; long fr = 0;
    setRow(m, 0, 2.f, Gradus::MODE_ADD);
    fire(m, fr, {{0, PLUS}});
    float afterTrig = out(m);
    for (int i = 0; i < 10000; i++) m.process(makeArgs(fr++));
    report(MOD, "holds_between_triggers", out(m) - afterTrig,
           out(m) == afterTrig && std::isfinite(out(m)));
    report(MOD, "hold_value", afterTrig, near(afterTrig, 2.f));
}

// One knob, two directions: the minus side subtracts what the plus adds.
static void testPlusMinusAdd() {
    Gradus m; long fr = 0;
    setRow(m, 0, 1.5f, Gradus::MODE_ADD);
    for (int i = 0; i < 4; i++) fire(m, fr, {{0, PLUS}});
    report(MOD, "plus_adds", out(m), near(out(m), 6.f));
    for (int i = 0; i < 3; i++) fire(m, fr, {{0, MINUS}});
    report(MOD, "minus_subtracts", out(m), near(out(m), 1.5f));
    // and straight through zero into negative territory
    for (int i = 0; i < 3; i++) fire(m, fr, {{0, MINUS}});
    report(MOD, "minus_goes_negative", out(m), near(out(m), -3.f));
}

// One knob, two targets: the minus side jumps to the negative of it.
static void testPlusMinusJump() {
    Gradus m; long fr = 0;
    setRow(m, 0, 7.25f, Gradus::MODE_JUMP);
    fire(m, fr, {{0, PLUS}});
    report(MOD, "plus_jumps_positive", out(m), near(out(m), 7.25f));
    fire(m, fr, {{0, MINUS}});
    report(MOD, "minus_jumps_negative", out(m), near(out(m), -7.25f));
    fire(m, fr, {{0, MINUS}});
    report(MOD, "jump_is_idempotent", out(m), near(out(m), -7.25f));
}

// A held gate is not a stream of steps: only the rising edge counts.
static void testEdgeNotLevel() {
    Gradus m; long fr = 0;
    setRow(m, 0, 1.f, Gradus::MODE_ADD);
    m.inputs[Gradus::PLUS1_INPUT].setVoltage(0.f);
    m.process(makeArgs(fr++));
    m.inputs[Gradus::PLUS1_INPUT].setVoltage(5.f);
    for (int i = 0; i < 500; i++) m.process(makeArgs(fr++));
    report(MOD, "gate_held_is_one_step", out(m), near(out(m), 1.f));
    m.inputs[Gradus::PLUS1_INPUT].setVoltage(0.f);
    m.process(makeArgs(fr++));
    m.inputs[Gradus::PLUS1_INPUT].setVoltage(5.f);
    m.process(makeArgs(fr++));
    report(MOD, "second_edge_steps_again", out(m), near(out(m), 2.f));
}

// The buttons do what the inputs do, and a held button is one step.
static void testButtons() {
    Gradus m; long fr = 0;
    setRow(m, 2, 2.f, Gradus::MODE_ADD);
    press(m, fr, 2, PLUS);
    report(MOD, "plus_button_adds", out(m), near(out(m), 2.f));
    press(m, fr, 2, MINUS);
    press(m, fr, 2, MINUS);
    report(MOD, "minus_button_subtracts", out(m), near(out(m), -2.f));
    m.params[Gradus::PLUS1_PARAM + 2].setValue(1.f);
    for (int i = 0; i < 500; i++) m.process(makeArgs(fr++));
    report(MOD, "held_button_is_one_step", out(m), near(out(m), 0.f));
    m.params[Gradus::PLUS1_PARAM + 2].setValue(0.f);
    // a button and an input on the same row are the same event source
    setRow(m, 3, 1.f, Gradus::MODE_JUMP);
    press(m, fr, 3, PLUS);
    report(MOD, "button_jumps_too", out(m), near(out(m), 1.f));
}

// Rule 1: a jump in the same sample discards the relative moves, it does
// not land beside them.
static void testJumpBeatsAdd() {
    Gradus m; long fr = 0;
    setRow(m, 0, 2.f, Gradus::MODE_ADD);
    setRow(m, 3, 4.f, Gradus::MODE_JUMP);
    setRow(m, 6, 1.f, Gradus::MODE_ADD);
    fire(m, fr, {{0, PLUS}, {3, PLUS}, {6, MINUS}});
    report(MOD, "jump_beats_add", out(m), near(out(m), 4.f));
    // also when the jump is above the adds rather than between them
    Gradus n; long fr2 = 0;
    setRow(n, 0, 4.f, Gradus::MODE_JUMP);
    setRow(n, 7, 2.f, Gradus::MODE_ADD);
    fire(n, fr2, {{0, MINUS}, {7, PLUS}});
    report(MOD, "jump_beats_add_from_above", out(n), near(out(n), -4.f));
}

// Rule 2: among simultaneous jumps the last in reading order wins, which
// is the lowest row, and within a row the minus side.
static void testLastJumpWins() {
    Gradus m; long fr = 0;
    setRow(m, 1, 2.f, Gradus::MODE_JUMP);
    setRow(m, 4, 5.f, Gradus::MODE_JUMP);
    setRow(m, 5, 9.f, Gradus::MODE_JUMP);
    fire(m, fr, {{1, PLUS}, {4, PLUS}, {5, PLUS}});
    report(MOD, "lowest_jump_wins", out(m), near(out(m), 9.f));
    // the winner is the row's position, not the order the rows were wired
    fire(m, fr, {{5, PLUS}, {1, PLUS}});
    report(MOD, "lowest_jump_wins_regardless", out(m), near(out(m), 9.f));
    // both sides of one row at once: minus is read after plus
    fire(m, fr, {{4, PLUS}, {4, MINUS}});
    report(MOD, "minus_wins_within_a_row", out(m), near(out(m), -5.f));
}

// Rule 3: with no jump in the sample, every add is applied, signs and all.
static void testAddsSum() {
    Gradus m; long fr = 0;
    setRow(m, 0, 1.f, Gradus::MODE_ADD);
    setRow(m, 1, 2.f, Gradus::MODE_ADD);
    setRow(m, 2, 0.5f, Gradus::MODE_ADD);
    fire(m, fr, {{0, PLUS}, {1, PLUS}, {2, MINUS}});
    report(MOD, "adds_sum", out(m), near(out(m), 2.5f));
    // opposite sides of the same row cancel rather than fighting
    fire(m, fr, {{1, PLUS}, {1, MINUS}});
    report(MOD, "row_sides_cancel", out(m), near(out(m), 2.5f));
}

// The clip knob is the output's range, and the value cannot leave it.
static void testClip() {
    Gradus m; long fr = 0;
    setRow(m, 0, 3.f, Gradus::MODE_ADD);

    setClip(m, Gradus::CLIP_UNI);
    for (int i = 0; i < 20; i++) fire(m, fr, {{0, PLUS}});
    report(MOD, "uni_ceiling", out(m), near(out(m), 10.f));
    for (int i = 0; i < 20; i++) fire(m, fr, {{0, MINUS}});
    report(MOD, "uni_floor", out(m), near(out(m), 0.f));

    setClip(m, Gradus::CLIP_BI5);
    for (int i = 0; i < 20; i++) fire(m, fr, {{0, PLUS}});
    report(MOD, "bi5_ceiling", out(m), near(out(m), 5.f));
    for (int i = 0; i < 20; i++) fire(m, fr, {{0, MINUS}});
    report(MOD, "bi5_floor", out(m), near(out(m), -5.f));

    setClip(m, Gradus::CLIP_BI10);
    for (int i = 0; i < 20; i++) fire(m, fr, {{0, PLUS}});
    report(MOD, "bi10_ceiling", out(m), near(out(m), 10.f));
    for (int i = 0; i < 20; i++) fire(m, fr, {{0, MINUS}});
    report(MOD, "bi10_floor", out(m), near(out(m), -10.f));

    setClip(m, Gradus::CLIP_NONE);
    for (int i = 0; i < 20; i++) fire(m, fr, {{0, PLUS}});
    report(MOD, "no_clip_runs_free", out(m), near(out(m), 50.f));
    report(MOD, "no_clip_finite", out(m), std::isfinite(out(m)));
}

// Narrowing the clip pulls a value that is now out of range back in, on
// the next sample rather than the next trigger.
static void testClipNarrows() {
    Gradus m; long fr = 0;
    setRow(m, 0, 9.f, Gradus::MODE_JUMP);
    setClip(m, Gradus::CLIP_BI10);
    fire(m, fr, {{0, PLUS}});
    report(MOD, "clip_wide_holds_nine", out(m), near(out(m), 9.f));
    setClip(m, Gradus::CLIP_BI5);
    m.process(makeArgs(fr++));
    report(MOD, "clip_narrowing_pulls_in", out(m), near(out(m), 5.f));
}

// The output survives a patch save and reload.
static void testPatchRoundTrip() {
    Gradus m; long fr = 0;
    setRow(m, 0, 2.5f, Gradus::MODE_ADD);
    fire(m, fr, {{0, MINUS}});
    fire(m, fr, {{0, MINUS}});
    json_t* j = m.dataToJson();

    Gradus n; long fr2 = 0;
    n.dataFromJson(j);
    n.process(makeArgs(fr2++));
    report(MOD, "patch_restores_value", n.value - m.value, near(n.value, m.value));
    report(MOD, "patch_restores_output", out(n), near(out(n), -5.f));
    json_decref(j);
}

// A garbage value in the patch cannot put the output out of range.
static void testPatchClamps() {
    Gradus n; long fr = 0;
    json_t* j = json_object();
    json_object_set_new(j, "value", json_real(1e9));
    n.dataFromJson(j);
    n.process(makeArgs(fr++));
    report(MOD, "patch_value_clamped", out(n), near(out(n), 10.f));
    json_decref(j);

    Gradus p; long fr3 = 0;
    json_t* k = json_object();
    json_object_set_new(k, "value", json_real(std::numeric_limits<double>::infinity()));
    p.dataFromJson(k);
    p.process(makeArgs(fr3++));
    report(MOD, "patch_infinity_rejected", out(p), std::isfinite(out(p)));
    json_decref(k);
}

// Random triggers on random rows and sides, with the clip moving under
// them: the output stays finite and inside whatever range is selected.
static void testFuzz() {
    Gradus m; long fr = 0;
    for (int i = 0; i < Gradus::ROWS; i++)
        setRow(m, i, random::uniform() * 10.f, random::uniform() < 0.5f
               ? Gradus::MODE_ADD : Gradus::MODE_JUMP);
    Stats s;
    bool inRange = true;
    for (int n = 0; n < 20000; n++) {
        if (n % 2000 == 0) setClip(m, (int) (random::uniform() * 3.f));
        for (int i = 0; i < Gradus::ROWS; i++) {
            m.inputs[Gradus::PLUS1_INPUT + i].setVoltage(
                random::uniform() < 0.02f ? 5.f : 0.f);
            m.inputs[Gradus::MINUS1_INPUT + i].setVoltage(
                random::uniform() < 0.02f ? 5.f : 0.f);
        }
        m.process(makeArgs(fr++));
        float v = out(m);
        s.add(v);
        if (!(v >= -10.f - 1e-5f && v <= 10.f + 1e-5f)) inRange = false;
    }
    report(MOD, "fuzz_finite", (double) s.nans, s.nans == 0);
    report(MOD, "fuzz_in_range", s.peak, inRange);
}

SMOKE_MAIN(testHold, testPlusMinusAdd, testPlusMinusJump, testEdgeNotLevel,
           testButtons, testJumpBeatsAdd, testLastJumpWins, testAddsSum,
           testClip, testClipNarrows, testPatchRoundTrip, testPatchClamps,
           testFuzz)
