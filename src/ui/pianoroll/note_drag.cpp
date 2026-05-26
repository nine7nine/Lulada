// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/pianoroll/note_drag.hpp"
#include "ui/pianoroll/pianoroll_grid.hpp"
#include "ui/pianoroll/pianoroll_view.hpp"
#include "ui/pianoroll/midi_note_diff_command.hpp"
#include "services/timeline/midi_note_region.hpp"

#include <element/services.hpp>
#include <element/ui.hpp>

#include <algorithm>

namespace element {

namespace {

/* Helper: push a populated MidiNoteDiffCommand through the global
 * UndoManager.  Falls back to direct apply when no GuiService /
 * UndoManager is available (defensive -- in practice the GuiService
 * is always present in a running Element session).  Notifies the
 * parent PianoRollView so it can broadcast region-edited (drives
 * ArrangementView's live note-count badge refresh). */
void commitDiff (PianoRollGrid& grid,
                 std::unique_ptr<MidiNoteDiffCommand> cmd,
                 const juce::String& displayName)
{
    if (cmd == nullptr || cmd->isEmpty()) return;

    auto& services = grid.getServices();
    if (auto* gui = services.find<GuiService>())
    {
        /* GuiService::getUndoManager() returns a reference, not a
         * pointer -- always valid in a live session. */
        gui->getUndoManager().perform (cmd.release(), displayName);
    }
    else
    {
        /* Fallback: apply directly + drop.  No undo trail in this path
         * but the edit still lands. */
        cmd->perform();
    }

    if (auto* view = grid.findParentComponentOfClass<PianoRollView>())
        view->notifyRegionEdited();
}

} // namespace

//==============================================================================
// NoteDragMove -- move the current selection by a snap-aligned beat
// delta + pitch delta.  Records before/after for every moved note.

class NoteDragMove final : public NoteDrag
{
public:
    NoteDragMove (PianoRollGrid& grid, MidiNoteRegion& region,
                   const juce::MouseEvent& e)
        : regionId_ (juce::Uuid())   /* filled below from grid -> view */
    {
        (void) grid;
        anchorBeat_  = (double) e.x / (double) juce::jmax (1, grid.getPxPerBeat());
        anchorPitch_ = grid.pitchForY (e.y);

        /* Capture the original state of every selected note so we can
         * compute the delta and so MidiNoteDiffCommand has the
         * before-state for undo. */
        const auto* snap = region.loadSnapshot();
        if (snap == nullptr) return;

        const auto& sel = grid.selection();
        for (const auto& n : *snap)
        {
            if (sel.count (n.id))
                original_.push_back (n);
        }

        /* Cache the bound region uuid + resolver for commit time --
         * pulled from the grid's parent view. */
        if (auto* view = grid.findParentComponentOfClass<PianoRollView>())
        {
            regionId_ = view->getBoundRegionId();
            resolver_ = view->getRegionResolver();
        }
    }

    void mouseDrag (const juce::MouseEvent& e, PianoRollGrid& grid) override
    {
        const double curBeat  = (double) e.x / (double) juce::jmax (1, grid.getPxPerBeat());
        const int    curPitch = grid.pitchForY (e.y);

        /* Snap the BEAT delta (not the absolute position) so the
         * gesture feels relative.  Pitch is integer so no snap
         * needed.  When snap is off, drag is continuous. */
        const double rawDeltaBeats = curBeat - anchorBeat_;
        deltaBeats_ = grid.isSnapEnabled()
            ? grid.snapBeat (rawDeltaBeats)
            : rawDeltaBeats;
        deltaPitch_ = curPitch - anchorPitch_;

        grid.repaint();
    }

    void mouseUp (const juce::MouseEvent& /*e*/, PianoRollGrid& grid) override
    {
        if (original_.empty()) return;
        if (deltaBeats_ == 0.0 && deltaPitch_ == 0) return;

        auto* region = resolver_ ? resolver_ (regionId_) : nullptr;
        if (region == nullptr) return;

        auto cmd = std::make_unique<MidiNoteDiffCommand> (regionId_, resolver_);
        for (const auto& before : original_)
        {
            MidiNote after = before;
            after.onBeat = juce::jmax (0.0, before.onBeat + deltaBeats_);
            after.pitch  = juce::jlimit (0, 127, before.pitch + deltaPitch_);
            cmd->recordUpdate (before.id, before, after);
        }

        commitDiff (grid, std::move (cmd), "Move MIDI notes");
    }

