// Copyright 2023 Kushview, LLC <info@kushview.net>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <element/juce/core.hpp>
#include <element/juce/gui_basics.hpp>
#include <element/ui/content.hpp>

#include <boost/signals2/connection.hpp>

#include <memory>
#include <unordered_map>

namespace element {

class Context;
class MeterBridgeView;
class GuiService;
class ContentContainer;
class NavigationConcertinaPanel;
class NodeChannelStripView;
class VirtualKeyboardView;

class StandardContent : public Content,
                        public juce::ApplicationCommandTarget,
                        public juce::DragAndDropContainer,
                        public juce::DragAndDropTarget,
                        public juce::FileDragAndDropTarget {
public:
    StandardContent (Context& ctx);
    ~StandardContent() noexcept;

    void resizeContent (const juce::Rectangle<int>& area) override;

    NavigationConcertinaPanel* getNavigationConcertinaPanel() const { return nav.get(); }

    void setMainView (const juce::String& name);
    void setSecondaryView (const juce::String& name);
    juce::String getMainViewName() const;
    juce::Component* getMainViewComponent() const;
    juce::String getAccessoryViewName() const;

    void nextMainView();
    void backMainView();

    void saveState (juce::PropertiesFile*) override;
    void restoreState (juce::PropertiesFile*) override;

    int getNavSize();

    bool isVirtualKeyboardVisible() const;
    void setVirtualKeyboardVisible (const bool isVisible);
    void toggleVirtualKeyboard();
    VirtualKeyboardView* getVirtualKeyboardView() const;

    void setNodeChannelStripVisible (const bool isVisible);
    bool isNodeChannelStripVisible() const;

    void setMeterBridgeVisible (bool);
    bool isMeterBridgeVisible() const;

    void setCurrentNode (const Node& node) override;

    void stabilize (const bool refreshDataPathTrees = false) override;
    void stabilizeViews() override;

    void setShowAccessoryView (const bool show);
    bool showAccessoryView() const;

    // Drag and drop
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

    bool isInterestedInDragSource (const SourceDetails& dragSourceDetails) override;
    void itemDropped (const SourceDetails& dragSourceDetails) override;

    void getSessionState (juce::String&) override;
    void applySessionState (const juce::String&) override;

    void presentView (std::unique_ptr<View>) override;
    void presentView (const juce::String&) override;

    void setMainView (ContentView* v);

    //==========================================================================
    void setExtraView (juce::Component*);
    Component* extraView() { return _extra.get(); }

protected:
    virtual ContentView* createContentView (const juce::String&) { return nullptr; }

private:
    std::unique_ptr<NavigationConcertinaPanel> nav;
    friend class ContentContainer;
    std::unique_ptr<ContentContainer> container;
    juce::StretchableLayoutManager layout;
    class Resizer;
    friend class Resizer;
    std::unique_ptr<Resizer> bar1;

    std::unique_ptr<NodeChannelStripView> nodeStrip;

    bool statusBarVisible { true };
    int statusBarSize;
    bool toolBarVisible { true };
    int toolBarSize;
    int virtualKeyboardSize = 80;
    int nodeStripSize = 80;

    juce::String lastMainView;

    std::unique_ptr<juce::Component> _extra;

    boost::signals2::connection sessionLoadedConn;

    /* ContentView caching: views are constructed once on first
     * request and reused across switches.  Replaces the previous
     * destroy/recreate dance, where switching back to a view (e.g.
     * GraphEditorView) paid the full BlockComponent + ConnectorComponent
     * rebuild cost (~15 ms on a dense session) on every entry.
     *
     * The map owns the views; ContentContainer's primary/secondary
     * are non-owning raw pointers into this map.  Map is declared
     * BEFORE container in the destruction-order sense so the views
     * outlive the container's reference-clear sequence run from
     * StandardContent's destructor. */
    std::unordered_map<juce::String, std::unique_ptr<ContentView>> viewCache_;

    /** Cache-aware view lookup -- returns an existing instance for
     *  `name` or constructs a new one (and inserts into viewCache_)
     *  if none exists.  Returns nullptr only when no view type
     *  matches the name. */
    ContentView* lookupOrCreateMainView (const juce::String& name);
    ContentView* lookupOrCreateSecondaryView (const juce::String& name);

    void resizerMouseDown();
    void resizerMouseUp();
    void updateLayout();
    void setContentView (ContentView* view, const bool accessory = false);

    friend class GuiService;
    juce::ApplicationCommandTarget* getNextCommandTarget() override;
    void getAllCommands (juce::Array<juce::CommandID>& commands) override;
    void getCommandInfo (juce::CommandID commandID, juce::ApplicationCommandInfo& result) override;
    bool perform (const InvocationInfo& info) override;
};

} // namespace element
