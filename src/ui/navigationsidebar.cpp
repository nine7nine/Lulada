// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/navigationsidebar.hpp"
#include "ui/blocktoolbutton.hpp"
#include "ui/toolbaricons.hpp"
#include "ui/fontcache.hpp"

#include <element/context.hpp>

namespace element {

/* =================================================================
 * Layout constants -- INDEPENDENT of the toolbar's geometry.
 * ================================================================= */
static constexpr int   kHorizPad     = 8;
/* Vertical inset from sidebar top/bottom to first/last icon AND to
 * the active panel's top/bottom border.  Shared so the icon column
 * and the panel encapsulation start at the same Y. */
static constexpr int   kFramePad     = 2;
static constexpr int   kIconGap      = 4;
static constexpr int   kIconSize     = 40;
/* Panel styling -- chosen to match BlockToolButton's LCD body so the
 * active icon + content panel read as a matched pair (same border
 * thickness, same corner radius, same recessed fill) but stay
 * visually distinct via a small gap between them. */
static constexpr float kPanelCorner  = 3.0f;   /* matches BlockToolButton's cornerSize */
static constexpr float kPanelBorderW = 1.2f;   /* matches BlockToolButton's border stroke */
static constexpr int   kPanelGap     = 4;      /* gap between icon column and panel */

/* =================================================================
 * Section registry: name -> icon drawer + tint colour.  Single source
 * of truth for the icon strip's button visuals AND the active-section
 * tint that frames the content panel.
 * ================================================================= */
namespace {

inline void drawSidebarToggle (juce::Graphics& g,
                                juce::Rectangle<float> b,
                                juce::Colour fg,
                                bool currentlyExpanded)
{
    ui::iconNavToggle (g, b, fg, currentlyExpanded);
}

struct IconRegistryEntry
{
    void (*drawer)(juce::Graphics&, juce::Rectangle<float>, juce::Colour);
    juce::Colour tint;
};

IconRegistryEntry iconForSection (const juce::String& name)
{
    if (name == "Session")
        return { &ui::iconSession,       juce::Colour::fromRGB ( 80, 200, 170) };
    if (name == "Graph")
        return { &ui::iconGraph,         juce::Colour::fromRGB (110, 170, 110) };
    if (name == "Node")
        return { &ui::iconNodeBlock,     juce::Colour::fromRGB (220, 140,  60) };
    if (name == "Editor")
        return { &ui::iconKnobs,         juce::Colour::fromRGB (160, 100, 180) };
    if (name == "Plugins")
        return { &ui::iconPluginManager, juce::Colour::fromRGB ( 80, 160, 200) };
    if (name == "Data Path")
        return { &ui::iconDisk,          juce::Colour::fromRGB (200, 180, 100) };
    return { nullptr, juce::Colour() };
}

} // namespace

/* =================================================================
 * IconStrip -- always visible.  Hosts the chevron toggle (first) +
 * one BlockToolButton per concertina section.  Section buttons drive
 * setActiveSection() on the sidebar; chevron toggles collapse state.
 * ================================================================= */
class NavigationSidebar::IconStrip : public juce::Component
{
public:
    IconStrip (NavigationSidebar& s) : strip_ (s) {}

    /** (Re)build buttons from the inner concertina's actual panels.
     *  No chevron / toggle entry -- clicking an already-active
     *  section icon collapses the sidebar; clicking an inactive icon
     *  expands and switches to that section. */
    void rebuild()
    {
        buttons_.clear();
        sectionNames_.clear();

        if (auto* c = strip_.concertina())
        {
            for (int i = 0; i < c->getNumPanels(); ++i)
            {
                auto* panel = c->getPanel (i);
                if (panel == nullptr) continue;
                const auto name = panel->getName();
                if (name.isEmpty()) continue;

                const auto entry = iconForSection (name);
                auto btn = std::make_unique<BlockToolButton> ("", entry.tint);
                btn->setLcdBody (true);
                if (entry.drawer != nullptr)
                {
                    btn->setIcon ([drawer = entry.drawer]
                                   (juce::Graphics& g,
                                    juce::Rectangle<float> r,
                                    juce::Colour fg) {
                        drawer (g, r, fg);
                    });
                }
                else
                {
                    btn->setLabel (name.substring (0, 1));
                }
                btn->setTooltip (name);
                btn->onClick = [this, name] {
                    /* Toggle behaviour: clicking the already-active
                     * section icon while expanded -> collapse.  All
                     * other paths -> expand + switch to that section. */
                    if (! strip_.isCollapsed()
                        && strip_.getActiveSection() == name)
                    {
                        strip_.setCollapsed (true);
                    }
                    else
                    {
                        strip_.revealSection (name);
                    }
                };
                addAndMakeVisible (btn.get());
                buttons_.push_back (std::move (btn));
                sectionNames_.push_back (name);
            }
        }
        refreshActiveIndicator();
        resized();
    }