    void paintOverlay (juce::Graphics& g, PianoRollGrid& grid) override
    {
        if (original_.empty()) return;
        if (deltaBeats_ == 0.0 && deltaPitch_ == 0) return;

        const int rowH = grid.rowHeight();
        const int pxb  = grid.getPxPerBeat();

        g.setColour (juce::Colours::white.withAlpha (0.45f));
        for (const auto& before : original_)
        {
            const double newBeat  = juce::jmax (0.0, before.onBeat + deltaBeats_);
            const int    newPitch = juce::jlimit (0, 127, before.pitch + deltaPitch_);
            const int x = (int) std::round (newBeat * pxb);
            const int w = juce::jmax (2, (int) std::round (before.lengthBeats * pxb));
            const int y = grid.yForPitch (newPitch);
            const int h = juce::jmax (1, rowH - 1);
            g.drawRect (x, y, w, h, 2);
        }
    }

private:
    juce::Uuid                                regionId_;
    std::function<MidiNoteRegion* (const juce::Uuid&)> resolver_;
    std::vector<MidiNote>                     original_;
    double anchorBeat_  { 0.0 };
    int    anchorPitch_ { 60 };
    double deltaBeats_  { 0.0 };
    int    deltaPitch_  { 0 };
};

//==============================================================================
// NoteDragCreate -- pencil-tool note creation.  In-flight preview
// shows the new note at (snapBeat, pitch) with the live length; on
// mouseUp the note is recordAdd-ed via MidiNoteDiffCommand.

class NoteDragCreate final : public NoteDrag
{
public:
    NoteDragCreate (PianoRollGrid& grid, MidiNoteRegion& region,
                     const juce::MouseEvent& e)
    {
        (void) region;
        const double rawBeat = (double) e.x / (double) juce::jmax (1, grid.getPxPerBeat());
        anchorBeat_ = grid.isSnapEnabled()
            ? juce::jmax (0.0, grid.snapBeat (rawBeat))
            : juce::jmax (0.0, rawBeat);
        anchorPitch_ = grid.pitchForY (e.y);

        /* Initial length = current snap division (Ableton/Zrythm
         * convention -- pencil-clicked note matches the grid grain).
         * Falls back to a quarter beat when snap is off. */
        const double initLen = grid.isSnapEnabled() && grid.getSnapDivision() > 0.0
            ? grid.getSnapDivision()
            : 0.25;

        live_.id          = 0;   /* assigned at commit time */
        live_.pitch       = anchorPitch_;
        live_.velocity    = 100;
        live_.channel     = 1;
        live_.onBeat      = anchorBeat_;
        live_.lengthBeats = initLen;

        if (auto* view = grid.findParentComponentOfClass<PianoRollView>())
        {
            regionId_ = view->getBoundRegionId();
            resolver_ = view->getRegionResolver();
        }
    }

    void mouseDrag (const juce::MouseEvent& e, PianoRollGrid& grid) override
    {
        const double curBeat = (double) e.x / (double) juce::jmax (1, grid.getPxPerBeat());
        const double minLen = grid.isSnapEnabled() && grid.getSnapDivision() > 0.0
            ? grid.getSnapDivision()
            : 0.0625;
        const double rawLen = curBeat - anchorBeat_;
        const double snapped = grid.isSnapEnabled()
            ? grid.snapBeat (rawLen)
            : rawLen;
        live_.lengthBeats = juce::jmax (minLen, snapped);
        grid.repaint();
    }

