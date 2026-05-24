// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <element/juce/gui_basics.hpp>
#include "ui/fontcache.hpp"

namespace element {

/* Confirm dialog -- top-level DocumentWindow.  Mirrors the exact
 * construction order of PluginWindow (pluginwindow.cpp:242-336),
 * which is known to render correctly on the winelib+JUCE-Linux X11
 * stack even while MainWindow holds an attached OpenGLContext.
 *
 * Callback-based (async).  The window deletes itself after the
 * callback returns -- caller does not own the lifetime.  Usage:
 *   SessionPromptDialog::showYesNoCancel ("Save Session", "...",
 *       [] (SessionPromptDialog::Result r) { ... });
 */
class SessionPromptDialog : public juce::DocumentWindow
{
public:
    enum class Result { Cancel = 0, Yes = 1, No = 2 };
    using Callback = std::function<void (Result)>;

    static void showYesNoCancel (const juce::String& title,
                                  const juce::String& message,
                                  Callback callback)
    {
        new SessionPromptDialog (title, message,
                                  /*includeCancel=*/true, std::move (callback));
    }

    static void showYesNo (const juce::String& title,
                            const juce::String& message,
                            Callback callback)
    {
        new SessionPromptDialog (title, message,
                                  /*includeCancel=*/false, std::move (callback));
    }

    void closeButtonPressed() override
    {
        fire (Result::Cancel);
    }

private:
    class ContentPanel : public juce::Component
    {
    public:
        ContentPanel (const juce::String& message,
                      bool includeCancel,
                      std::function<void (Result)> onResult)
            : message_ (message), includeCancel_ (includeCancel)
        {
            setSize (440, 160);

            addAndMakeVisible (yesBtn_);
            addAndMakeVisible (noBtn_);
            if (includeCancel_)
                addAndMakeVisible (cancelBtn_);

            yesBtn_   .setButtonText ("Yes");
            noBtn_    .setButtonText ("No");
            cancelBtn_.setButtonText ("Cancel");

            yesBtn_   .onClick = [onResult] { onResult (Result::Yes); };
            noBtn_    .onClick = [onResult] { onResult (Result::No); };
            cancelBtn_.onClick = [onResult] { onResult (Result::Cancel); };
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (juce::Colour (0xff'1c'1c'1c));
            g.setColour (juce::Colour (0xff'd8'd8'd8));
            g.setFont (monoFont (12.0f, juce::Font::plain));
            g.drawFittedText (message_,
                              getLocalBounds().reduced (16, 16)
                                              .withTrimmedBottom (44),
                              juce::Justification::centred,
                              4);
        }

        void resized() override
        {
            constexpr int kBtnH = 28;
            constexpr int kBtnW = 76;
            constexpr int kBtnGap = 8;
            constexpr int kBottomPad = 10;

            auto r = getLocalBounds().removeFromBottom (kBtnH + kBottomPad)
                                     .withTrimmedBottom (kBottomPad);

            const int totalBtnW = (includeCancel_ ? 3 : 2) * kBtnW
                                + (includeCancel_ ? 2 : 1) * kBtnGap;
            r = r.withSizeKeepingCentre (totalBtnW, kBtnH);

            yesBtn_.setBounds (r.removeFromLeft (kBtnW));
            r.removeFromLeft (kBtnGap);
            noBtn_.setBounds (r.removeFromLeft (kBtnW));
            if (includeCancel_)
            {
                r.removeFromLeft (kBtnGap);
                cancelBtn_.setBounds (r.removeFromLeft (kBtnW));
            }
        }

    private:
        juce::String     message_;
        bool             includeCancel_;
        juce::TextButton yesBtn_, noBtn_, cancelBtn_;
    };

    SessionPromptDialog (const juce::String& title,
                          const juce::String& message,
                          bool includeCancel,
                          Callback cb)
        : juce::DocumentWindow (title,
                                 juce::Colour (0xff'1c'1c'1c),
                                 juce::DocumentWindow::closeButton,
                                 /*addToDesktop=*/false),
          callback_ (std::move (cb))
    {
        setUsingNativeTitleBar (true);
        setSize (440, 160);

        auto* panel = new ContentPanel (message, includeCancel,
            [this] (Result r) { fire (r); });
        setContentOwned (panel, true);
        setResizable (false, false);

        /* Centre relative to the primary display. */
        if (auto* disp = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
        {
            const auto area = disp->userArea;
            setTopLeftPosition (area.getCentreX() - getWidth()  / 2,
                                area.getCentreY() - getHeight() / 2);
        }

        /* JUCE-Linux on the winelib stack: the Component's visible
         * flag ends up false by the time addToDesktop() runs, so the
         * peer is created mapped-hidden (isOnDesktop=1 + non-null
         * peer + isVisible()=0 -- confirmed via Logger trace).
         * Explicit setVisible before addToDesktop ensures the new
         * peer is mapped visible.  Same fix likely applies to a
         * future Wayland port. */
        setVisible (true);
        addToDesktop();
        toFront (true);
    }

    void fire (Result r)
    {
        if (fired_) return;
        fired_ = true;

        if (callback_)
            callback_ (r);

        /* Defer self-delete so the button click's stack unwinds before
         * the window is torn down. */
        juce::Component::SafePointer<SessionPromptDialog> self (this);
        juce::MessageManager::callAsync ([self]() {
            if (auto* w = self.getComponent())
            {
                w->removeFromDesktop();
                delete w;
            }
        });
    }

    Callback callback_;
    bool     fired_ { false };
};

} // namespace element
