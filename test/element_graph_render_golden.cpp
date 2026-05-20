// SPDX-FileCopyrightText: Copyright (C) Kushview, LLC.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Golden harness for the layered parallel renderer.  Each test builds
// a graph topology, renders N blocks under the sequential executor,
// captures the audio output, then re-runs the same topology under the
// parallel executor and demands byte-identical output.

#include <boost/test/unit_test.hpp>

#include <element/context.hpp>

#include "fixture/PreparedGraph.h"
#include "fixture/TestNode.h"
#include "engine/graphnode.hpp"
#include "engine/ionode.hpp"

#include <cstring>
#include <thread>
#include <vector>

using namespace element;

namespace {

/* Deterministic synthetic source/effect.  Writes the same per-block
 * fingerprint regardless of how many times you call render() with the
 * same numSamples — block counter resets on prepareToRender so the
 * sequential and parallel passes see identical inputs. */
class SynthNode : public Processor
{
public:
    SynthNode (int seed_, int audioIns_, int audioOuts_)
        : Processor (0),
          seed (seed_),
          numAudioIns (audioIns_),
          numAudioOuts (audioOuts_)
    {
        SynthNode::refreshPorts();
    }

    bool wantsContext() const noexcept override { return true; }

    void prepareToRender (double sr, int bs) override
    {
        setRenderDetails (sr, bs);
        blockCounter = 0;
    }

    void releaseResources() override { blockCounter = 0; }

    void render (RenderContext& rc) override
    {
        const int n = rc.audio.getNumSamples();
        const int chans = rc.audio.getNumChannels();
        for (int ch = 0; ch < chans; ++ch)
        {
            float* dst = rc.audio.getWritePointer (ch);
            const float baseIn = (chans > 0 && ch < chans) ? dst[0] : 0.0f;
            const uint32_t mix = (uint32_t) seed * 2654435761u
                                ^ (uint32_t) ch * 374761393u
                                ^ (uint32_t) blockCounter * 668265263u;
            for (int i = 0; i < n; ++i)
            {
                const uint32_t h = mix + (uint32_t) i * 1597334677u;
                const float pattern = (float) ((int32_t) h) * (1.0f / 2147483648.0f);
                /* Mix the input with the synth pattern: passthrough is
                 * lossless for nodes with audio inputs (effect-shape) and
                 * still deterministic for sources (no input → zero). */
                dst[i] = (i == 0 ? baseIn : dst[i]) * 0.5f + pattern * 0.5f;
                (void) baseIn;
            }
        }
        ++blockCounter;
    }

    void renderBypassed (RenderContext&) override {}

    int getNumPrograms() const override { return 1; }
    int getCurrentProgram() const override { return 0; }
    const String getProgramName (int) const override { return "default"; }
    void setCurrentProgram (int) override {}
    void getState (MemoryBlock&) override {}
    void setState (const void*, int) override {}

    void getPluginDescription (PluginDescription& d) const override
    {
        d.pluginFormatName = "Element";
        d.fileOrIdentifier = "element.synthNode";
        d.manufacturerName = "Element";
    }

    void refreshPorts() override
    {
        PortList p;
        uint32 port = 0;
        for (int c = 0; c < numAudioIns; ++c)
            p.add (PortType::Audio, port++, c, String ("in_") + String (c), String ("In ") + String (c), true);
        for (int c = 0; c < numAudioOuts; ++c)
            p.add (PortType::Audio, port++, c, String ("out_") + String (c), String ("Out ") + String (c), false);
        setPorts (p);
    }

protected:
    void initialize() override {}

private:
    const int seed;
    const int numAudioIns, numAudioOuts;
    int blockCounter = 0;
};

constexpr int kSampleRate = 44100;
constexpr int kBlockSize = 512;
constexpr int kNumBlocks = 8;

/* Render N blocks and concatenate the audio output channel-major into a
 * flat float vector for byte-comparison. */
std::vector<float> renderAndCapture (GraphNode& graph, bool parallel)
{
    /* Force phase-reset so the synthetic counter agrees across both passes. */
    graph.releaseResources();
    graph.prepareToRender ((double) kSampleRate, kBlockSize);
    graph.setParallelExecutionEnabled (parallel);

    std::vector<float> out;
    out.reserve ((size_t) kNumBlocks * (size_t) kBlockSize * 2);

    juce::AudioSampleBuffer audio (2, kBlockSize);
    juce::MidiBuffer midi;
    for (int b = 0; b < kNumBlocks; ++b)
    {
        audio.clear();
        midi.clear();
        RenderContext rc (audio, audio, midi, kBlockSize);
        graph.render (rc);
        for (int ch = 0; ch < audio.getNumChannels(); ++ch)
        {
            const float* src = audio.getReadPointer (ch);
            out.insert (out.end(), src, src + kBlockSize);
        }
    }
    return out;
}

bool poolAvailable()
{
    return (int) std::thread::hardware_concurrency() >= 3;
}

void compareSequentialVsParallel (PreparedGraph& fix, const char* label)
{
    auto seqOut = renderAndCapture (fix.graph, false);
    auto parOut = renderAndCapture (fix.graph, true);
    BOOST_REQUIRE_MESSAGE (seqOut.size() == parOut.size(),
                          label << ": capture sizes differ");
    BOOST_REQUIRE_MESSAGE (std::memcmp (seqOut.data(), parOut.data(),
                                        seqOut.size() * sizeof (float)) == 0,
                          label << ": parallel output diverges from sequential");
}

uint32 audioOutPort (Processor* p, int ch) { return p->getPortForChannel (PortType::Audio, ch, false); }
uint32 audioInPort  (Processor* p, int ch) { return p->getPortForChannel (PortType::Audio, ch, true);  }

} // namespace

