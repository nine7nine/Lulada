// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "services/samplebankpool.hpp"

#include <unistd.h>   // ::access -- native POSIX existence check
#include <fcntl.h>    // O_RDONLY
#include <sys/stat.h>

namespace element {

using juce::ScopedLock;
using juce::ValueTree;
using juce::Identifier;
using juce::String;
using juce::MemoryBlock;
using juce::MemoryOutputStream;
using juce::MemoryInputStream;
using juce::GZIPCompressorOutputStream;
using juce::File;
using juce::StringArray;

SampleBankPool& SampleBankPool::get()
{
    static SampleBankPool instance;
    return instance;
}

int SampleBankPool::getNumInstruments() const
{
    const ScopedLock sl (lock_);
    return (int) instruments_.size();
}

SamplerInstrument::Ptr SampleBankPool::getInstrument (int index) const
{
    const ScopedLock sl (lock_);
    if (index < 0 || index >= (int) instruments_.size()) return nullptr;
    return instruments_[(size_t) index];
}

SamplerInstrument::Ptr SampleBankPool::addInstrument()
{
    SamplerInstrument::Ptr inst;
    {
        ScopedLock sl (lock_);
        if ((int) instruments_.size() >= kNumBanks) return nullptr;
        inst = SamplerInstrument::Ptr (new SamplerInstrument());
        instruments_.push_back (inst);
    }
    sendChangeMessage();
    return inst;
}

bool SampleBankPool::ensureInstrumentExists (int targetIndex)
{
    if (targetIndex < 0 || targetIndex >= kNumBanks) return false;
    bool grew = false;
    {
        ScopedLock sl (lock_);
        while ((int) instruments_.size() <= targetIndex)
        {
            if ((int) instruments_.size() >= kNumBanks) break;
            instruments_.push_back (
                SamplerInstrument::Ptr (new SamplerInstrument()));
            grew = true;
        }
    }
    if (grew) sendChangeMessage();
    return (int) instruments_.size() > targetIndex;
}

void SampleBankPool::removeInstrument (int index)
{
    {
        ScopedLock sl (lock_);
        if (index < 0 || index >= (int) instruments_.size()) return;
        if (instruments_.size() <= 1) return;
        instruments_.erase (instruments_.begin() + index);
    }
    sendChangeMessage();
}

void SampleBankPool::clearInstrument (int index)
{
    bool dirty = false;
    {
        ScopedLock sl (lock_);
        if (index < 0 || index >= (int) instruments_.size()) return;
        if (auto inst = instruments_[(size_t) index])
        {
            inst->clear();
            dirty = true;
        }
    }
    if (dirty) sendChangeMessage();
}

void SampleBankPool::clearAll()
{
    {
        ScopedLock sl (lock_);
        instruments_.clear();
        hasLoaded_ = false;
    }
    sendChangeMessage();
}

bool SampleBankPool::hasLoaded() const noexcept
{
    const ScopedLock sl (lock_);
    return hasLoaded_;
}

void SampleBankPool::resetLoadFlag() noexcept
{
    const ScopedLock sl (lock_);
    hasLoaded_ = false;
}

/* =====================================================================
 * Serialisation -- wire format matches the legacy SamplerNode per-bank
 * XML so existing sessions migrate without conversion.  Format:
 *
 *   <bankPool>
 *     <instr idx=... name=... fadeout=... ...>
 *       <slot idx=... name=... sourceFile=... ... />
 *       ...
 *     </instr>
 *     ...
 *   </bankPool>
 * ===================================================================*/

void SampleBankPool::getStateInformation (MemoryBlock& dest)
{
    ValueTree tree ("bankPool");

    ScopedLock sl (lock_);

    for (size_t ii = 0; ii < instruments_.size(); ++ii)
    {
        auto inst = instruments_[ii];
        if (inst == nullptr) continue;

        ValueTree instTree ("instr");
        instTree.setProperty ("idx",  (int) ii,        nullptr);
        instTree.setProperty ("name", inst->name,      nullptr);
        instTree.setProperty ("fadeout", (int) inst->fadeoutRate, nullptr);
        instTree.setProperty ("avType",  (int) inst->autoVib.type,  nullptr);
        instTree.setProperty ("avSweep", (int) inst->autoVib.sweep, nullptr);
        instTree.setProperty ("avDepth", (int) inst->autoVib.depth, nullptr);
        instTree.setProperty ("avRate",  (int) inst->autoVib.rate,  nullptr);
        instTree.setProperty ("mono",            inst->mono,              nullptr);
        instTree.setProperty ("portamentoMs",    (double) inst->portamentoTimeMs, nullptr);
        instTree.setProperty ("envSampleRel",    inst->envSampleRelative, nullptr);

        auto writeEnv = [&](const FT2Envelope& e, const String& prefix)
        {
            instTree.setProperty (Identifier (prefix + "Len"),     (int) e.length,       nullptr);
            instTree.setProperty (Identifier (prefix + "Flags"),   (int) e.flags,        nullptr);
            instTree.setProperty (Identifier (prefix + "Sus"),     (int) e.sustainPoint, nullptr);
            instTree.setProperty (Identifier (prefix + "LoopS"),   (int) e.loopStart,    nullptr);
            instTree.setProperty (Identifier (prefix + "LoopE"),   (int) e.loopEnd,      nullptr);
            String pts;
            for (int i = 0; i < (int) e.length; ++i)
                pts += String (e.points[i].x) + ":" + String (e.points[i].y) + ";";
            instTree.setProperty (Identifier (prefix + "Pts"), pts, nullptr);
        };
        writeEnv (inst->volumeEnv, "ve");
        writeEnv (inst->panEnv,    "pe");

        for (int i = 0; i < SamplerInstrument::kNumSlots; ++i)
        {
            const auto* slot = inst->getSlot (i);
            if (slot == nullptr) continue;
            ValueTree slotTree ("slot");
            slotTree.setProperty ("idx",          i,                    nullptr);
            slotTree.setProperty ("name",         slot->name,           nullptr);
            slotTree.setProperty ("sourceFile",   String (slot->sourceFile), nullptr);
            slotTree.setProperty ("rootNote",     slot->rootNote,       nullptr);
            slotTree.setProperty ("relativeNote", slot->relativeNote,   nullptr);
            slotTree.setProperty ("finetune",     slot->finetune,       nullptr);
            slotTree.setProperty ("volume",       slot->volume,         nullptr);
            slotTree.setProperty ("pan",          slot->panning,        nullptr);
            slotTree.setProperty ("loopMode",     (int) slot->loopMode, nullptr);
            slotTree.setProperty ("loopStart",    slot->loopStart,      nullptr);
            slotTree.setProperty ("loopLength",   slot->loopLength,     nullptr);
            slotTree.setProperty ("busIndex",     slot->busIndex,           nullptr);
            instTree.appendChild (slotTree, nullptr);
        }

        /* 128-byte keymap CSV (note-to-slot, FT2-style). */
        {
            String km;
            for (int n = 0; n < 128; ++n)
            {
                if (n > 0) km += ",";
                km += String ((int) inst->slotForNote (n));
            }
            instTree.setProperty ("keymap", km, nullptr);
        }

        tree.appendChild (instTree, nullptr);
    }

    MemoryOutputStream out (dest, false);
    { GZIPCompressorOutputStream gzip (out); tree.writeToStream (gzip); }
}

void SampleBankPool::setStateInformation (const void* data, int size)
{
    if (data == nullptr || size <= 0) return;
    const auto tree = ValueTree::readFromGZIPData (data, (size_t) size);
    if (! tree.isValid()) return;

    /* Accept both new ("bankPool") and legacy ("sampler") root tag --
     * legacy migration path runs when a SamplerNode forwards its own
     * embedded bank-data XML into the pool on first load. */
    const auto typeName = tree.getType().toString();
    if (typeName != "bankPool" && typeName != "sampler") return;

    ScopedLock sl (lock_);

    instruments_.clear();
    juce::AudioFormatManager fmt;
    fmt.registerBasicFormats();

    for (int i = 0; i < tree.getNumChildren(); ++i)
    {
        const auto instTree = tree.getChild (i);
        if (instTree.getType() != Identifier ("instr")) continue;

        SamplerInstrument::Ptr inst (new SamplerInstrument());
        inst->name        = instTree.getProperty ("name", "").toString();
        inst->fadeoutRate = (uint16_t) (int) instTree.getProperty ("fadeout", 0);
        inst->autoVib.type  = (uint8_t) (int) instTree.getProperty ("avType",  0);
        inst->autoVib.sweep = (uint8_t) (int) instTree.getProperty ("avSweep", 0);
        inst->autoVib.depth = (uint8_t) (int) instTree.getProperty ("avDepth", 0);
        inst->autoVib.rate  = (uint8_t) (int) instTree.getProperty ("avRate",  0);
        inst->mono            = (bool)  instTree.getProperty ("mono", false);
        inst->portamentoTimeMs = (float) (double) instTree.getProperty ("portamentoMs", 80.0);
        inst->envSampleRelative = (bool) instTree.getProperty ("envSampleRel", true);

        auto readEnv = [&](FT2Envelope& e, const String& prefix)
        {
            const int rawLen = (int) instTree.getProperty (Identifier (prefix + "Len"),   0);
            const int rawSus = (int) instTree.getProperty (Identifier (prefix + "Sus"),   0);
            const int rawLoS = (int) instTree.getProperty (Identifier (prefix + "LoopS"), 0);
            const int rawLoE = (int) instTree.getProperty (Identifier (prefix + "LoopE"), 0);
            e.length       = (uint8_t) juce::jlimit (0, 12, rawLen);
            e.flags        = (uint8_t) (int) instTree.getProperty (Identifier (prefix + "Flags"), 0);
            e.sustainPoint = (uint8_t) juce::jlimit (0, juce::jmax (0, (int) e.length - 1), rawSus);
            e.loopStart    = (uint8_t) juce::jlimit (0, juce::jmax (0, (int) e.length - 1), rawLoS);
            e.loopEnd      = (uint8_t) juce::jlimit ((int) e.loopStart,
                                                       juce::jmax (0, (int) e.length - 1), rawLoE);

            const String pts = instTree.getProperty (Identifier (prefix + "Pts"), "").toString();
            const auto parts = StringArray::fromTokens (pts, ";", "");
            int n = 0;
            for (const auto& s : parts)
            {
                if (n >= 12) break;
                const auto xy = StringArray::fromTokens (s, ":", "");
                if (xy.size() != 2) continue;
                e.points[n].x = (int16_t) xy[0].getIntValue();
                e.points[n].y = (int16_t) xy[1].getIntValue();
                ++n;
            }
            if (e.length == 0 && n > 0) e.length = (uint8_t) juce::jmin (12, n);
            if (e.length > (uint8_t) n) e.length = (uint8_t) n;
        };
        readEnv (inst->volumeEnv, "ve");
        readEnv (inst->panEnv,    "pe");

        for (int c = 0; c < instTree.getNumChildren(); ++c)
        {
            const auto slotTree = instTree.getChild (c);
            if (slotTree.getType() != Identifier ("slot")) continue;
            const int idx = (int) slotTree.getProperty ("idx", 0);
            if (idx < 0 || idx >= SamplerInstrument::kNumSlots) continue;

            auto fresh = std::make_unique<SamplerSampleSlot>();
            const std::string savedPath =
                slotTree.getProperty ("sourceFile", "").toString().toRawUTF8();

            if (! savedPath.empty() && ::access (savedPath.c_str(), R_OK) == 0)
            {
                const File f { String (savedPath) };
                if (auto loaded = inst->prepareSlot (f, fmt))
                {
                    fresh->data16L          = std::move (loaded->data16L);
                    fresh->data16R          = std::move (loaded->data16R);
                    fresh->isStereo         = loaded->isStereo;
                    fresh->numSamples       = loaded->numSamples;
                    fresh->sourceSampleRate = loaded->sourceSampleRate;
                    fresh->sourceFile       = loaded->sourceFile;
                }
                else
                {
                    fresh->sourceFile = savedPath;
                }
            }
            else
            {
                fresh->sourceFile = savedPath;
            }

            fresh->name         = slotTree.getProperty ("name", "").toString();
            fresh->rootNote     = (int) slotTree.getProperty ("rootNote", 60);
            fresh->relativeNote = (int) slotTree.getProperty ("relativeNote", 0);
            fresh->finetune     = (int) slotTree.getProperty ("finetune", 0);
            fresh->volume       = (float) (double) slotTree.getProperty ("volume", 1.0);
            fresh->panning      = (float) (double) slotTree.getProperty ("pan", 0.5);
            fresh->loopMode     = (SamplerLoopMode) (int) slotTree.getProperty ("loopMode", 0);
            fresh->loopStart    = (int) slotTree.getProperty ("loopStart",  0);
            fresh->loopLength   = (int) slotTree.getProperty ("loopLength", 0);

            if (slotTree.hasProperty ("busIndex"))
            {
                fresh->busIndex = juce::jlimit (0, SamplerNode::kNumBuses - 1,
                                                (int) slotTree.getProperty ("busIndex", 0));
            }
            else if (slotTree.hasProperty ("busSend1"))
            {
                float best = 0.0f; int bestIdx = 0;
                for (int b = 0; b < SamplerNode::kNumBuses; ++b)
                {
                    const float s = (float) (double) slotTree.getProperty (
                        Identifier ("busSend" + String (b + 1)), 0.0);
                    if (s > best) { best = s; bestIdx = b; }
                }
                fresh->busIndex = bestIdx;
            }
            else if (slotTree.hasProperty ("busAssign"))
            {
                const int oldAssign = (int) slotTree.getProperty ("busAssign", 0);
                fresh->busIndex = juce::jmax (0, oldAssign - 1);
            }

            inst->commitSlot (idx, std::move (fresh));
        }

        const String savedKeymap = instTree.getProperty ("keymap", "").toString();
        if (savedKeymap.isNotEmpty())
        {
            auto parts = StringArray::fromTokens (savedKeymap, ",", "");
            const int n = juce::jmin (128, parts.size());
            for (int k = 0; k < n; ++k)
            {
                const int s = juce::jlimit (0, SamplerInstrument::kNumSlots - 1,
                                            parts[k].getIntValue());
                inst->setSlotForNote (k, s);
            }
        }

        instruments_.push_back (inst);
    }

    hasLoaded_ = true;
    sendChangeMessage();
}

} // namespace element
