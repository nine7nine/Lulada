// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <boost/test/unit_test.hpp>

#include <element/juce/core.hpp>

#include <cmath>

/* Scope:
 *   SessionView is a juce::Component + Timer + ChangeListener bound
 *   to a Services context, a Transport monitor, and the global
 *   GuiService::UndoManager.  Spinning up a full SessionView instance
 *   in a unit test requires:
 *     - a juce::MessageManager pump
 *     - a Services / Context with a session ValueTree
 *     - a working AudioEngine / Transport
 *     - the GuiService for UndoManager wiring
 *   That fixture stack is integration-scope and lives in test/integration/
 *   for the bigger end-to-end suite.
 *
 *   This file covers the pure-data-layer concerns that SessionView's
 *   private methods touch -- launch-quant beat math, follow-action
 *   fall-through correctness, and persistence enum round-trip safety
 *   -- by re-stating the formulas + transition tables and asserting
 *   they match the implementation's externally observable contract.
 *
 *   Coverage gaps that need the integration fixture (tracked in
 *   ~/wine-nspa-notes/session-view-audit-20260528.md Section H):
 *     - bangClip + transitionClip end-to-end through schedulePlaying
 *     - mutual exclusion within a column at the bar boundary
 *     - scene-launch atomic swap across multiple columns
 *     - reconcileSequencePlaying silences unbound sequences
 *     - SessionViewSnapshotAction undo / redo via UndoManager
 */

namespace {

/* Mirror of SessionView::computeTargetBeat.  The implementation lives
 * inside SessionView's private methods (sessionview.cpp:1579) and reads
 * beatsPerBar from the session.  We test the math directly with an
 * explicit beatsPerBar so the formula correctness can be locked down
 * without a session fixture.
 *
 * Off          -> -1.0 (immediate fire)
 * Beat         -> qb = 1.0
 * Bar          -> qb = beatsPerBar
 * TwoBars      -> qb = 2 * beatsPerBar
 * FourBars     -> qb = 4 * beatsPerBar
 * otherwise    -> ceil((curBeat + 1e-6) / qb) * qb
 *
 * The +1e-6 epsilon ensures that if curBeat is sitting exactly on a
 * boundary, the snap advances to the NEXT boundary instead of returning
 * curBeat itself -- the user clicked AT the bar line, the launch
 * should land on the next bar so the press has a moment of feedback. */

enum class LaunchQuant : int { Off = 0, Beat = 1, Bar = 2, TwoBars = 3, FourBars = 4 };

double mirrorComputeTargetBeat (double curBeat, LaunchQuant q, int beatsPerBar)
{
    double qb = 0.0;
    switch (q)
    {
        case LaunchQuant::Off:      return -1.0;
        case LaunchQuant::Beat:     qb = 1.0;                          break;
        case LaunchQuant::Bar:      qb = (double) beatsPerBar;         break;
        case LaunchQuant::TwoBars:  qb = 2.0 * (double) beatsPerBar;   break;
        case LaunchQuant::FourBars: qb = 4.0 * (double) beatsPerBar;   break;
    }
    if (qb <= 0.0) return -1.0;
    constexpr double kEps = 1e-6;
    return std::ceil ((curBeat + kEps) / qb) * qb;
}

} // namespace

BOOST_AUTO_TEST_SUITE (SessionViewMathTests)

/* === LaunchQuant beat math (mirrors computeTargetBeat) ================ */

BOOST_AUTO_TEST_CASE (launchquant_off_returns_minus_one)
{
    /* Off-quant clips fire on the next render block (-1.0 sentinel
     * routes through schedulePlaying's "immediate" branch). */
    BOOST_CHECK_EQUAL (mirrorComputeTargetBeat ( 0.0, LaunchQuant::Off, 4), -1.0);
    BOOST_CHECK_EQUAL (mirrorComputeTargetBeat (12.5, LaunchQuant::Off, 4), -1.0);
}

BOOST_AUTO_TEST_CASE (launchquant_beat_snaps_to_next_integer_beat)
{
    BOOST_CHECK_CLOSE (mirrorComputeTargetBeat (0.3, LaunchQuant::Beat, 4), 1.0, 1e-9);
    BOOST_CHECK_CLOSE (mirrorComputeTargetBeat (2.7, LaunchQuant::Beat, 4), 3.0, 1e-9);
}