BOOST_AUTO_TEST_SUITE (GraphRenderGolden)

BOOST_AUTO_TEST_CASE (SerialChain)
{
    /* 1: Synth → Effect → graph output.  Layer count is small; gate
     * may keep this sequential anyway — but the parallel path must
     * still produce byte-identical output. */
    if (! poolAvailable()) { BOOST_TEST_MESSAGE ("skip: hwc < 3"); return; }
    PreparedGraph fix;
    auto* src = new SynthNode (1, 0, 2);
    auto* fx  = new SynthNode (2, 2, 2);
    fix.graph.addNode (src);
    fix.graph.addNode (fx);
    fix.graph.addConnection (src->nodeId, audioOutPort (src, 0), fx->nodeId, audioInPort (fx, 0));
    fix.graph.addConnection (src->nodeId, audioOutPort (src, 1), fx->nodeId, audioInPort (fx, 1));
    fix.graph.rebuild();
    compareSequentialVsParallel (fix, "SerialChain");
}

BOOST_AUTO_TEST_CASE (TwoParallelBranches)
{
    /* 2: SynthA, SynthB both feed Mix.  SynthA and SynthB sit in the
     * SAME layer (no buffer aliasing between their outputs); parallel
     * dispatcher should run them on separate workers. */
    if (! poolAvailable()) { BOOST_TEST_MESSAGE ("skip: hwc < 3"); return; }
    PreparedGraph fix;
    auto* a   = new SynthNode (10, 0, 2);
    auto* b   = new SynthNode (20, 0, 2);
    auto* mix = new SynthNode (30, 2, 2);
    fix.graph.addNode (a);
    fix.graph.addNode (b);
    fix.graph.addNode (mix);
    fix.graph.addConnection (a->nodeId, audioOutPort (a, 0), mix->nodeId, audioInPort (mix, 0));
    fix.graph.addConnection (a->nodeId, audioOutPort (a, 1), mix->nodeId, audioInPort (mix, 1));
    fix.graph.addConnection (b->nodeId, audioOutPort (b, 0), mix->nodeId, audioInPort (mix, 0));
    fix.graph.addConnection (b->nodeId, audioOutPort (b, 1), mix->nodeId, audioInPort (mix, 1));
    fix.graph.rebuild();
    compareSequentialVsParallel (fix, "TwoParallelBranches");
}

BOOST_AUTO_TEST_CASE (ThreeParallelBranches)
{
    if (! poolAvailable()) { BOOST_TEST_MESSAGE ("skip: hwc < 3"); return; }
    PreparedGraph fix;
    auto* a   = new SynthNode (100, 0, 2);
    auto* b   = new SynthNode (200, 0, 2);
    auto* c   = new SynthNode (300, 0, 2);
    auto* mix = new SynthNode (400, 2, 2);
    fix.graph.addNode (a);
    fix.graph.addNode (b);
    fix.graph.addNode (c);
    fix.graph.addNode (mix);
    for (auto* s : { a, b, c }) {
        fix.graph.addConnection (s->nodeId, audioOutPort (s, 0), mix->nodeId, audioInPort (mix, 0));
        fix.graph.addConnection (s->nodeId, audioOutPort (s, 1), mix->nodeId, audioInPort (mix, 1));
    }
    fix.graph.rebuild();
    compareSequentialVsParallel (fix, "ThreeParallelBranches");
}