    /** Update each button's toggle state so the currently-active
     *  section reads as "lit".  Called by NavigationSidebar after a
     *  setActiveSection / setCollapsed transition. */
    void refreshActiveIndicator()
    {
        const auto active = strip_.getActiveSection();
        const bool collapsed = strip_.isCollapsed();
        for (size_t i = 0; i < buttons_.size(); ++i)
        {
            const bool isActive = (! collapsed) && sectionNames_[i] == active;
            buttons_[i]->setToggleState (isActive, juce::dontSendNotification);
            /* Active icon body is painted by NavigationSidebar as
             * part of the unified icon+bridge+panel shape, so its
             * BlockToolButton skips its own body+border drawing
             * (shell-only mode) -- the icon glyph + hover overlay
             * still paint normally on top of the unified surface. */
            buttons_[i]->setPaintShellOnly (isActive);
            buttons_[i]->setOpenRightEdge  (false);
            buttons_[i]->repaint();
        }
    }

    /** Local Y-range of the currently-active section's icon, or
     *  empty when no section is active.  IconStrip is positioned at
     *  sidebar (0, 0), so these coords are also valid in sidebar
     *  local space and can be used directly for bridge painting. */
    juce::Range<int> getActiveIconYRange() const
    {
        const auto active = strip_.getActiveSection();
        for (size_t i = 0; i < buttons_.size(); ++i)
        {
            if (sectionNames_[i] == active)
                return { buttons_[i]->getY(),
                          buttons_[i]->getBottom() };
        }
        return {};
    }

    void paint (juce::Graphics& g) override { juce::ignoreUnused (g); }