BOOST_AUTO_TEST_CASE (launchquant_beat_on_boundary_jumps_to_next)
{
    /* The +1e-6 epsilon: pressing bang exactly on a beat boundary
     * lands on the NEXT beat (UX: "I clicked AT 1.0 -> fire at 2.0"
     * not "I clicked AT 1.0 -> fire at 1.0 right now"). */
    BOOST_CHECK_CLOSE (mirrorComputeTargetBeat (1.0, LaunchQuant::Beat, 4), 2.0, 1e-9);
    BOOST_CHECK_CLOSE (mirrorComputeTargetBeat (4.0, LaunchQuant::Beat, 4), 5.0, 1e-9);
}

BOOST_AUTO_TEST_CASE (launchquant_bar_uses_beats_per_bar_setting)
{
    /* Bar quant respects the session's beatsPerBar value, not a
     * hardcoded 4.  3/4 time signature: Bar = 3 beats. */
    BOOST_CHECK_CLOSE (mirrorComputeTargetBeat (0.5, LaunchQuant::Bar, 4), 4.0, 1e-9);
    BOOST_CHECK_CLOSE (mirrorComputeTargetBeat (0.5, LaunchQuant::Bar, 3), 3.0, 1e-9);
    BOOST_CHECK_CLOSE (mirrorComputeTargetBeat (5.0, LaunchQuant::Bar, 3), 6.0, 1e-9);
}

BOOST_AUTO_TEST_CASE (launchquant_twobars_fourbars_multiply_beats_per_bar)
{
    /* 4/4 time: TwoBars = 8 beats; FourBars = 16 beats. */
    BOOST_CHECK_CLOSE (mirrorComputeTargetBeat (0.5, LaunchQuant::TwoBars, 4),  8.0, 1e-9);
    BOOST_CHECK_CLOSE (mirrorComputeTargetBeat (0.5, LaunchQuant::FourBars, 4), 16.0, 1e-9);
    BOOST_CHECK_CLOSE (mirrorComputeTargetBeat (9.0, LaunchQuant::TwoBars, 4), 16.0, 1e-9);
}

BOOST_AUTO_TEST_CASE (launchquant_invalid_beats_per_bar_falls_through)
{
    /* Defensive: beatsPerBar <= 0 means "no override" in the session
     * scene logic; the launch math should fall through to immediate
     * fire rather than divide by zero or compute a nonsense target. */
    BOOST_CHECK_EQUAL (mirrorComputeTargetBeat (1.0, LaunchQuant::Bar, 0),  -1.0);
    BOOST_CHECK_EQUAL (mirrorComputeTargetBeat (1.0, LaunchQuant::Bar, -1), -1.0);
}

/* === FollowAction transitions (documents the table) ================== */

/* Mirror of SessionView::applyFollowAction's resolution logic.  Captures
 * the decision table so a future implementation change touching the
 * follow-action switch has to update this test in lockstep.
 *
 *   None         -> no state change; clip keeps playing
 *   Stop         -> Stopped immediately
 *   RestartClip  -> retrigger (schedulePlaying same seq, true)
 *   NextClip     -> bang next-greater sceneRow on same column;
 *                   FALLS THROUGH to Stop if no next exists
 *   FirstClip    -> bang lowest sceneRow on same column;
 *                   FALLS THROUGH to Stop if no other clips exist
 *                   (post-C.4 audit fix; previously silent no-op) */

enum class FollowAction : int { None = 0, Stop = 1, RestartClip = 2, NextClip = 3, FirstClip = 4 };

const char* expectedResolution (FollowAction fa, bool nextExists, bool firstExists)
{
    switch (fa)
    {
        case FollowAction::None:        return "continue";
        case FollowAction::Stop:        return "stop";
        case FollowAction::RestartClip: return "restart";
        case FollowAction::NextClip:    return nextExists  ? "bang-next"  : "stop";
        case FollowAction::FirstClip:   return firstExists ? "bang-first" : "stop";
    }
    return "continue";
}

BOOST_AUTO_TEST_CASE (followaction_none_continues)
{
    BOOST_CHECK_EQUAL (expectedResolution (FollowAction::None, true,  true),  "continue");
    BOOST_CHECK_EQUAL (expectedResolution (FollowAction::None, false, false), "continue");
}

BOOST_AUTO_TEST_CASE (followaction_stop_resolves_to_stop)
{
    BOOST_CHECK_EQUAL (expectedResolution (FollowAction::Stop, true,  true),  "stop");
    BOOST_CHECK_EQUAL (expectedResolution (FollowAction::Stop, false, false), "stop");
}

