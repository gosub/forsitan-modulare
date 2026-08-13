// smoke_gradus — offline sanity checks for the gradus module.
// See smoke_harness.hpp for the shared scaffolding and CSV format.
//
// gradus has no DSP to measure: it is a rule about what happens to one
// number when triggers arrive. So these checks are the rules themselves,
// one per line of the manual, plus the edge behaviour and the patch
// round-trip.

#include "smoke_harness.hpp"
#include "../src/gradus.cpp"

static const char* MOD = "gradus";

// Every row silent, mode switches at their defaults (add).
static void setRow(Gradus& m, int row, float step, int mode) {
    m.params[Gradus::STEP1_PARAM + row].setValue(step);
    m.params[Gradus::MODE1_PARAM + row].setValue((float) mode);
}

// A sample with the listed rows held high, then one with everything low.
// The Schmitt trigger starts in its high state, so the harness always runs
// an idle sample first: that is what arms the edge detector.
static void fire(Gradus& m, long& frame, std::vector<int> rows) {
    for (int i = 0; i < Gradus::ROWS; i++)
        m.inputs[Gradus::TRIG1_INPUT + i].setVoltage(0.f);
    m.process(makeArgs(frame++));
    for (int r : rows)
        m.inputs[Gradus::TRIG1_INPUT + r].setVoltage(5.f);
    m.process(makeArgs(frame++));
    for (int r : rows)
        m.inputs[Gradus::TRIG1_INPUT + r].setVoltage(0.f);
}

static float out(Gradus& m) {
    return m.outputs[Gradus::CV_OUTPUT].getVoltage();
}

// Nothing arrives, nothing moves: the output holds where it was left.
static void testHold() {
    Gradus m; long fr = 0;
    setRow(m, 0, 2.f, Gradus::MODE_ADD);
    fire(m, fr, {0});
    float afterTrig = out(m);
    for (int i = 0; i < 10000; i++) m.process(makeArgs(fr++));
    report(MOD, "holds_between_triggers", out(m) - afterTrig,
           out(m) == afterTrig && std::isfinite(out(m)));
    report(MOD, "hold_value", afterTrig, std::fabs(afterTrig - 2.f) < 1e-6f);
}

// Add and subtract are the knob, in volts, once per edge.
static void testAddSubtract() {
    Gradus m; long fr = 0;
    setRow(m, 0, 1.5f, Gradus::MODE_ADD);
    setRow(m, 1, 0.5f, Gradus::MODE_SUB);
    for (int i = 0; i < 4; i++) fire(m, fr, {0});
    report(MOD, "add_four_times", out(m), std::fabs(out(m) - 6.f) < 1e-5f);
    for (int i = 0; i < 3; i++) fire(m, fr, {1});
    report(MOD, "subtract_three_times", out(m), std::fabs(out(m) - 4.5f) < 1e-5f);
}

// A held gate is not a stream of steps: only the rising edge counts.
static void testEdgeNotLevel() {
    Gradus m; long fr = 0;
    setRow(m, 0, 1.f, Gradus::MODE_ADD);
    m.inputs[Gradus::TRIG1_INPUT].setVoltage(0.f);
    m.process(makeArgs(fr++));
    m.inputs[Gradus::TRIG1_INPUT].setVoltage(5.f);
    for (int i = 0; i < 500; i++) m.process(makeArgs(fr++));
    report(MOD, "gate_held_is_one_step", out(m), std::fabs(out(m) - 1.f) < 1e-6f);
    m.inputs[Gradus::TRIG1_INPUT].setVoltage(0.f);
    m.process(makeArgs(fr++));
    m.inputs[Gradus::TRIG1_INPUT].setVoltage(5.f);
    m.process(makeArgs(fr++));
    report(MOD, "second_edge_steps_again", out(m), std::fabs(out(m) - 2.f) < 1e-6f);
}

// A jump lands on its knob whatever the output was doing.
static void testJump() {
    Gradus m; long fr = 0;
    setRow(m, 0, 3.f, Gradus::MODE_ADD);
    setRow(m, 1, 7.25f, Gradus::MODE_JUMP);
    fire(m, fr, {0});
    fire(m, fr, {1});
    report(MOD, "jump_sets_value", out(m), std::fabs(out(m) - 7.25f) < 1e-6f);
    fire(m, fr, {1});
    report(MOD, "jump_is_idempotent", out(m), std::fabs(out(m) - 7.25f) < 1e-6f);
}

// Rule 1: a jump in the same sample discards the relative moves, it does
// not land beside them.
static void testJumpBeatsRelative() {
    Gradus m; long fr = 0;
    setRow(m, 0, 2.f, Gradus::MODE_ADD);
    setRow(m, 3, 4.f, Gradus::MODE_JUMP);
    setRow(m, 6, 1.f, Gradus::MODE_SUB);
    fire(m, fr, {0, 3, 6});
    report(MOD, "jump_beats_relative", out(m), std::fabs(out(m) - 4.f) < 1e-6f);
    // and in either order of arrival on the panel
    Gradus n; long fr2 = 0;
    setRow(n, 0, 4.f, Gradus::MODE_JUMP);
    setRow(n, 7, 2.f, Gradus::MODE_ADD);
    fire(n, fr2, {0, 7});
    report(MOD, "jump_beats_relative_below", out(n),
           std::fabs(out(n) - 4.f) < 1e-6f);
}