    void resized() override
    {
        if (buttons_.empty()) return;
        auto r = getLocalBounds();
        const int x = (r.getWidth() - kIconSize) / 2;
        int y = r.getY() + kFramePad;
        for (auto& b : buttons_)
        {
            b->setBounds (x, y, kIconSize, kIconSize);
            y += kIconSize + kIconGap;
        }
    }

private:
    NavigationSidebar& strip_;
    std::vector<std::unique_ptr<BlockToolButton>> buttons_;
    std::vector<juce::String>                     sectionNames_;
};

/* =================================================================
 * NavigationSidebar
 * ================================================================= */
NavigationSidebar::NavigationSidebar (Context& g) : context_ (g)
{
    /* Concertina is constructed and added as an INVISIBLE child so
     * its updateContent() can walk the parent chain via
     * ViewHelpers::getGlobals(this) (PluginsPanelView construction
     * relies on that walk to reach the Context).  We never render
     * the concertina; it's just a panel registry whose
     * findPanelByName() output we reparent into our content column. */
    panel_     = std::make_unique<NavigationConcertinaPanel> (g);
    addChildComponent (panel_.get());   /* in tree, but not visible */
    iconStrip_ = std::make_unique<IconStrip> (*this);
    addAndMakeVisible (iconStrip_.get());
}

NavigationSidebar::~NavigationSidebar()
{
    /* Detach the active section panel BEFORE the concertina (which
     * owns it via OwnedArray) destructs.  Without this, the
     * member-destruction order is:
     *   iconStrip_ (deletes its own children fine)
     *   panel_ (concertina) -> ~OwnedArray deletes Session/Graph/...
     *     -> deleted panels' destructors try to deparent themselves
     *        from NavigationSidebar -- but NavigationSidebar may
     *        already be mid-destruction in some scenarios.
     * Pre-detaching guarantees no dangling-child reference can be
     * walked during cleanup. */
    if (activeContent_ != nullptr)
        removeChildComponent (activeContent_);
    activeContent_ = nullptr;
}

void NavigationSidebar::updateContent()
{
    panel_->updateContent();
    iconStrip_->rebuild();

    /* Default-select the first section if nothing's active yet, so
     * the expanded state isn't an empty content column. */
    if (activeSection_.isEmpty())
        setActiveSection ("Session");
    else
        setActiveSection (activeSection_);
}

void NavigationSidebar::setCollapsed (bool collapse)
{
    if (collapse == collapsed_) return;

    if (! collapsed_)
        expandedWidth_ = juce::jmax (170, getWidth());

    collapsed_ = collapse;
    if (activeContent_ != nullptr)
        activeContent_->setVisible (! collapsed_);
    iconStrip_->refreshActiveIndicator();
    repaint();
    resized();
    if (onLayoutInvalidated) onLayoutInvalidated();
}

void NavigationSidebar::setExpandedWidth (int w) noexcept
{
    expandedWidth_ = juce::jmax (170, w);
}

int NavigationSidebar::getDesiredWidth() const noexcept
{
    return collapsed_ ? kCollapsedW : expandedWidth_;
}

void NavigationSidebar::swapActiveContent (juce::Component* next)
{
    if (activeContent_ == next) return;
    if (activeContent_ != nullptr)
    {
        activeContent_->setVisible (false);
        removeChildComponent (activeContent_);
    }
    activeContent_ = next;
    if (activeContent_ != nullptr)
    {
        addAndMakeVisible (activeContent_);
        activeContent_->setVisible (! collapsed_);
    }
}

void NavigationSidebar::setActiveSection (const juce::String& name)
{
    auto* p = panel_->findPanelByName (name);
    if (p == nullptr) return;
    activeSection_ = name;
    swapActiveContent (p);
    iconStrip_->refreshActiveIndicator();
    repaint();
    resized();
}

void NavigationSidebar::revealSection (const juce::String& name)
{
    if (collapsed_)
        setCollapsed (false);
    setActiveSection (name);
}

void NavigationSidebar::saveState (juce::PropertiesFile* props)
{
    panel_->saveState (props);
    if (props == nullptr) return;
    props->setValue ("navCollapsed",     collapsed_);
    props->setValue ("navExpandedWidth", expandedWidth_);
    props->setValue ("navActiveSection", activeSection_);
}

void NavigationSidebar::restoreState (juce::PropertiesFile* props)
{
    panel_->restoreState (props);
    if (props == nullptr) return;
    expandedWidth_ = juce::jmax (170, props->getIntValue ("navExpandedWidth",
                                                            expandedWidth_));
    activeSection_ = props->getValue ("navActiveSection", activeSection_);
    if (activeSection_.isEmpty()) activeSection_ = "Session";
    setActiveSection (activeSection_);

    const bool wantCollapsed = props->getBoolValue ("navCollapsed", false);
    if (wantCollapsed != collapsed_)
    {
        collapsed_ = wantCollapsed;
        if (activeContent_ != nullptr)
            activeContent_->setVisible (! collapsed_);
        iconStrip_->refreshActiveIndicator();
        repaint();
        resized();
    }
}

void NavigationSidebar::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff'16'19'1a));

    if (collapsed_ || activeContent_ == nullptr)
        return;

