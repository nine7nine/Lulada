// Copyright 2023 Kushview, LLC <info@kushview.net>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <element/ui.hpp>
#include <element/processor.hpp>

#include "ui/guicommon.hpp"
#include "ui/pluginwindow.hpp"
#include "ui/contextmenus.hpp"
#include <element/ui/grapheditor.hpp>
#include "nodes/volumeeditor.hpp"
#include "presetmanager.hpp"

namespace element {
static Array<PluginWindow*> activePluginWindows;

class PluginWindowToolbar : public Toolbar
{
public:
    enum Items
    {
        BypassPlugin = 1
    };

    PluginWindowToolbar() {}
    ~PluginWindowToolbar() {}
};

class PluginWindowContent : public Component,
                            public ComponentListener,
                            public Button::Listener
{
public:
    PluginWindowContent (Component* const _editor, const Node& _node)
        : editor (_editor), object (_node.getObject()), node (_node)
    {
        nativeEditor = nullptr != dynamic_cast<AudioProcessorEditor*> (_editor) && nullptr == dynamic_cast<GenericAudioProcessorEditor*> (_editor);

        toolbar.reset (new PluginWindowToolbar());
        addAndMakeVisible (toolbar.get());
        toolbar->setBounds (0, 0, getWidth(), 24);

        addAndMakeVisible (editor.get());
        editor->addComponentListener (this);

        addAndMakeVisible (nodeButton);
        nodeButton.setButtonText ("n");
        nodeButton.setColour (TextButton::buttonOnColourId, Colours::red);
        nodeButton.addListener (this);

        addAndMakeVisible (powerButton);
        powerButton.setColour (SettingButton::backgroundOnColourId,
                               findColour (SettingButton::backgroundColourId));
        powerButton.setColour (SettingButton::backgroundColourId, Colors::toggleBlue);
        powerButton.getToggleStateValue().referTo (node.getPropertyAsValue (tags::bypass));
        powerButton.setClickingTogglesState (true);
        powerButton.addListener (this);

        addAndMakeVisible (onTopButton);
        onTopButton.setButtonText ("^");
        onTopButton.setTooltip ("Keep plugin window on top of others");
        onTopButton.addListener (this);

        addAndMakeVisible (muteButton);
        muteButton.setYesNoText ("M", "M");
        muteButton.setColour (SettingButton::backgroundOnColourId, Colors::toggleRed);
        muteButton.getToggleStateValue().referTo (node.getPropertyAsValue (tags::mute));
        muteButton.setClickingTogglesState (true);
        muteButton.addListener (this);

        updateSize();
    }

    ~PluginWindowContent() noexcept
    {
        powerButton.removeListener (this);

        if (object && editor)
        {
            if (auto* proc = object->getAudioProcessor())
                if (auto* const e = dynamic_cast<AudioProcessorEditor*> (editor.get()))
                    proc->editorBeingDeleted (e);
        }

        editor = nullptr;
        toolbar = nullptr;
        leftPanel = nullptr;
        rightPanel = nullptr;
    }

    void updateSize()
    {
        // Toolbar disabled — JUCE 8's Toolbar::getThickness() returns
        // 0 for an item-less Toolbar, so the resized() block below
        // never positioned anything anyway; the 24px space we were
        // reserving here just produced a black gap at the bottom of
        // the plugin window.  Drop the reserved space and the 4px
        // padding entirely so the plugin GUI fills the window.
        const int height = jmax (editor->getHeight(), 100);
        setSize (editor->getWidth(), height);
    }

    void resized() override
    {
        editor->removeComponentListener (this);
        auto r (getLocalBounds());

        // Toolbar disabled — editor fills the entire content area.
        // Internal-type editors (NodeEditor::resizable()) get the
        // same full area via resizeInternalType.
        if (! resizeInternalType (r))
            editor->setBounds (0, 0, editor->getWidth(), editor->getHeight());
        editor->addComponentListener (this);
    }

    bool resizeInternalType (const Rectangle<int>& r)
    {
        auto ed = dynamic_cast<NodeEditor*> (editor.get());
        if (nullptr != ed && ed->resizable())
        {
            ed->setBounds (r);
            return true;
        }

        return false;
    }

    void buttonClicked (Button* button) override
    {
        if (button == &powerButton)
        {
            if (object && object->isSuspended() != node.isBypassed())
                object->suspendProcessing (node.isBypassed());
        }
        else if (button == &nodeButton)
        {
            auto* const world = ViewHelpers::getGlobals (this);
            auto* callback = new MenuCallback (this, node);
            NodePopupMenu& menu (callback->menu);
            menu.addSeparator();
            menu.addOptionsSubmenu();
            if (world)
                menu.addPresetsMenu (world->presets());
            menu.show (0, 0, 0, 0, callback);
        }
        else if (button == &onTopButton)
        {
            if (auto* pw = findParentComponentOfClass<PluginWindow>())
            {
                if (editor)
                    removeChildComponent (editor.get());

                pw->setAlwaysOnTop (! pw->isAlwaysOnTop());
                node.setProperty (tags::windowOnTop, pw->isAlwaysOnTop());

                if (editor)
                    addChildComponent (editor.get());

                resized();
            }
        }
        else if (button == &muteButton)
        {
            node.setMuted (muteButton.getToggleState());
        }

        stabilizeComponents();
    }

    void componentMovedOrResized (Component& c, bool wasMoved, bool wasResized) override
    {
        if (editor != nullptr && editor.get() != &c)
            return;
        if (wasResized)
            updateSize();
        ignoreUnused (wasMoved);
    }

    Toolbar* getToolbar() const { return toolbar.get(); }

    void stabilizeComponents()
    {
        if (auto* pw = findParentComponentOfClass<PluginWindow>())
        {
            onTopButton.setToggleState (pw->isAlwaysOnTop(), dontSendNotification);
        }
    }

    void handleMenuResult (int result)
    {
        // noop, might need when the menu gets more complex
    }

private:
    JUCE_DECLARE_WEAK_REFERENCEABLE (PluginWindowContent);
    std::unique_ptr<PluginWindowToolbar> toolbar;
    SettingButton nodeButton;
    PowerButton powerButton;
    SettingButton onTopButton;
    SettingButton muteButton;
    Value bypassValue;
    bool nativeEditor = false;
    std::unique_ptr<Component> editor, leftPanel, rightPanel;
    ProcessorPtr object;
    Node node;

    class MenuCallback : public ModalComponentManager::Callback
    {
    public:
        MenuCallback (PluginWindowContent* c, const Node& n)
            : content (c), menu (n)
        {
        }

        void modalStateFinished (int returnValue) override
        {
            if (! content.wasObjectDeleted())
                if (auto* const msg = menu.createMessageForResultCode (returnValue))
                    ViewHelpers::postMessageFor (content.get(), msg);
        }

        WeakReference<PluginWindowContent> content;
        NodePopupMenu menu;
    };

    AudioProcessor* getProcessor() { return (object != nullptr) ? object->getAudioProcessor() : nullptr; }
};

void PluginWindow::DelayedNodeFocus::timerCallback()
{
    if (! window.isActiveWindow())
        return;
    if (auto* const cc = ViewHelpers::findContentComponent())
    {
        auto node = window.getNode();
        if (node.isValid())
            if (auto* const gui = cc->services().find<GuiService>())
                gui->selectNode (node);
    }
}

PluginWindow::PluginWindow (GuiService& g, Component* const ui, const Node& n)
    : DocumentWindow (n.getName(), Colors::backgroundColor, DocumentWindow::minimiseButton | DocumentWindow::closeButton, false),
      gui (g),
      owner (n.getObject()),
      node (n),
      delayedNodeFocus (*this)
{
#if JUCE_LINUX || JUCE_BSD
    setUsingNativeTitleBar (true);
#else
    setUsingNativeTitleBar (true);
#endif

    setLookAndFeel (&g.getLookAndFeel());
    setSize (400, 300);

    name = node.getPropertyAsValue (tags::name);
    name.addListener (this);
    setName (node.getDisplayName());

    if (node.isValid())
    {
        setTopLeftPosition (node.data().getProperty (tags::windowX, Random::getSystemRandom().nextInt (500)),
                            node.data().getProperty (tags::windowY, Random::getSystemRandom().nextInt (500)));
        node.data().setProperty (tags::windowVisible, true, 0);
    }

    bool windowResize = false;
    bool useResizeHandle = false;

    if (nullptr != dynamic_cast<GenericAudioProcessorEditor*> (ui))
    {
        windowResize = false;
        useResizeHandle = false;
    }
    else if (auto* ed = dynamic_cast<AudioProcessorEditor*> (ui))
    {
        windowResize = false;
        useResizeHandle = ed->isResizable() && ed->resizableCorner == nullptr;
    }
    else if (nullptr != dynamic_cast<VolumeNodeEditor*> (ui))
    {
        windowResize = false;
        useResizeHandle = false;
    }
    else if (auto eed = dynamic_cast<Editor*> (ui))
    {
        windowResize = eed->resizable();
        useResizeHandle = windowResize;
    }
    else
    {
        windowResize = true;
        useResizeHandle = false;
    }

    const bool defaultOnTop = g.context().settings().pluginWindowsOnTop();
    setAlwaysOnTop ((bool) node.getProperty (tags::windowOnTop, defaultOnTop));

    auto* const content = new PluginWindowContent (ui, node);
    setContentOwned (content, true);
    setResizable (windowResize, useResizeHandle);

    /* GPU renderer for native editors only.  Detect a Wine-bridged
     * plugin editor by checking the editor (and its direct children)
     * for a WineHWNDEmbedComponent.  VST/VST3/CLAP editors hold the
     * embed as a direct member added via addAndMakeVisible in their
     * ctor -- see juce_CLAPPluginFormat.cpp:57, juce_VST3PluginFormat
     * .cpp:575.  juce::Component has no findChildComponentOfClass<>,
     * so the scan is hand-rolled.  If no embed is found, the editor
     * is a pure-JUCE Component tree and attaching OpenGLContext lifts
     * its paint onto the GPU like MainWindow does -- big win for
     * SamplerEditor / TrackerEditor / CompressorEditor + GenericAPE
     * waveform / curve repaints under load.  Continuous repaint stays
     * off; JUCE invalidates regions and the GL renderer handles the
     * upload. */
    auto hasWineEmbed = [] (juce::Component* c) -> bool
    {
        if (c == nullptr) return false;
        if (dynamic_cast<juce::WineHWNDEmbedComponent*> (c) != nullptr) return true;
        for (int i = 0; i < c->getNumChildComponents(); ++i)
            if (dynamic_cast<juce::WineHWNDEmbedComponent*> (c->getChildComponent (i)) != nullptr)
                return true;
        return false;
    };
    if (! hasWineEmbed (ui))
    {
        glContext = std::make_unique<juce::OpenGLContext>();
        glContext->setContinuousRepainting (false);
        glContext->attachTo (*this);
    }

    addToDesktop();
    content->stabilizeComponents();
}

PluginWindow::~PluginWindow()
{
    delayedNodeFocus.stopTimer();
    if (glContext != nullptr)
        glContext->detach();
    name.removeListener (this);
    clearContentComponent();
    setLookAndFeel (nullptr);
}

float PluginWindow::getDesktopScaleFactor() const { return 1.f; }

Content* PluginWindow::getElementContentComponent() const
{
    return gui.content();
}

Toolbar* PluginWindow::getToolbar() const
{
    if (PluginWindowContent* pwc = dynamic_cast<PluginWindowContent*> (getContentComponent()))
        return pwc->getToolbar();
    return nullptr;
}

void PluginWindow::restoreAlwaysOnTopState()
{
    if (node.isValid())
    {
        const auto shouldBeOnTop = (bool) node.getProperty (tags::windowOnTop);
        setAlwaysOnTop (shouldBeOnTop);
        if (shouldBeOnTop)
            toFront (false);
    }
}

void PluginWindow::resized()
{
    DocumentWindow::resized();
    if (getWidth() <= 140)
    {
        setName ({});
    }
    else
    {
        setName (node.getDisplayName());
    }
}

void PluginWindow::activeWindowStatusChanged()
{
    if (isActiveWindow())
        delayedNodeFocus.trigger();

    if (nullptr == getContentComponent() || isActiveWindow())
        return;
    gui.checkForegroundStatus();
}

void PluginWindow::updateGraphNode (Processor* newNode, Component* newEditor)
{
    jassert (nullptr != newNode && nullptr != newEditor);
    owner = newNode;
    setContentOwned (newEditor, true);
}

void PluginWindow::moved()
{
    node.setProperty (tags::windowX, getX());
    node.setProperty (tags::windowY, getY());
}

void PluginWindow::closeButtonPressed()
{
    gui.closePluginWindow (this);
}

} // namespace element