    void mouseUp (const juce::MouseEvent& /*e*/, PianoRollGrid& grid) override
    {
        auto* region = resolver_ ? resolver_ (regionId_) : nullptr;
        if (region == nullptr) return;

        /* Clamp length to remaining region span. */
        live_.lengthBeats = juce::jmin (live_.lengthBeats,
                                         juce::jmax (0.25, region->lengthBeats - live_.onBeat));

        auto cmd = std::make_unique<MidiNoteDiffCommand> (regionId_, resolver_);
        cmd->recordAdd (*region, live_);

        commitDiff (grid, std::move (cmd), "Add MIDI note");

        /* Select the new note so further edits target it. */
        grid.selectOnly (live_.id);
    }

    void paintOverlay (juce::Graphics& g, PianoRollGrid& grid) override
    {
        const int pxb = grid.getPxPerBeat();
        const int rowH = grid.rowHeight();
        const int x = (int) std::round (live_.onBeat * pxb);
        const int w = juce::jmax (2, (int) std::round (live_.lengthBeats * pxb));
        const int y = grid.yForPitch (live_.pitch);
        const int h = juce::jmax (1, rowH - 1);
        g.setColour (juce::Colours::yellow.withAlpha (0.7f));
        g.fillRect (x, y, w, h);
        g.setColour (juce::Colours::yellow);
        g.drawRect (x, y, w, h, 2);
    }

private:
    juce::Uuid                                regionId_;
    std::function<MidiNoteRegion* (const juce::Uuid&)> resolver_;
    MidiNote live_ {};
    double   anchorBeat_  { 0.0 };
    int      anchorPitch_ { 60 };
};

//==============================================================================
// NoteDragResize -- drag the right edge of a note to change its
// lengthBeats.

class NoteDragResize final : public NoteDrag
{
public:
    NoteDragResize (PianoRollGrid& grid, MidiNoteRegion& region,
                     std::uint64_t hitId, const juce::MouseEvent& e)
        : hitId_ (hitId)
    {
        anchorX_ = e.x;

        const auto* snap = region.loadSnapshot();
        if (snap != nullptr)
        {
            for (const auto& n : *snap)
            {
                if (n.id == hitId_)
                {
                    before_ = n;
                    break;
                }
            }
        }

        if (auto* view = grid.findParentComponentOfClass<PianoRollView>())
        {
            regionId_ = view->getBoundRegionId();
            resolver_ = view->getRegionResolver();
        }
    }

    void mouseDrag (const juce::MouseEvent& e, PianoRollGrid& grid) override
    {
        const double pxb     = (double) juce::jmax (1, grid.getPxPerBeat());
        const double newEnd  = (double) e.x / pxb;
        const double minLen = grid.isSnapEnabled() && grid.getSnapDivision() > 0.0
            ? grid.getSnapDivision()
            : 0.0625;
        const double rawLen = newEnd - before_.onBeat;
        const double snapped = grid.isSnapEnabled()
            ? grid.snapBeat (rawLen)
            : rawLen;
        liveLen_ = juce::jmax (minLen, snapped);
        grid.repaint();
    }

    void mouseUp (const juce::MouseEvent& /*e*/, PianoRollGrid& grid) override
    {
        if (before_.id == 0) return;
        if (liveLen_ == before_.lengthBeats) return;

        MidiNote after = before_;
        after.lengthBeats = liveLen_;

        auto cmd = std::make_unique<MidiNoteDiffCommand> (regionId_, resolver_);
        cmd->recordUpdate (before_.id, before_, after);
        commitDiff (grid, std::move (cmd), "Resize MIDI note");
    }

    void paintOverlay (juce::Graphics& g, PianoRollGrid& grid) override
    {
        if (before_.id == 0) return;
        const int pxb = grid.getPxPerBeat();
        const int rowH = grid.rowHeight();
        const int x = (int) std::round (before_.onBeat * pxb);
        const int w = juce::jmax (2, (int) std::round (liveLen_ * pxb));
        const int y = grid.yForPitch (before_.pitch);
        const int h = juce::jmax (1, rowH - 1);
        g.setColour (juce::Colours::cyan.withAlpha (0.6f));
        g.drawRect (x, y, w, h, 2);
    }

private:
    juce::Uuid                                regionId_;
    std::function<MidiNoteRegion* (const juce::Uuid&)> resolver_;
    std::uint64_t hitId_ { 0 };
    MidiNote      before_ {};
    int           anchorX_ { 0 };
    double        liveLen_ { 0.25 };
};

//==============================================================================
// NoteDragMarquee -- rubber-band rect.  On mouseUp, all notes whose
// rect intersects the marquee are added to selection (or erased if
// eraseMode_).

class NoteDragMarquee final : public NoteDrag
{
public:
    NoteDragMarquee (PianoRollGrid& grid, const juce::MouseEvent& e, bool eraseMode)
        : eraseMode_ (eraseMode)
    {
        start_ = e.getPosition();
        live_  = juce::Rectangle<int> (start_, start_);

        if (auto* view = grid.findParentComponentOfClass<PianoRollView>())
        {
            regionId_ = view->getBoundRegionId();
            resolver_ = view->getRegionResolver();
        }
    }