    const auto tint = iconForSection (activeSection_).tint;
    const auto borderCol = tint.isTransparent()
                            ? juce::Colour (0xff'4a'4a'4a)
                            : tint;
    const auto lcdFill = juce::Colour (0xff'0c'10'14);

    const auto yRange = iconStrip_->getActiveIconYRange();
    if (yRange.isEmpty())
        return;

    /* ---- Unified shape ----
     * Single Path traces: icon body (3 sides) -> top bridge across
     * gap -> panel body (closed top/right/bottom) -> bottom bridge ->
     * back to icon body.  One fillPath + one strokePath guarantees
     * the icon/bridge/panel borders share the same anti-aliasing and
     * meet exactly at corners.
     *
     * All coordinates use a 1-px inset so the active icon's visible
     * body matches the inactive BlockToolButtons (which paint inside
     * getLocalBounds().reduced(1)).  Without this inset, the active
     * icon reads as 2px taller + wider than its neighbours. */
    constexpr float kInset = 1.0f;
    const float iconL = (float) ((kCollapsedW - kIconSize) / 2) + kInset;
    const float iconR = iconL + (float) kIconSize - 2.0f * kInset;
    const float iconT = (float) yRange.getStart() + kInset;
    const float iconB = (float) yRange.getEnd()   - kInset;

    const float panelL = iconR + (float) kPanelGap;
    const float panelR = (float) getWidth() - kInset;
    const float panelT = (float) kFramePad   + kInset;
    const float panelB = (float) (getHeight() - kFramePad) - kInset;

    const float r  = kPanelCorner;
    const float bw = kPanelBorderW;

    juce::Path shape;

    /* Start at icon top after the rounded top-left corner. */
    shape.startNewSubPath (iconL + r, iconT);
    /* Top edge of icon -- continues straight across the bridge to
     * panel left at the icon's top Y. */
    shape.lineTo (iconR, iconT);
    shape.lineTo (panelL, iconT);
    /* Up the panel left edge to panel top (zero-length when active
     * is the first icon -- iconT == panelT). */
    shape.lineTo (panelL, panelT);
    /* Panel top edge -- square top-left, rounded top-right. */
    shape.lineTo (panelR - r, panelT);
    shape.cubicTo (panelR, panelT,  panelR, panelT,  panelR, panelT + r);
    /* Panel right edge. */
    shape.lineTo (panelR, panelB - r);
    shape.cubicTo (panelR, panelB,  panelR, panelB,  panelR - r, panelB);
    /* Panel bottom edge -- back to square bottom-left at panel left. */
    shape.lineTo (panelL, panelB);
    /* Up the panel left edge to icon bottom Y. */
    shape.lineTo (panelL, iconB);
    /* Bottom bridge across the gap to icon right at icon bottom Y. */
    shape.lineTo (iconR, iconB);
    /* Bottom edge of icon -- straight then rounded bottom-left. */
    shape.lineTo (iconL + r, iconB);
    shape.cubicTo (iconL, iconB,  iconL, iconB,  iconL, iconB - r);
    /* Left edge of icon -- straight then rounded top-left. */
    shape.lineTo (iconL, iconT + r);
    shape.cubicTo (iconL, iconT,  iconL, iconT,  iconL + r, iconT);
    shape.closeSubPath();

    g.setColour (lcdFill);
    g.fillPath (shape);
    g.setColour (borderCol);
    g.strokePath (shape, juce::PathStrokeType (bw));

    /* Active icon's right side is degenerate inside the unified
     * shape -- we want to keep the panel's right edge ROUNDED so
     * the path-edit above gave a clean look on the right.  Panel
     * right edges already rounded; nothing more to do. */
}

void NavigationSidebar::resized()
{
    auto r = getLocalBounds();
    iconStrip_->setBounds (r.removeFromLeft (kCollapsedW));

    if (! collapsed_ && activeContent_ != nullptr)
    {
        /* Panel inner bounds must match the unified shape painted in
         * paint().  That shape uses a 1-px inset on the panel's
         * outer perimeter so it visually matches the inactive icons'
         * reduced(1) BlockToolButton bodies.  Inner content then
         * insets further to clear the 1.2px border.
         *
         * Icon "right edge" in the shape is at (iconR - inset).  The
         * shape's bridge crosses kPanelGap from there to panelL.
         * To match resized() bounds with that geometry: the inactive
         * icons sit at button x = (kCollapsedW - kIconSize)/2, so the
         * shape's iconR (after inset) = that x + kIconSize - 1.  Panel
         * left starts kPanelGap px to the right of that point. */
        constexpr int kInset = 1;
        const int iconRightInset = (kCollapsedW - kIconSize) / 2 + kIconSize - kInset;
        const int panelX = iconRightInset + kPanelGap;
        auto panelRect = juce::Rectangle<int> (
            panelX, kFramePad + kInset,
            getWidth() - panelX - kInset,
            getHeight() - 2 * (kFramePad + kInset));
        const int innerInset = (int) std::ceil (kPanelBorderW) + 1;
        activeContent_->setBounds (panelRect.reduced (innerInset, innerInset));
    }
}

} // namespace element