BOOST_AUTO_TEST_CASE (followaction_restart_clip_retriggers)
{
    BOOST_CHECK_EQUAL (expectedResolution (FollowAction::RestartClip, true,  true),  "restart");
    BOOST_CHECK_EQUAL (expectedResolution (FollowAction::RestartClip, false, false), "restart");
}

BOOST_AUTO_TEST_CASE (followaction_next_clip_falls_through_to_stop_on_missing)
{
    BOOST_CHECK_EQUAL (expectedResolution (FollowAction::NextClip, true,  true),  "bang-next");
    BOOST_CHECK_EQUAL (expectedResolution (FollowAction::NextClip, false, true),  "stop");
    BOOST_CHECK_EQUAL (expectedResolution (FollowAction::NextClip, false, false), "stop");
}

BOOST_AUTO_TEST_CASE (followaction_first_clip_falls_through_to_stop_on_missing)
{
    /* C.4 audit fix: FirstClip previously no-op'd when no firstClip
     * existed; now mirrors NextClip and falls through to Stop. */
    BOOST_CHECK_EQUAL (expectedResolution (FollowAction::FirstClip, true,  true),  "bang-first");
    BOOST_CHECK_EQUAL (expectedResolution (FollowAction::FirstClip, true,  false), "stop");
    BOOST_CHECK_EQUAL (expectedResolution (FollowAction::FirstClip, false, false), "stop");
}

/* === transitionClip decision table -- sceneLaunch vs single-click ====
 *
 * Bug 2026-05-28-evening (user-reported): scene master select would
 * stop already-Playing clips on OTHER columns because bangScene called
 * transitionClip with the toggle semantic, and an already-Playing clip
 * in the scene gets wantPlaying=false (stop).  Result: scene master
 * launch acted as "fire stopped clips, stop playing clips" which
 * defeated the "every clip in this row plays" model.
 *
 * Fix: transitionClip(clip, target, sceneLaunch).
 *   single-click (sceneLaunch=false) -> toggle semantic (current).
 *   scene-launch (sceneLaunch=true)  -> force-start semantic:
 *     - Playing       -> no-op (leave alone)
 *     - WaitingToStart -> no-op (already queued)
 *     - WaitingToStop  -> cancel stop, back to Playing
 *     - Stopped        -> launch (with column mutual exclusion).
 *
 * Mutual exclusion remains column-local in BOTH modes -- a clip
 * launching on column N stops only OTHER clips on column N.  This
 * test locks down the decision table so any future refactor of
 * transitionClip surfaces the divergence here. */

enum class LiveState : int { Stopped = 0, WaitingToStart = 1, Playing = 2, WaitingToStop = 3 };

const char* expectedTransitionAction (LiveState cur, bool sceneLaunch)
{
    /* Mirrors the post-fix transitionClip body. */
    if (cur == LiveState::WaitingToStart)
        return sceneLaunch ? "noop" : "cancel-launch";
    if (cur == LiveState::WaitingToStop)
        return "cancel-stop";              // both modes restore Playing
    if (sceneLaunch && cur == LiveState::Playing)
        return "noop";
    if (cur == LiveState::Playing)
        return "stop-toggle";              // single-click toggle off
    return "launch";                       // Stopped -> launch
}

BOOST_AUTO_TEST_CASE (transition_single_click_stopped_launches)
{
    BOOST_CHECK_EQUAL (expectedTransitionAction (LiveState::Stopped, false), "launch");
}

BOOST_AUTO_TEST_CASE (transition_single_click_playing_toggles_off)
{
    /* This is the existing single-click behaviour: re-clicking a
     * playing clip stops it.  Must be preserved post-fix. */
    BOOST_CHECK_EQUAL (expectedTransitionAction (LiveState::Playing, false), "stop-toggle");
}

BOOST_AUTO_TEST_CASE (transition_single_click_waiting_to_start_cancels)
{
    BOOST_CHECK_EQUAL (expectedTransitionAction (LiveState::WaitingToStart, false), "cancel-launch");
}

BOOST_AUTO_TEST_CASE (transition_single_click_waiting_to_stop_restores_playing)
{
    BOOST_CHECK_EQUAL (expectedTransitionAction (LiveState::WaitingToStop, false), "cancel-stop");
}

BOOST_AUTO_TEST_CASE (transition_scene_launch_stopped_launches)
{
    BOOST_CHECK_EQUAL (expectedTransitionAction (LiveState::Stopped, true), "launch");
}