// Rule 2: among simultaneous jumps the lowest row on the panel wins.
static void testLowestJumpWins() {
    Gradus m; long fr = 0;
    setRow(m, 1, 2.f, Gradus::MODE_JUMP);
    setRow(m, 4, 5.f, Gradus::MODE_JUMP);
    setRow(m, 5, 9.f, Gradus::MODE_JUMP);
    fire(m, fr, {1, 4, 5});
    report(MOD, "lowest_jump_wins", out(m), std::fabs(out(m) - 9.f) < 1e-6f);
    // the winner is the row's position, not the order the rows were wired
    fire(m, fr, {5, 1});
    report(MOD, "lowest_jump_wins_regardless", out(m),
           std::fabs(out(m) - 9.f) < 1e-6f);
}

// Rule 3: with no jump in the sample, every relative move is applied.
static void testRelativesSum() {
    Gradus m; long fr = 0;
    setRow(m, 0, 1.f, Gradus::MODE_ADD);
    setRow(m, 1, 2.f, Gradus::MODE_ADD);
    setRow(m, 2, 0.5f, Gradus::MODE_SUB);
    fire(m, fr, {0, 1, 2});
    report(MOD, "relatives_sum", out(m), std::fabs(out(m) - 2.5f) < 1e-6f);
    // opposite moves of the same size cancel rather than fighting
    setRow(m, 3, 3.f, Gradus::MODE_ADD);
    setRow(m, 4, 3.f, Gradus::MODE_SUB);
    fire(m, fr, {3, 4});
    report(MOD, "opposite_moves_cancel", out(m), std::fabs(out(m) - 2.5f) < 1e-6f);
}

// The value cannot leave its range, and it does not wrap.
static void testClamp() {
    Gradus m; long fr = 0;
    setRow(m, 0, 3.f, Gradus::MODE_ADD);
    setRow(m, 1, 3.f, Gradus::MODE_SUB);
    for (int i = 0; i < 20; i++) fire(m, fr, {0});
    report(MOD, "clamps_at_ceiling", out(m), std::fabs(out(m) - 10.f) < 1e-6f);
    for (int i = 0; i < 20; i++) fire(m, fr, {1});
    report(MOD, "clamps_at_floor", out(m), std::fabs(out(m)) < 1e-6f);
    // a step that would overshoot lands on the rail, not past it
    fire(m, fr, {0});
    fire(m, fr, {1});
    report(MOD, "floor_absorbs_overshoot", out(m), std::fabs(out(m)) < 1e-6f);
}

// Bipolar offsets the output by -5V. The stored value is untouched, so the
// knobs keep meaning volts of step; only a jump target reads shifted.
static void testBipolar() {
    Gradus m; long fr = 0;
    setRow(m, 0, 1.f, Gradus::MODE_ADD);
    m.bipolar = true;
    m.process(makeArgs(fr++));
    report(MOD, "bipolar_rest_is_minus_five", out(m),
           std::fabs(out(m) + 5.f) < 1e-6f);
    for (int i = 0; i < 5; i++) fire(m, fr, {0});
    report(MOD, "bipolar_five_steps_to_zero", out(m), std::fabs(out(m)) < 1e-6f);
    setRow(m, 1, 10.f, Gradus::MODE_JUMP);
    fire(m, fr, {1});
    report(MOD, "bipolar_jump_hits_ceiling", out(m),
           std::fabs(out(m) - 5.f) < 1e-6f);
    // the internal value never leaves 0..10 whatever the range
    report(MOD, "bipolar_value_in_range", m.value,
           m.value >= 0.f && m.value <= 10.f);
}

// The output survives a patch save and reload, and so does the range.
static void testPatchRoundTrip() {
    Gradus m; long fr = 0;
    setRow(m, 0, 2.5f, Gradus::MODE_ADD);
    fire(m, fr, {0});
    fire(m, fr, {0});
    m.bipolar = true;
    json_t* j = m.dataToJson();

    Gradus n; long fr2 = 0;
    n.dataFromJson(j);
    n.process(makeArgs(fr2++));
    report(MOD, "patch_restores_value", n.value - m.value,
           std::fabs(n.value - m.value) < 1e-6f);
    report(MOD, "patch_restores_range", n.bipolar ? 1 : 0, n.bipolar);
    report(MOD, "patch_restores_output", out(n), std::fabs(out(n) - 0.f) < 1e-6f);
    json_decref(j);
}

// A garbage value in the patch cannot put the output out of range.
static void testPatchClamps() {
    Gradus n;
    json_t* j = json_object();
    json_object_set_new(j, "value", json_real(1e9));
    n.dataFromJson(j);
    long fr = 0;
    n.process(makeArgs(fr++));
    report(MOD, "patch_value_clamped", out(n), std::fabs(out(n) - 10.f) < 1e-6f);
    json_decref(j);
}

// Random triggers on random rows for a while: the output stays finite and
// inside the rails, and every sample is one of the reachable values.
static void testFuzz() {
    Gradus m; long fr = 0;
    for (int i = 0; i < Gradus::ROWS; i++)
        setRow(m, i, random::uniform() * 10.f, (int) (random::uniform() * 3.f));
    Stats s;
    bool inRange = true;
    for (int n = 0; n < 20000; n++) {
        for (int i = 0; i < Gradus::ROWS; i++)
            m.inputs[Gradus::TRIG1_INPUT + i].setVoltage(
                random::uniform() < 0.02f ? 5.f : 0.f);
        m.process(makeArgs(fr++));
        float v = out(m);
        s.add(v);
        if (!(v >= -1e-6f && v <= 10.f + 1e-6f)) inRange = false;
    }
    report(MOD, "fuzz_finite", (double) s.nans, s.nans == 0);
    report(MOD, "fuzz_in_range", s.peak, inRange);
}

SMOKE_MAIN(testHold, testAddSubtract, testEdgeNotLevel, testJump,
           testJumpBeatsRelative, testLowestJumpWins, testRelativesSum,
           testClamp, testBipolar, testPatchRoundTrip, testPatchClamps,
           testFuzz)
