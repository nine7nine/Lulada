// Copyright 2023 Kushview, LLC <info@kushview.net>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <element/graph.hpp>
#include <element/node.hpp>
#include <element/context.hpp>
#include <element/devices.hpp>
#include <element/settings.hpp>
#include <element/services.hpp>
#include <element/engine.hpp>
#include <element/ui.hpp>
#include <element/ui/content.hpp>

#include "services/deviceservice.hpp"
#include "services/mappingservice.hpp"
#include "ui/sessionpromptdialog.hpp"
#include "services/presetservice.hpp"
#include "services/sessionservice.hpp"

namespace element {

class SessionService::ChangeResetter : public AsyncUpdater
{
public:
    explicit ChangeResetter (SessionService& sc) : owner (sc) {}
    ~ChangeResetter() = default;

    void handleAsyncUpdate() override
    {
        owner.resetChanges (false);
        jassert (! owner.hasSessionChanged());
    }

private:
    SessionService& owner;
};

SessionService::SessionService() {}
SessionService::~SessionService() {}

void SessionService::activate()
{
    currentSession = context().session();
    document.reset (new SessionDocument (currentSession));
    changeResetter.reset (new ChangeResetter (*this));
    document->setFile (DataPath::defaultSessionDir());
}

void SessionService::deactivate()
{
    auto& world = context();
    auto& settings (world.settings());
    auto* props = settings.getUserSettings();

    if (document)
    {
        if (document->getFile().existsAsFile())
            props->setValue (Settings::lastSessionKey, document->getFile().getFullPathName());
        document = nullptr;
    }

    changeResetter->cancelPendingUpdate();
    changeResetter.reset (nullptr);

    currentSession->clear();
    currentSession = nullptr;
}

void SessionService::openDefaultSession()
{
    if (auto* gc = sibling<GuiService>())
        gc->closeAllPluginWindows();

    loadNewSessionData();
    refreshOtherControllers();
    sibling<GuiService>()->stabilizeContent();
    resetChanges (true);
}

void SessionService::openFile (const File& file)
{
    bool didSomething = true;

    if (file.hasFileExtension ("elg"))
    {
        ValueTree data (Node::parse (file));
        String error;

        if (Node::isProbablyGraphNode (data))
        {
            Model model (data);

            if (model.version() != EL_GRAPH_VERSION)
                data = Node::migrate (model.data(), error);

            if (data.isValid() && error.isEmpty())
            {
                Node node (data, true);
                node.forEach ([] (const ValueTree& tree) {
                    if (! tree.hasType (types::Node))
                        return;
                    auto ref = tree;
                    ref.setProperty (tags::uuid, Uuid().toString(), nullptr);
                });

                if (auto* ec = sibling<EngineService>())
                    ec->addGraph (node, false);
            }
        }
        else
        {
            error = "File does not seem to be an Element graph.";
        }

        if (error.isNotEmpty())
        {
            AlertWindow::showMessageBoxAsync (AlertWindow::WarningIcon, "Invalid graph", error);
        }
    }
    else if (file.hasFileExtension ("els"))
    {
        /* Common load path -- runs after the (possibly-async) save
         * prompt resolves.  Captured by value so the lambda survives
         * past openFile() returning. */
        auto doLoad = [this, file] ()
        {
            Session::ScopedFrozenLock freeze (*currentSession);
            Result result = document->loadFrom (file, true);

            if (result.wasOk())
            {
                auto& gui = *sibling<GuiService>();
                gui.closeAllPluginWindows();
                refreshOtherControllers();

                if (auto* cc = gui.content())
                {
                    auto ui = currentSession->data().getOrCreateChildWithName (tags::ui, nullptr);
                    cc->applySessionState (ui.getProperty ("content").toString());
                }

                sibling<GuiService>()->stabilizeContent();
                resetChanges();
            }
            jassert (! hasSessionChanged());
            changeResetter->triggerAsyncUpdate();
        };

        if (document->hasChangedSinceSaved())
        {
            /* Native top-level Save / Discard / Cancel prompt --
             * replaces juce::FileBasedDocument::saveIfNeededAndUser
             * Agrees's in-process modal overlay ("Closing
             * document...") with our SessionPromptDialog. */
            const juce::String name = document->getDocumentTitle();
            SessionPromptDialog::showSaveDiscardCancel (
                "Closing document...",
                "Do you want to save the changes to \"" + name + "\"?",
                [this, doLoad] (SessionPromptDialog::Result r)
                {
                    if (r == SessionPromptDialog::Result::Yes)
                    {
                        document->save (true, true);
                        doLoad();
                    }
                    else if (r == SessionPromptDialog::Result::No)
                    {
                        doLoad();
                    }
                    /* Cancel -- abort the open entirely. */
                });
            /* Skip the post-block triggerAsyncUpdate; the lambda
             * fires it after the load actually runs. */
            return;
        }

        doLoad();
        return;
    }
    else
    {
        didSomething = false;
    }

    if (didSomething)
    {
        if (auto* gc = sibling<GuiService>())
            if (! file.hasFileExtension ("els"))
                gc->stabilizeContent();
        changeResetter->triggerAsyncUpdate();
    }
}

const File SessionService::getSessionFile() const
{
    return document != nullptr ? document->getFile() : File();
}

void SessionService::exportGraph (const Node& node, const File& targetFile)
{
    if (! node.hasNodeType (types::Graph))
    {
        jassertfalse;
        return;
    }

    TemporaryFile tempFile (targetFile);
    if (node.writeToFile (tempFile.getFile()))
        tempFile.overwriteTargetFileWithTemporary();
}

void SessionService::importGraph (const File& file)
{
    openFile (file);
}

void SessionService::closeSession()
{
    DBG ("[SC] close session");
}

bool SessionService::hasSessionChanged()
{
    return (document) ? document->hasChangedSinceSaved() : false;
}

void SessionService::resetChanges (const bool resetDocumentFile)
{
    jassert (document);
    if (resetDocumentFile)
        document->setFile ({});
    document->setChangedFlag (false);
    jassert (! document->hasChangedSinceSaved());
}

void SessionService::saveSession (const bool saveAs, const bool askForFile, const bool showError)
{
    jassert (document && currentSession);
    auto result = FileBasedDocument::userCancelledSave;

    auto& gui = *sibling<GuiService>();

    if (auto* cc = gui.content())
    {
        String state;
        cc->getSessionState (state);
        auto ui = currentSession->data().getOrCreateChildWithName (tags::ui, nullptr);
        ui.setProperty ("content", state, nullptr);
    }

    sigWillSave();

    /* Save-As path now lives at the call site as a GuiService::requestFile
     * onAccept that hits saveSessionTo() directly — see the
     * Commands::sessionSaveAs handler in guiservice.cpp.  Keeping the
     * `saveAs` param for API stability; callers that pass true must now
     * route via requestFile + saveSessionTo instead. */
    jassert (! saveAs);
    if (saveAs) return;

    result = document->save (askForFile, showError);

    if (result == FileBasedDocument::userCancelledSave)
        return;

    if (result == FileBasedDocument::savedOk)
    {
        // ensure change messages are flushed so the changed flag doesn't reset
        currentSession->dispatchPendingMessages();
        document->setChangedFlag (false);
        jassert (! hasSessionChanged());
        if (auto* us = context().settings().getUserSettings())
            us->setValue (Settings::lastSessionKey, document->getFile().getFullPathName());

        if (saveAs)
        {
            sibling<UI>()->recentFiles().addFile (document->getFile());
            currentSession->data().setProperty (tags::name,
                                                document->getFile().getFileNameWithoutExtension(),
                                                nullptr);
        }
    }
}

bool SessionService::saveSessionTo (const File& target)
{
    jassert (document && currentSession);
    if (target == File()) return false;

    auto& gui = *sibling<GuiService>();
    if (auto* cc = gui.content())
    {
        String state;
        cc->getSessionState (state);
        auto ui = currentSession->data().getOrCreateChildWithName (tags::ui, nullptr);
        ui.setProperty ("content", state, nullptr);
    }

    sigWillSave();

    /* Normalize extension — same fixup the FileChooser-driven path does
     * after the user picks. */
    File chosen = (target.getFileExtension() == ".els")
                      ? target
                      : target.withFileExtension (".els");

    const auto result = document->saveAs (chosen, true, true, true);
    if (result != FileBasedDocument::savedOk) return false;

    currentSession->dispatchPendingMessages();
    document->setChangedFlag (false);
    if (auto* us = context().settings().getUserSettings())
        us->setValue (Settings::lastSessionKey, document->getFile().getFullPathName());

    sibling<UI>()->recentFiles().addFile (document->getFile());
    currentSession->data().setProperty (tags::name,
                                        document->getFile().getFileNameWithoutExtension(),
                                        nullptr);
    return true;
}

void SessionService::newSession()
{
    jassert (document && currentSession);

    /* Common reset path that runs after either the user picked Save
     * (and we saved) or Don't Save -- skip on Cancel. */
    auto finishNewSession = [this]()
    {
        sibling<GuiService>()->closeAllPluginWindows();
        loadNewSessionData();
        refreshOtherControllers();
        sibling<GuiService>()->stabilizeContent();
        resetChanges (true);
    };

    if (! document->hasChangedSinceSaved())
    {
        finishNewSession();
        return;
    }

    /* Native top-level confirm dialog (SessionPromptDialog).  Replaces
     * juce::AlertWindow's in-process modal overlay. */
    SessionPromptDialog::showYesNoCancel (
        "Save Session?",
        "The current session has changes. Would you like to save it?",
        [this, finishNewSession] (SessionPromptDialog::Result r)
        {
            if (r == SessionPromptDialog::Result::Yes)
            {
                document->save (true, true);
                finishNewSession();
            }
            else if (r == SessionPromptDialog::Result::No)
            {
                finishNewSession();
            }
            /* Cancel -- do nothing, session stays as-is. */
        });
}

void SessionService::loadNewSessionData()
{
    currentSession->clear();
    const auto file = context().settings().getDefaultNewSessionFile();
    bool wasLoaded = false;

    if (file.existsAsFile())
    {
        ValueTree data;
        if (auto xml = XmlDocument::parse (file))
            data = ValueTree::fromXml (*xml);
        if (data.isValid() && data.hasType (types::Session) && EL_SESSION_VERSION == (int) data.getProperty (tags::version))
            wasLoaded = currentSession->loadData (data);
    }

    if (! wasLoaded)
    {
        /* Element: prefer the device's registered port count over the
         * audio engine's *active* channel count.  AudioEngine::
         * getNumChannels reflects which channels are currently
         * connected / checked in the JUCE Active Channels selector,
         * which can be a subset of the device's actual port count.
         * The user expects the Graph I/O Audio In/Out node port
         * count to match what they configured (e.g. 8 JACK output
         * ports), not just whatever subset they happen to have
         * connected to a downstream destination at session-create
         * time. */
        auto* device = context().devices().getCurrentAudioDevice();
        int numIn  = device != nullptr ? device->getInputChannelNames().size()  : 2;
        int numOut = device != nullptr ? device->getOutputChannelNames().size() : 2;
        if (numIn  <= 0) numIn  = 2;
        if (numOut <= 0) numOut = 2;
        currentSession->clear();
        /* On the JACK build the graph's abstract MIDI ports + the
         * Graph I/O MIDI pseudo-nodes they drive are unused — JACK
         * MIDI flows via JackMidiInputNode/All directly.  Start with
         * 0 MIDI ports so new graphs come up clean (Audio In + Audio
         * Out only). */
#if ELEMENT_USE_JACK
        constexpr bool wantGraphMidiIn  = false;
        constexpr bool wantGraphMidiOut = false;
#else
        constexpr bool wantGraphMidiIn  = true;
        constexpr bool wantGraphMidiOut = true;
#endif
        currentSession->addGraph (
            Graph::create ("Graph", numIn, numOut, wantGraphMidiIn, wantGraphMidiOut),
            true);
    }
}

void SessionService::refreshOtherControllers()
{
    sibling<EngineService>()->sessionReloaded();
    sibling<DeviceService>()->refresh();
    sibling<MappingService>()->learn (false);
    sibling<PresetService>()->refresh();
    sigSessionLoaded();
}

} // namespace element
