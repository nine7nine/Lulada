// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <element/juce/gui_basics.hpp>
#include <element/ui/navigation.hpp>

namespace element {

class Context;

/** Collapsible navigation sidebar.  VS-Code activity-bar pattern:
 *  the vertical icon column on the left is ALWAYS visible; when the
 *  sidebar is expanded a single content panel sits to its right
 *  showing the currently-active section.
 *
 *  States:
 *   - Collapsed: icon strip only (kCollapsedW wide).
 *   - Expanded:  icon strip on left + active section's panel on
 *                right.  No concertina stacking -- only ONE section
 *                shows at a time, selected by clicking its icon.
 *
 *  Section panels are still constructed by NavigationConcertinaPanel
 *  (kept as a panel registry); we just take ownership of their
 *  visual parent so they render directly inside the sidebar's
 *  right column rather than inside the concertina's accordion. */
class NavigationSidebar : public juce::Component
{
public:
    NavigationSidebar (Context& g);
    ~NavigationSidebar() override;

    NavigationConcertinaPanel*       concertina()       noexcept { return panel_.get(); }
    const NavigationConcertinaPanel* concertina() const noexcept { return panel_.get(); }

    template <class T>
    inline T* findPanel() { return panel_->findPanel<T>(); }

    /** Build (or rebuild) the section panels in the registry +
     *  refresh the icon strip + select a default active section. */
    void updateContent();

    /* ---- Collapse state ---- */

    void setCollapsed (bool);
    bool isCollapsed() const noexcept { return collapsed_; }

    int  getExpandedWidth() const noexcept { return expandedWidth_; }
    void setExpandedWidth (int w) noexcept;

    /** Width the parent layout should request given the current
     *  collapse state. */
    int getDesiredWidth() const noexcept;

    /* Sidebar collapsed metrics are deliberately INDEPENDENT of the
     * toolbar's geometry -- the toolbar uses big labeled buttons for
     * primary actions, the sidebar uses smaller bezel-less LCD
     * buttons for secondary navigation.  Visual hierarchy is the
     * goal, not pixel-matched alignment. */
    static constexpr int kCollapsedW = 56;

    /* ---- Persistence ---- */

    void saveState   (juce::PropertiesFile*);
    void restoreState (juce::PropertiesFile*);

    /** Switch which section's panel is shown in the content area.
     *  No-op if the name doesn't resolve to a built panel.  If the
     *  sidebar is collapsed, this implicitly expands it. */
    void setActiveSection (const juce::String& name);
    juce::String getActiveSection() const noexcept { return activeSection_; }

    /** Public entry used by icon-strip click handlers + by external
     *  callers (e.g. a future "show node properties" command). */
    void revealSection (const juce::String& name);

    /** Fires whenever the desired width / layout changes.
     *  StandardContent wires this to its own layout invalidation. */
    std::function<void()> onLayoutInvalidated;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class IconStrip;

    Context& context_;

    /* Panel registry -- still constructed via NavigationConcertinaPanel
     * because that's where each section's specific construction
     * boilerplate lives.  We never display the concertina itself; we
     * just pull panels out by name and reparent them into the
     * sidebar's content column. */
    std::unique_ptr<NavigationConcertinaPanel> panel_;

    std::unique_ptr<IconStrip>                  iconStrip_;
    juce::Component*                            activeContent_ { nullptr };
    juce::String                                activeSection_;

    bool collapsed_ { false };
    int  expandedWidth_ { 304 };

    void swapActiveContent (juce::Component* next);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NavigationSidebar)
};

} // namespace element