BOOST_AUTO_TEST_CASE (BranchThenChain)
{
    /* SynthA → FxA ↘
     *               → Mix
     * SynthB → FxB ↗
     * Two parallel chains, each two ops deep, then a final mix.  Layer
     * computation should split into 3 layers: {A,B} ∥ {FxA,FxB} ∥ {Mix}.
     */
    if (! poolAvailable()) { BOOST_TEST_MESSAGE ("skip: hwc < 3"); return; }
    PreparedGraph fix;
    auto* a   = new SynthNode (11, 0, 2);
    auto* b   = new SynthNode (22, 0, 2);
    auto* fxa = new SynthNode (33, 2, 2);
    auto* fxb = new SynthNode (44, 2, 2);
    auto* mix = new SynthNode (55, 2, 2);
    fix.graph.addNode (a);
    fix.graph.addNode (b);
    fix.graph.addNode (fxa);
    fix.graph.addNode (fxb);
    fix.graph.addNode (mix);
    fix.graph.addConnection (a->nodeId, audioOutPort (a, 0), fxa->nodeId, audioInPort (fxa, 0));
    fix.graph.addConnection (a->nodeId, audioOutPort (a, 1), fxa->nodeId, audioInPort (fxa, 1));
    fix.graph.addConnection (b->nodeId, audioOutPort (b, 0), fxb->nodeId, audioInPort (fxb, 0));
    fix.graph.addConnection (b->nodeId, audioOutPort (b, 1), fxb->nodeId, audioInPort (fxb, 1));
    fix.graph.addConnection (fxa->nodeId, audioOutPort (fxa, 0), mix->nodeId, audioInPort (mix, 0));
    fix.graph.addConnection (fxa->nodeId, audioOutPort (fxa, 1), mix->nodeId, audioInPort (mix, 1));
    fix.graph.addConnection (fxb->nodeId, audioOutPort (fxb, 0), mix->nodeId, audioInPort (mix, 0));
    fix.graph.addConnection (fxb->nodeId, audioOutPort (fxb, 1), mix->nodeId, audioInPort (mix, 1));
    fix.graph.rebuild();
    compareSequentialVsParallel (fix, "BranchThenChain");
}

BOOST_AUTO_TEST_CASE (FanOutAndConverge)
{
    /* Single source fans into 3 effects in parallel, then converge.
     * Effect ops share the same read buffer (source's output) but write
     * to distinct destination buffers — they belong in the same layer
     * (read/read does not conflict).
     */
    if (! poolAvailable()) { BOOST_TEST_MESSAGE ("skip: hwc < 3"); return; }
    PreparedGraph fix;
    auto* src = new SynthNode (7, 0, 2);
    auto* fx1 = new SynthNode (71, 2, 2);
    auto* fx2 = new SynthNode (72, 2, 2);
    auto* fx3 = new SynthNode (73, 2, 2);
    auto* mix = new SynthNode (79, 2, 2);
    fix.graph.addNode (src);
    fix.graph.addNode (fx1);
    fix.graph.addNode (fx2);
    fix.graph.addNode (fx3);
    fix.graph.addNode (mix);
    for (auto* fx : { fx1, fx2, fx3 }) {
        fix.graph.addConnection (src->nodeId, audioOutPort (src, 0), fx->nodeId, audioInPort (fx, 0));
        fix.graph.addConnection (src->nodeId, audioOutPort (src, 1), fx->nodeId, audioInPort (fx, 1));
        fix.graph.addConnection (fx->nodeId,  audioOutPort (fx, 0),  mix->nodeId, audioInPort (mix, 0));
        fix.graph.addConnection (fx->nodeId,  audioOutPort (fx, 1),  mix->nodeId, audioInPort (mix, 1));
    }
    fix.graph.rebuild();
    compareSequentialVsParallel (fix, "FanOutAndConverge");
}

BOOST_AUTO_TEST_CASE (HeavyMultiBranch)
{
    /* 4 sources × 1 effect each, all converging in a final mixer.
     * Most ProcessBufferOps per layer of the suite — should fully
     * exercise the worker-chunking path. */
    if (! poolAvailable()) { BOOST_TEST_MESSAGE ("skip: hwc < 3"); return; }
    PreparedGraph fix;
    SynthNode* sources[4];
    SynthNode* effects[4];
    for (int i = 0; i < 4; ++i)
    {
        sources[i] = new SynthNode (1000 + i, 0, 2);
        effects[i] = new SynthNode (2000 + i, 2, 2);
        fix.graph.addNode (sources[i]);
        fix.graph.addNode (effects[i]);
    }
    auto* mix = new SynthNode (9999, 2, 2);
    fix.graph.addNode (mix);
    for (int i = 0; i < 4; ++i)
    {
        fix.graph.addConnection (sources[i]->nodeId, audioOutPort (sources[i], 0), effects[i]->nodeId, audioInPort (effects[i], 0));
        fix.graph.addConnection (sources[i]->nodeId, audioOutPort (sources[i], 1), effects[i]->nodeId, audioInPort (effects[i], 1));
        fix.graph.addConnection (effects[i]->nodeId, audioOutPort (effects[i], 0), mix->nodeId, audioInPort (mix, 0));
        fix.graph.addConnection (effects[i]->nodeId, audioOutPort (effects[i], 1), mix->nodeId, audioInPort (mix, 1));
    }
    fix.graph.rebuild();
    compareSequentialVsParallel (fix, "HeavyMultiBranch");
}

BOOST_AUTO_TEST_SUITE_END()