    void mouseDrag (const juce::MouseEvent& e, PianoRollGrid& grid) override
    {
        live_ = juce::Rectangle<int> (start_, e.getPosition());
        grid.repaint();
    }

    void mouseUp (const juce::MouseEvent& /*e*/, PianoRollGrid& grid) override
    {
        auto* region = resolver_ ? resolver_ (regionId_) : nullptr;
        if (region == nullptr) return;

        const auto* snap = region->loadSnapshot();
        if (snap == nullptr) return;

        std::vector<std::uint64_t> hits;
        const int pxb = grid.getPxPerBeat();
        const int rowH = grid.rowHeight();
        for (const auto& n : *snap)
        {
            const int x = (int) std::round (n.onBeat * pxb);
            const int w = juce::jmax (2, (int) std::round (n.lengthBeats * pxb));
            const int y = grid.yForPitch (n.pitch);
            const int h = juce::jmax (1, rowH - 1);
            const juce::Rectangle<int> rect (x, y, w, h);
            if (live_.intersects (rect))
                hits.push_back (n.id);
        }

        if (eraseMode_)
        {
            if (! hits.empty())
            {
                auto cmd = std::make_unique<MidiNoteDiffCommand> (regionId_, resolver_);
                for (auto id : hits)
                    cmd->recordRemove (*region, id);
                commitDiff (grid, std::move (cmd), "Erase MIDI notes");
            }
        }
        else
        {
            for (auto id : hits)
                grid.selectAdd (id);
        }
    }

    void paintOverlay (juce::Graphics& g, PianoRollGrid& /*grid*/) override
    {
        const auto rect = live_;
        g.setColour (juce::Colours::white.withAlpha (eraseMode_ ? 0.15f : 0.10f));
        g.fillRect (rect);
        g.setColour (juce::Colours::white.withAlpha (0.7f));
        g.drawRect (rect, 1);
    }

private:
    juce::Uuid                                regionId_;
    std::function<MidiNoteRegion* (const juce::Uuid&)> resolver_;
    juce::Point<int>     start_;
    juce::Rectangle<int> live_;
    bool                 eraseMode_ { false };
};

//==============================================================================
// Factory.

std::unique_ptr<NoteDrag> NoteDrag::makeMove (PianoRollGrid& grid,
                                                MidiNoteRegion& region,
                                                std::uint64_t /*hitId*/,
                                                const juce::MouseEvent& e)
{
    return std::make_unique<NoteDragMove> (grid, region, e);
}

std::unique_ptr<NoteDrag> NoteDrag::makeCreate (PianoRollGrid& grid,
                                                  MidiNoteRegion& region,
                                                  const juce::MouseEvent& e)
{
    return std::make_unique<NoteDragCreate> (grid, region, e);
}

std::unique_ptr<NoteDrag> NoteDrag::makeResize (PianoRollGrid& grid,
                                                  MidiNoteRegion& region,
                                                  std::uint64_t hitId,
                                                  const juce::MouseEvent& e)
{
    return std::make_unique<NoteDragResize> (grid, region, hitId, e);
}

std::unique_ptr<NoteDrag> NoteDrag::makeMarquee (PianoRollGrid& grid,
                                                   const juce::MouseEvent& e,
                                                   bool eraseMode)
{
    return std::make_unique<NoteDragMarquee> (grid, e, eraseMode);
}

} // namespace element