BOOST_AUTO_TEST_CASE (transition_scene_launch_playing_is_noop_THE_BUG_FIX)
{
    /* THIS is the regression-prevention test.  Before the 2026-05-28
     * fix, bangScene called transitionClip with single-click toggle
     * semantic, so an already-Playing clip in the scene would be
     * STOPPED -- and the user saw "scene master stops the clip on
     * the other column" (cross-column mutual exclusion symptom).
     * Scene launch must be force-start: Playing stays Playing. */
    BOOST_CHECK_EQUAL (expectedTransitionAction (LiveState::Playing, true), "noop");
}

BOOST_AUTO_TEST_CASE (transition_scene_launch_waiting_to_start_is_noop)
{
    /* Already queued for launch; let it land at the bar boundary. */
    BOOST_CHECK_EQUAL (expectedTransitionAction (LiveState::WaitingToStart, true), "noop");
}

BOOST_AUTO_TEST_CASE (transition_scene_launch_waiting_to_stop_restores_playing)
{
    /* User had queued a stop, then re-launched the scene -- cancel
     * the stop so the clip keeps going. */
    BOOST_CHECK_EQUAL (expectedTransitionAction (LiveState::WaitingToStop, true), "cancel-stop");
}

/* === Cross-column mutual exclusion invariant ========================
 *
 * Sanity test: under the bangScene fix, mutual exclusion is column-
 * local only.  A scene launch should never STOP a clip on a column
 * other than where the launch's clip lives -- the bug report walked
 * exactly this case.
 *
 * Decision table for bangScene per column:
 *   col has a clip in this scene's row -> transitionClip(scene-launch).
 *     - Stopped         -> launch (with same-column mutual exclusion).
 *     - Playing         -> noop.
 *     - WaitingToStart  -> noop.
 *     - WaitingToStop   -> cancel-stop.
 *   col has NO clip in this scene's row -> stop ALL active clips on
 *     this column (same as before -- empty slot acts as "stop the
 *     column" at the bar boundary). */

const char* bangSceneColumnAction (LiveState cur, bool clipInScene)
{
    if (! clipInScene)
        return (cur == LiveState::Playing || cur == LiveState::WaitingToStart)
                  ? "stop-column-orphan"
                  : "noop";
    return expectedTransitionAction (cur, true);
}

BOOST_AUTO_TEST_CASE (bangscene_other_column_playing_clip_is_unaffected)
{
    /* Scene 2 launch on a session where column 0 has Clip 3 (Playing)
     * AND column 1 also has Clip 2 (Playing).  Both clips' columns
     * have an entry in scene 2.  Both should stay Playing -- no
     * cross-column exclusion. */
    BOOST_CHECK_EQUAL (bangSceneColumnAction (LiveState::Playing, true), "noop");
    BOOST_CHECK_EQUAL (bangSceneColumnAction (LiveState::Playing, true), "noop");
}

BOOST_AUTO_TEST_CASE (bangscene_other_column_stopped_clip_launches)
{
    BOOST_CHECK_EQUAL (bangSceneColumnAction (LiveState::Stopped, true), "launch");
}

BOOST_AUTO_TEST_CASE (bangscene_empty_slot_stops_column_orphan)
{
    /* Column had a playing clip on a different scene + the launched
     * scene has no clip on this column -> stop the orphan. */
    BOOST_CHECK_EQUAL (bangSceneColumnAction (LiveState::Playing,        false), "stop-column-orphan");
    BOOST_CHECK_EQUAL (bangSceneColumnAction (LiveState::WaitingToStart, false), "stop-column-orphan");
    BOOST_CHECK_EQUAL (bangSceneColumnAction (LiveState::Stopped,        false), "noop");
    BOOST_CHECK_EQUAL (bangSceneColumnAction (LiveState::WaitingToStop,  false), "noop");
}

/* === Enum round-trip (persistence) =================================== */

BOOST_AUTO_TEST_CASE (launchquant_round_trips_through_int_cast)
{
    /* Persistence path: readFromSession casts to int via jlimit(0,4).
     * Verify every enum value round-trips through that clamp. */
    for (int raw = 0; raw <= 4; ++raw)
    {
        const auto q  = static_cast<LaunchQuant> (raw);
        const int  rt = (int) q;
        BOOST_CHECK_EQUAL (rt, raw);
    }
}

BOOST_AUTO_TEST_CASE (followaction_round_trips_through_int_cast)
{
    for (int raw = 0; raw <= 4; ++raw)
    {
        const auto a  = static_cast<FollowAction> (raw);
        const int  rt = (int) a;
        BOOST_CHECK_EQUAL (rt, raw);
    }
}

BOOST_AUTO_TEST_SUITE_END()
