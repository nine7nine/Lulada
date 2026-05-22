// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "nodes/sampler.hpp"
#include "services/diskopservice.hpp"

#include <pthread.h>
#include <sched.h>
#include <unistd.h>      // ::access ::open ::read ::close (native POSIX file I/O)
#include <fcntl.h>       // O_RDONLY O_CLOEXEC
#include <sys/stat.h>    // ::fstat for file size
#include <cerrno>        // errno EINTR

/* Vendored ft2-clone mixer (BSD-3-Clause). See src/engine/sampler/. */
extern "C" {
#include "engine/sampler/ft2_audio.h"
#include "engine/sampler/ft2_mix.h"
#include "engine/sampler/ft2_mix_interpolation.h"
}

namespace element {

namespace {

/* mixFunc indices: layout in ft2_mix.c bottom of file:
 *   no-ramp 8-bit:   0..14  (NoLoop/Loop/Pingpong) × (None,S8,Lin,S16,Cubic)
 *   no-ramp 16-bit: 15..29  same
 *   ramped  8-bit:  30..44
 *   ramped 16-bit:  45..59
 * Within a (bit-depth, ramp) row of 15: per (interp 0..4) × (loop 0..2).
 * Layout per interp: NoLoop, Loop, Pingpong. */
constexpr int kBase16NoRamp = 15;
constexpr int kBase16Ramped = 45;

constexpr int interpOffset (int interpMode)
{
    switch (interpMode)
    {
        case 0: return  0;
        case 1: return  6;
        case 2: return 12;
        case 3: return  9;
        default: return 0;
    }
}

std::unique_ptr<int16_t[]> convertFloatToInt16 (const float* src, int n)
{
    std::unique_ptr<int16_t[]> out (new int16_t[(size_t) n]);
    for (int i = 0; i < n; ++i)
    {
        float v = src[i] * 32767.0f;
        if (v >  32767.f) v =  32767.f;
        if (v < -32767.f) v = -32767.f;
        out[(size_t) i] = (int16_t) v;
    }
    return out;
}

void ensureMixerInterpolationTablesReady()
{
    static std::once_flag flag;
    std::call_once (flag, [] { setupMixerInterpolationTables(); });
}

/* Shared slot palette so the keymap + bank slot-list + Disk Op sample
 * bank all colour matching slots identically.  32 distinct hues; slot
 * -1 / unloaded → neutral gray. */
inline juce::Colour slotPaletteColour (int slot, float sat = 0.55f, float val = 0.85f)
{
    if (slot < 0) return juce::Colour { 0xff'30'30'30 };
    const float h = (slot % 32) / 32.0f;
    return juce::Colour::fromHSV (h, sat, val, 1.0f);
}

/* FT2 auto-vibrato sine table (256 entries, -64..+64).  Sourced from
 * ft2_tables.c::autoVibSineTab.  Used by AutoVibrato pitch modulation. */
static const int8_t kAutoVibSineTab[256] = {
      0,   2,   3,   5,   6,   8,   9,  11,  12,  14,  16,  17,  19,  20,  22,  23,
     24,  26,  27,  29,  30,  32,  33,  34,  36,  37,  38,  39,  41,  42,  43,  44,
     45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  56,  57,  58,  59,
     59,  60,  60,  61,  61,  62,  62,  62,  63,  63,  63,  64,  64,  64,  64,  64,
     64,  64,  64,  64,  64,  64,  63,  63,  63,  62,  62,  62,  61,  61,  60,  60,
     59,  59,  58,  57,  56,  56,  55,  54,  53,  52,  51,  50,  49,  48,  47,  46,
     45,  44,  43,  42,  41,  39,  38,  37,  36,  34,  33,  32,  30,  29,  27,  26,
     24,  23,  22,  20,  19,  17,  16,  14,  12,  11,   9,   8,   6,   5,   3,   2,
      0,  -2,  -3,  -5,  -6,  -8,  -9, -11, -12, -14, -16, -17, -19, -20, -22, -23,
    -24, -26, -27, -29, -30, -32, -33, -34, -36, -37, -38, -39, -41, -42, -43, -44,
    -45, -46, -47, -48, -49, -50, -51, -52, -53, -54, -55, -56, -56, -57, -58, -59,
    -59, -60, -60, -61, -61, -62, -62, -62, -63, -63, -63, -64, -64, -64, -64, -64,
    -64, -64, -64, -64, -64, -64, -63, -63, -63, -62, -62, -62, -61, -61, -60, -60,
    -59, -59, -58, -57, -56, -56, -55, -54, -53, -52, -51, -50, -49, -48, -47, -46,
    -45, -44, -43, -42, -41, -39, -38, -37, -36, -34, -33, -32, -30, -29, -27, -26,
    -24, -23, -22, -20, -19, -17, -16, -14, -12, -11,  -9,  -8,  -6,  -5,  -3,  -2
};

} // anonymous namespace


/* ===========================================================================
 * SamplerInstrument
 * ========================================================================*/

SamplerInstrument::SamplerInstrument()
{
    for (auto& s : slots) s.reset();

    /* Default envelopes: disabled.  Voices fall back to SamplerNode's
     * global ADSR until the user enables the FT2 envelope.  A minimal
     * 2-point full-volume shape is preloaded so that "enable" produces
     * audible output immediately rather than silence. */
    volumeEnv.length = 2;
    volumeEnv.points[0] = { 0,   64 };
    volumeEnv.points[1] = { 50,  64 };
    volumeEnv.sustainPoint = 0;
    volumeEnv.loopStart = 0;
    volumeEnv.loopEnd = 1;
    volumeEnv.flags = 0;

    panEnv.length = 2;
    panEnv.points[0] = { 0,   32 };
    panEnv.points[1] = { 50,  32 };
    panEnv.sustainPoint = 0;
    panEnv.loopStart = 0;
    panEnv.loopEnd = 1;
    panEnv.flags = 0;
}

std::unique_ptr<SamplerSampleSlot>
SamplerInstrument::prepareSlot (const File& file, AudioFormatManager& fmt)
{
    /* Native POSIX file I/O on the Linux side -- ::open / ::read /
     * ::close, never juce::File methods that would round-trip through
     * wineserver on winelib.  juce::File is used ONLY to extract the
     * path string + display name; no fs operations on it.  Audio
     * decode runs on the in-memory buffer via juce::MemoryInputStream
     * so the AudioFormatManager never touches the fs after this
     * function returns. */
    const std::string path = file.getFullPathName().toRawUTF8();
    if (path.empty()) return nullptr;

    const int fd = ::open (path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return nullptr;

    struct stat st {};
    if (::fstat (fd, &st) != 0 || st.st_size <= 0)
    {
        ::close (fd);
        return nullptr;
    }
    /* Bound memory at 1 GiB for malformed / truncated files.  Matches
     * the "10 min at 48 kHz @ 16-bit stereo = ~110 MB" worst case the
     * old code implicitly capped via maxLen below, with headroom. */
    static constexpr off_t kMaxBytes = (off_t) 1024 * 1024 * 1024;
    if (st.st_size > kMaxBytes)
    {
        ::close (fd);
        return nullptr;
    }

    const size_t bytes = (size_t) st.st_size;
    MemoryBlock buf (bytes, false /*initialiseToZero*/);

    size_t total = 0;
    while (total < bytes)
    {
        const ssize_t r = ::read (fd, (char*) buf.getData() + total, bytes - total);
        if (r < 0)
        {
            if (errno == EINTR) continue;
            ::close (fd);
            return nullptr;
        }
        if (r == 0) break;
        total += (size_t) r;
    }
    ::close (fd);
    if (total != bytes) return nullptr;   // short read -- bail

    /* Decode from in-memory buffer.  This juce overload takes a
     * unique_ptr<InputStream> and consumes it whether or not a
     * format matches.  The MemoryInputStream references buf without
     * copying; buf must outlive the reader (it does -- both go out
     * of scope at the end of this function, reader first). */
    std::unique_ptr<InputStream> mis (
        new MemoryInputStream (buf.getData(), buf.getSize(),
                               false /*keepInternalCopyOfData*/));
    std::unique_ptr<AudioFormatReader> reader (fmt.createReaderFor (std::move (mis)));
    if (reader == nullptr) return nullptr;

    const int64_t maxLen = (int64_t) 10 * 60 * (int64_t) reader->sampleRate;
    const int n = (int) std::min ((int64_t) reader->lengthInSamples, maxLen);
    if (n <= 0) return nullptr;
    const bool stereo = reader->numChannels >= 2;

    AudioBuffer<float> tmp ((int) reader->numChannels, n);
    reader->read (&tmp, 0, n, 0, true, true);

    auto s = std::make_unique<SamplerSampleSlot>();
    s->name             = file.getFileNameWithoutExtension();
    s->sourceFile       = path;   // native POSIX path for persistence
    s->numSamples       = n;
    s->sourceSampleRate = reader->sampleRate;
    s->rootNote         = 60;
    s->relativeNote     = 0;
    s->isStereo         = stereo;
    s->data16L          = convertFloatToInt16 (tmp.getReadPointer (0), n);
    if (stereo)
        s->data16R = convertFloatToInt16 (tmp.getReadPointer (1), n);
    return s;
}

bool SamplerInstrument::commitSlot (int slot, std::unique_ptr<SamplerSampleSlot> data)
{
    if (slot < 0 || slot >= kNumSlots) return false;
    if (data == nullptr) return false;
    slots[(size_t) slot] = std::move (data);
    if (! keymapUserModified) autoSpreadKeymap();
    return true;
}

bool SamplerInstrument::loadSampleToSlot (int slot, const File& file,
                                          AudioFormatManager& fmt)
{
    auto s = prepareSlot (file, fmt);
    return commitSlot (slot, std::move (s));
}

void SamplerInstrument::clearSlot (int slot)
{
    if (slot < 0 || slot >= kNumSlots) return;
    slots[(size_t) slot].reset();
    if (! keymapUserModified) autoSpreadKeymap();
}

void SamplerInstrument::clear()
{
    for (auto& s : slots) s.reset();
    name = String();
    keymapUserModified = false;
    autoSpreadKeymap();
}

int SamplerInstrument::slotForNote (int midiNote) const noexcept
{
    if (midiNote < 0 || midiNote > 127) return -1;
    return (int) noteToSlot[midiNote];
}

void SamplerInstrument::setSlotForNote (int midiNote, int slot)
{
    if (midiNote < 0 || midiNote > 127) return;
    noteToSlot[midiNote] = (uint8_t) jlimit (0, kNumSlots - 1, slot);
    keymapUserModified = true;
}

void SamplerInstrument::autoSpreadKeymap()
{
    Array<int> loadedSlots;
    for (int i = 0; i < kNumSlots; ++i)
        if (slots[(size_t) i] != nullptr) loadedSlots.add (i);

    if (loadedSlots.isEmpty())
    {
        for (int n = 0; n < 128; ++n) noteToSlot[n] = 0;
        return;
    }

    const int n = loadedSlots.size();
    for (int note = 0; note < 128; ++note)
    {
        const int idx = (note * n) / 128;
        noteToSlot[note] = (uint8_t) loadedSlots[idx];
    }
}

SamplerSampleSlot*       SamplerInstrument::getSlot (int slot)
    { return (slot >= 0 && slot < kNumSlots) ? slots[(size_t) slot].get() : nullptr; }
const SamplerSampleSlot* SamplerInstrument::getSlot (int slot) const
    { return (slot >= 0 && slot < kNumSlots) ? slots[(size_t) slot].get() : nullptr; }

int SamplerInstrument::firstLoadedSlot() const noexcept
{
    for (int i = 0; i < kNumSlots; ++i)
        if (slots[(size_t) i] != nullptr) return i;
    return -1;
}

int SamplerInstrument::numLoaded() const noexcept
{
    int c = 0;
    for (const auto& s : slots) if (s != nullptr) ++c;
    return c;
}


/* ===========================================================================
 * Ft2SamplerSound — JUCE shim that holds a back-reference to SamplerNode
 * so voices can look up the instrument bound to the playing MIDI channel
 * at note-on time.  Multi-instrument routing point.
 * ========================================================================*/
class Ft2SamplerSound : public SynthesiserSound
{
public:
    explicit Ft2SamplerSound (SamplerNode& n) : node (n) {}
    bool appliesToNote    (int) override { return true; }
    bool appliesToChannel (int) override { return true; }
    SamplerNode& getNode() const { return node; }
private:
    SamplerNode& node;
};


/* ===========================================================================
 * ChannelTrackingSynth — JUCE's SynthesiserVoice doesn't expose the
 * playing MIDI channel, but we need it for multi-instrument routing.
 * Forward the channel to the owning SamplerNode before invoking noteOn,
 * where voices read it back during startNote().
 *
 * Also hosts the voice-pool worker dispatch: when active voice count and
 * sub-block size both exceed a threshold, renderVoices fans the voice
 * loop out to per-worker scratch buffers (parallel) and sums the result
 * back into the output buffer.  Workers run at SCHED_FIFO 70 (one rung
 * below NSPA's audio-thread priority 80) so the main audio thread always
 * preempts them.  Below threshold the base scalar path is preserved
 * verbatim — small voice loads see zero behaviour change.
 * ========================================================================*/

/* Single worker thread owned by ChannelTrackingSynth.  Waits on workReady,
 * renders its assigned voice range into its own scratch, signals
 * jobCompleted.  Stack-shaped sync via juce::WaitableEvent (futex-backed
 * on Linux); no allocations after prepare(). */
class SamplerWorker : public juce::Thread
{
public:
    explicit SamplerWorker (const String& threadName) : juce::Thread (threadName) {}

    void prepare (int channels, int maxSamples)
    {
        workerScratch.setSize (channels, maxSamples, false, true, true);
    }

    void postJob (juce::SynthesiserVoice** vs, int s, int e, int subNum, int channels)
    {
        voices         = vs;
        jobStart       = s;
        jobEnd         = e;
        renderSubNum   = subNum;
        /* Clear ALL channels — voices write to Main + any of the
         * kNumBuses aux channels per their slot's busAssign.  Only
         * clearing 0/1 left bus channels dirty across jobs. */
        const int n = workerScratch.getNumChannels();
        for (int c = 0; c < n && c < channels; ++c)
            workerScratch.clear (c, 0, subNum);
        workReady.signal();
    }

    void waitForJobCompletion() { jobCompleted.wait (-1); }

    void requestStop()
    {
        signalThreadShouldExit();
        workReady.signal();
    }

    void run() override
    {
        /* Promote to SCHED_FIFO 70 — best-effort.  If the user lacks
         * RTPRIO capability the call just fails and the worker runs at
         * SCHED_OTHER.  Functionally still correct, just less RT-tight. */
        sched_param p{};
        p.sched_priority = 70;
        pthread_setschedparam (pthread_self(), SCHED_FIFO, &p);

        while (! threadShouldExit())
        {
            workReady.wait (-1);
            if (threadShouldExit()) break;

            for (int i = jobStart; i < jobEnd; ++i)
            {
                if (auto* v = voices[i])
                    v->renderNextBlock (workerScratch, 0, renderSubNum);
            }

            jobCompleted.signal();
        }
    }

    juce::AudioBuffer<float> workerScratch;

private:
    juce::WaitableEvent      workReady, jobCompleted;
    juce::SynthesiserVoice** voices       = nullptr;
    int                      jobStart     = 0;
    int                      jobEnd       = 0;
    int                      renderSubNum = 0;
};

class Ft2SamplerVoice;

class ChannelTrackingSynth : public Synthesiser
{
public:
    explicit ChannelTrackingSynth (SamplerNode& owner) : node (owner) {}
    ~ChannelTrackingSynth() override { stopWorkers(); }

    /** Mono-mode per-instrument tracking.  Held-notes vector holds keys
     *  in press order for last-note-priority + legato release.  active
     *  voice is the single sounding voice for this instrument; cleared
     *  by noteOff once the held set empties (the underlying Synthesiser
     *  noteOff fades the voice out and we stop tracking it). */
    struct MonoState
    {
        std::vector<int>    heldNotes;
        Ft2SamplerVoice*    activeVoice = nullptr;
    };

    /* Bodies defined out-of-line after Ft2SamplerVoice's class — they
     * need its complete type for dynamic_cast + member access. */
    void noteOn  (int midiChannel, int midiNoteNumber, float velocity) override;
    void noteOff (int midiChannel, int midiNoteNumber, float velocity,
                  bool allowTailOff) override;

    /** Clear mono state for a specific instrument — called from
     *  SamplerNode::removeInstrument so the raw-pointer key doesn't
     *  dangle.  Voice pointers in the cleared state are dropped to
     *  avoid use-after-free if a stale lookup ever fires. */
    void forgetInstrument (SamplerInstrument* inst) noexcept
    {
        monoStates.erase (inst);
    }

    /* JUCE's Synthesiser swallows program-change events without exposing
     * an override hook — we run our own MIDI scan in SamplerNode::processBlock
     * before renderNextBlock to handle them.  See SamplerNode::processBlock. */

    /* Lazy worker-pool init.  Called from SamplerNode::prepareToPlay.
     * 2 workers is the sweet spot — bigger pools hurt due to scratch-
     * summing overhead in the main thread (Amdahl); 1 worker is no win
     * over scalar.  GUI must stay fast (memory rule), so we cap. */
    void prepareWorkers (int numCh, int maxSamples)
    {
        constexpr int kNumWorkers = 2;
        if ((int) workers.size() == kNumWorkers)
        {
            for (auto& w : workers) w->prepare (numCh, maxSamples);
            return;
        }
        stopWorkers();
        workers.clear();
        for (int i = 0; i < kNumWorkers; ++i)
        {
            auto w = std::make_unique<SamplerWorker> ("SamplerWorker" + String (i));
            w->prepare (numCh, maxSamples);
            w->startThread();
            workers.push_back (std::move (w));
        }
    }

    void stopWorkers()
    {
        for (auto& w : workers) w->requestStop();
        for (auto& w : workers) w->stopThread (500);
        workers.clear();
    }

protected:
    /* Parallel renderVoices: dispatch voice range to N workers when
     * load justifies the dispatch overhead.  Below threshold (few
     * voices OR small sub-block), fall through to base Synthesiser
     * path verbatim — zero overhead for typical light usage. */
    void renderVoices (juce::AudioBuffer<float>& outputAudio,
                       int startSample, int numSamples) override
    {
        /* kMinVoices: dispatch cost ~1-5µs per worker; only worth it
         *             when per-worker payload dwarfs that overhead.
         * kMinSamples: dense MIDI fragments the sub-block; parallel
         *              overhead would dominate for tiny ranges. */
        constexpr int kMinVoices  = 16;
        constexpr int kMinSamples = 64;

        if (workers.empty() || numSamples < kMinSamples)
        {
            Synthesiser::renderVoices (outputAudio, startSample, numSamples);
            return;
        }

        const int totalVoices = getNumVoices();
        int activeCount = 0;
        for (int i = 0; i < totalVoices; ++i)
            if (auto* v = getVoice (i); v != nullptr && v->isVoiceActive())
                ++activeCount;

        if (activeCount < kMinVoices)
        {
            Synthesiser::renderVoices (outputAudio, startSample, numSamples);
            return;
        }

        /* Snapshot the voice pointer list.  voicePtrs is owned by this
         * synth (main audio thread); workers only read it during the
         * postJob → waitForJobCompletion window where this thread is
         * blocked, so no race. */
        voicePtrs.clearQuick();
        for (int i = 0; i < totalVoices; ++i)
            if (auto* v = getVoice (i))
                voicePtrs.add (v);

        const int N = voicePtrs.size();
        if (N == 0) return;

        const int  nWorkers = (int) workers.size();
        const int  per      = (N + nWorkers - 1) / nWorkers;
        const int  channels = outputAudio.getNumChannels();

        for (int w = 0; w < nWorkers; ++w)
        {
            const int s = w * per;
            const int e = juce::jmin ((w + 1) * per, N);
            if (s >= e) continue;
            workers[(size_t) w]->postJob (voicePtrs.getRawDataPointer(),
                                          s, e, numSamples, channels);
        }

        for (int w = 0; w < nWorkers; ++w)
            if (w * per < N)
                workers[(size_t) w]->waitForJobCompletion();

        /* Sum worker scratches into output across ALL channels —
         * voices write to Main (ch 0/1) and any of the aux bus pairs
         * per their slot's busAssign.  outputAudio is the same buffer
         * the scalar path would have written into; addFrom does +=,
         * matching scalar's semantics. */
        const int outCh = outputAudio.getNumChannels();
        for (auto& w : workers)
        {
            const int wCh = w->workerScratch.getNumChannels();
            const int n   = juce::jmin (outCh, wCh, channels);
            for (int c = 0; c < n; ++c)
                outputAudio.addFrom (c, startSample, w->workerScratch, c, 0, numSamples);
        }
    }

private:
    SamplerNode&                                 node;
    std::vector<std::unique_ptr<SamplerWorker>>  workers;
    juce::Array<juce::SynthesiserVoice*>         voicePtrs;

    /* Mono-mode per-instrument state.  Keyed by raw SamplerInstrument*;
     * forgetInstrument cleans up on removeInstrument so the key never
     * dangles.  Touched only from the audio thread (noteOn/noteOff fire
     * from Synthesiser::renderNextBlock). */
    std::unordered_map<SamplerInstrument*, MonoState> monoStates;
};


/* ===========================================================================
 * Ft2SamplerVoice — ft2 mixer + FT2 vol/pan envelopes + fadeout + autoVib.
 *
 * Tick model: every kSamplesPerEnvTick output samples we advance the
 * envelope state by one FT2 tick (50Hz nominal).  Within a render block
 * we process [up to] one tick at a time, splitting numSamples at tick
 * boundaries.  Envelope output multiplies into voice gain; fadeout
 * multiplies on top once keyOff is engaged; autoVib modulates voice.delta
 * before each tick segment is rendered.
 * ========================================================================*/
class Ft2SamplerVoice : public SynthesiserVoice
{
public:
    explicit Ft2SamplerVoice (SamplerNode& owner_) : owner (owner_) {}

    bool canPlaySound (SynthesiserSound* s) override
    {
        return dynamic_cast<Ft2SamplerSound*> (s) != nullptr;
    }

    int  getCurrentSamplePos() const noexcept
    {
        /* During pingpong's backward sweep, the ft2 mixer stores position
         * as the bit-inverse of (smpPtr - revBase) where revBase = base +
         * loopStart + loopEnd.  That gives smpPtr-base = loopStart + loopEnd
         * - 1 - position — the actual sample-array index the playhead
         * should render at.  Forward sweep stores position == (smpPtr -
         * base) directly, so the no-encoding case stays trivial. */
        if (voice.samplingBackwards)
        {
            const int32_t loopEnd = voice.loopStart + voice.loopLength;
            return voice.loopStart + loopEnd - 1 - voice.position;
        }
        return voice.position;
    }
    bool isPlayingActive() const noexcept     { return voice.active; }
    const SamplerSampleSlot* getCurrentSlot() const noexcept { return slotPtr; }

    void startNote (int midiNote, float velocity,
                    SynthesiserSound* sound, int /*pitchWheel*/) override
    {
        auto* s = dynamic_cast<Ft2SamplerSound*> (sound);
        if (s == nullptr) { clearCurrentNote(); return; }

        const int chan = juce::jlimit (1, 16, s->getNode().getLastNoteChannel());
        instrument = s->getNode().getInstrumentForChannel (chan);
        if (instrument == nullptr) { clearCurrentNote(); return; }

        const int slotIdx = instrument->slotForNote (midiNote);
        const auto* slot = instrument->getSlot (slotIdx);
        if (slot == nullptr || ! slot->isLoaded()) { clearCurrentNote(); return; }

        slotPtr      = slot;
        slotIdx_     = slotIdx;
        slotIsStereo = slot->isStereo && slot->data16R != nullptr;
        playedNote   = midiNote;
        velocityLin  = juce::jlimit (0.0f, 1.0f, velocity);

        const bool loopRequested = (slot->loopMode == SamplerLoopMode::kForward)
                                || (slot->loopMode == SamplerLoopMode::kPingpong);
        const bool loopActive = loopRequested
                               && slot->loopLength > 0
                               && slot->loopStart >= 0
                               && slot->loopStart < slot->numSamples;
        const bool loop     = loopActive && slot->loopMode == SamplerLoopMode::kForward;
        const bool pingpong = loopActive && slot->loopMode == SamplerLoopMode::kPingpong;

        voice = {};
        voice.base16     = slot->data16L.get();
        voice.sampleEnd  = slot->numSamples;
        if (loopActive)
        {
            voice.loopStart  = jlimit (0, slot->numSamples - 1, slot->loopStart);
            voice.loopLength = jlimit (1, slot->numSamples - voice.loopStart,
                                         slot->loopLength);
            voice.sampleEnd  = voice.loopStart + voice.loopLength;
            voice.loopType   = pingpong ? 2 : 1;

            /* Pingpong needs revBase16 set up — ft2-clone formula:
             * revBase16 = &base16[loopStart + loopEnd] where
             * loopEnd = loopStart + loopLength.  When sampling
             * backwards the mixer reads via revBase[-position], so
             * revBase16 must point past the loop end.  We never set
             * this before → null deref on the first backward pass. */
            if (pingpong)
            {
                voice.revBase16 = slot->data16L.get()
                                  + (voice.loopStart * 2) + voice.loopLength;
            }
        }
        voice.position   = 0;
        voice.positionFrac = 0;
        voice.active     = true;
        voice.mixFuncOffset = (uint8_t) owner.getMixFuncIndexForCurrentMode (loop, pingpong);

        /* Pitch: 12tet semitones + slot finetune + slot relativeNote.
         * currentSemis tracks the live (possibly mid-glide) semitone
         * offset; beginGlideTo updates targetSemis + glide step. */
        const double semis    = (double) (midiNote - slot->rootNote)
                              + (double) slot->relativeNote
                              + (double) slot->finetune / 128.0;
        currentSemis  = semis;
        targetSemis   = semis;
        glideStepSemisPerTick = 0.0;
        glideTicksRemaining   = 0;
        basePitchMul = std::pow (2.0, semis / 12.0);
        setVoiceDelta (basePitchMul);

        const float gain = velocityLin * slot->volume;
        const float pan  = jlimit (0.0f, 1.0f, slot->panning);
        voice.fVolume = gain;
        slotPan = pan;
        setVoiceGain (gain, pan);

        /* Reset FT2 envelope + fadeout + autoVib state.
         *
         * Pre-seed the envelope state to "we just arrived at points[0]"
         * instead of (pos=0, tick=0, value=0).  advanceEnvelope does
         * ++tick BEFORE the `tick == points[pos].x` check, so the
         * landing-on-point-0 case (typical: points[0].x == 0) could
         * never fire and value sat at 0 forever.  Combined with
         * applyEnvelopeToVoice multiplying fEnvVol(=0) into gain,
         * the voice played at full baseline gain for one tick then
         * snapped to zero — audible click on every note when
         * envelopes were enabled.
         *
         * Seeded state: tick = points[0].x, value = points[0].y * 256,
         * pos = 1 (next target), delta = lerp slope to points[1]
         * (or 0 if it's a single-point envelope). */
        keyOff = false;
        auto seedEnv = [] (const FT2Envelope& env, uint8_t& pos, uint16_t& tick,
                           int16_t& value, int16_t& delta)
        {
            pos = 0; tick = 0; value = 0; delta = 0;
            if (! (env.flags & FT2Envelope::kEnabled)) return;
            if (env.length == 0) return;

            tick  = (uint16_t) env.points[0].x;
            value = (int16_t) ((int8_t) env.points[0].y << 8);
            pos   = 1;
            if (pos < env.length)
            {
                const int16_t xDiff = (int16_t) (env.points[1].x - env.points[0].x);
                const int8_t  yDiff = (int8_t)  (env.points[1].y - env.points[0].y);
                if (xDiff > 0) delta = (int16_t) ((yDiff << 8) / xDiff);
            }
        };
        seedEnv (instrument->volumeEnv, volEnvPos, volEnvTick, volEnvValue, volEnvDelta);
        seedEnv (instrument->panEnv,    panEnvPos, panEnvTick, panEnvValue, panEnvDelta);
        fadeoutVol = 32768;
        autoVibPos = 0;  autoVibAmp = 0;  autoVibSweepCounter = 0;
        if (instrument != nullptr && instrument->autoVib.depth > 0)
        {
            if (instrument->autoVib.sweep > 0)
            {
                autoVibAmp = 0;
                autoVibSweepCounter = (instrument->autoVib.depth << 8) / instrument->autoVib.sweep;
            }
            else
            {
                autoVibAmp = (uint16_t) (instrument->autoVib.depth << 8);
                autoVibSweepCounter = 0;
            }
        }
        envSampleAccum = 0;

        /* Per-voice envelope tick rate.  Default 0 = "use owner's
         * absolute 50 Hz tick rate".  When the instrument opts into
         * envSampleRelative, scale so the longest enabled envelope's
         * last point lines up with sample end — short one-shots get
         * a fast envelope, long pads get a slow one, all from the
         * same envelope shape. */
        envSamplesPerTickOverride = 0;
        if (instrument != nullptr && instrument->envSampleRelative && slot->numSamples > 0)
        {
            int lastX = 0;
            auto take = [&] (const FT2Envelope& e) {
                if ((e.flags & FT2Envelope::kEnabled) && e.length > 0)
                    lastX = juce::jmax<int> (lastX, e.points[e.length - 1].x);
            };
            take (instrument->volumeEnv);
            take (instrument->panEnv);
            if (lastX > 0)
                envSamplesPerTickOverride = juce::jmax (1, slot->numSamples / lastX);
        }

        /* FT2 envelope path uses sample-position-driven envelopes; the
         * fallback ADSR is only used when both vol+pan envelopes are
         * disabled. */
        const auto a = owner.getAdsr();
        ADSR::Parameters p { a.attack, a.decay, a.sustain, a.release };
        adsr.setSampleRate (getSampleRate());
        adsr.setParameters (p);
        adsr.reset();
        adsr.noteOn();

        /* Apply envelope multiplier to voice gain BEFORE the first
         * mixChunk so the initial samples already reflect
         * points[0].y — otherwise the first tick boundary inside
         * renderNextBlock would jump from baseline gain to env-scaled
         * gain, producing a step discontinuity on every note. */
        applyEnvelopeToVoice();
    }

    void stopNote (float /*velocity*/, bool allowTailOff) override
    {
        if (allowTailOff)
        {
            keyOff = true;
            adsr.noteOff();
            /* If no fadeout configured AND no env sustain, end voice when
             * sample / ADSR completes naturally. */
        }
        else
        {
            adsr.reset();
            voice.active = false;
            clearCurrentNote();
        }
    }

    /** MIDI pitch wheel.  JUCE passes the raw 14-bit value (0..16383,
     *  centre 8192).  Convert to a fractional-semitone pitch ratio
     *  (±kPitchBendSemis full range — hardcoded to 2 for now) and
     *  fold it into the per-voice pitchBendMul; applyEnvelopeToVoice
     *  multiplies it into voice.delta every envelope tick. */
    void pitchWheelMoved (int value) override
    {
        constexpr double kPitchBendSemis = 2.0;     /* ± range */
        const double norm  = (value - 8192) / 8192.0;       /* -1..+1 */
        const double semis = norm * kPitchBendSemis;
        pitchBendMul = std::pow (2.0, semis / 12.0);
    }

    /** MIDI CC.  We implement CC1 (mod wheel) → vibrato amount.  If
     *  the instrument has autoVib configured, mod wheel scales /
     *  augments its amplitude; if not, mod wheel synthesises a default
     *  sine vibrato at rate=8 so "wiggle the wheel, hear vibrato"
     *  works out of the box.  All other CCs are ignored for now. */
    void controllerMoved (int controllerNumber, int newValue) override
    {
        if (controllerNumber == 1)
            modWheelAmount = juce::jlimit (0.0f, 1.0f, newValue / 127.0f);
    }

    void renderNextBlock (AudioBuffer<float>& out,
                          int startSample, int numSamples) override
    {
        if (! voice.active && ! adsr.isActive()) return;
        if (out.getNumChannels() < 2)            return;
        if (numSamples <= 0)                     return;

        /* Re-validate the slot every block.  The slot's data16L
         * unique_ptr can be replaced from the UI thread (loadSampleToSlot
         * / clearSlot / FX-page cut/paste/crop) — if that happens while
         * a voice is playing the old slot, voice.base16 dangles.  Re-
         * fetch by index every block; if it changed or is no longer
         * loaded, end the voice gracefully instead of reading freed
         * memory. */
        if (instrument == nullptr) { voice.active = false; clearCurrentNote(); return; }
        const auto* curSlot = instrument->getSlot (slotIdx_);
        if (curSlot == nullptr || ! curSlot->isLoaded() || curSlot != slotPtr)
        {
            voice.active = false;
            clearCurrentNote();
            return;
        }
        /* Slot pointer matches our cached one — but data16L's underlying
         * unique_ptr may have been swapped out in place (resizing edits
         * keep the SamplerSampleSlot object alive while replacing its
         * buffers).  Re-pull the base pointer from the slot every
         * block so we always read from the live buffer. */
        voice.base16 = curSlot->data16L.get();
        if (voice.base16 == nullptr) { voice.active = false; clearCurrentNote(); return; }
        slotIsStereo = curSlot->isStereo && curSlot->data16R != nullptr;

        /* Use the per-voice override (sample-relative envelope) when
         * set in startNote; otherwise fall back to the owner's
         * absolute 50 Hz tick rate. */
        const int samplesPerTick = envSamplesPerTickOverride > 0
                                    ? envSamplesPerTickOverride
                                    : owner.getSamplesPerEnvTick();

        AudioBuffer<float> scratch (2, numSamples);
        scratch.clear();

        /* Walk the block in tick-aligned chunks.  Each chunk: tick
         * envelope state once (if a tick boundary fell inside), then
         * mix sample. */
        int pos = 0;
        int safetyIters = 0;
        const int safetyCap = numSamples * 8 + 16;  /* worst-case bound */
        while (pos < numSamples && voice.active)
        {
            if (++safetyIters > safetyCap) break;   /* never spin forever */

            /* If the accumulator already crossed a tick boundary (e.g.,
             * carry from a previous block), fire the tick immediately
             * BEFORE asking for a mixChunk — chunk would be ≤ 0 here
             * and the old code spun on ++envSampleAccum. */
            if (envSampleAccum >= samplesPerTick)
            {
                envSampleAccum -= samplesPerTick;
                tickEnvelopes();
                advanceGlide();
                applyEnvelopeToVoice();
                continue;
            }

            const int chunk = juce::jmin (numSamples - pos,
                                          samplesPerTick - envSampleAccum);
            if (chunk <= 0) break;                  /* defensive — shouldn't reach */

            mixChunk (scratch, pos, chunk);
            pos += chunk;
            envSampleAccum += chunk;
            if (envSampleAccum >= samplesPerTick)
            {
                envSampleAccum -= samplesPerTick;
                tickEnvelopes();
                advanceGlide();
                applyEnvelopeToVoice();
            }
        }

        /* Mix scratch → slot's assigned bus, scaled by per-bus master
         * gain.  Output layout:
         *   ch 0..1 : Bus 1   (first output = JUCE main bus)
         *   ch 2..3 : Bus 2
         *   ch 4..5 : Bus 3
         *   ch 6..7 : Bus 4
         * Slot's busIndex is set per-slot on the Bank page; the per-
         * bus master gains (4 sliders below the preview) modulate the
         * whole bus output. */
        const auto* sL = scratch.getReadPointer (0);
        const auto* sR = scratch.getReadPointer (1);

        const int   bIdx = juce::jlimit (0, SamplerNode::kNumBuses - 1, slotPtr->busIndex);
        const float gain = juce::jlimit (0.0f, 2.0f, owner.busGain[bIdx]);
        const int   outCh = out.getNumChannels();
        const int   chL   = 2 * bIdx;
        const int   chR   = chL + 1;
        if (chR >= outCh || gain <= 0.0f) return;

        float* busL = out.getWritePointer (chL) + startSample;
        float* busR = out.getWritePointer (chR) + startSample;

        const bool envEnabled = (instrument != nullptr)
                              && (instrument->volumeEnv.flags & FT2Envelope::kEnabled);

        if (envEnabled)
        {
            for (int i = 0; i < numSamples; ++i)
            {
                busL[i] += sL[i] * gain;
                busR[i] += sR[i] * gain;
            }
        }
        else
        {
            for (int i = 0; i < numSamples; ++i)
            {
                const float env = adsr.getNextSample();
                busL[i] += sL[i] * gain * env;
                busR[i] += sR[i] * gain * env;
            }
        }

        /* End-of-voice gate: FT2 path ends when fadeoutVol falls to 0
         * (post-keyOff) OR when sample ends.  Non-env path: ADSR done. */
        if (envEnabled)
        {
            if (! voice.active || (keyOff && fadeoutVol == 0
                                   && instrument->fadeoutRate > 0))
            {
                voice.active = false;
                clearCurrentNote();
            }
        }
        else
        {
            if (! voice.active || ! adsr.isActive())
            {
                voice.active = false;
                clearCurrentNote();
            }
        }
    }

private:
    /* === per-tick chunk mixer.  No envelope arithmetic here — only the
     *     ft2 voice mixer kernels. ============================================ */
    void mixChunk (AudioBuffer<float>& scratch, int startInScratch, int count)
    {
        if (count <= 0 || slotPtr == nullptr) return;
        /* base16 is re-validated each renderNextBlock entry, but stay
         * defensive — never hand a null buffer to the ft2 mixer kernels
         * (which read base + position with no NULL guard of their own). */
        if (voice.base16 == nullptr) return;
        const uint32_t soff = (uint32_t) startInScratch;
        const uint32_t cnt  = (uint32_t) count;

        /* Per-call audio_t — was a global in upstream ft2-clone (DOS-era
         * single output).  Stack-local here so parallel voice-pool / graph
         * mixing can each carry their own scratch target without races. */
        audio_t mixCtx;
        mixCtx.fMixBufferL = scratch.getWritePointer (0);
        mixCtx.fMixBufferR = scratch.getWritePointer (1);
        mixCtx.fQuickVolRampSamplesMul = 0.0f;

        if (slotIsStereo)
        {
            voice_t snap = voice;
            voice.fCurrVolumeL   = gLL;
            voice.fCurrVolumeR   = gLR;
            voice.fTargetVolumeL = gLL;
            voice.fTargetVolumeR = gLR;
            voice.base16    = slotPtr->data16L.get();
            voice.revBase16 = voice.loopType == 2
                                ? slotPtr->data16L.get() + (voice.loopStart * 2) + voice.loopLength
                                : nullptr;
            mixFuncDispatch[voice.mixFuncOffset] (&voice, &mixCtx, soff, cnt);

            voice_t afterFirst = voice;

            voice = snap;
            voice.fCurrVolumeL   = gRL;
            voice.fCurrVolumeR   = gRR;
            voice.fTargetVolumeL = gRL;
            voice.fTargetVolumeR = gRR;
            voice.base16    = slotPtr->data16R.get();
            voice.revBase16 = voice.loopType == 2
                                ? slotPtr->data16R.get() + (voice.loopStart * 2) + voice.loopLength
                                : nullptr;
            mixFuncDispatch[voice.mixFuncOffset] (&voice, &mixCtx, soff, cnt);

            voice.active = voice.active && afterFirst.active;
        }
        else
        {
            mixFuncDispatch[voice.mixFuncOffset] (&voice, &mixCtx, soff, cnt);
        }
    }

    /* === FT2 envelope tick (port of updateVolPanAutoVib). =================== */
    void tickEnvelopes()
    {
        if (instrument == nullptr) return;

        /* fadeout on keyOff */
        if (keyOff && instrument->fadeoutRate > 0)
        {
            if (instrument->fadeoutRate > fadeoutVol) fadeoutVol = 0;
            else                                      fadeoutVol -= instrument->fadeoutRate;
        }

        advanceEnvelope (instrument->volumeEnv, volEnvPos, volEnvTick,
                         volEnvValue, volEnvDelta);
        advanceEnvelope (instrument->panEnv,    panEnvPos, panEnvTick,
                         panEnvValue, panEnvDelta);

        /* autoVibrato sweep ramp-in */
        if (instrument->autoVib.depth > 0 && autoVibSweepCounter > 0 && ! keyOff)
        {
            uint32_t amp = (uint32_t) autoVibAmp + autoVibSweepCounter;
            const uint32_t cap = (uint32_t) instrument->autoVib.depth << 8;
            if (amp >= cap) { autoVibAmp = (uint16_t) cap; autoVibSweepCounter = 0; }
            else            { autoVibAmp = (uint16_t) amp; }
        }
        /* Mod wheel synthesises a default-rate vibrato when the
         * instrument has no autoVib rate configured — so wiggling the
         * wheel produces audible motion out of the box.  Otherwise
         * autoVib.rate drives the phase as before. */
        const uint8_t rate = (instrument->autoVib.rate > 0)
                              ? instrument->autoVib.rate
                              : (modWheelAmount > 0.001f ? (uint8_t) 8 : (uint8_t) 0);
        autoVibPos = (uint8_t) (autoVibPos + rate);
    }

    /* Advance an envelope state by one FT2 tick.  Mirrors the per-tick
     * branch of updateVolPanAutoVib (sustain hold + loop + linear segment
     * interpolation). */
    void advanceEnvelope (const FT2Envelope& env, uint8_t& pos, uint16_t& tick,
                          int16_t& value, int16_t& delta)
    {
        if (! (env.flags & FT2Envelope::kEnabled)) return;
        if (env.length == 0) return;

        ++tick;

        if (pos < env.length && tick == (uint16_t) env.points[pos].x)
        {
            value = (int16_t) ((int8_t) env.points[pos].y << 8);
            uint8_t newPos = (uint8_t) (pos + 1);

            if (env.flags & FT2Envelope::kLoop)
            {
                --newPos;
                if (newPos == env.loopEnd)
                {
                    const bool holdAtSustain = (env.flags & FT2Envelope::kSustain)
                                            && newPos == env.sustainPoint
                                            && ! keyOff;
                    if (! holdAtSustain)
                    {
                        newPos = env.loopStart;
                        tick   = (uint16_t) env.points[newPos].x;
                        value  = (int16_t) ((int8_t) env.points[newPos].y << 8);
                    }
                }
                ++newPos;
            }

            if (newPos < env.length)
            {
                bool interp = true;
                if ((env.flags & FT2Envelope::kSustain) && ! keyOff)
                {
                    if ((uint8_t) (newPos - 1) == env.sustainPoint)
                    {
                        --newPos;
                        delta = 0;
                        interp = false;
                    }
                }

                if (interp)
                {
                    pos = newPos;
                    const int16_t x0 = env.points[newPos - 1].x;
                    const int16_t x1 = env.points[newPos].x;
                    const int16_t xDiff = x1 - x0;
                    if (xDiff > 0)
                    {
                        const int16_t y0 = env.points[newPos - 1].y;
                        const int16_t y1 = env.points[newPos].y;
                        const int8_t yDiff = (int8_t) (y1 - y0);
                        delta = (int16_t) ((yDiff << 8) / xDiff);
                    }
                    else
                    {
                        delta = 0;
                    }
                }
            }
            else
            {
                delta = 0;
            }
            return;
        }

        /* Inter-point linear interpolation. */
        value = (int16_t) (value + delta);
        const uint8_t hi = (uint8_t) (value >> 8);
        if (hi > 64)
        {
            value = (hi <= 160) ? (int16_t) (64 * 256) : 0;
            delta = 0;
        }
    }

    /** Combine velocity / slot vol / FT2 vol-env / fadeout into voice
     *  gain.  Re-derive panning from FT2 pan-env.  Apply autoVib pitch
     *  modulation to voice.delta. */
    void applyEnvelopeToVoice()
    {
        if (slotPtr == nullptr || instrument == nullptr) return;

        const FT2Envelope& ve = instrument->volumeEnv;
        const FT2Envelope& pe = instrument->panEnv;

        float fEnvVol = 1.0f;
        if (ve.flags & FT2Envelope::kEnabled)
        {
            float v = (uint16_t) volEnvValue * (1.0f / (64.0f * 256.0f));
            if (v > 1.0f) v = 1.0f;
            if (v < 0.0f) v = 0.0f;
            fEnvVol = v;
        }
        const float fFadeout = fadeoutVol * (1.0f / 32768.0f);
        const float gain = velocityLin * slotPtr->volume * fEnvVol * fFadeout;

        float pan = slotPan;
        if (pe.flags & FT2Envelope::kEnabled)
        {
            /* pan env value -32..+32 (after centre subtract).  Combine
             * with base pan as additive offset 0..1. */
            float pv = (uint16_t) panEnvValue * (1.0f / (64.0f * 256.0f));
            if (pv > 1.0f) pv = 1.0f;
            if (pv < 0.0f) pv = 0.0f;
            const float delta = pv - 0.5f;
            const float panMul = 1.0f - std::abs (slotPan - 0.5f) * 2.0f;
            pan = juce::jlimit (0.0f, 1.0f, slotPan + delta * panMul);
        }

        /* Ramp gain change from current to new target over one
         * envelope tick — kills the periodic 50 Hz step-discontinuity
         * buzz that piecewise-constant per-tick gain produces.  See
         * setVoiceGain doc above. */
        setVoiceGain (gain, pan, /*useRamp=*/true);

        /* autoVib pitch modulation + mod-wheel synthesised vibrato.
         * Always call setVoiceDelta so basePitchMul / pitchBendMul
         * changes (glide, MIDI pitch wheel) reach the mixer even when
         * vibrato is off — previously the no-autoVib path left
         * voice.delta stuck at startNote's value. */
        double mult = 1.0;
        const bool useAutoVib = instrument->autoVib.depth > 0;
        const bool useModVib  = modWheelAmount > 0.001f;

        if (useAutoVib || useModVib)
        {
            int16_t vibVal;
            switch (instrument->autoVib.type)
            {
                case 1: vibVal = (autoVibPos > 127) ? 64 : -64; break;
                case 2: vibVal = (int16_t) ((((autoVibPos >> 1) + 64) & 127) - 64); break;
                case 3: vibVal = (int16_t) (((-(autoVibPos >> 1) + 64) & 127) - 64); break;
                default: vibVal = kAutoVibSineTab[autoVibPos];
            }
            /* Effective amp combines the swept autoVibAmp with a
             * mod-wheel contribution scaled to be musically usable at
             * full deflection (~50% of max autoVib range = ~50 cents
             * peak at the sine peak). */
            const uint32_t modAmp     = (uint32_t) (modWheelAmount * 8192.0f);
            const uint32_t effAmp     = juce::jmin (65535u,
                                                    (uint32_t) autoVibAmp + modAmp);
            const int32_t scaled = (vibVal * (int16_t) effAmp) >> (6 + 8);
            /* scaled is in "period" units; for our delta-driven voice
             * apply as a fractional semitone perturbation.  64 ~= ±1 semi
             * at full depth at the sine peak; scale to cents:
             * cents ≈ scaled * (100/64). */
            const double cents = scaled * (100.0 / 64.0);
            mult = std::pow (2.0, cents / 1200.0);
        }
        setVoiceDelta (basePitchMul * mult * pitchBendMul);
    }

    /* ChannelTrackingSynth.noteOn / .noteOff call the glide helpers + the
     * voice-status accessors below.  All other callers are inside this
     * class.  Friend the synth class to keep the helpers private (no
     * public surface change). */
    friend class ChannelTrackingSynth;

    /** Begin gliding pitch + velocity toward a new MIDI note over
     *  @p portamentoMs.  Does NOT reset envelopes / fadeout — the
     *  voice continues as one continuous note (mono+legato semantics).
     *  Glide is linear in the semitone domain so the rate is musically
     *  perceptible (constant cents/sec); basePitchMul is recomputed on
     *  each tick from currentSemis. */
    void beginGlideTo (int newMidiNote, float velocity, float portamentoMs)
    {
        if (slotPtr == nullptr) return;

        playedNote   = newMidiNote;
        velocityLin  = juce::jlimit (0.0f, 1.0f, velocity);

        const double newSemis = (double) (newMidiNote - slotPtr->rootNote)
                              + (double) slotPtr->relativeNote
                              + (double) slotPtr->finetune / 128.0;
        targetSemis = newSemis;

        if (portamentoMs <= 0.001f)
        {
            currentSemis          = newSemis;
            glideStepSemisPerTick = 0.0;
            glideTicksRemaining   = 0;
            basePitchMul          = std::pow (2.0, currentSemis / 12.0);
        }
        else
        {
            const double tickRate = (double) SamplerNode::kEnvTickRateHz;
            const double ticks    = (portamentoMs / 1000.0) * tickRate;
            glideTicksRemaining   = juce::jmax (1, (int) std::ceil (ticks));
            glideStepSemisPerTick = (targetSemis - currentSemis)
                                  / (double) glideTicksRemaining;
        }
    }

    /** Advance the mono-portamento glide by one envelope tick.  Called
     *  from renderNextBlock's per-tick loop, between tickEnvelopes and
     *  applyEnvelopeToVoice so the new basePitchMul flows into the
     *  mixer through applyEnvelopeToVoice's unconditional setVoiceDelta. */
    void advanceGlide()
    {
        if (glideTicksRemaining <= 0) return;
        --glideTicksRemaining;
        if (glideTicksRemaining == 0)
            currentSemis = targetSemis;
        else
            currentSemis += glideStepSemisPerTick;
        basePitchMul = std::pow (2.0, currentSemis / 12.0);
    }

    /** Read-only access for ChannelTrackingSynth's mono-mode lookup. */
    const SamplerInstrument*    getInstrumentPtr() const noexcept { return instrument.get(); }
    float                       getVelocityLin()   const noexcept { return velocityLin; }

    void setVoiceDelta (double pitchMul)
    {
        const double playRate = pitchMul * slotPtr->sourceSampleRate / getSampleRate();
        voice.delta = (uint64_t) jlimit (0.0, 1e18, playRate * 4294967296.0);
    }

    /** Update voice L/R gain.  When @p useRamp is true, set fCurrVolume
     *  alone and configure the ft2 mixer's per-sample ramp (fVolume*Delta
     *  + volumeRampLength) to glide toward the new fTargetVolume over one
     *  envelope tick worth of samples.  When false, snap the new gain
     *  instantly (clear ramp fields).
     *
     *  Why: the ft2 mixer's RENDER_*_SMP macros already increment
     *  fVolumeL/R by fVolume*Delta on every output sample (see
     *  src/engine/sampler/ft2_mix_macros.h).  By zeroing the ramp fields
     *  on every gain update we forced piecewise-constant gain — every
     *  FT2 envelope tick (50 Hz) became a hard step change, producing a
     *  periodic 50 Hz buzz / clicking train during held notes (worse
     *  during live envelope edits when the deltas are larger).
     *  Targeting one-tick rampLength makes the next tick's update land
     *  right as the previous ramp finishes → continuous gain envelope. */
    void setVoiceGain (float gain, float pan, bool useRamp = false)
    {
        voice.fVolume = gain;

        float newL, newR;
        if (slotIsStereo)
        {
            const float r = jmax (0.0f, 2.0f * (pan - 0.5f));
            const float l = jmax (0.0f, 2.0f * (0.5f - pan));
            gLL = gain * (1.0f - r);
            gRR = gain * (1.0f - l);
            gLR = gain * r;
            gRL = gain * l;
            newL = gLL;
            newR = gLR;

            /* Stereo path can't honour the per-sample ramp because
             * mixChunk overwrites voice.fCurrVolume{L,R} from the
             * per-pass cross-mix terms (gLL/gLR/gRL/gRR) on every
             * block.  Force instant set here; stereo smoothing would
             * need per-cross-mix persistent current values + ramped
             * targets — TODO. */
            useRamp = false;
        }
        else
        {
            newL = gain * (1.0f - pan);
            newR = gain *          pan;
        }

        voice.fTargetVolumeL = newL;
        voice.fTargetVolumeR = newR;

        if (useRamp)
        {
            const int rampSamples = juce::jmax (1, owner.getSamplesPerEnvTick());
            voice.volumeRampLength = (uint32_t) rampSamples;
            voice.fVolumeLDelta = (newL - voice.fCurrVolumeL) / (float) rampSamples;
            voice.fVolumeRDelta = (newR - voice.fCurrVolumeR) / (float) rampSamples;
        }
        else
        {
            voice.fCurrVolumeL = newL;
            voice.fCurrVolumeR = newR;
            voice.fVolumeLDelta = 0.0f;
            voice.fVolumeRDelta = 0.0f;
            voice.volumeRampLength = 0;
        }
    }

    SamplerNode& owner;
    voice_t voice {};
    ADSR adsr;
    SamplerInstrument::Ptr instrument;

    /* Static (per-note) state. */
    const SamplerSampleSlot* slotPtr = nullptr;
    int   slotIdx_    = -1;             /* re-validated each render block */
    bool slotIsStereo = false;
    int   playedNote = 60;
    float velocityLin = 1.0f;
    float slotPan = 0.5f;
    double basePitchMul = 1.0;

    /** Glide state for mono+portamento.  semitone-domain so the glide
     *  is musically linear (constant rate in semis/sec); basePitchMul
     *  is the live ratio recomputed from currentSemis on each step.
     *  When glideTicksRemaining == 0 the voice is at its target. */
    double currentSemis = 0.0;
    double targetSemis  = 0.0;
    double glideStepSemisPerTick = 0.0;
    int    glideTicksRemaining   = 0;

    /** MIDI controller state — JUCE calls pitchWheelMoved /
     *  controllerMoved per voice when the host dispatches channel
     *  events through Synthesiser.  Carried across notes so a long-
     *  held bend or sustained mod-wheel position persists between
     *  legato notes (we DON'T reset in startNote — channel-wide CCs
     *  should outlive individual notes). */
    double pitchBendMul   = 1.0;      /* multiplied into voice.delta */
    float  modWheelAmount = 0.0f;     /* 0..1 — drives extra vibrato */

    /* Stereo render-gain cache. */
    float gLL = 1.0f, gLR = 0.0f, gRL = 0.0f, gRR = 1.0f;

    /* FT2 envelope state. */
    bool      keyOff = false;
    uint8_t   volEnvPos = 0;
    uint16_t  volEnvTick = 0;
    int16_t   volEnvValue = 0;
    int16_t   volEnvDelta = 0;
    uint8_t   panEnvPos = 0;
    uint16_t  panEnvTick = 0;
    int16_t   panEnvValue = 0;
    int16_t   panEnvDelta = 0;
    uint16_t  fadeoutVol = 32768;
    uint8_t   autoVibPos = 0;
    uint16_t  autoVibAmp = 0;
    uint16_t  autoVibSweepCounter = 0;

    /* Tick-quantizer: samples since last envelope tick. */
    int       envSampleAccum = 0;
    /* 0 = use SamplerNode::getSamplesPerEnvTick() (50 Hz absolute).
     * >0 = per-voice override computed in startNote when the
     * instrument has envSampleRelative on. */
    int       envSamplesPerTickOverride = 0;
};


/* ===========================================================================
 * ChannelTrackingSynth out-of-line method bodies.  Defined here so the
 * Ft2SamplerVoice methods they invoke (beginGlideTo, getInstrumentPtr,
 * getVelocityLin) see the complete type.
 * ========================================================================*/

void ChannelTrackingSynth::noteOn (int midiChannel, int midiNoteNumber, float velocity)
{
    node.setLastNoteChannel (midiChannel);

    const auto instrument = node.getInstrumentForChannel (midiChannel);
    const bool monoMode   = (instrument != nullptr) && instrument->mono;

    if (monoMode)
    {
        auto& state = monoStates[instrument.get()];

        /* Last-note priority: remove dups, append new key. */
        state.heldNotes.erase (std::remove (state.heldNotes.begin(),
                                            state.heldNotes.end(),
                                            midiNoteNumber),
                               state.heldNotes.end());
        state.heldNotes.push_back (midiNoteNumber);

        /* Re-trigger on a held voice: retune in place (glide), keep
         * envelope / fadeout / autoVib state — mono+legato semantics. */
        if (state.activeVoice != nullptr
            && state.activeVoice->isVoiceActive()
            && state.activeVoice->getInstrumentPtr() == instrument.get())
        {
            state.activeVoice->beginGlideTo (midiNoteNumber,
                                             velocity,
                                             instrument->portamentoTimeMs);
            return;
        }

        /* No live voice — standard noteOn allocates one, then capture it. */
        Synthesiser::noteOn (midiChannel, midiNoteNumber, velocity);

        for (int i = 0; i < getNumVoices(); ++i)
        {
            auto* v = dynamic_cast<Ft2SamplerVoice*> (getVoice (i));
            if (v == nullptr) continue;
            if (! v->isVoiceActive()) continue;
            if (v->getCurrentlyPlayingNote() != midiNoteNumber) continue;
            if (v->getInstrumentPtr() != instrument.get())      continue;
            state.activeVoice = v;
            break;
        }
        return;
    }

    Synthesiser::noteOn (midiChannel, midiNoteNumber, velocity);
}

void ChannelTrackingSynth::noteOff (int midiChannel, int midiNoteNumber,
                                    float velocity, bool allowTailOff)
{
    const auto instrument = node.getInstrumentForChannel (midiChannel);
    const bool monoMode   = (instrument != nullptr) && instrument->mono;

    if (monoMode)
    {
        auto it = monoStates.find (instrument.get());
        if (it != monoStates.end())
        {
            auto& state = it->second;
            state.heldNotes.erase (std::remove (state.heldNotes.begin(),
                                                state.heldNotes.end(),
                                                midiNoteNumber),
                                   state.heldNotes.end());

            /* Legato release: glide back to most-recent held key, no
             * release-envelope, voice keeps sounding. */
            if (! state.heldNotes.empty()
                && state.activeVoice != nullptr
                && state.activeVoice->isVoiceActive())
            {
                state.activeVoice->beginGlideTo (state.heldNotes.back(),
                                                 state.activeVoice->getVelocityLin(),
                                                 instrument->portamentoTimeMs);
                return;
            }

            /* No keys held — stop the active voice DIRECTLY.  We can't
             * route through Synthesiser::noteOff here: that helper
             * iterates voices matching on getCurrentlyPlayingNote(),
             * but our glide updates the voice's pitch + our internal
             * playedNote without touching JUCE's currentlyPlayingNote
             * (which stays at the note that started the voice via
             * Synthesiser::noteOn).  So if the user pressed C then E
             * and releases E, JUCE looks for a voice playing E, finds
             * none (the live voice's JUCE-side note is still C), and
             * the voice runs forever.  We hold the activeVoice ptr
             * already from noteOn — call stopNote on it directly.
             *
             * Trade-off: bypasses Synthesiser's sustain-pedal /
             * sostenuto bookkeeping for this voice.  Acceptable for
             * mono lead/bass usage; revisit if sustain-pedal+mono
             * interaction becomes important. */
            if (state.activeVoice != nullptr && state.activeVoice->isVoiceActive())
            {
                state.activeVoice->stopNote (velocity, allowTailOff);
                state.activeVoice = nullptr;
                return;
            }
            state.activeVoice = nullptr;
        }
    }

    Synthesiser::noteOff (midiChannel, midiNoteNumber, velocity, allowTailOff);
}


/* ===========================================================================
 * Helpers — note names.
 * ========================================================================*/

static String midiNoteName (int n)
{
    if (n < 0 || n > 127) return "--";
    static const char* names[12] = {
        "C-","C#","D-","D#","E-","F-","F#","G-","G#","A-","A#","B-"
    };
    return String (names[n % 12]) + String ((n / 12) - 1);
}

/* ===========================================================================
 * SampleWaveformView — waveform + draggable loop markers + live playheads.
 * ========================================================================*/
class SampleWaveformView : public Component,
                           private Timer,
                           private juce::ScrollBar::Listener
{
public:
    using PlayheadQuery = std::function<std::vector<int> (const SamplerSampleSlot*)>;

    SampleWaveformView (std::function<SamplerSampleSlot*()> getSlot_,
                        PlayheadQuery playheadsFor_)
        : getSlot     (std::move (getSlot_)),
          playheadsFor (std::move (playheadsFor_)),
          hScroll_     (false)
    {
        hScroll_.addListener (this);
        hScroll_.setAutoHide (false);
        addAndMakeVisible (hScroll_);
        startTimerHz (30);
    }

    ~SampleWaveformView() override
    {
        hScroll_.removeListener (this);
        stopTimer();
    }

    void resized() override
    {
        auto r = getLocalBounds();
        hScroll_.setBounds (r.removeFromBottom (kScrollH));
    }

    void paint (Graphics& g) override
    {
        auto fullBounds = getLocalBounds().toFloat();
        fullBounds.removeFromBottom ((float) kScrollH);
        const auto bounds = fullBounds.reduced (2.0f);
        g.setColour (Colour { 0xff'10'10'10 });
        g.fillRect (bounds);
        g.setColour (Colour { 0xff'2a'2a'2a });
        g.drawRect (bounds, 1.0f);

        auto* slot = getSlot ? getSlot() : nullptr;
        if (slot == nullptr || ! slot->isLoaded())
        {
            g.setColour (Colour { 0xff'5a'5a'5a });
            g.setFont (FontOptions (Font::getDefaultMonospacedFontName(), 11.0f, Font::plain));
            g.drawText ("(load a sample to set loop points)",
                        bounds.toFloat(), Justification::centred);
            lastSeenN_ = 0;
            return;
        }

        const int   w = juce::roundToInt (bounds.getWidth());
        const int   n = slot->numSamples;
        const float h = bounds.getHeight();
        const float midY = bounds.getCentreY();
        const float halfH = h * 0.45f;
        const auto* d = slot->data16L.get();
        if (n <= 0 || w <= 0 || d == nullptr) return;

        /* (Re-)init the viewport to full sample when the slot changes
         * (different numSamples). */
        if (lastSeenN_ != n || viewEnd_ <= viewStart_ || viewEnd_ > n)
        {
            viewStart_ = 0;
            viewEnd_   = n;
            lastSeenN_ = n;
            syncScrollBar();
        }
        const int vRange = viewEnd_ - viewStart_;

        /* Two render modes:
         *   - Envelope mode (vRange > 4*w): per-pixel min/max envelope.
         *   - Sample-line mode (vRange <= 4*w, moderate-to-deep zoom):
         *     connect each sample at its exact subpixel position with
         *     a line plot, dotting each sample once spacing > 4 px.
         *
         * Threshold is 4× pixel count rather than 1× — at moderate
         * zooms with smooth content the envelope's per-pixel scan
         * produced visible moiré against the pixel grid.  Line mode
         * handles 4 samples/pixel cleanly via path stroking. */
        g.setColour (Colour { 0xff'5a'a5'd0 });
        if (vRange > 4 * w)
        {
            for (int x = 0; x < w; ++x)
            {
                const int64_t s0 = viewStart_ + (int64_t) x * vRange / w;
                const int64_t s1 = juce::jmax (s0 + 1,
                                                (int64_t) viewStart_ + (int64_t)(x + 1) * vRange / w);
                int mn = INT16_MAX, mx = INT16_MIN;
                for (int64_t i = s0; i < s1 && i < n; ++i)
                {
                    const int v = d[i];
                    if (v < mn) mn = v;
                    if (v > mx) mx = v;
                }
                const float yMin = midY - (mn / 32768.0f) * halfH;
                const float yMax = midY - (mx / 32768.0f) * halfH;
                g.drawLine (bounds.getX() + x, yMax, bounds.getX() + x, yMin);
            }
        }
        else
        {
            const double pxPerSample = (double) w / (double) juce::jmax (1, vRange);
            Path linePath;
            bool started = false;
            for (int64_t s = viewStart_; s <= viewEnd_ && s < n; ++s)
            {
                const float xpx = bounds.getX() + (float) ((s - viewStart_) * pxPerSample);
                const float ypx = midY - (d[s] / 32768.0f) * halfH;
                if (! started) { linePath.startNewSubPath (xpx, ypx); started = true; }
                else            linePath.lineTo (xpx, ypx);
            }
            g.strokePath (linePath, PathStrokeType (1.2f));

            if (pxPerSample >= 4.0)
            {
                for (int64_t s = viewStart_; s <= viewEnd_ && s < n; ++s)
                {
                    const float xpx = bounds.getX() + (float) ((s - viewStart_) * pxPerSample);
                    const float ypx = midY - (d[s] / 32768.0f) * halfH;
                    g.fillEllipse (xpx - 1.5f, ypx - 1.5f, 3.0f, 3.0f);
                }
            }
        }

        if (slot->loopMode != SamplerLoopMode::kNone && slot->loopLength > 0)
        {
            const float x0 = bounds.getX() + sampleToPixelF (slot->loopStart, w);
            const float x1 = bounds.getX() + sampleToPixelF (slot->loopStart + slot->loopLength, w);
            g.setColour (Colour { 0x33'ff'a0'40 });
            g.fillRect (x0, bounds.getY(), x1 - x0, bounds.getHeight());
            g.setColour (Colour { 0xff'ff'a0'40 });
            g.fillRect (x0 - 1.0f, bounds.getY(), 2.0f, bounds.getHeight());
            g.fillRect (x1 - 1.0f, bounds.getY(), 2.0f, bounds.getHeight());
            g.fillEllipse (x0 - 4.0f, bounds.getY() - 0.0f, 8.0f, 8.0f);
            g.fillEllipse (x1 - 4.0f, bounds.getBottom() - 8.0f, 8.0f, 8.0f);
        }

        if (playheadsFor)
        {
            const auto positions = playheadsFor (slot);
            g.setColour (Colour { 0xff'40'ff'80 });
            for (int pos : positions)
            {
                if (pos < viewStart_ || pos >= viewEnd_) continue;
                const float x = bounds.getX() + sampleToPixelF (pos, w);
                g.fillRect (x - 0.5f, bounds.getY(), 1.5f, bounds.getHeight());
            }
        }

        /* Tiny zoom indicator (e.g. "1.0x" / "12.3x") top-right. */
        if (vRange > 0 && n > 0)
        {
            const float zoomX = (float) n / (float) vRange;
            g.setColour (Colour { 0xff'7a'7a'7a });
            g.setFont (FontOptions (Font::getDefaultMonospacedFontName(), 10.0f, Font::plain));
            const String label = (zoomX < 9.99f)
                                 ? String::formatted ("%.2fx", zoomX)
                                 : String::formatted ("%.1fx", zoomX);
            g.drawText (label,
                        Rectangle<float> (bounds.getRight() - 48, bounds.getY() + 2,
                                          44, 12),
                        Justification::right);
        }
    }

    void mouseDown (const MouseEvent& e) override
    {
        auto* slot = getSlot ? getSlot() : nullptr;
        if (slot == nullptr || ! slot->isLoaded()) return;
        if (slot->loopMode == SamplerLoopMode::kNone || slot->loopLength <= 0)
        {
            slot->loopMode   = SamplerLoopMode::kForward;
            slot->loopStart  = pixelToSample (e.x);
            slot->loopLength = juce::jmax (1, slot->numSamples - slot->loopStart);
            draggingMarker   = 1;
            repaint();
            return;
        }
        const int x0 = sampleToPixel (slot->loopStart);
        const int x1 = sampleToPixel (slot->loopStart + slot->loopLength);
        draggingMarker = (std::abs (e.x - x0) < std::abs (e.x - x1)) ? 0 : 1;
    }

    void mouseDrag (const MouseEvent& e) override
    {
        if (draggingMarker < 0) return;
        auto* slot = getSlot ? getSlot() : nullptr;
        if (slot == nullptr || ! slot->isLoaded()) return;

        const int newPos = pixelToSample (e.x);
        if (draggingMarker == 0)
        {
            const int oldEnd = slot->loopStart + slot->loopLength;
            slot->loopStart  = juce::jlimit (0, oldEnd - 1, newPos);
            slot->loopLength = oldEnd - slot->loopStart;
        }
        else
        {
            slot->loopLength = juce::jlimit (1, slot->numSamples - slot->loopStart,
                                              newPos - slot->loopStart);
        }
        repaint();
    }

    void mouseUp (const MouseEvent&) override { draggingMarker = -1; }

    /** Trackpad pinch — primary zoom gesture, anchored at the cursor. */
    void mouseMagnify (const MouseEvent& e, float scaleFactor) override
    {
        applyZoom (e.x, (double) scaleFactor);
    }

    /** Trackpad 2-finger scroll.  deltaX → pan, deltaY → zoom (fallback
     *  for users without a pinch-capable trackpad). */
    void mouseWheelMove (const MouseEvent& e, const MouseWheelDetails& w) override
    {
        auto* slot = getSlot ? getSlot() : nullptr;
        if (slot == nullptr || ! slot->isLoaded()) return;

        if (std::abs (w.deltaX) > std::abs (w.deltaY))
        {
            const int range = viewEnd_ - viewStart_;
            const int shift = (int) std::lround (range * -w.deltaX * 0.5);
            const int n = slot->numSamples;
            int newStart = juce::jlimit (0, juce::jmax (0, n - range),
                                          viewStart_ + shift);
            viewStart_ = newStart;
            viewEnd_   = newStart + range;
            syncScrollBar();
            repaint();
        }
        else if (std::abs (w.deltaY) > 0.001f)
        {
            const double factor = (w.deltaY > 0) ? (1.0 / 1.20) : 1.20;
            applyZoom (e.x, factor);
        }
    }

    void mouseDoubleClick (const MouseEvent&) override
    {
        auto* slot = getSlot ? getSlot() : nullptr;
        if (slot != nullptr && slot->isLoaded())
        {
            viewStart_ = 0;
            viewEnd_   = slot->numSamples;
            syncScrollBar();
            repaint();
        }
    }

private:
    void applyZoom (int anchorPx, double factor)
    {
        auto* slot = getSlot ? getSlot() : nullptr;
        if (slot == nullptr || ! slot->isLoaded()) return;
        const int n = slot->numSamples;
        if (viewEnd_ <= viewStart_) { viewStart_ = 0; viewEnd_ = n; }
        const int anchorSamp = pixelToSample (anchorPx);
        int newRange = juce::jmax (4, (int) std::lround ((viewEnd_ - viewStart_) / factor));
        newRange = juce::jmin (n, newRange);
        const double tRel = double (anchorSamp - viewStart_)
                          / juce::jmax (1, viewEnd_ - viewStart_);
        int newStart = (int) std::lround (anchorSamp - tRel * newRange);
        newStart = juce::jlimit (0, juce::jmax (0, n - newRange), newStart);
        viewStart_ = newStart;
        viewEnd_   = newStart + newRange;
        syncScrollBar();
        repaint();
    }
public:

private:
    void timerCallback() override
    {
        if (! isShowing()) return;     /* free CPU when off-screen */
        auto* slot = getSlot ? getSlot() : nullptr;
        std::vector<int> cur = slot != nullptr ? playheadsFor (slot) : std::vector<int>();
        if (cur == lastPlayheads_)
            return;
        lastPlayheads_ = std::move (cur);
        repaint();
    }

    int pixelToSample (int x) const
    {
        const auto bounds = getLocalBounds().reduced (2);
        const int w = bounds.getWidth();
        if (w <= 0) return viewStart_;
        const int range = juce::jmax (1, viewEnd_ - viewStart_);
        return (int) juce::jlimit ((int64_t) viewStart_, (int64_t) viewEnd_,
                                    (int64_t) viewStart_
                                       + (int64_t) (x - bounds.getX()) * range / w);
    }
    int sampleToPixel (int s) const
    {
        const auto bounds = getLocalBounds().reduced (2);
        if (viewEnd_ <= viewStart_) return bounds.getX();
        return bounds.getX() + (int) ((int64_t) (s - viewStart_) * bounds.getWidth()
                                         / juce::jmax (1, viewEnd_ - viewStart_));
    }
    float sampleToPixelF (int s, int w) const
    {
        if (viewEnd_ <= viewStart_ || w <= 0) return 0.0f;
        return (float) ((int64_t) (s - viewStart_) * w
                         / juce::jmax (1, viewEnd_ - viewStart_));
    }

    std::function<SamplerSampleSlot*()> getSlot;
    PlayheadQuery playheadsFor;
    std::vector<int> lastPlayheads_;
    int draggingMarker = -1;

    /* Viewport state — defaults reset whenever slot length changes. */
    int viewStart_ = 0, viewEnd_ = 0;
    int lastSeenN_ = 0;

    /* Horizontal scroll bar — fallback for trackpad/pinch users when
     * winelib doesn't forward XInput2 horizontal-scroll / magnify
     * events through to JUCE.  Visible regardless. */
    static constexpr int kScrollH = 12;
    juce::ScrollBar hScroll_;

    void syncScrollBar()
    {
        auto* slot = getSlot ? getSlot() : nullptr;
        const int n = (slot && slot->isLoaded()) ? slot->numSamples : 0;
        if (n <= 0) { hScroll_.setRangeLimits (0.0, 1.0); hScroll_.setCurrentRange (0.0, 1.0); return; }
        hScroll_.setRangeLimits (0.0, (double) n);
        hScroll_.setCurrentRange ((double) viewStart_,
                                   (double) (viewEnd_ - viewStart_),
                                   dontSendNotification);
    }

    void scrollBarMoved (juce::ScrollBar*, double newStart) override
    {
        auto* slot = getSlot ? getSlot() : nullptr;
        if (slot == nullptr || ! slot->isLoaded()) return;
        const int n = slot->numSamples;
        const int range = viewEnd_ - viewStart_;
        const int newS = juce::jlimit (0, juce::jmax (0, n - range), (int) newStart);
        viewStart_ = newS;
        viewEnd_   = newS + range;
        repaint();
    }
};


/* ===========================================================================
 * FT2EnvelopeView — multi-point envelope editor.  Up to 12 points,
 * draggable; sustain + loop markers drawn vertically.  Single envelope per
 * view; we instantiate two (volume + pan) in the editor.
 * ========================================================================*/
class FT2EnvelopeView : public Component
{
public:
    FT2EnvelopeView (FT2Envelope& env_, const String& title_,
                     std::function<void()> onChange_)
        : env (env_), title (title_), onChange (std::move (onChange_)) {}

    void paint (Graphics& g) override
    {
        const auto area = getLocalBounds().toFloat().reduced (4.0f);
        g.setColour (Colour { 0xff'12'12'12 });
        g.fillRect (area);
        g.setColour ((env.flags & FT2Envelope::kEnabled) ? Colour { 0xff'4a'7a'b5 }
                                                         : Colour { 0xff'30'30'30 });
        g.drawRect (area, 1.0f);

        /* Title + flag chips. */
        g.setColour (Colour { 0xff'9a'9a'9a });
        g.setFont (FontOptions (Font::getDefaultMonospacedFontName(), 10.0f, Font::plain));
        const String flagStr = String::formatted ("%s%s%s",
            (env.flags & FT2Envelope::kEnabled) ? "ON "  : "off ",
            (env.flags & FT2Envelope::kSustain) ? "SUS " : "",
            (env.flags & FT2Envelope::kLoop)    ? "LOOP" : "");
        g.drawText (title + "  " + flagStr, area.reduced (4.0f, 2.0f),
                    Justification::topLeft);

        if (env.length < 2) return;

        const float xMax = (float) env.points[env.length - 1].x;
        const float yMax = 64.0f;
        auto pxAt = [&] (float x) { return area.getX() + (x / xMax) * area.getWidth(); };
        auto pyAt = [&] (float y) { return area.getY() + (1.0f - y / yMax) * area.getHeight(); };

        /* Sustain marker (vertical line). */
        if ((env.flags & FT2Envelope::kSustain) && env.sustainPoint < env.length)
        {
            const float xs = pxAt ((float) env.points[env.sustainPoint].x);
            g.setColour (Colour { 0x88'ff'40'40 });
            g.fillRect (xs - 0.5f, area.getY(), 1.0f, area.getHeight());
        }
        /* Loop region shade. */
        if ((env.flags & FT2Envelope::kLoop)
            && env.loopStart < env.length && env.loopEnd < env.length
            && env.loopEnd >= env.loopStart)
        {
            const float xa = pxAt ((float) env.points[env.loopStart].x);
            const float xb = pxAt ((float) env.points[env.loopEnd].x);
            g.setColour (Colour { 0x33'40'a0'40 });
            g.fillRect (xa, area.getY(), xb - xa, area.getHeight());
        }

        /* Polyline + handles. */
        Path curve;
        curve.startNewSubPath (pxAt ((float) env.points[0].x), pyAt ((float) env.points[0].y));
        for (int i = 1; i < env.length; ++i)
            curve.lineTo (pxAt ((float) env.points[i].x), pyAt ((float) env.points[i].y));
        g.setColour ((env.flags & FT2Envelope::kEnabled) ? Colour { 0xff'5a'a5'd0 }
                                                         : Colour { 0xff'4a'4a'4a });
        g.strokePath (curve, PathStrokeType (2.0f));

        for (int i = 0; i < env.length; ++i)
        {
            const float x = pxAt ((float) env.points[i].x);
            const float y = pyAt ((float) env.points[i].y);
            g.setColour ((i == env.sustainPoint && (env.flags & FT2Envelope::kSustain))
                           ? Colour { 0xff'ff'40'40 }
                           : Colour { 0xff'ff'a0'40 });
            g.fillEllipse (x - 4.0f, y - 4.0f, 8.0f, 8.0f);
        }
    }

    void mouseDown (const MouseEvent& e) override
    {
        const auto area = getLocalBounds().toFloat().reduced (4.0f);
        draggingPoint = -1;
        if (env.length < 1) return;
        const float xMax = (float) env.points[env.length - 1].x;
        if (xMax <= 0) return;

        for (int i = 0; i < env.length; ++i)
        {
            const float x = area.getX() + ((float) env.points[i].x / xMax) * area.getWidth();
            const float y = area.getY() + (1.0f - (float) env.points[i].y / 64.0f) * area.getHeight();
            if (Point<float> (x, y).getDistanceFrom (e.position) < 10.0f)
            {
                draggingPoint = i;
                if (e.mods.isPopupMenu())
                {
                    showPointMenu (i);
                    return;
                }
                return;
            }
        }
        if (e.mods.isPopupMenu()) showCanvasMenu();
    }

    void mouseDrag (const MouseEvent& e) override
    {
        if (draggingPoint < 0 || draggingPoint >= env.length) return;
        const auto area = getLocalBounds().toFloat().reduced (4.0f);
        const float xMax = (float) jmax<int> (1, env.points[env.length - 1].x);

        const float xRel = jlimit (0.0f, 1.0f,
                                   (e.position.x - area.getX()) / area.getWidth());
        const float yRel = 1.0f - jlimit (0.0f, 1.0f,
                                   (e.position.y - area.getY()) / area.getHeight());
        const int newX = (int) (xRel * xMax);
        const int newY = (int) (yRel * 64.0f);

        /* Point 0 X-locked at 0.  Other points x must stay strictly
         * ascending. */
        if (draggingPoint == 0)
        {
            env.points[0].y = (int16_t) jlimit (0, 64, newY);
        }
        else
        {
            const int prevX = env.points[draggingPoint - 1].x;
            const int nextX = (draggingPoint + 1 < env.length)
                                ? env.points[draggingPoint + 1].x
                                : 324;
            env.points[draggingPoint].x = (int16_t) jlimit (prevX + 1, nextX - 1, newX);
            env.points[draggingPoint].y = (int16_t) jlimit (0, 64, newY);
        }

        repaint();
        if (onChange) onChange();
    }

    void mouseUp (const MouseEvent&) override { draggingPoint = -1; }

    void mouseDoubleClick (const MouseEvent& e) override
    {
        /* Add point between two neighbours, or replace y if on existing. */
        const auto area = getLocalBounds().toFloat().reduced (4.0f);
        if (env.length >= 12 || env.length < 1) return;
        const float xMax = (float) env.points[env.length - 1].x;
        const float xRel = jlimit (0.0f, 1.0f, (e.position.x - area.getX()) / area.getWidth());
        const float yRel = 1.0f - jlimit (0.0f, 1.0f, (e.position.y - area.getY()) / area.getHeight());
        const int newX = (int) (xRel * xMax);
        const int newY = (int) (yRel * 64.0f);

        int insertAt = 1;
        for (; insertAt < env.length; ++insertAt)
            if (env.points[insertAt].x > newX) break;

        for (int i = env.length; i > insertAt; --i)
            env.points[i] = env.points[i - 1];
        env.points[insertAt] = { (int16_t) newX, (int16_t) newY };
        env.length = (uint8_t) (env.length + 1);

        repaint();
        if (onChange) onChange();
    }

private:
    void showPointMenu (int pointIdx)
    {
        PopupMenu m;
        m.addItem (1, "Sustain point",
                   pointIdx > 0 && pointIdx < env.length,
                   (env.flags & FT2Envelope::kSustain) && env.sustainPoint == pointIdx);
        m.addItem (2, "Loop start",
                   pointIdx < env.length,
                   (env.flags & FT2Envelope::kLoop) && env.loopStart == pointIdx);
        m.addItem (3, "Loop end",
                   pointIdx < env.length && pointIdx >= env.loopStart,
                   (env.flags & FT2Envelope::kLoop) && env.loopEnd == pointIdx);
        m.addSeparator();
        m.addItem (4, "Remove point", env.length > 2 && pointIdx > 0);
        const int sel = m.showAt (this);
        switch (sel)
        {
            case 1:
                env.sustainPoint = (uint8_t) pointIdx;
                env.flags ^= FT2Envelope::kSustain;
                break;
            case 2:
                env.loopStart = (uint8_t) pointIdx;
                env.flags |= FT2Envelope::kLoop;
                break;
            case 3:
                env.loopEnd = (uint8_t) pointIdx;
                env.flags |= FT2Envelope::kLoop;
                break;
            case 4:
                for (int i = pointIdx; i + 1 < env.length; ++i)
                    env.points[i] = env.points[i + 1];
                env.length = (uint8_t) (env.length - 1);
                if (env.sustainPoint >= env.length) env.sustainPoint = (uint8_t) (env.length - 1);
                if (env.loopStart   >= env.length) env.loopStart   = (uint8_t) (env.length - 1);
                if (env.loopEnd     >= env.length) env.loopEnd     = (uint8_t) (env.length - 1);
                break;
            default: return;
        }
        repaint();
        if (onChange) onChange();
    }

    void showCanvasMenu()
    {
        PopupMenu m;
        m.addItem (1, "Enable envelope",  true, (env.flags & FT2Envelope::kEnabled));
        m.addItem (2, "Clear loop",       true, (env.flags & FT2Envelope::kLoop));
        m.addItem (3, "Clear sustain",    true, (env.flags & FT2Envelope::kSustain));
        const int sel = m.showAt (this);
        switch (sel)
        {
            case 1: env.flags ^= FT2Envelope::kEnabled; break;
            case 2: env.flags &= ~FT2Envelope::kLoop;   break;
            case 3: env.flags &= ~FT2Envelope::kSustain; break;
            default: return;
        }
        repaint();
        if (onChange) onChange();
    }

    FT2Envelope& env;
    String title;
    std::function<void()> onChange;
    int draggingPoint = -1;
};


/* ===========================================================================
 * SamplerKeymap — FT2-style key-to-slot map editor.  Shows 2 octaves of
 * piano keys at a time (24 keys), each tinted by its assigned slot
 * (0..15 → 16-step palette).  Click a key (or drag-paint a range) to
 * assign the currently active slot to that MIDI note range.
 *
 * Octave-shift buttons live in the parent InstPage; this component
 * just renders the visible window starting at `baseOctave_` and
 * dispatches mouse events to keymap mutations on the bound instrument.
 * ========================================================================*/
class SamplerKeymap : public Component
{
public:
    using GetInstrument = std::function<SamplerInstrument::Ptr()>;
    using GetActiveSlot = std::function<int()>;

    SamplerKeymap (GetInstrument getInst_, GetActiveSlot getSlot_)
        : getInst (std::move (getInst_)),
          getSlot (std::move (getSlot_)) {}

    void setBaseOctave (int oct)
    {
        baseOctave_ = juce::jlimit (-1, 8, oct);
        repaint();
    }
    int  getBaseOctave() const noexcept { return baseOctave_; }

    void paint (Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat().reduced (2.0f);
        g.setColour (Colour { 0xff'10'10'10 });
        g.fillRect (bounds);

        const auto inst = getInst ? getInst() : nullptr;
        const int  baseNote = (baseOctave_ + 1) * 12;     /* MIDI: C-1 = 0 */
        constexpr int kWhitePerOct = 7;
        const int totalWhite = kWhitePerOct * 2;          /* 14 whites in 2 oct */
        const float whiteW = bounds.getWidth() / (float) totalWhite;
        const float whiteH = bounds.getHeight();
        const float blackH = whiteH * 0.62f;
        const float blackW = whiteW * 0.6f;

        static const bool isWhite[12] = { true,false,true,false,true,true,false,
                                          true,false,true,false,true };
        static const int blackOffsetPC[12] = { -1, 0, -1, 1, -1, -1, 3, -1, 4, -1, 5, -1 };

        /* Natural piano-key colours.  Slot tint is laid on top as a
         * translucent overlay rather than blended into the base, so
         * the white/black distinction reads independently of the slot
         * mapping. */
        const Colour grayWhite  { 0xff'9a'9a'9a };   /* light-gray "white" */
        const Colour grayBlack  { 0xff'14'14'14 };   /* near-black */
        int whiteIdx = 0;
        for (int n = 0; n < 24; ++n)
        {
            const int note = baseNote + n;
            const int pc   = note % 12;
            if (! isWhite[pc]) continue;
            const float x  = bounds.getX() + whiteIdx * whiteW;
            const int assignedSlot = inst ? inst->slotForNote (note) : -1;
            const bool hasAssign   = inst && assignedSlot >= 0
                                  && inst->getSlot (assignedSlot)
                                  && inst->getSlot (assignedSlot)->isLoaded();

            /* Base: light-gray "white" key in its natural shade. */
            g.setColour (grayWhite);
            g.fillRect (x + 1.0f, bounds.getY(), whiteW - 1.0f, whiteH);

            /* Slot-color translucent overlay covers the entire key,
             * tinting without losing the gray underneath. */
            if (hasAssign)
            {
                g.setColour (slotPaletteColour (assignedSlot, 0.7f, 0.85f)
                               .withAlpha (0.35f));
                g.fillRect (x + 1.0f, bounds.getY(), whiteW - 1.0f, whiteH);

                /* Top stripe — fully opaque slot colour for the eye. */
                g.setColour (slotPaletteColour (assignedSlot, 0.75f, 0.9f));
                g.fillRect (x + 1.0f, bounds.getY(), whiteW - 1.0f, 6.0f);
            }

            g.setColour (Colour { 0xff'1a'1a'1a });
            g.drawRect (x, bounds.getY(), whiteW, whiteH, 1.0f);

            /* Slot # — big bold white centred. */
            if (hasAssign)
            {
                g.setColour (Colours::white);
                g.setFont (FontOptions (Font::getDefaultMonospacedFontName(),
                                        16.0f, Font::bold));
                g.drawText (String (assignedSlot + 1),
                            Rectangle<float> (x, bounds.getCentreY() - 8,
                                              whiteW, 24),
                            Justification::centred);
            }
            else
            {
                g.setColour (Colour { 0xff'40'40'40 });
                g.setFont (FontOptions (Font::getDefaultMonospacedFontName(),
                                        13.0f, Font::bold));
                g.drawText (String (CharPointer_UTF8 ("\xe2\x80\x94")),
                            Rectangle<float> (x, bounds.getCentreY() - 8,
                                              whiteW, 16),
                            Justification::centred);
            }
            ++whiteIdx;
        }

        /* Pass 2: blacks — darker gray base, slot tint when assigned,
         * bold white slot number centred. */
        for (int n = 0; n < 24; ++n)
        {
            const int note = baseNote + n;
            const int pc   = note % 12;
            if (isWhite[pc]) continue;
            const int bIdx = blackOffsetPC[pc];
            const int oct  = n / 12;
            const float xCentre = bounds.getX()
                                  + (oct * kWhitePerOct + bIdx + 1) * whiteW;
            const float x = xCentre - blackW * 0.5f;
            const int assignedSlot = inst ? inst->slotForNote (note) : -1;
            const bool hasAssign   = inst && assignedSlot >= 0
                                  && inst->getSlot (assignedSlot)
                                  && inst->getSlot (assignedSlot)->isLoaded();

            /* Base: near-black key. */
            g.setColour (grayBlack);
            g.fillRect (x, bounds.getY(), blackW, blackH);

            /* Translucent slot overlay — black-key contrast is harder
             * to read, so push opacity a bit higher than whites. */
            if (hasAssign)
            {
                g.setColour (slotPaletteColour (assignedSlot, 0.7f, 0.85f)
                               .withAlpha (0.45f));
                g.fillRect (x, bounds.getY(), blackW, blackH);

                /* Slim top stripe in full slot colour. */
                g.setColour (slotPaletteColour (assignedSlot, 0.75f, 0.9f));
                g.fillRect (x, bounds.getY(), blackW, 4.0f);
            }

            g.setColour (Colour { 0xff'08'08'08 });
            g.drawRect (x, bounds.getY(), blackW, blackH, 1.0f);

            if (hasAssign)
            {
                g.setColour (Colours::white);
                g.setFont (FontOptions (Font::getDefaultMonospacedFontName(),
                                        12.0f, Font::bold));
                g.drawText (String (assignedSlot + 1),
                            Rectangle<float> (x, bounds.getY() + blackH * 0.5f - 8,
                                              blackW, 16),
                            Justification::centred);
            }
        }

        /* Octave labels. */
        g.setColour (Colour { 0xff'70'70'70 });
        g.setFont (FontOptions (Font::getDefaultMonospacedFontName(),
                                10.0f, Font::bold));
        for (int oct = 0; oct < 2; ++oct)
        {
            const float x = bounds.getX() + oct * kWhitePerOct * whiteW + 2.0f;
            g.drawText (String ("C") + String (baseOctave_ + oct),
                        Rectangle<float> (x, bounds.getY(), 24, 14),
                        Justification::topLeft);
        }
    }

    void mouseDown (const MouseEvent& e) override
    {
        const int note = noteAt (e.x, e.y);
        if (note < 0) return;
        const auto inst = getInst ? getInst() : nullptr;
        if (inst == nullptr) return;

        if (e.mods.isPopupMenu())
        {
            /* Right-click → pick slot inline (no need to leave Inst page). */
            PopupMenu m;
            for (int i = 0; i < SamplerInstrument::kNumSlots; ++i)
            {
                const auto* slot = inst->getSlot (i);
                const String label = String::formatted ("%02d  %s", i + 1,
                    (slot && slot->isLoaded()) ? slot->name.toRawUTF8() : "(empty)");
                m.addItem (i + 1, label,
                           true,
                           inst->slotForNote (note) == i);
            }
            const int chosen = m.showAt (this);
            if (chosen >= 1 && chosen <= SamplerInstrument::kNumSlots)
                inst->setSlotForNote (note, chosen - 1);
            repaint();
            return;
        }

        const int slot  = getSlot ? getSlot() : -1;
        if (slot < 0 || slot >= SamplerInstrument::kNumSlots) return;
        inst->setSlotForNote (note, slot);
        dragAnchorNote_ = note;
        repaint();
    }

    void mouseWheelMove (const MouseEvent& e, const MouseWheelDetails& w) override
    {
        const int note = noteAt (e.x, e.y);
        if (note < 0) return;
        const auto inst = getInst ? getInst() : nullptr;
        if (inst == nullptr) return;
        const int cur = inst->slotForNote (note);
        int next = cur + (w.deltaY > 0 ? +1 : -1);
        if (next < 0)                                    next = SamplerInstrument::kNumSlots - 1;
        if (next >= SamplerInstrument::kNumSlots)        next = 0;
        inst->setSlotForNote (note, next);
        repaint();
    }
    void mouseDrag (const MouseEvent& e) override
    {
        const int note = noteAt (e.x, e.y);
        if (note < 0) return;
        if (dragAnchorNote_ < 0) return;
        const auto inst = getInst ? getInst() : nullptr;
        if (inst == nullptr) return;
        const int slot  = getSlot ? getSlot() : -1;
        if (slot < 0 || slot >= SamplerInstrument::kNumSlots) return;
        const int lo = juce::jmin (dragAnchorNote_, note);
        const int hi = juce::jmax (dragAnchorNote_, note);
        for (int n = lo; n <= hi; ++n)
            inst->setSlotForNote (n, slot);
        repaint();
    }
    void mouseUp (const MouseEvent&) override { dragAnchorNote_ = -1; }

private:
    int noteAt (int px, int py) const
    {
        const auto bounds = getLocalBounds().reduced (2);
        if (! bounds.contains (px, py)) return -1;
        const int baseNote = (baseOctave_ + 1) * 12;
        constexpr int kWhitePerOct = 7;
        const int totalWhite = kWhitePerOct * 2;
        const float whiteW = bounds.getWidth() / (float) totalWhite;
        const float whiteH = bounds.getHeight();
        const float blackH = whiteH * 0.6f;
        const float blackW = whiteW * 0.6f;

        /* Test blacks first (they overlap whites). */
        static const bool isWhite[12] = { true,false,true,false,true,true,false,
                                          true,false,true,false,true };
        static const int blackOffsetPC[12] = { -1, 0, -1, 1, -1, -1, 3, -1, 4, -1, 5, -1 };
        const float relY = py - bounds.getY();
        if (relY < blackH)
        {
            for (int n = 0; n < 24; ++n)
            {
                const int note = baseNote + n;
                const int pc   = note % 12;
                if (isWhite[pc]) continue;
                const int bIdx = blackOffsetPC[pc];
                const int oct  = n / 12;
                const float xCentre = bounds.getX()
                                      + (oct * kWhitePerOct + bIdx + 1) * whiteW;
                const float xL = xCentre - blackW * 0.5f;
                if (px >= xL && px <= xL + blackW) return note;
            }
        }
        /* Whites. */
        int whiteIdx = 0;
        for (int n = 0; n < 24; ++n)
        {
            const int note = baseNote + n;
            const int pc   = note % 12;
            if (! isWhite[pc]) continue;
            const float xL = bounds.getX() + whiteIdx * whiteW;
            if (px >= xL && px <= xL + whiteW) return note;
            ++whiteIdx;
        }
        return -1;
    }

    /** Shim — old in-class palette is now the shared free function. */
    static Colour slotColour (int slot) { return slotPaletteColour (slot); }

    GetInstrument getInst;
    GetActiveSlot getSlot;
    int baseOctave_   = 4;   /* shows C4..B5 by default */
    int dragAnchorNote_ = -1;
};


/* ===========================================================================
 * SamplerInstPage — the "Edit instrument" page of the paged Sampler
 * editor.  Shows two envelope editors (vol+pan) at larger size, sustain
 * + loop point controls per envelope, fadeout + auto-vib sliders, the
 * 2-octave keymap with octave-shift navigation, and predef-envelope
 * preset buttons.
 * ========================================================================*/
class SamplerInstPage : public Component
{
public:
    using GetInstrument = std::function<SamplerInstrument::Ptr()>;
    using GetActiveSlot = std::function<int()>;
    using SetActiveSlot = std::function<void (int)>;

    SamplerInstPage (GetInstrument gi, GetActiveSlot gs, SetActiveSlot ss)
        : getInst (std::move (gi)),
          getActiveSlot (gs),
          setActiveSlot (std::move (ss)),
          keymap (getInst, std::move (gs))
    {
        addAndMakeVisible (keymap);

        slotPickLbl.setText ("Paint slot:", dontSendNotification);
        slotPickLbl.setColour (Label::textColourId, Colour { 0xff'b0'b0'b0 });
        slotPickLbl.setFont (FontOptions (Font::getDefaultMonospacedFontName(),
                                          11.0f, Font::plain));
        addAndMakeVisible (slotPickLbl);

        slotPickValue.setJustificationType (Justification::centred);
        slotPickValue.setColour (Label::textColourId, Colour { 0xff'd0'80'40 });
        slotPickValue.setFont (FontOptions (Font::getDefaultMonospacedFontName(),
                                            13.0f, Font::bold));
        addAndMakeVisible (slotPickValue);

        configureNudge (slotDownBtn, "-", [this] {
            const int s = getActiveSlot();
            setActiveSlot (juce::jmax (0, s - 1));
            refresh();
        });
        configureNudge (slotUpBtn, "+", [this] {
            const int s = getActiveSlot();
            setActiveSlot (juce::jmin (SamplerInstrument::kNumSlots - 1, s + 1));
            refresh();
        });

        for (int p = 1; p <= 6; ++p)
        {
            auto btn = std::make_unique<TextButton> (String (p));
            const int preset = p;
            btn->onClick = [this, preset] { applyPredefinedEnvelope (preset); };
            btn->setColour (TextButton::buttonColourId, Colour { 0xff'24'24'24 });
            btn->setColour (TextButton::textColourOffId, Colour { 0xff'd0'd0'd0 });
            addAndMakeVisible (*btn);
            predefBtns.add (std::move (btn));
        }

        configureNudge (volAddBtn, "Add",  [this] { addEnvPoint (true);   });
        configureNudge (volDelBtn, "Del",  [this] { delLastEnvPoint (true); });
        configureNudge (panAddBtn, "Add",  [this] { addEnvPoint (false);  });
        configureNudge (panDelBtn, "Del",  [this] { delLastEnvPoint (false); });

        configureNudge (volSusUp,  "+", [this] { nudgeEnvPoint (true,  +1, false); });
        configureNudge (volSusDn,  "-", [this] { nudgeEnvPoint (true,  -1, false); });
        configureNudge (volLoopStartUp, "+", [this] { nudgeEnvLoop (true,  true,  +1); });
        configureNudge (volLoopStartDn, "-", [this] { nudgeEnvLoop (true,  true,  -1); });
        configureNudge (volLoopEndUp,   "+", [this] { nudgeEnvLoop (true,  false, +1); });
        configureNudge (volLoopEndDn,   "-", [this] { nudgeEnvLoop (true,  false, -1); });

        configureNudge (panSusUp,  "+", [this] { nudgeEnvPoint (false, +1, false); });
        configureNudge (panSusDn,  "-", [this] { nudgeEnvPoint (false, -1, false); });
        configureNudge (panLoopStartUp, "+", [this] { nudgeEnvLoop (false, true,  +1); });
        configureNudge (panLoopStartDn, "-", [this] { nudgeEnvLoop (false, true,  -1); });
        configureNudge (panLoopEndUp,   "+", [this] { nudgeEnvLoop (false, false, +1); });
        configureNudge (panLoopEndDn,   "-", [this] { nudgeEnvLoop (false, false, -1); });

        for (auto* l : { &volSusLbl, &volLoopLbl, &panSusLbl, &panLoopLbl,
                         &fadeLbl, &avSpeedLbl, &avDepthLbl, &avSweepLbl,
                         &predefLbl, &keymapLbl })
        {
            l->setColour (Label::textColourId, Colour { 0xff'b0'b0'b0 });
            l->setFont (FontOptions (Font::getDefaultMonospacedFontName(), 11.0f, Font::plain));
            addAndMakeVisible (*l);
        }
        volSusLbl   .setText ("Sust",    dontSendNotification);
        volLoopLbl  .setText ("Loop",    dontSendNotification);
        panSusLbl   .setText ("Sust",    dontSendNotification);
        panLoopLbl  .setText ("Loop",    dontSendNotification);
        fadeLbl     .setText ("Fadeout", dontSendNotification);
        avSpeedLbl  .setText ("Vib rate",dontSendNotification);
        avDepthLbl  .setText ("Vib depth", dontSendNotification);
        avSweepLbl  .setText ("Vib sweep", dontSendNotification);
        predefLbl   .setText ("Predef:", dontSendNotification);
        keymapLbl   .setText ("Keymap:", dontSendNotification);

        configureSlider (fadeoutSlider, 0.0, 4095.0, 1.0, [this] (double v) {
            if (auto inst = getInst()) inst->fadeoutRate = (uint16_t) v;
        });
        configureSlider (avRateSlider, 0.0, 63.0, 1.0, [this] (double v) {
            if (auto inst = getInst()) inst->autoVib.rate = (uint8_t) v;
        });
        configureSlider (avDepthSlider, 0.0, 15.0, 1.0, [this] (double v) {
            if (auto inst = getInst()) inst->autoVib.depth = (uint8_t) v;
        });
        configureSlider (avSweepSlider, 0.0, 255.0, 1.0, [this] (double v) {
            if (auto inst = getInst()) inst->autoVib.sweep = (uint8_t) v;
        });

        avTypeCombo.addItem ("Sine",    1);
        avTypeCombo.addItem ("Square",  2);
        avTypeCombo.addItem ("Ramp+",   3);
        avTypeCombo.addItem ("Ramp-",   4);
        avTypeCombo.setColour (ComboBox::backgroundColourId, Colour { 0xff'24'24'24 });
        avTypeCombo.setColour (ComboBox::textColourId,        Colour { 0xff'd0'd0'd0 });
        avTypeCombo.onChange = [this] {
            if (auto inst = getInst())
                inst->autoVib.type = (uint8_t) (avTypeCombo.getSelectedId() - 1);
        };
        addAndMakeVisible (avTypeCombo);

        configureFlag (volEnabledBtn,  "Env enabled", [this] (bool v) {
            toggleFlag (true, FT2Envelope::kEnabled, v);
        });
        configureFlag (volSustainBtn,  "Sustain",     [this] (bool v) {
            toggleFlag (true, FT2Envelope::kSustain, v);
        });
        configureFlag (volLoopBtn,     "Loop",        [this] (bool v) {
            toggleFlag (true, FT2Envelope::kLoop, v);
        });
        configureFlag (panEnabledBtn,  "Env enabled", [this] (bool v) {
            toggleFlag (false, FT2Envelope::kEnabled, v);
        });
        configureFlag (panSustainBtn,  "Sustain",     [this] (bool v) {
            toggleFlag (false, FT2Envelope::kSustain, v);
        });
        configureFlag (panLoopBtn,     "Loop",        [this] (bool v) {
            toggleFlag (false, FT2Envelope::kLoop, v);
        });

        configureNudge (octDownBtn, "<oct", [this] {
            keymap.setBaseOctave (keymap.getBaseOctave() - 1);
        });
        configureNudge (octUpBtn,   "oct>", [this] {
            keymap.setBaseOctave (keymap.getBaseOctave() + 1);
        });

        /* Mono toggle + portamento time.  Mono is a toggle on the Inst
         * (per-instrument); portamento glides pitch when retriggering a
         * held mono voice.  Engine: ChannelTrackingSynth + voice
         * beginGlideTo. */
        configureFlag (monoBtn, "Mono", [this] (bool v) {
            if (auto inst = getInst()) inst->mono = v;
        });

        portaLbl.setText ("Porta", dontSendNotification);
        portaLbl.setColour (Label::textColourId, Colour { 0xff'b0'b0'b0 });
        portaLbl.setFont (FontOptions (Font::getDefaultMonospacedFontName(), 11.0f, Font::plain));
        addAndMakeVisible (portaLbl);

        configureSlider (portamentoSlider, 0.0, 1000.0, 1.0, [this] (double v) {
            if (auto inst = getInst()) inst->portamentoTimeMs = (float) v;
        });
        portamentoSlider.setTextValueSuffix (" ms");

        /* "Env follows sample" — when on, envelope ticks scale at note-on
         * so the longest enabled envelope's last point lines up with
         * sample end (per voice). */
        configureFlag (envFollowBtn, "Env~Smp", [this] (bool v) {
            if (auto inst = getInst()) inst->envSampleRelative = v;
        });

        rebuildEnvViews();
    }

    /** Re-bind the envelope editors when the active instrument changes. */
    void rebuildEnvViews()
    {
        volEnvView.reset();
        panEnvView.reset();
        if (auto inst = getInst())
        {
            volEnvView.reset (new FT2EnvelopeView (inst->volumeEnv, "Volume",
                [this] { repaint(); }));
            panEnvView.reset (new FT2EnvelopeView (inst->panEnv, "Panning",
                [this] { repaint(); }));
            addAndMakeVisible (volEnvView.get());
            addAndMakeVisible (panEnvView.get());
        }
        refreshFlagStates();
        resized();
    }

    void refresh()
    {
        repaint();
        if (volEnvView) volEnvView->repaint();
        if (panEnvView) panEnvView->repaint();
        if (auto inst = getInst())
        {
            fadeoutSlider.setValue ((double) inst->fadeoutRate, dontSendNotification);
            avRateSlider .setValue ((double) inst->autoVib.rate,  dontSendNotification);
            avDepthSlider.setValue ((double) inst->autoVib.depth, dontSendNotification);
            avSweepSlider.setValue ((double) inst->autoVib.sweep, dontSendNotification);
            avTypeCombo  .setSelectedId ((int) inst->autoVib.type + 1, dontSendNotification);
            portamentoSlider.setValue ((double) inst->portamentoTimeMs, dontSendNotification);
            refreshFlagStates();
        }
        slotPickValue.setText (String::formatted ("%02d", getActiveSlot() + 1),
                               dontSendNotification);
        keymap.repaint();
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (8);

        /* Top strip: predef preset buttons + active-slot picker. */
        auto top = r.removeFromTop (24);
        predefLbl.setBounds (top.removeFromLeft (60));
        for (auto& b : predefBtns) {
            b->setBounds (top.removeFromLeft (28));
            top.removeFromLeft (2);
        }
        /* Slot picker right side. */
        slotUpBtn   .setBounds (top.removeFromRight (24));
        top.removeFromRight (2);
        slotPickValue.setBounds (top.removeFromRight (38));
        top.removeFromRight (2);
        slotDownBtn .setBounds (top.removeFromRight (24));
        top.removeFromRight (4);
        slotPickLbl .setBounds (top.removeFromRight (90));
        r.removeFromTop (6);

        /* Envelope edit area: 2 horizontal panels, stacked. */
        const int envH = juce::jmax (90, (r.getHeight() - 230) / 2);
        layoutEnv (r.removeFromTop (envH), true);
        r.removeFromTop (4);
        layoutEnv (r.removeFromTop (envH), false);
        r.removeFromTop (8);

        /* Per-instrument params row. */
        auto row = r.removeFromTop (24);
        monoBtn      .setBounds (row.removeFromLeft (60));  row.removeFromLeft (8);
        portaLbl     .setBounds (row.removeFromLeft (40));
        portamentoSlider.setBounds (row.removeFromLeft (110));  row.removeFromLeft (12);
        envFollowBtn .setBounds (row.removeFromLeft (76));  row.removeFromLeft (12);
        fadeLbl    .setBounds (row.removeFromLeft (60));
        fadeoutSlider.setBounds (row.removeFromLeft (120)); row.removeFromLeft (12);
        avSpeedLbl .setBounds (row.removeFromLeft (70));
        avRateSlider.setBounds (row.removeFromLeft (90));  row.removeFromLeft (12);
        avDepthLbl .setBounds (row.removeFromLeft (80));
        avDepthSlider.setBounds (row.removeFromLeft (90));  row.removeFromLeft (12);
        avSweepLbl .setBounds (row.removeFromLeft (80));
        avSweepSlider.setBounds (row.removeFromLeft (90));  row.removeFromLeft (12);
        avTypeCombo.setBounds (row.removeFromLeft (80));
        r.removeFromTop (10);

        /* Keymap header (label + oct controls). */
        auto kmTop = r.removeFromTop (22);
        keymapLbl.setBounds (kmTop.removeFromLeft (60));
        octUpBtn  .setBounds (kmTop.removeFromRight (50));   kmTop.removeFromRight (4);
        octDownBtn.setBounds (kmTop.removeFromRight (50));
        r.removeFromTop (2);
        keymap.setBounds (r.removeFromTop (juce::jmax (60, r.getHeight())));
    }

private:
    void layoutEnv (Rectangle<int> area, bool isVol)
    {
        auto canvas = area.removeFromLeft (area.getWidth() - 168);
        if (isVol && volEnvView) volEnvView->setBounds (canvas);
        if (! isVol && panEnvView) panEnvView->setBounds (canvas);

        area.removeFromLeft (8);
        /* Right strip: Add/Del row + flag toggles + sustain/loop nudges. */
        auto& sus  = isVol ? volSusLbl  : panSusLbl;
        auto& loop = isVol ? volLoopLbl : panLoopLbl;
        auto& enabledBtn = isVol ? volEnabledBtn : panEnabledBtn;
        auto& susBtn     = isVol ? volSustainBtn : panSustainBtn;
        auto& loopBtn    = isVol ? volLoopBtn    : panLoopBtn;
        auto& addBtn = isVol ? volAddBtn : panAddBtn;
        auto& delBtn = isVol ? volDelBtn : panDelBtn;
        auto& susUp  = isVol ? volSusUp  : panSusUp;
        auto& susDn  = isVol ? volSusDn  : panSusDn;
        auto& lsUp   = isVol ? volLoopStartUp : panLoopStartUp;
        auto& lsDn   = isVol ? volLoopStartDn : panLoopStartDn;
        auto& leUp   = isVol ? volLoopEndUp   : panLoopEndUp;
        auto& leDn   = isVol ? volLoopEndDn   : panLoopEndDn;

        auto col = area.reduced (0);
        /* Add / Del envelope points row. */
        auto adRow = col.removeFromTop (20);
        addBtn.setBounds (adRow.removeFromLeft (74));
        adRow.removeFromLeft (4);
        delBtn.setBounds (adRow.removeFromLeft (74));
        col.removeFromTop (6);

        enabledBtn.setBounds (col.removeFromTop (20)); col.removeFromTop (3);
        susBtn    .setBounds (col.removeFromTop (20)); col.removeFromTop (3);
        loopBtn   .setBounds (col.removeFromTop (20)); col.removeFromTop (6);

        auto susRow = col.removeFromTop (20);
        sus.setBounds (susRow.removeFromLeft (50));
        susUp.setBounds (susRow.removeFromRight (22));
        susRow.removeFromRight (2);
        susDn.setBounds (susRow.removeFromRight (22));
        col.removeFromTop (3);
        auto lsRow = col.removeFromTop (20);
        loop.setBounds (lsRow.removeFromLeft (50));
        lsUp.setBounds (lsRow.removeFromRight (22));
        lsRow.removeFromRight (2);
        lsDn.setBounds (lsRow.removeFromRight (22));
        col.removeFromTop (3);
        auto leRow = col.removeFromTop (20);
        leRow.removeFromLeft (50);
        leUp.setBounds (leRow.removeFromRight (22));
        leRow.removeFromRight (2);
        leDn.setBounds (leRow.removeFromRight (22));
    }

    void configureNudge (TextButton& b, const String& t, std::function<void()> on)
    {
        b.setButtonText (t);
        b.onClick = std::move (on);
        b.setColour (TextButton::buttonColourId, Colour { 0xff'22'22'22 });
        b.setColour (TextButton::textColourOffId, Colour { 0xff'd0'd0'd0 });
        addAndMakeVisible (b);
    }
    void configureSlider (Slider& s, double lo, double hi, double step,
                          std::function<void (double)> on)
    {
        s.setSliderStyle (Slider::LinearBar);
        s.setRange (lo, hi, step);
        s.setColour (Slider::backgroundColourId, Colour { 0xff'24'24'24 });
        s.setColour (Slider::trackColourId,      Colour { 0xff'4a'7a'b5 });
        s.onValueChange = [&s, on] { on (s.getValue()); };
        addAndMakeVisible (s);
    }
    void configureFlag (TextButton& b, const String& t,
                        std::function<void (bool)> on)
    {
        b.setButtonText (t);
        b.setClickingTogglesState (true);
        b.setColour (TextButton::buttonOnColourId, Colour { 0xff'4a'7a'b5 });
        b.setColour (TextButton::textColourOffId,  Colour { 0xff'a0'a0'a0 });
        b.setColour (TextButton::textColourOnId,   Colours::white);
        b.onClick = [&b, on] { on (b.getToggleState()); };
        addAndMakeVisible (b);
    }

    void toggleFlag (bool isVol, uint8_t bit, bool wantOn)
    {
        auto inst = getInst();
        if (inst == nullptr) return;
        FT2Envelope& e = isVol ? inst->volumeEnv : inst->panEnv;
        if (wantOn) e.flags |=  bit;
        else        e.flags &= ~bit;
        if (volEnvView) volEnvView->repaint();
        if (panEnvView) panEnvView->repaint();
    }
    void refreshFlagStates()
    {
        auto inst = getInst();
        if (inst == nullptr) return;
        const auto& ve = inst->volumeEnv;
        const auto& pe = inst->panEnv;
        volEnabledBtn.setToggleState (ve.flags & FT2Envelope::kEnabled, dontSendNotification);
        volSustainBtn.setToggleState (ve.flags & FT2Envelope::kSustain, dontSendNotification);
        volLoopBtn   .setToggleState (ve.flags & FT2Envelope::kLoop,    dontSendNotification);
        panEnabledBtn.setToggleState (pe.flags & FT2Envelope::kEnabled, dontSendNotification);
        panSustainBtn.setToggleState (pe.flags & FT2Envelope::kSustain, dontSendNotification);
        panLoopBtn   .setToggleState (pe.flags & FT2Envelope::kLoop,    dontSendNotification);
        monoBtn      .setToggleState (inst->mono,                       dontSendNotification);
        envFollowBtn .setToggleState (inst->envSampleRelative,          dontSendNotification);
    }

    /** Append a sensible new point at xMax+20, y = previous point's y.
     *  Mirrors FT2's "Add" button — extends the envelope tail. */
    void addEnvPoint (bool isVol)
    {
        auto inst = getInst();
        if (inst == nullptr) return;
        FT2Envelope& e = isVol ? inst->volumeEnv : inst->panEnv;
        if (e.length >= 12) return;
        if (e.length == 0)
        {
            e.points[0] = { 0, 64 };
            e.length = 1;
        }
        const int prevX = e.points[e.length - 1].x;
        const int prevY = e.points[e.length - 1].y;
        const int newX  = juce::jmin (324, prevX + 20);
        e.points[e.length] = { (int16_t) newX, (int16_t) prevY };
        e.length = (uint8_t) (e.length + 1);
        if (volEnvView) volEnvView->repaint();
        if (panEnvView) panEnvView->repaint();
    }
    /** Remove the trailing point.  FT2's "Del". */
    void delLastEnvPoint (bool isVol)
    {
        auto inst = getInst();
        if (inst == nullptr) return;
        FT2Envelope& e = isVol ? inst->volumeEnv : inst->panEnv;
        if (e.length <= 2) return;     /* keep at least 2 (degenerate avoided) */
        e.length = (uint8_t) (e.length - 1);
        if (e.sustainPoint >= e.length) e.sustainPoint = (uint8_t) (e.length - 1);
        if (e.loopStart   >= e.length) e.loopStart   = (uint8_t) (e.length - 1);
        if (e.loopEnd     >= e.length) e.loopEnd     = (uint8_t) (e.length - 1);
        if (volEnvView) volEnvView->repaint();
        if (panEnvView) panEnvView->repaint();
    }

    void nudgeEnvPoint (bool isVol, int delta, bool /*loopMode*/)
    {
        auto inst = getInst();
        if (inst == nullptr) return;
        FT2Envelope& e = isVol ? inst->volumeEnv : inst->panEnv;
        e.sustainPoint = (uint8_t) juce::jlimit (0, juce::jmax (0, (int) e.length - 1),
                                                   (int) e.sustainPoint + delta);
        if (volEnvView) volEnvView->repaint();
        if (panEnvView) panEnvView->repaint();
    }
    void nudgeEnvLoop (bool isVol, bool isStart, int delta)
    {
        auto inst = getInst();
        if (inst == nullptr) return;
        FT2Envelope& e = isVol ? inst->volumeEnv : inst->panEnv;
        const int maxIdx = juce::jmax (0, (int) e.length - 1);
        if (isStart)
            e.loopStart = (uint8_t) juce::jlimit (0, maxIdx, (int) e.loopStart + delta);
        else
            e.loopEnd   = (uint8_t) juce::jlimit ((int) e.loopStart, maxIdx,
                                                   (int) e.loopEnd + delta);
        if (volEnvView) volEnvView->repaint();
        if (panEnvView) panEnvView->repaint();
    }

    /** FT2-style predef shapes — applied to the volume envelope. */
    void applyPredefinedEnvelope (int preset)
    {
        auto inst = getInst();
        if (inst == nullptr) return;
        FT2Envelope& e = inst->volumeEnv;
        e.flags = FT2Envelope::kEnabled;
        e.sustainPoint = 0;
        e.loopStart    = 0;
        e.loopEnd      = 0;
        switch (preset)
        {
            case 1: /* Pluck: A-D, fast decay */
                e.length = 3;
                e.points[0] = {  0,  0 };
                e.points[1] = {  2, 64 };
                e.points[2] = { 30,  0 };
                break;
            case 2: /* AD-sustain */
                e.length = 4;
                e.points[0] = {  0,  0 };
                e.points[1] = {  4, 64 };
                e.points[2] = { 30, 50 };
                e.points[3] = { 50, 50 };
                e.sustainPoint = 3;
                e.flags |= FT2Envelope::kSustain;
                break;
            case 3: /* Pad: slow attack, sustained */
                e.length = 4;
                e.points[0] = {   0,  0 };
                e.points[1] = {  50, 50 };
                e.points[2] = { 100, 64 };
                e.points[3] = { 150, 64 };
                e.sustainPoint = 3;
                e.flags |= FT2Envelope::kSustain;
                break;
            case 4: /* Tremolo loop */
                e.length = 4;
                e.points[0] = {  0,  0 };
                e.points[1] = {  4, 64 };
                e.points[2] = { 20, 20 };
                e.points[3] = { 40, 64 };
                e.loopStart = 1; e.loopEnd = 3;
                e.flags |= FT2Envelope::kLoop;
                break;
            case 5: /* Sustain — FT2 default. */
                e.length = 2;
                e.points[0] = { 0,  64 };
                e.points[1] = { 50, 64 };
                break;
            case 6: /* Decay-only (no sustain) */
                e.length = 2;
                e.points[0] = {  0, 64 };
                e.points[1] = { 80,  0 };
                break;
            default: break;
        }
        refreshFlagStates();
        if (volEnvView) volEnvView->repaint();
    }

    GetInstrument getInst;
    GetActiveSlot getActiveSlot;
    SetActiveSlot setActiveSlot;
    SamplerKeymap keymap;
    std::unique_ptr<FT2EnvelopeView> volEnvView, panEnvView;

    Label slotPickLbl, slotPickValue;
    TextButton slotDownBtn, slotUpBtn;

    OwnedArray<TextButton> predefBtns;
    Label predefLbl, keymapLbl;
    Label volSusLbl, volLoopLbl, panSusLbl, panLoopLbl;
    Label fadeLbl, avSpeedLbl, avDepthLbl, avSweepLbl;

    TextButton volEnabledBtn, volSustainBtn, volLoopBtn;
    TextButton panEnabledBtn, panSustainBtn, panLoopBtn;
    TextButton volAddBtn, volDelBtn, panAddBtn, panDelBtn;
    TextButton volSusUp,  volSusDn,  panSusUp,  panSusDn;
    TextButton volLoopStartUp, volLoopStartDn, volLoopEndUp, volLoopEndDn;
    TextButton panLoopStartUp, panLoopStartDn, panLoopEndUp, panLoopEndDn;

    Slider fadeoutSlider, avRateSlider, avDepthSlider, avSweepSlider;
    ComboBox avTypeCombo;
    TextButton octDownBtn, octUpBtn;

    /* Mono mode + portamento (Inst-level).  See SamplerInstrument::mono /
     * portamentoTimeMs + ChannelTrackingSynth::noteOn for the engine
     * side. */
    TextButton monoBtn;
    Label      portaLbl;
    Slider     portamentoSlider;
    TextButton envFollowBtn;
};


/* ===========================================================================
 * SampleEditCanvas — large waveform display with range selection +
 * viewport zoom.  Coordinates are int sample indices (0..numSamples).
 * Range selection: [selStart_..selEnd_) — half-open.  Viewport:
 * [viewStart_..viewEnd_).  Renders only the visible window.
 * ========================================================================*/
class SampleEditCanvas : public Component,
                         private juce::ScrollBar::Listener,
                         private juce::Timer
{
public:
    using GetSlot = std::function<SamplerSampleSlot*()>;
    using RangeChangedCb = std::function<void()>;
    using PlayheadQuery  = std::function<std::vector<int>(const SamplerSampleSlot*)>;

    explicit SampleEditCanvas (GetSlot gs, RangeChangedCb cb = {})
        : getSlot (std::move (gs)), onRangeChanged (std::move (cb)),
          hScroll_ (false)
    {
        hScroll_.addListener (this);
        hScroll_.setAutoHide (false);
        addAndMakeVisible (hScroll_);
    }
    ~SampleEditCanvas() override { stopTimer(); hScroll_.removeListener (this); }

    /** Provide a callback returning the current per-voice playhead
     *  sample positions for this slot.  When set, the canvas polls at
     *  30 Hz and repaints when positions change — same model as
     *  SampleWaveformView's live indicator on the Bank page. */
    void setPlayheadQuery (PlayheadQuery q)
    {
        playheadsFor_ = std::move (q);
        if (playheadsFor_) startTimerHz (30);
        else               stopTimer();
    }

    void resized() override
    {
        auto r = getLocalBounds();
        hScroll_.setBounds (r.removeFromBottom (kScrollH));
    }

    void setViewport (int s, int e)
    {
        viewStart_ = s; viewEnd_ = e;
        syncScrollBar();
        repaint();
    }
    void setSelection (int s, int e)
    {
        selStart_ = juce::jmin (s, e);
        selEnd_   = juce::jmax (s, e);
        if (onRangeChanged) onRangeChanged();
        repaint();
    }
    void clearSelection() { selStart_ = selEnd_ = -1; if (onRangeChanged) onRangeChanged(); repaint(); }
    int  getSelStart() const noexcept { return selStart_; }
    int  getSelEnd()   const noexcept { return selEnd_; }
    int  getViewStart() const noexcept { return viewStart_; }
    int  getViewEnd()   const noexcept { return viewEnd_; }
    bool hasSelection() const noexcept { return selStart_ >= 0 && selEnd_ > selStart_; }

    void paint (Graphics& g) override
    {
        auto fullBounds = getLocalBounds().toFloat();
        fullBounds.removeFromBottom ((float) kScrollH);
        const auto bounds = fullBounds.reduced (2.0f);
        g.setColour (Colour { 0xff'0c'0c'0c });
        g.fillRect (bounds);
        g.setColour (Colour { 0xff'2a'2a'2a });
        g.drawRect (bounds, 1.0f);

        auto* slot = getSlot ? getSlot() : nullptr;
        if (slot == nullptr || ! slot->isLoaded())
        {
            g.setColour (Colour { 0xff'5a'5a'5a });
            g.setFont (FontOptions (Font::getDefaultMonospacedFontName(), 12.0f, Font::plain));
            g.drawText ("(no sample loaded for this slot)",
                        bounds, Justification::centred);
            return;
        }

        const int n = slot->numSamples;
        if (viewEnd_ <= 0 || viewEnd_ > n) viewEnd_ = n;
        if (viewStart_ < 0 || viewStart_ >= viewEnd_) viewStart_ = 0;

        const int vRange = viewEnd_ - viewStart_;
        if (vRange <= 0) return;

        const int   w  = juce::roundToInt (bounds.getWidth());
        const float h  = bounds.getHeight();
        const float midY = bounds.getCentreY();
        const float halfH = h * 0.45f;
        const auto* d = slot->data16L.get();
        if (w <= 0) return;

        /* Selection shading (under the waveform). */
        if (hasSelection())
        {
            const float sx0 = sampleToPixel (selStart_, w);
            const float sx1 = sampleToPixel (selEnd_,   w);
            if (sx1 > sx0)
            {
                g.setColour (Colour { 0x44'40'a0'ff });
                g.fillRect (bounds.getX() + sx0, bounds.getY(),
                            sx1 - sx0, bounds.getHeight());
            }
        }

        /* Loop region shade (over selection, under waveform). */
        if (slot->loopMode != SamplerLoopMode::kNone && slot->loopLength > 0)
        {
            const float lx0 = sampleToPixel (slot->loopStart, w);
            const float lx1 = sampleToPixel (slot->loopStart + slot->loopLength, w);
            if (lx1 > lx0)
            {
                g.setColour (Colour { 0x33'ff'a0'40 });
                g.fillRect (bounds.getX() + lx0, bounds.getY(),
                            lx1 - lx0, bounds.getHeight());
            }
        }

        /* Centerline. */
        g.setColour (Colour { 0xff'2a'2a'2a });
        g.drawHorizontalLine ((int) midY, bounds.getX(), bounds.getRight());

        /* Min/max envelope per pixel column. */
        /* Two render modes:
         *   - Envelope mode (vRange > 4*w): per-pixel min/max envelope.
         *   - Sample-line mode (vRange <= 4*w, moderate-to-deep zoom):
         *     connect each sample at its exact subpixel position with
         *     a line plot, dotting each sample once spacing > 4 px.
         *
         * Threshold is 4× pixel count rather than 1× — at moderate
         * zooms with smooth content the envelope's per-pixel scan
         * produced visible moiré against the pixel grid.  Line mode
         * handles 4 samples/pixel cleanly via path stroking. */
        g.setColour (Colour { 0xff'5a'a5'd0 });
        if (vRange > 4 * w)
        {
            for (int x = 0; x < w; ++x)
            {
                const int64_t s0 = viewStart_ + (int64_t) x * vRange / w;
                const int64_t s1 = juce::jmax (s0 + 1,
                                                (int64_t) viewStart_ + (int64_t)(x + 1) * vRange / w);
                int mn = INT16_MAX, mx = INT16_MIN;
                for (int64_t i = s0; i < s1 && i < n; ++i)
                {
                    const int v = d[i];
                    if (v < mn) mn = v;
                    if (v > mx) mx = v;
                }
                const float yMin = midY - (mn / 32768.0f) * halfH;
                const float yMax = midY - (mx / 32768.0f) * halfH;
                g.drawLine (bounds.getX() + x, yMax, bounds.getX() + x, yMin);
            }
        }
        else
        {
            const double pxPerSample = (double) w / (double) juce::jmax (1, vRange);
            Path linePath;
            bool started = false;
            for (int64_t s = viewStart_; s <= viewEnd_ && s < n; ++s)
            {
                const float xpx = bounds.getX() + (float) ((s - viewStart_) * pxPerSample);
                const float ypx = midY - (d[s] / 32768.0f) * halfH;
                if (! started) { linePath.startNewSubPath (xpx, ypx); started = true; }
                else            linePath.lineTo (xpx, ypx);
            }
            g.strokePath (linePath, PathStrokeType (1.2f));

            if (pxPerSample >= 4.0)
            {
                for (int64_t s = viewStart_; s <= viewEnd_ && s < n; ++s)
                {
                    const float xpx = bounds.getX() + (float) ((s - viewStart_) * pxPerSample);
                    const float ypx = midY - (d[s] / 32768.0f) * halfH;
                    g.fillEllipse (xpx - 1.5f, ypx - 1.5f, 3.0f, 3.0f);
                }
            }
        }

        /* Selection edge markers. */
        if (hasSelection())
        {
            const float sx0 = sampleToPixel (selStart_, w);
            const float sx1 = sampleToPixel (selEnd_,   w);
            g.setColour (Colour { 0xff'40'a0'ff });
            g.fillRect (bounds.getX() + sx0 - 1.0f, bounds.getY(), 2.0f, bounds.getHeight());
            g.fillRect (bounds.getX() + sx1 - 1.0f, bounds.getY(), 2.0f, bounds.getHeight());
        }

        /* Loop region markers + draggable handle dots — matches the
         * Bank-page SampleWaveformView so the visual affordance is
         * consistent across the two surfaces. */
        if (slot->loopMode != SamplerLoopMode::kNone && slot->loopLength > 0)
        {
            const float lx0 = sampleToPixel (slot->loopStart, w);
            const float lx1 = sampleToPixel (slot->loopStart + slot->loopLength, w);
            g.setColour (Colour { 0xff'ff'a0'40 });
            g.fillRect (bounds.getX() + lx0 - 1.0f, bounds.getY(), 2.0f, bounds.getHeight());
            g.fillRect (bounds.getX() + lx1 - 1.0f, bounds.getY(), 2.0f, bounds.getHeight());
            g.fillEllipse (bounds.getX() + lx0 - 4.0f, bounds.getY(),                 8.0f, 8.0f);
            g.fillEllipse (bounds.getX() + lx1 - 4.0f, bounds.getBottom() - 8.0f,     8.0f, 8.0f);
        }

        /* Live playheads (per-voice sample positions) — green vertical
         * tick lines, clipped to the current viewport. */
        if (playheadsFor_)
        {
            const auto positions = playheadsFor_ (slot);
            g.setColour (Colour { 0xff'40'ff'80 });
            for (int pos : positions)
            {
                if (pos < viewStart_ || pos >= viewEnd_) continue;
                const float x = bounds.getX() + sampleToPixel (pos, w);
                g.fillRect (x - 0.5f, bounds.getY(), 1.5f, bounds.getHeight());
            }
        }
    }

    void mouseDown (const MouseEvent& e) override
    {
        auto* slot = getSlot ? getSlot() : nullptr;
        if (slot == nullptr || ! slot->isLoaded()) return;

        /* If a loop is configured, check for hit on either of its
         * handles BEFORE falling through to selection drag.  Hit zone
         * is the half-width of the handle dot plus a bit of slop.
         * Either handle can be grabbed independently of the
         * selection — selection-based loop set (the toolbar's
         * "Loop=Sel" button) still works as the convenience path. */
        if (slot->loopMode != SamplerLoopMode::kNone && slot->loopLength > 0)
        {
            const auto bounds = getLocalBounds().reduced (2);
            bounds.toFloat();
            const int w     = bounds.getWidth();
            const int xL    = bounds.getX() + (int) sampleToPixel (slot->loopStart, w);
            const int xR    = bounds.getX() + (int) sampleToPixel (slot->loopStart + slot->loopLength, w);
            const int dxL   = std::abs (e.x - xL);
            const int dxR   = std::abs (e.x - xR);
            constexpr int kLoopHitTolPx = 8;

            if (dxL <= kLoopHitTolPx || dxR <= kLoopHitTolPx)
            {
                draggingLoopMarker_ = (dxL <= dxR) ? 0 : 1;
                return;
            }
        }

        const int s = pixelToSample (e.x);
        if (e.mods.isShiftDown() && hasSelection())
            setSelection (selStart_, s);
        else
            { dragAnchor_ = s; setSelection (s, s); }
    }
    void mouseDrag (const MouseEvent& e) override
    {
        auto* slot = getSlot ? getSlot() : nullptr;
        if (slot == nullptr || ! slot->isLoaded()) return;

        if (draggingLoopMarker_ >= 0)
        {
            const int newPos = pixelToSample (e.x);
            if (draggingLoopMarker_ == 0)
            {
                const int oldEnd  = slot->loopStart + slot->loopLength;
                slot->loopStart   = juce::jlimit (0, oldEnd - 1, newPos);
                slot->loopLength  = oldEnd - slot->loopStart;
            }
            else
            {
                slot->loopLength  = juce::jlimit (1,
                                                  slot->numSamples - slot->loopStart,
                                                  newPos - slot->loopStart);
            }
            if (onRangeChanged) onRangeChanged();
            repaint();
            return;
        }

        if (dragAnchor_ < 0) return;
        setSelection (dragAnchor_, pixelToSample (e.x));
    }
    void mouseUp (const MouseEvent&) override
    {
        draggingLoopMarker_ = -1;
        dragAnchor_         = -1;
    }
    void mouseDoubleClick (const MouseEvent&) override
    {
        auto* slot = getSlot ? getSlot() : nullptr;
        if (slot == nullptr || ! slot->isLoaded()) return;
        setSelection (0, slot->numSamples);
    }

    /** Trackpad pinch — primary zoom gesture, anchored at cursor. */
    void mouseMagnify (const MouseEvent& e, float scaleFactor) override
    {
        applyZoom (e.x, (double) scaleFactor);
    }

    /** Trackpad 2-finger scroll: deltaX = pan, deltaY = zoom (fallback). */
    void mouseWheelMove (const MouseEvent& e, const MouseWheelDetails& w) override
    {
        auto* slot = getSlot ? getSlot() : nullptr;
        if (slot == nullptr || ! slot->isLoaded()) return;
        const int n = slot->numSamples;

        if (std::abs (w.deltaX) > std::abs (w.deltaY))
        {
            const int range = viewEnd_ - viewStart_;
            const int shift = (int) std::lround (range * -w.deltaX * 0.5);
            int newStart = juce::jlimit (0, juce::jmax (0, n - range),
                                          viewStart_ + shift);
            setViewport (newStart, newStart + range);
        }
        else if (std::abs (w.deltaY) > 0.001f)
        {
            const double factor = (w.deltaY > 0) ? (1.0 / 1.20) : 1.20;
            applyZoom (e.x, factor);
        }
    }

private:
    void applyZoom (int anchorPx, double factor)
    {
        auto* slot = getSlot ? getSlot() : nullptr;
        if (slot == nullptr || ! slot->isLoaded()) return;
        const int n = slot->numSamples;
        if (viewEnd_ <= viewStart_) { viewStart_ = 0; viewEnd_ = n; }
        const int anchorSamp = pixelToSample (anchorPx);
        int newRange = juce::jmax (4, (int) std::lround ((viewEnd_ - viewStart_) / factor));
        newRange = juce::jmin (n, newRange);
        const double tRel = double (anchorSamp - viewStart_)
                          / juce::jmax (1, viewEnd_ - viewStart_);
        int newStart = (int) std::lround (anchorSamp - tRel * newRange);
        newStart = juce::jlimit (0, juce::jmax (0, n - newRange), newStart);
        setViewport (newStart, newStart + newRange);
    }

    int pixelToSample (int x) const
    {
        const auto bounds = getLocalBounds().reduced (2);
        const int w = bounds.getWidth();
        if (w <= 0) return viewStart_;
        const int vRange = viewEnd_ - viewStart_;
        return juce::jlimit (viewStart_, viewEnd_,
                             viewStart_ + (int) ((int64_t) (x - bounds.getX()) * vRange / w));
    }
    float sampleToPixel (int s, int w) const
    {
        const int vRange = viewEnd_ - viewStart_;
        if (vRange <= 0 || w <= 0) return 0.0f;
        return (float) ((int64_t) (s - viewStart_) * w / vRange);
    }

    /* Horizontal scrollbar — same fallback story as SampleWaveformView. */
    static constexpr int kScrollH = 12;
    juce::ScrollBar hScroll_;

    void syncScrollBar()
    {
        auto* slot = getSlot ? getSlot() : nullptr;
        const int n = (slot && slot->isLoaded()) ? slot->numSamples : 0;
        if (n <= 0) { hScroll_.setRangeLimits (0.0, 1.0); hScroll_.setCurrentRange (0.0, 1.0); return; }
        hScroll_.setRangeLimits (0.0, (double) n);
        hScroll_.setCurrentRange ((double) viewStart_,
                                   (double) (viewEnd_ - viewStart_),
                                   dontSendNotification);
    }

    void scrollBarMoved (juce::ScrollBar*, double newStart) override
    {
        auto* slot = getSlot ? getSlot() : nullptr;
        if (slot == nullptr || ! slot->isLoaded()) return;
        const int n = slot->numSamples;
        const int range = viewEnd_ - viewStart_;
        const int newS = juce::jlimit (0, juce::jmax (0, n - range), (int) newStart);
        viewStart_ = newS;
        viewEnd_   = newS + range;
        repaint();
    }

    void timerCallback() override
    {
        if (! isShowing()) return;        /* free CPU when off-screen */
        if (! playheadsFor_) return;
        auto* slot = getSlot ? getSlot() : nullptr;
        std::vector<int> cur = slot != nullptr ? playheadsFor_ (slot)
                                                : std::vector<int>();
        if (cur == lastPlayheads_) return;
        lastPlayheads_ = std::move (cur);
        repaint();
    }

    GetSlot getSlot;
    RangeChangedCb onRangeChanged;
    PlayheadQuery  playheadsFor_;
    std::vector<int> lastPlayheads_;
    int viewStart_ = 0, viewEnd_ = 0;
    int selStart_  = -1, selEnd_  = -1;
    int dragAnchor_         = -1;
    int draggingLoopMarker_ = -1;  /* -1 none, 0 = loopStart, 1 = loopEnd */
};


/* ===========================================================================
 * SamplerSamplePage — Sample editor.  Big waveform + range select + zoom
 * + cut/copy/paste/crop + length/loop info + loop-mode radio.
 *
 * Operations mutate the int16 buffer in place; loopStart/loopLength are
 * adjusted to stay in range when the buffer shrinks/grows.  Clipboard is
 * per-pane state (one shared buffer for all slots within this editor
 * instance — fine for a single-window app).
 * ========================================================================*/
class SamplerSamplePage : public Component
{
public:
    using GetSlot = std::function<SamplerSampleSlot*()>;
    using OnChange = std::function<void()>;
    using PlayheadQuery = SampleEditCanvas::PlayheadQuery;

    /** Pass-through to the underlying SampleEditCanvas — see its
     *  setPlayheadQuery for behaviour. */
    void setPlayheadQuery (PlayheadQuery q) { canvas.setPlayheadQuery (std::move (q)); }

    SamplerSamplePage (GetSlot gs, OnChange cb)
        : getSlot (std::move (gs)),
          onChange (std::move (cb)),
          canvas ([this] { return getSlot ? getSlot() : nullptr; },
                  [this] { refreshInfo(); })
    {
        addAndMakeVisible (canvas);

        configureBtn (cutBtn,    "Cut",       [this] { doCut(); });
        configureBtn (copyBtn,   "Copy",      [this] { doCopy(); });
        configureBtn (pasteBtn,  "Paste",     [this] { doPaste(); });
        configureBtn (cropBtn,   "Crop",      [this] { doCrop(); });
        configureBtn (selAllBtn, "Sel All",   [this] { selectAll(); });
        configureBtn (selNoneBtn,"Sel None",  [this] { canvas.clearSelection(); refreshInfo(); });
        configureBtn (zoomInBtn, "Zoom in",   [this] { zoomToSelection(); });
        configureBtn (zoomOutBtn,"Zoom out",  [this] { zoomOut(); });
        configureBtn (zoomAllBtn,"Show all",  [this] { zoomAll(); });
        configureBtn (loopSetBtn,"Loop=Sel",  [this] { loopFromSelection(); });

        loopRadio.addItem ("Loop: Off",      1);
        loopRadio.addItem ("Loop: Forward",  2);
        loopRadio.addItem ("Loop: Pingpong", 3);
        loopRadio.setColour (ComboBox::backgroundColourId, Colour { 0xff'24'24'24 });
        loopRadio.setColour (ComboBox::textColourId,        Colour { 0xff'd0'd0'd0 });
        loopRadio.onChange = [this] {
            if (auto* s = getSlot ? getSlot() : nullptr)
                s->loopMode = (SamplerLoopMode) (loopRadio.getSelectedId() - 1);
            if (onChange) onChange();
            refreshInfo();
            canvas.repaint();
        };
        addAndMakeVisible (loopRadio);

        for (auto* l : { &infoLength, &infoSel, &infoLoop, &infoView })
        {
            l->setColour (Label::textColourId, Colour { 0xff'b0'b0'b0 });
            l->setFont (FontOptions (Font::getDefaultMonospacedFontName(), 11.0f, Font::plain));
            addAndMakeVisible (*l);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (8);

        /* Top: action bar. */
        auto topRow = r.removeFromTop (24);
        const int btnW = 70;
        cutBtn   .setBounds (topRow.removeFromLeft (btnW)); topRow.removeFromLeft (3);
        copyBtn  .setBounds (topRow.removeFromLeft (btnW)); topRow.removeFromLeft (3);
        pasteBtn .setBounds (topRow.removeFromLeft (btnW)); topRow.removeFromLeft (3);
        cropBtn  .setBounds (topRow.removeFromLeft (btnW)); topRow.removeFromLeft (10);
        selAllBtn.setBounds (topRow.removeFromLeft (btnW)); topRow.removeFromLeft (3);
        selNoneBtn.setBounds(topRow.removeFromLeft (btnW)); topRow.removeFromLeft (10);
        zoomInBtn.setBounds (topRow.removeFromLeft (btnW)); topRow.removeFromLeft (3);
        zoomOutBtn.setBounds(topRow.removeFromLeft (btnW)); topRow.removeFromLeft (3);
        zoomAllBtn.setBounds(topRow.removeFromLeft (btnW)); topRow.removeFromLeft (10);
        loopSetBtn.setBounds(topRow.removeFromLeft (90));   topRow.removeFromLeft (10);
        loopRadio .setBounds(topRow.removeFromLeft (150));
        r.removeFromTop (8);

        /* Bottom info row. */
        auto bot = r.removeFromBottom (20);
        infoLength.setBounds (bot.removeFromLeft (180));
        infoSel   .setBounds (bot.removeFromLeft (260));
        infoLoop  .setBounds (bot.removeFromLeft (200));
        infoView  .setBounds (bot);
        r.removeFromBottom (4);

        /* Rest: canvas. */
        canvas.setBounds (r);
    }

    /** Called when the page becomes visible or slot changes. */
    void refresh()
    {
        auto* s = getSlot ? getSlot() : nullptr;
        if (s != nullptr && s->numSamples > 0)
        {
            if (canvas.getViewEnd() <= 0 || canvas.getViewEnd() > s->numSamples)
                canvas.setViewport (0, s->numSamples);

            /* Selection persists across slot changes (so a Sample-page
             * range edit on slot N keeps that range as you flip back).
             * But if the new slot is SHORTER than the old selection
             * endpoints, downstream Cut / Paste / Crop would read past
             * the new buffer.  Clamp / drop. */
            const int n  = s->numSamples;
            const int ss = canvas.getSelStart();
            const int se = canvas.getSelEnd();
            if (ss >= 0 && se > ss)
            {
                const int newSS = juce::jmin (ss, n);
                const int newSE = juce::jmin (se, n);
                if (newSE > newSS) canvas.setSelection (newSS, newSE);
                else                canvas.clearSelection();
            }
        }
        else
        {
            canvas.setViewport (0, 0);
            canvas.clearSelection();
        }
        loopRadio.setSelectedId (s ? (int) s->loopMode + 1 : 1, dontSendNotification);
        refreshInfo();
        canvas.repaint();
    }

private:
    void refreshInfo()
    {
        auto* s = getSlot ? getSlot() : nullptr;
        infoLength.setText (s ? String::formatted ("Length: %d", s->numSamples)
                              : "Length: —", dontSendNotification);
        if (s && canvas.hasSelection())
            infoSel.setText (String::formatted ("Sel: %d..%d (%d)",
                                                  canvas.getSelStart(),
                                                  canvas.getSelEnd(),
                                                  canvas.getSelEnd() - canvas.getSelStart()),
                              dontSendNotification);
        else
            infoSel.setText ("Sel: (none)", dontSendNotification);
        infoLoop.setText (s ? String::formatted ("Loop: %d + %d",
                                                  s->loopStart, s->loopLength)
                            : "Loop: —", dontSendNotification);
        infoView.setText (String::formatted ("View: %d..%d",
                                              canvas.getViewStart(), canvas.getViewEnd()),
                          dontSendNotification);
    }

    void configureBtn (TextButton& b, const String& t, std::function<void()> on)
    {
        b.setButtonText (t);
        b.onClick = std::move (on);
        b.setColour (TextButton::buttonColourId,    Colour { 0xff'24'24'24 });
        b.setColour (TextButton::textColourOffId,   Colour { 0xff'd0'd0'd0 });
        addAndMakeVisible (b);
    }

    void selectAll()
    {
        auto* s = getSlot ? getSlot() : nullptr;
        if (s == nullptr || ! s->isLoaded()) return;
        canvas.setSelection (0, s->numSamples);
        refreshInfo();
    }
    void zoomAll()
    {
        auto* s = getSlot ? getSlot() : nullptr;
        if (s == nullptr) return;
        canvas.setViewport (0, juce::jmax (0, s->numSamples));
        refreshInfo();
    }
    void zoomToSelection()
    {
        if (! canvas.hasSelection()) return;
        canvas.setViewport (canvas.getSelStart(), canvas.getSelEnd());
        refreshInfo();
    }
    void zoomOut()
    {
        auto* s = getSlot ? getSlot() : nullptr;
        if (s == nullptr) return;
        const int vs = canvas.getViewStart();
        const int ve = canvas.getViewEnd();
        const int span = ve - vs;
        const int cx = (vs + ve) / 2;
        const int newSpan = juce::jmin (s->numSamples, span * 2);
        const int newVs = juce::jmax (0, cx - newSpan / 2);
        const int newVe = juce::jmin (s->numSamples, newVs + newSpan);
        canvas.setViewport (newVs, newVe);
        refreshInfo();
    }

    /** Cut: copy selection to clipboard, then splice out of the buffer. */
    void doCut()
    {
        if (! copyToClipboard()) return;
        spliceOutSelection();
    }
    bool doCopy() { return copyToClipboard(); }

    bool copyToClipboard()
    {
        auto* s = getSlot ? getSlot() : nullptr;
        if (s == nullptr || ! canvas.hasSelection()) return false;
        const int a = canvas.getSelStart();
        const int b = canvas.getSelEnd();
        const int n = juce::jmax (0, b - a);
        if (n <= 0) return false;
        clipL.assign (s->data16L.get() + a, s->data16L.get() + b);
        if (s->isStereo && s->data16R)
        { clipR.assign (s->data16R.get() + a, s->data16R.get() + b); clipStereo = true; }
        else
        { clipR.clear(); clipStereo = false; }
        return true;
    }

    /** Paste: replace selection (or insert at sel-start if no range). */
    void doPaste()
    {
        auto* s = getSlot ? getSlot() : nullptr;
        if (s == nullptr || clipL.empty()) return;

        const int oldN = s->numSamples;
        const int a = juce::jmax (0, canvas.hasSelection() ? canvas.getSelStart() : oldN);
        const int b = juce::jmax (a, canvas.hasSelection() ? canvas.getSelEnd()   : a);
        const int newN = oldN - (b - a) + (int) clipL.size();

        std::unique_ptr<int16_t[]> newL (new int16_t[(size_t) newN]);
        std::unique_ptr<int16_t[]> newR;
        if (s->isStereo) newR.reset (new int16_t[(size_t) newN]);

        /* Pre-selection. */
        std::memcpy (newL.get(), s->data16L.get(), (size_t) a * sizeof (int16_t));
        if (s->isStereo && s->data16R)
            std::memcpy (newR.get(), s->data16R.get(), (size_t) a * sizeof (int16_t));
        /* Clipboard. */
        std::memcpy (newL.get() + a, clipL.data(), clipL.size() * sizeof (int16_t));
        if (s->isStereo && newR)
        {
            if (clipStereo && ! clipR.empty())
                std::memcpy (newR.get() + a, clipR.data(), clipR.size() * sizeof (int16_t));
            else
                std::memcpy (newR.get() + a, clipL.data(), clipL.size() * sizeof (int16_t));
        }
        /* Post-selection. */
        const int tailLen = oldN - b;
        if (tailLen > 0)
        {
            std::memcpy (newL.get() + a + (int) clipL.size(),
                         s->data16L.get() + b, (size_t) tailLen * sizeof (int16_t));
            if (s->isStereo && newR)
                std::memcpy (newR.get() + a + (int) clipL.size(),
                             s->data16R.get() + b, (size_t) tailLen * sizeof (int16_t));
        }

        s->data16L  = std::move (newL);
        s->data16R  = std::move (newR);
        s->numSamples = newN;
        fixupLoopAfterEdit (s, a, (int) clipL.size() - (b - a));
        canvas.setSelection (a, a + (int) clipL.size());
        zoomAll();
        notifyChange();
    }

    void spliceOutSelection()
    {
        auto* s = getSlot ? getSlot() : nullptr;
        if (s == nullptr || ! canvas.hasSelection()) return;
        const int a = canvas.getSelStart();
        const int b = canvas.getSelEnd();
        const int oldN = s->numSamples;
        const int newN = oldN - (b - a);
        if (newN <= 0)
        {
            /* Wipe sample entirely. */
            s->data16L.reset();
            s->data16R.reset();
            s->numSamples = 0;
            s->loopStart  = 0;
            s->loopLength = 0;
            canvas.clearSelection();
            zoomAll();
            notifyChange();
            return;
        }
        std::unique_ptr<int16_t[]> newL (new int16_t[(size_t) newN]);
        std::unique_ptr<int16_t[]> newR;
        std::memcpy (newL.get(), s->data16L.get(), (size_t) a * sizeof (int16_t));
        std::memcpy (newL.get() + a, s->data16L.get() + b,
                     (size_t) (oldN - b) * sizeof (int16_t));
        if (s->isStereo && s->data16R)
        {
            newR.reset (new int16_t[(size_t) newN]);
            std::memcpy (newR.get(), s->data16R.get(), (size_t) a * sizeof (int16_t));
            std::memcpy (newR.get() + a, s->data16R.get() + b,
                         (size_t) (oldN - b) * sizeof (int16_t));
        }
        s->data16L  = std::move (newL);
        s->data16R  = std::move (newR);
        s->numSamples = newN;
        fixupLoopAfterEdit (s, a, -(b - a));
        canvas.clearSelection();
        zoomAll();
        notifyChange();
    }

    /** Crop: keep selection, drop everything else. */
    void doCrop()
    {
        auto* s = getSlot ? getSlot() : nullptr;
        if (s == nullptr || ! canvas.hasSelection()) return;
        const int a = canvas.getSelStart();
        const int b = canvas.getSelEnd();
        const int newN = b - a;
        if (newN <= 0) return;
        std::unique_ptr<int16_t[]> newL (new int16_t[(size_t) newN]);
        std::unique_ptr<int16_t[]> newR;
        std::memcpy (newL.get(), s->data16L.get() + a, (size_t) newN * sizeof (int16_t));
        if (s->isStereo && s->data16R)
        {
            newR.reset (new int16_t[(size_t) newN]);
            std::memcpy (newR.get(), s->data16R.get() + a, (size_t) newN * sizeof (int16_t));
        }
        s->data16L  = std::move (newL);
        s->data16R  = std::move (newR);
        s->numSamples = newN;
        s->loopStart  = juce::jlimit (0, newN - 1,
                                       s->loopStart - a);
        s->loopLength = juce::jlimit (0, newN - s->loopStart, s->loopLength);
        canvas.setSelection (0, newN);
        zoomAll();
        notifyChange();
    }

    void loopFromSelection()
    {
        auto* s = getSlot ? getSlot() : nullptr;
        if (s == nullptr || ! canvas.hasSelection()) return;
        const int a = canvas.getSelStart();
        const int b = canvas.getSelEnd();
        s->loopStart  = juce::jlimit (0, s->numSamples - 1, a);
        s->loopLength = juce::jlimit (1, s->numSamples - s->loopStart, b - a);
        if (s->loopMode == SamplerLoopMode::kNone)
            s->loopMode = SamplerLoopMode::kForward;
        loopRadio.setSelectedId ((int) s->loopMode + 1, dontSendNotification);
        refreshInfo();
        canvas.repaint();
        notifyChange();
    }

    static void fixupLoopAfterEdit (SamplerSampleSlot* s, int editAt, int delta)
    {
        if (s == nullptr) return;
        const int loopEnd = s->loopStart + s->loopLength;
        const auto shift = [editAt, delta] (int& v) {
            if (v >= editAt) v += delta;
            if (v < 0) v = 0;
        };
        int newStart = s->loopStart;
        int newEnd   = loopEnd;
        shift (newStart);
        shift (newEnd);
        s->loopStart  = juce::jlimit (0, juce::jmax (0, s->numSamples - 1), newStart);
        s->loopLength = juce::jlimit (0, s->numSamples - s->loopStart,
                                       newEnd - s->loopStart);
    }

    void notifyChange()
    {
        if (onChange) onChange();
        refreshInfo();
        canvas.repaint();
    }

    GetSlot getSlot;
    OnChange onChange;
    SampleEditCanvas canvas;

    TextButton cutBtn, copyBtn, pasteBtn, cropBtn;
    TextButton selAllBtn, selNoneBtn;
    TextButton zoomInBtn, zoomOutBtn, zoomAllBtn;
    TextButton loopSetBtn;
    ComboBox   loopRadio;
    Label      infoLength, infoSel, infoLoop, infoView;

    std::vector<int16_t> clipL, clipR;
    bool                 clipStereo = false;
};


/* ===========================================================================
 * Shared waveshaper transfer function.  x ∈ [-1, +1] → y ∈ [-1, +1].
 * Shared between SamplerFXPage::applyWaveshaper and the shape preview
 * canvas so both compute identically. */
inline double waveShaperSample (int kind, double drive, double x)
{
    const double d = juce::jlimit (1.0, 50.0, drive);
    switch (kind)
    {
        case 1: /* Tanh — smooth saturation */
            return std::tanh (d * x);
        case 2: /* Soft clip — rational, less harmonic content */
            return (x * d) / (1.0 + std::abs (x * d));
        case 3: /* Hard clip — top + bottom snap */
            return juce::jlimit (-1.0, 1.0, d * x);
        case 4: /* Wave fold — bouncy reflections, can over-drive into harmonics */
            return std::sin (d * x * juce::MathConstants<double>::halfPi);
        case 5: /* Asymmetric — heavy positive drive, light negative (tube-ish) */
            return x > 0.0 ? std::tanh (d * x)
                           : std::tanh (d * 0.35 * x);
        case 6: /* Bit crush — quantise to 2..16 bits */
            {
                const int bits = (int) juce::jlimit (1.0, 16.0, 17.0 - d * 0.32);
                const double levels = std::pow (2.0, bits) - 1.0;
                return std::round (x * levels) / levels;
            }
        default: return x;
    }
}


/* ===========================================================================
 * Shared generator-wave shape function.  `frac` is the phase ∈ [0,1).
 * Returns y ∈ [-1, +1].  Used by both the FX page's Generate effect
 * and the small live-preview canvas next to the wave combo. */
inline double genWaveSample (int kind, double frac)
{
    switch (kind)
    {
        case 1: /* Sine */
            return std::sin (frac * juce::MathConstants<double>::twoPi);
        case 2: /* Square */
            return frac < 0.5 ? 1.0 : -1.0;
        case 3: /* Triangle */
            return frac < 0.5 ? (-1.0 + 4.0 * frac)
                              : ( 3.0 - 4.0 * frac);
        case 4: /* Saw up */
            return 2.0 * frac - 1.0;
        case 5: /* Saw down */
            return 1.0 - 2.0 * frac;
        case 6: /* Pulse 25% */
            return frac < 0.25 ? 1.0 : -1.0;
        case 7: /* Pulse 12.5% */
            return frac < 0.125 ? 1.0 : -1.0;
        case 8: /* Half-sine (|sin|) */
            return 2.0 * std::abs (std::sin (frac * juce::MathConstants<double>::pi)) - 1.0;
        case 9: /* Quarter-sine (positive half) */
            return frac < 0.5
                   ? std::sin (frac * 2.0 * juce::MathConstants<double>::pi)
                   : -1.0;
        case 10: /* White noise */
            return (juce::Random::getSystemRandom().nextDouble() * 2.0) - 1.0;
        default: return 0.0;
    }
}


/* ===========================================================================
 * WaveShaperCanvas — draws the input→output transfer function of the
 * currently-selected shaper at the active drive.  Diagonal grid for x=y
 * reference; the shaped curve drawn over it. */
class WaveShaperCanvas : public Component
{
public:
    void setParams (int kind, double drive)
    {
        if (kind == kind_ && std::abs (drive - drive_) < 1e-6) return;
        kind_ = kind; drive_ = drive;
        repaint();
    }

    void paint (Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat().reduced (2.0f);
        g.setColour (Colour { 0xff'10'10'10 });
        g.fillRect (bounds);
        g.setColour (Colour { 0xff'2a'2a'2a });
        g.drawRect (bounds, 1.0f);

        const float midX = bounds.getCentreX();
        const float midY = bounds.getCentreY();
        g.setColour (Colour { 0xff'2a'2a'2a });
        g.drawHorizontalLine ((int) midY, bounds.getX(), bounds.getRight());
        g.drawVerticalLine   ((int) midX, bounds.getY(), bounds.getBottom());
        /* Diagonal x=y reference. */
        g.setColour (Colour { 0xff'1f'1f'1f });
        g.drawLine (bounds.getX(), bounds.getBottom(), bounds.getRight(), bounds.getY(), 1.0f);

        const int   w = juce::roundToInt (bounds.getWidth());
        const float halfW = bounds.getWidth()  * 0.5f;
        const float halfH = bounds.getHeight() * 0.5f;
        if (w <= 0) return;

        Path curve;
        bool started = false;
        for (int x = 0; x < w; ++x)
        {
            const double xn = (x / (double) w) * 2.0 - 1.0;   /* -1..+1 */
            const double yn = waveShaperSample (kind_, drive_, xn);
            const float xpx = bounds.getX() + x;
            const float ypx = midY - (float) yn * halfH;
            if (! started) { curve.startNewSubPath (xpx, ypx); started = true; }
            else            curve.lineTo (xpx, ypx);
        }
        g.setColour (Colour { 0xff'd0'80'40 });
        g.strokePath (curve, PathStrokeType (1.5f));
        juce::ignoreUnused (halfW);
    }

private:
    int    kind_ = 1;
    double drive_ = 1.0;
};


/* ===========================================================================
 * WavePreviewCanvas — small visualisation of the currently-selected
 * generator wave at the chosen cycles + amplitude.  Renders pure DSP
 * — no audio, no sample data — so it's free to repaint cheaply when
 * any of the inputs change. */
class WavePreviewCanvas : public Component
{
public:
    void setParams (int kind, double cycles, double amplitude)
    {
        if (kind == kind_ && std::abs (cycles - cycles_) < 1e-6
            && std::abs (amplitude - amp_) < 1e-6)
            return;
        kind_ = kind; cycles_ = cycles; amp_ = amplitude;
        repaint();
    }

    /** Tell the preview what the waveshaper would do.  When `kind`
     *  is > 0 the canvas overlays a second trace showing the
     *  generator output after passing through the shaper. */
    void setShaperParams (int kind, double drive, double mix)
    {
        if (kind == shaperKind_ && std::abs (drive - shaperDrive_) < 1e-6
            && std::abs (mix - shaperMix_) < 1e-6)
            return;
        shaperKind_ = kind; shaperDrive_ = drive; shaperMix_ = mix;
        repaint();
    }

    void paint (Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat().reduced (2.0f);
        g.setColour (Colour { 0xff'10'10'10 });
        g.fillRect (bounds);
        g.setColour (Colour { 0xff'2a'2a'2a });
        g.drawRect (bounds, 1.0f);

        const float midY  = bounds.getCentreY();
        const float halfH = bounds.getHeight() * 0.45f;
        g.setColour (Colour { 0xff'2a'2a'2a });
        g.drawHorizontalLine ((int) midY, bounds.getX(), bounds.getRight());

        const int w = juce::roundToInt (bounds.getWidth());
        if (w <= 0 || cycles_ <= 0.0) return;

        const float gain = juce::jlimit (0.0f, 1.0f, (float) amp_);
        const bool shaperOn = shaperKind_ > 0 && shaperMix_ > 0.0;

        /* Pass 1: raw generator trace.  Faint when the shaper is on
         * (acts as a reference), full when it's off. */
        Path raw;
        bool started = false;
        for (int x = 0; x < w; ++x)
        {
            const double phase = double (x) / double (w) * cycles_;
            const double frac  = phase - std::floor (phase);
            const double y = genWaveSample (kind_, frac) * gain;
            const float ypx = midY - (float) y * halfH;
            if (! started) { raw.startNewSubPath (bounds.getX() + x, ypx); started = true; }
            else            raw.lineTo            (bounds.getX() + x, ypx);
        }
        g.setColour (shaperOn
                       ? Colour { 0xff'5a'a5'd0 }.withAlpha (0.35f)
                       : Colour { 0xff'5a'a5'd0 });
        g.strokePath (raw, PathStrokeType (1.5f));

        /* Pass 2: post-shaper trace (only when shaper engaged). */
        if (shaperOn)
        {
            Path shaped;
            started = false;
            for (int x = 0; x < w; ++x)
            {
                const double phase = double (x) / double (w) * cycles_;
                const double frac  = phase - std::floor (phase);
                const double rawY  = genWaveSample (kind_, frac) * gain;
                const double shapedY = waveShaperSample (shaperKind_, shaperDrive_, rawY);
                const double mixed = rawY * (1.0 - shaperMix_) + shapedY * shaperMix_;
                const float ypx = midY - (float) mixed * halfH;
                if (! started) { shaped.startNewSubPath (bounds.getX() + x, ypx); started = true; }
                else            shaped.lineTo            (bounds.getX() + x, ypx);
            }
            g.setColour (Colour { 0xff'd0'80'40 });
            g.strokePath (shaped, PathStrokeType (1.8f));
        }
    }

private:
    int    kind_   = 1;
    double cycles_ = 1.0;
    double amp_    = 1.0;
    int    shaperKind_  = 0;     /* 0 = no shaper overlay */
    double shaperDrive_ = 1.0;
    double shaperMix_   = 0.0;
};


/* ===========================================================================
 * SamplerFXPage — sample-DSP effects panel.  Each effect operates on the
 * currently-selected range of the slot's buffer (or the whole sample if
 * no selection).  Algorithms ported from ft2-clone's
 * ft2_sample_ed_features.c (BSD-3) — math kept, SDL UI dropped.
 *
 * Effects implemented:
 *   Amplify   (gain × samples)
 *   Normalize (peak-scale to 1.0)
 *   LP filter (single-pole, normalised cutoff 0..1)
 *   HP filter (single-pole, normalised cutoff 0..1)
 *   Generator (sine / square / triangle / saw — replace or mix)
 *   X-Fade    (cross-fade loop boundary smoothing)
 *   Reverse   (flip selection)
 * ========================================================================*/
class SamplerFXPage : public Component
{
public:
    using GetSlot  = std::function<SamplerSampleSlot*()>;
    using OnChange = std::function<void()>;

    SamplerFXPage (GetSlot gs, OnChange cb)
        : getSlot (std::move (gs)),
          onChange (std::move (cb))
    {
        /* === Volume section. */
        configureSectionHeader (volSection_,  "Volume");
        configureBtn (amplifyBtn,   "Amplify",   [this] { applyAmplify(); });
        configureBtn (normalizeBtn, "Normalize", [this] { applyNormalize(); });
        configureBtn (reverseBtn,   "Reverse",   [this] { applyReverse(); });
        configureSlider (gainSlider, 0.0, 400.0, 0.1, "%");
        gainSlider.setValue (100.0);
        configureLabel (gainLbl, "Gain");

        /* === Filter section. */
        configureSectionHeader (filterSection_, "Filter");
        configureBtn (lpBtn, "Low-pass",  [this] { applyFilter (true);  });
        configureBtn (hpBtn, "High-pass", [this] { applyFilter (false); });
        configureSlider (cutoffSlider, 0.001, 1.0, 0.001, "norm");
        cutoffSlider.setValue (0.25);
        configureLabel (cutoffLbl, "Cutoff");

        /* === Generator section. */
        configureSectionHeader (genSection_, "Generator");
        configureBtn (genBtn, "Generate", [this] { applyGenerator(); });
        configureSlider (cyclesSlider,    0.5, 64.0,  0.5, "cyc");
        cyclesSlider.setValue (1.0);
        configureSlider (amplitudeSlider, 0.0, 100.0, 0.1, "%");
        amplitudeSlider.setValue (100.0);
        configureLabel (cyclesLbl, "Cycles");
        configureLabel (amplLbl,   "Amplitude");

        genWaveCombo.addItem ("Sine",            1);
        genWaveCombo.addItem ("Square",          2);
        genWaveCombo.addItem ("Triangle",        3);
        genWaveCombo.addItem ("Saw up",          4);
        genWaveCombo.addItem ("Saw down",        5);
        genWaveCombo.addItem ("Pulse 25%",       6);
        genWaveCombo.addItem ("Pulse 12.5%",     7);
        genWaveCombo.addItem ("Half-sine",       8);
        genWaveCombo.addItem ("Quarter-sine",    9);
        genWaveCombo.addItem ("Noise",          10);
        genWaveCombo.setSelectedId (1, dontSendNotification);
        genWaveCombo.setColour (ComboBox::backgroundColourId, Colour { 0xff'24'24'24 });
        genWaveCombo.setColour (ComboBox::textColourId,        Colour { 0xff'd0'd0'd0 });
        genWaveCombo.onChange = [this] { refreshWavePreview(); };
        addAndMakeVisible (genWaveCombo);

        cyclesSlider   .onValueChange = [this] { refreshWavePreview(); };
        amplitudeSlider.onValueChange = [this] { refreshWavePreview(); };
        addAndMakeVisible (wavePreview_);
        refreshWavePreview();

        genMixToggle.setButtonText ("Mix (vs replace)");
        genMixToggle.setColour (ToggleButton::textColourId, Colour { 0xff'b0'b0'b0 });
        addAndMakeVisible (genMixToggle);

        /* === Waveshaper section. */
        configureSectionHeader (shaperSection_, "Waveshaper");
        configureBtn (shaperBtn, "Apply shape", [this] { applyWaveshaper(); });
        configureSlider (shaperDriveSlider, 0.0, 100.0, 0.1, "%");
        shaperDriveSlider.setValue (20.0);
        configureSlider (shaperMixSlider,   0.0, 100.0, 0.1, "%");
        shaperMixSlider.setValue (100.0);
        configureLabel (shaperDriveLbl, "Drive");
        configureLabel (shaperMixLbl,   "Wet");

        shaperCombo.addItem ("Tanh",       1);
        shaperCombo.addItem ("Soft clip",  2);
        shaperCombo.addItem ("Hard clip",  3);
        shaperCombo.addItem ("Wave fold",  4);
        shaperCombo.addItem ("Asymmetric", 5);
        shaperCombo.addItem ("Bit crush",  6);
        shaperCombo.setSelectedId (1, dontSendNotification);
        shaperCombo.setColour (ComboBox::backgroundColourId, Colour { 0xff'24'24'24 });
        shaperCombo.setColour (ComboBox::textColourId,        Colour { 0xff'd0'd0'd0 });
        shaperCombo.onChange = [this] { refreshShaperPreview(); };
        addAndMakeVisible (shaperCombo);

        shaperDriveSlider.onValueChange = [this] { refreshShaperPreview(); };
        shaperMixSlider  .onValueChange = [this] { refreshShaperPreview(); };
        addAndMakeVisible (shaperPreview_);
        refreshShaperPreview();

        /* === Loop section. */
        configureSectionHeader (loopSection_, "Loop");
        configureBtn (xfadeBtn, "X-Fade loop", [this] { applyXFade(); });
        configureSlider (xfadeLenSlider, 1.0, 4096.0, 1.0, "smp");
        xfadeLenSlider.setValue (64.0);
        configureLabel (xfadeLenLbl, "X-Fade len");

        /* Status footer. */
        statusLbl.setText ("FX operate on the selection (or whole sample if no selection).",
                           dontSendNotification);
        statusLbl.setJustificationType (Justification::centredLeft);
        statusLbl.setColour (Label::textColourId, Colour { 0xff'7a'7a'7a });
        statusLbl.setFont (FontOptions (11.0f, Font::italic));
        addAndMakeVisible (statusLbl);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (8);

        constexpr int kLblW   = 90;
        constexpr int kBtnW   = 110;
        constexpr int kRowH   = 26;
        constexpr int kHdrH   = 18;
        constexpr int kGap    = 4;
        constexpr int kSect   = 10;
        constexpr int kPad    = 6;

        auto slotRow = [&] (const Label& lbl, Slider& s, TextButton& btn) {
            auto rr = r.removeFromTop (kRowH);
            const_cast<Label&> (lbl).setBounds (rr.removeFromLeft (kLblW));
            btn.setBounds (rr.removeFromRight (kBtnW));
            rr.removeFromRight (kPad);
            s.setBounds (rr);
            r.removeFromTop (kGap);
        };
        auto buttonRow = [&] (std::initializer_list<TextButton*> btns) {
            auto rr = r.removeFromTop (kRowH);
            rr.removeFromLeft (kLblW);
            for (auto* b : btns) {
                b->setBounds (rr.removeFromRight (kBtnW));
                rr.removeFromRight (kPad);
            }
            r.removeFromTop (kGap);
        };
        auto sectionLayout = [&] (Label& hdr) {
            hdr.setBounds (r.removeFromTop (kHdrH));
            r.removeFromTop (2);
        };
        auto sectionGap = [&] { r.removeFromTop (kSect); };

        /* === Volume. */
        sectionLayout (volSection_);
        slotRow (gainLbl, gainSlider, amplifyBtn);
        buttonRow ({ &reverseBtn, &normalizeBtn });
        sectionGap();

        /* === Filter. */
        sectionLayout (filterSection_);
        slotRow (cutoffLbl, cutoffSlider, lpBtn);
        buttonRow ({ &hpBtn });
        sectionGap();

        /* === Generator. */
        sectionLayout (genSection_);
        {
            auto rr = r.removeFromTop (kRowH);
            rr.removeFromLeft (kLblW);
            genWaveCombo.setBounds (rr.removeFromLeft (140));
            rr.removeFromLeft (8);
            genMixToggle.setBounds (rr.removeFromLeft (160));
            r.removeFromTop (kGap);
        }
        {
            const int previewH = juce::jmax (60, r.getHeight() * 16 / 100);
            auto rr = r.removeFromTop (previewH);
            rr.removeFromLeft (kLblW);
            rr.removeFromRight (kBtnW + kPad);
            wavePreview_.setBounds (rr);
            r.removeFromTop (kGap);
        }
        slotRow (cyclesLbl, cyclesSlider, genBtn);
        slotRow (amplLbl,   amplitudeSlider, dummyHiddenBtn_);   /* no per-row button */
        dummyHiddenBtn_.setVisible (false);
        sectionGap();

        /* === Waveshaper. */
        sectionLayout (shaperSection_);
        {
            auto rr = r.removeFromTop (kRowH);
            rr.removeFromLeft (kLblW);
            shaperCombo.setBounds (rr.removeFromLeft (140));
            r.removeFromTop (kGap);
        }
        {
            const int previewH = juce::jmax (70, r.getHeight() * 22 / 100);
            auto rr = r.removeFromTop (previewH);
            rr.removeFromLeft (kLblW);
            rr.removeFromRight (kBtnW + kPad);
            shaperPreview_.setBounds (rr);
            r.removeFromTop (kGap);
        }
        slotRow (shaperDriveLbl, shaperDriveSlider, shaperBtn);
        slotRow (shaperMixLbl,   shaperMixSlider,   dummyHiddenBtn_);
        sectionGap();

        /* === Loop. */
        sectionLayout (loopSection_);
        slotRow (xfadeLenLbl, xfadeLenSlider, xfadeBtn);
        sectionGap();

        /* === Footer. */
        auto foot = r.removeFromBottom (20);
        statusLbl.setBounds (foot);
    }

private:
    void configureBtn (TextButton& b, const String& t, std::function<void()> on)
    {
        b.setButtonText (t);
        b.onClick = std::move (on);
        b.setColour (TextButton::buttonColourId,    Colour { 0xff'24'24'24 });
        b.setColour (TextButton::textColourOffId,   Colour { 0xff'd0'd0'd0 });
        addAndMakeVisible (b);
    }
    void configureSlider (Slider& s, double lo, double hi, double step, const String& suffix)
    {
        s.setSliderStyle (Slider::LinearBar);
        s.setRange (lo, hi, step);
        s.setTextValueSuffix ("  " + suffix);
        s.setColour (Slider::backgroundColourId, Colour { 0xff'24'24'24 });
        s.setColour (Slider::trackColourId,      Colour { 0xff'4a'7a'b5 });
        addAndMakeVisible (s);
    }

    /** Operate on the active selection if any; otherwise whole sample. */
    bool getRange (int& a, int& b)
    {
        auto* s = getSlot ? getSlot() : nullptr;
        if (s == nullptr || ! s->isLoaded()) return false;
        a = 0;
        b = s->numSamples;
        if (selStart_ >= 0 && selEnd_ > selStart_)
        {
            a = juce::jmax (0, selStart_);
            b = juce::jmin (s->numSamples, selEnd_);
        }
        return b > a;
    }

    /** External link — called by the editor when the user selects a
     *  range in the Sample page.  FX page then operates on that range. */
public:
    void setSelection (int a, int b) { selStart_ = a; selEnd_ = b; }
private:

    void mutateInt16 (std::function<int16_t (int16_t)> fn,
                      std::function<int16_t (int16_t)> fnR = {})
    {
        auto* s = getSlot ? getSlot() : nullptr;
        int a, b;
        if (! getRange (a, b)) return;
        for (int i = a; i < b; ++i)
            s->data16L[i] = fn (s->data16L[i]);
        if (s->isStereo && s->data16R)
            for (int i = a; i < b; ++i)
                s->data16R[i] = (fnR ? fnR : fn) (s->data16R[i]);
        notifyChange();
    }

    void applyAmplify()
    {
        const double g = gainSlider.getValue() / 100.0;
        mutateInt16 ([g] (int16_t v) {
            const int x = (int) std::lround (v * g);
            return (int16_t) juce::jlimit (-32768, 32767, x);
        });
    }

    void applyNormalize()
    {
        auto* s = getSlot ? getSlot() : nullptr;
        int a, b;
        if (! getRange (a, b)) return;
        int peak = 1;
        for (int i = a; i < b; ++i)
            peak = juce::jmax (peak, (int) std::abs ((int) s->data16L[i]));
        if (s->isStereo && s->data16R)
            for (int i = a; i < b; ++i)
                peak = juce::jmax (peak, (int) std::abs ((int) s->data16R[i]));
        if (peak <= 0) return;
        const double target = 32767.0 * (amplitudeSlider.getValue() / 100.0);
        const double g = target / (double) peak;
        mutateInt16 ([g] (int16_t v) {
            const int x = (int) std::lround (v * g);
            return (int16_t) juce::jlimit (-32768, 32767, x);
        });
    }

    void applyReverse()
    {
        auto* s = getSlot ? getSlot() : nullptr;
        int a, b;
        if (! getRange (a, b)) return;
        for (int i = a, j = b - 1; i < j; ++i, --j)
        {
            std::swap (s->data16L[i], s->data16L[j]);
            if (s->isStereo && s->data16R)
                std::swap (s->data16R[i], s->data16R[j]);
        }
        notifyChange();
    }

    /** Single-pole IIR filter.  cutoff = normalised 0..1 (1 = nyquist).
     *  y[n] = y[n-1] + alpha * (x[n] - y[n-1])  — LP.
     *  HP form: y[n] = a * (y[n-1] + x[n] - x[n-1]). */
    void applyFilter (bool lowPass)
    {
        auto* s = getSlot ? getSlot() : nullptr;
        int a, b;
        if (! getRange (a, b)) return;
        const double cutoff = juce::jlimit (0.001, 1.0, cutoffSlider.getValue());
        const double alpha  = cutoff;

        auto runLP = [alpha] (int16_t* d, int from, int to) {
            double y = (double) d[from];
            for (int i = from; i < to; ++i)
            {
                y += alpha * ((double) d[i] - y);
                d[i] = (int16_t) juce::jlimit (-32768, 32767, (int) std::lround (y));
            }
        };
        auto runHP = [alpha] (int16_t* d, int from, int to) {
            double y = 0.0, prev = (double) d[from];
            const double aCoef = 1.0 - alpha;
            for (int i = from; i < to; ++i)
            {
                const double x = (double) d[i];
                y = aCoef * (y + x - prev);
                prev = x;
                d[i] = (int16_t) juce::jlimit (-32768, 32767, (int) std::lround (y));
            }
        };
        if (lowPass)
        {
            runLP (s->data16L.get(), a, b);
            if (s->isStereo && s->data16R) runLP (s->data16R.get(), a, b);
        }
        else
        {
            runHP (s->data16L.get(), a, b);
            if (s->isStereo && s->data16R) runHP (s->data16R.get(), a, b);
        }
        notifyChange();
    }

    void applyGenerator()
    {
        auto* s = getSlot ? getSlot() : nullptr;
        int a, b;
        if (! getRange (a, b)) return;
        const int   length = b - a;
        const double cycles = cyclesSlider.getValue();
        const double amp    = (amplitudeSlider.getValue() / 100.0) * 32767.0;
        const bool mix      = genMixToggle.getToggleState();
        const int wave      = genWaveCombo.getSelectedId();   /* 1..4 */

        auto sampleAt = [&] (int i) -> int16_t {
            const double phase = (double) i / (double) length * cycles;
            const double frac  = phase - std::floor (phase);
            const double y = genWaveSample (wave, frac);
            return (int16_t) juce::jlimit (-32768, 32767,
                                            (int) std::lround (y * amp));
        };

        for (int i = a; i < b; ++i)
        {
            const int16_t g = sampleAt (i - a);
            if (mix)
            {
                const int sum = (int) s->data16L[i] + (int) g;
                s->data16L[i] = (int16_t) juce::jlimit (-32768, 32767, sum);
                if (s->isStereo && s->data16R)
                {
                    const int sumR = (int) s->data16R[i] + (int) g;
                    s->data16R[i] = (int16_t) juce::jlimit (-32768, 32767, sumR);
                }
            }
            else
            {
                s->data16L[i] = g;
                if (s->isStereo && s->data16R) s->data16R[i] = g;
            }
        }
        notifyChange();
    }

    /** X-fade the loop boundary.  Linear cross-fade across N samples
     *  around loopStart and loopStart+loopLength using neighbours. */
    void applyXFade()
    {
        auto* s = getSlot ? getSlot() : nullptr;
        if (s == nullptr || ! s->isLoaded()) return;
        if (s->loopLength <= 0) return;
        const int N = juce::jlimit (1, juce::jmin (s->loopLength, 4096),
                                     (int) xfadeLenSlider.getValue());
        const int loopEnd = s->loopStart + s->loopLength;
        const int leftIn  = juce::jmax (0, s->loopStart - N / 2);
        const int rightIn = juce::jmin (s->numSamples, loopEnd + N / 2);

        auto fade = [&] (int16_t* d) {
            for (int i = 0; i < N && (loopEnd + i) < s->numSamples; ++i)
            {
                const double t = (double) i / (double) N;
                const int srcA = loopEnd + i;
                const int srcB = s->loopStart + i;
                const int x = (int) std::lround (d[srcA] * (1.0 - t) + d[srcB] * t);
                d[srcA] = (int16_t) juce::jlimit (-32768, 32767, x);
            }
        };
        fade (s->data16L.get());
        if (s->isStereo && s->data16R) fade (s->data16R.get());
        juce::ignoreUnused (leftIn, rightIn);
        notifyChange();
    }

    void notifyChange()
    {
        if (onChange) onChange();
    }

    GetSlot  getSlot;
    OnChange onChange;
    int      selStart_ = -1, selEnd_ = -1;

    void refreshWavePreview()
    {
        const double drive = 1.0 + shaperDriveSlider.getValue() / 100.0 * 40.0;
        const double mix   = shaperMixSlider.getValue() / 100.0;
        wavePreview_.setParams (genWaveCombo.getSelectedId(),
                                cyclesSlider.getValue(),
                                amplitudeSlider.getValue() / 100.0);
        wavePreview_.setShaperParams (shaperCombo.getSelectedId(), drive, mix);
    }
    void refreshShaperPreview()
    {
        const double drive = 1.0 + shaperDriveSlider.getValue() / 100.0 * 40.0;
        shaperPreview_.setParams (shaperCombo.getSelectedId(), drive);
        /* Shaper changes also reshape the generator preview output. */
        refreshWavePreview();
    }

    /** Apply the selected waveshaper to the active range. */
    void applyWaveshaper()
    {
        const int kind = shaperCombo.getSelectedId();
        const double drive = 1.0 + shaperDriveSlider.getValue() / 100.0 * 40.0;
        const double mix   = shaperMixSlider.getValue() / 100.0;
        mutateInt16 ([kind, drive, mix] (int16_t v) {
            const double x = v / 32767.0;
            const double y = waveShaperSample (kind, drive, x);
            const double mixed = x * (1.0 - mix) + y * mix;
            return (int16_t) juce::jlimit (-32768, 32767,
                                            (int) std::lround (mixed * 32767.0));
        });
    }

    /* === look helpers ============================================== */
    void configureSectionHeader (Label& l, const String& t)
    {
        l.setText (t, dontSendNotification);
        l.setColour (Label::textColourId, Colour { 0xff'8a'b5'd4 });
        l.setFont (FontOptions (13.0f, Font::bold));
        addAndMakeVisible (l);
    }
    void configureLabel (Label& l, const String& t)
    {
        l.setText (t, dontSendNotification);
        l.setColour (Label::textColourId, Colour { 0xff'b0'b0'b0 });
        l.setFont (FontOptions (12.0f, Font::plain));
        l.setJustificationType (Justification::centredLeft);
        addAndMakeVisible (l);
    }

    /* Section labels. */
    Label volSection_, filterSection_, genSection_, shaperSection_, loopSection_;

    /* Action buttons. */
    TextButton amplifyBtn, normalizeBtn, reverseBtn, lpBtn, hpBtn, genBtn, xfadeBtn;
    TextButton shaperBtn;
    TextButton dummyHiddenBtn_;   /* placeholder for slotRow without a button */

    /* Sliders. */
    Slider     gainSlider, cutoffSlider, cyclesSlider, amplitudeSlider, xfadeLenSlider;
    Slider     shaperDriveSlider, shaperMixSlider;

    /* Combos / toggles. */
    ComboBox   genWaveCombo, shaperCombo;
    ToggleButton genMixToggle;

    /* Preview canvases. */
    WavePreviewCanvas  wavePreview_;
    WaveShaperCanvas   shaperPreview_;

    /* Labels. */
    Label gainLbl, cutoffLbl, cyclesLbl, amplLbl, xfadeLenLbl, statusLbl;
    Label shaperDriveLbl, shaperMixLbl;
};


/* ===========================================================================
 * SamplerNodeEditor — paged container
 *   - Top nav: Bank | Inst | Sample | FX  +  instrument selector
 *   - Body: one of four pages, switched by the nav tabs
 *
 *   Bank   — slot list, per-slot params, waveform view (existing).
 *   Inst   — full envelope editor + keymap + per-instrument extras.
 *   Sample — sample editor (waveform + range + cut/copy/paste). [F2]
 *   FX     — sample effects (lp/hp/resonance/generators/normalize). [F3]
 * ========================================================================*/
class SamplerNodeEditor : public AudioProcessorEditor,
                          private Timer,
                          private ListBoxModel
{
public:
    enum class Page { kBank = 0, kInst, kSample, kFX };

    explicit SamplerNodeEditor (SamplerNode& s) : AudioProcessorEditor (s), node (s),
        waveformView ([this] { return currentSlot(); },
                      [this] (const SamplerSampleSlot* slot) {
                          return node.collectPlayheadsForSlot (slot);
                      }),
        instPage_   ([this] { return node.getInstrument (activeInstrument); },
                     [this] { return activeSlot; },
                     [this] (int s) {
                         activeSlot = juce::jlimit (0, SamplerInstrument::kNumSlots - 1, s);
                         slotList.selectRow (activeSlot, true, false);
                         refresh();
                     }),
        samplePage_ ([this] { return currentSlot(); },
                     [this] {
                         waveformView.repaint();
                         slotList.repaint();
                     }),
        fxPage_     ([this] { return currentSlot(); },
                     [this] {
                         waveformView.repaint();
                         slotList.repaint();
                     })
    {
        /* === Nav tabs (Bank / Inst / Sample / FX). === */
        auto setupNav = [this] (TextButton& b, const String& text, Page p) {
            b.setButtonText (text);
            b.setClickingTogglesState (true);
            b.setRadioGroupId (0xb01);
            b.setColour (TextButton::buttonColourId,    Colour { 0xff'24'24'24 });
            b.setColour (TextButton::buttonOnColourId,  Colour { 0xff'4a'7a'b5 });
            b.setColour (TextButton::textColourOffId,   Colour { 0xff'b0'b0'b0 });
            b.setColour (TextButton::textColourOnId,    Colours::white);
            b.onClick = [this, p] { switchPage (p); };
            addAndMakeVisible (b);
        };
        setupNav (bankNavBtn,   "Bank",   Page::kBank);
        setupNav (instNavBtn,   "Inst",   Page::kInst);
        setupNav (sampleNavBtn, "Sample", Page::kSample);
        setupNav (fxNavBtn,     "FX",     Page::kFX);
        bankNavBtn.setToggleState (true, dontSendNotification);

        addChildComponent (instPage_);
        addChildComponent (samplePage_);
        samplePage_.setPlayheadQuery ([this] (const SamplerSampleSlot* slot) {
            return node.collectPlayheadsForSlot (slot);
        });
        addChildComponent (fxPage_);
        /* Bank label -- read-only display of "NNN <name>".  Updated
         * by refreshInstLabel() on every refresh() tick + on nav
         * presses.  Matches the tracker editor's bank-display pattern. */
        instLabel.setJustificationType (Justification::centredLeft);
        instLabel.setFont (FontOptions (Font::getDefaultMonospacedFontName(),
                                        13.0f, Font::plain));
        instLabel.setColour (Label::backgroundColourId, Colour { 0xff'24'24'24 });
        instLabel.setColour (Label::textColourId,       Colour { 0xff'd0'd0'd0 });
        instLabel.setColour (Label::outlineColourId,    Colour { 0xff'3a'3a'3a });
        instLabel.setBorderSize ({ 0, 6, 0, 6 });
        addAndMakeVisible (instLabel);

        /* Prev / next nav -- step through the 128-bank grid the same
         * way Disk Op's instrument list does.  Lazy-allocate when
         * advancing past the current count so the user can populate
         * any of the 128 banks. */
        auto navigate = [this] (int delta) {
            const int target = juce::jlimit (0, SamplerNode::kMaxInstruments - 1,
                                             activeInstrument + delta);
            if (target == activeInstrument) return;
            while (target >= node.getNumInstruments())
                if (node.addInstrument() == nullptr) break;
            if (target >= node.getNumInstruments()) return;
            activeInstrument = target;
            rebuildEnvelopeViews();
            refresh();
        };
        instPrevBtn.setButtonText ("<");
        instPrevBtn.onClick = [navigate] { navigate (-1); };
        addAndMakeVisible (instPrevBtn);

        instNextBtn.setButtonText (">");
        instNextBtn.onClick = [navigate] { navigate (+1); };
        addAndMakeVisible (instNextBtn);

        instAddBtn.setButtonText ("+");
        instAddBtn.onClick = [this] {
            node.addInstrument();
            activeInstrument = node.getNumInstruments() - 1;
            rebuildEnvelopeViews();
            refresh();
        };
        addAndMakeVisible (instAddBtn);

        instDelBtn.setButtonText ("-");
        instDelBtn.onClick = [this] {
            if (node.getNumInstruments() <= 1) return;
            node.removeInstrument (activeInstrument);
            if (activeInstrument >= node.getNumInstruments())
                activeInstrument = node.getNumInstruments() - 1;
            rebuildEnvelopeViews();
            refresh();
        };
        addAndMakeVisible (instDelBtn);

        slotList.setModel (this);
        slotList.setRowHeight (24);
        slotList.setColour (ListBox::backgroundColourId, Colour { 0xff'14'14'14 });
        slotList.setColour (ListBox::outlineColourId,    Colour { 0xff'2a'2a'2a });
        slotList.setOutlineThickness (1);
        addAndMakeVisible (slotList);

        /* "Load from Disk Op" removed -- it was a one-shot single-
         * sample loader that didn't surface which file was about to
         * land, and is redundant with the Disk Op panel's own
         * load-into-active-slot affordance. */

        clearBtn.setButtonText ("Clear");
        clearBtn.onClick = [this] {
            if (auto inst = node.getInstrument (activeInstrument))
                inst->clearSlot (activeSlot);
            refresh();
        };
        addAndMakeVisible (clearBtn);

        configureParamSlider (rootSlider, "Root", 0.0, 127.0, 1.0,
            [this](double v) { if (auto* s = currentSlot()) s->rootNote = (int) v; });
        configureParamSlider (relSlider,  "Rel",  -96.0, 96.0, 1.0,
            [this](double v) { if (auto* s = currentSlot()) s->relativeNote = (int) v; });
        configureParamSlider (fineSlider, "Fine", -128.0, 127.0, 1.0,
            [this](double v) { if (auto* s = currentSlot()) s->finetune = (int) v; });
        configureParamSlider (volSlider, "Vol",   0.0, 1.0, 0.001,
            [this](double v) { if (auto* s = currentSlot()) s->volume = (float) v; });
        configureParamSlider (panSlider, "Pan",   0.0, 1.0, 0.001,
            [this](double v) { if (auto* s = currentSlot()) s->panning = (float) v; });

        /* Per-bus master gain sliders — 4 of them, pinned under the
         * preview.  Drive SamplerNode::busGain[] directly; voices read
         * the gain on each render block. */
        for (int b = 0; b < SamplerNode::kNumBuses; ++b)
        {
            configureParamSlider (busGainSliders[b],
                                  "Bus " + String (b + 1),
                                  0.0, 2.0, 0.001,
                                  [this, b] (double v) {
                                      node.busGain[b] = (float) v;
                                  });
        }

        loopCombo.addItem ("Loop: Off",      1);
        loopCombo.addItem ("Loop: Forward",  2);
        loopCombo.addItem ("Loop: Ping-pong", 3);
        loopCombo.onChange = [this] {
            if (auto* s = currentSlot())
                s->loopMode = (SamplerLoopMode) (loopCombo.getSelectedId() - 1);
            waveformView.repaint();
        };
        /* loopCombo is no longer surfaced on Bank — loop mode + range
         * editing lives on the Sample page (SamplerSamplePage::loopRadio
         * + canvas).  Object kept for reuse in case Bank-side quick-loop
         * comes back; not added to the page. */

        addAndMakeVisible (waveformView);

        /* Envelope tabs (Vol / Pan / AutoVib) live on the Inst page
         * (SamplerInstPage) — the duplicate set on Bank was confusing,
         * so we leave the tab content components wired (for state
         * round-trip across page switches) but don't surface the tab
         * strip on Bank.  Bank focuses on the slot grid + sample
         * preview waveform; per-slot params row above stays. */
        tabBar.addTab ("Vol",     Colour { 0xff'18'18'18 }, &volEnvWrap,    false);
        tabBar.addTab ("Pan",     Colour { 0xff'18'18'18 }, &panEnvWrap,    false);
        tabBar.addTab ("AutoVib", Colour { 0xff'18'18'18 }, &autoVibWrap,   false);
        tabBar.setTabBarDepth (22);

        /* Add / Del envelope-point buttons inside the Bank-page Vol/Pan
         * tabs.  Shared logic with the Inst page so envelopes are
         * editable from either place. */
        auto setupEnvBtn = [] (TextButton& b, const String& t) {
            b.setButtonText (t);
            b.setColour (TextButton::buttonColourId, Colour { 0xff'24'24'24 });
            b.setColour (TextButton::textColourOffId, Colour { 0xff'd0'd0'd0 });
        };
        setupEnvBtn (volTabAddBtn, "Add"); setupEnvBtn (volTabDelBtn, "Del");
        setupEnvBtn (panTabAddBtn, "Add"); setupEnvBtn (panTabDelBtn, "Del");
        volTabAddBtn.onClick = [this] { addEnvPointBank (true);  };
        volTabDelBtn.onClick = [this] { delEnvPointBank (true);  };
        panTabAddBtn.onClick = [this] { addEnvPointBank (false); };
        panTabDelBtn.onClick = [this] { delEnvPointBank (false); };

        configureParamSlider (fadeoutSlider, "Fade", 0.0, 4095.0, 1.0,
            [this](double v) {
                if (auto inst = node.getInstrument (activeInstrument))
                    inst->fadeoutRate = (uint16_t) v;
            });
        autoVibWrap.addAndMakeVisible (fadeoutSlider);

        configureParamSlider (avDepthSlider, "AV Depth", 0.0, 15.0, 1.0,
            [this](double v) {
                if (auto inst = node.getInstrument (activeInstrument))
                    inst->autoVib.depth = (uint8_t) v;
            });
        configureParamSlider (avRateSlider, "AV Rate", 0.0, 63.0, 1.0,
            [this](double v) {
                if (auto inst = node.getInstrument (activeInstrument))
                    inst->autoVib.rate = (uint8_t) v;
            });
        configureParamSlider (avSweepSlider, "AV Sweep", 0.0, 255.0, 1.0,
            [this](double v) {
                if (auto inst = node.getInstrument (activeInstrument))
                    inst->autoVib.sweep = (uint8_t) v;
            });
        avTypeCombo.addItem ("Sine", 1);
        avTypeCombo.addItem ("Square", 2);
        avTypeCombo.addItem ("Ramp Up", 3);
        avTypeCombo.addItem ("Ramp Dn", 4);
        avTypeCombo.onChange = [this] {
            if (auto inst = node.getInstrument (activeInstrument))
                inst->autoVib.type = (uint8_t) (avTypeCombo.getSelectedId() - 1);
        };
        autoVibWrap.addAndMakeVisible (avDepthSlider);
        autoVibWrap.addAndMakeVisible (avRateSlider);
        autoVibWrap.addAndMakeVisible (avSweepSlider);
        autoVibWrap.addAndMakeVisible (avTypeCombo);

        autoVibWrap.onResized = [this] {
            auto r = autoVibWrap.getLocalBounds().reduced (4);
            const int rowH = 22;
            fadeoutSlider.setBounds (r.removeFromTop (rowH)); r.removeFromTop (2);
            avDepthSlider.setBounds (r.removeFromTop (rowH)); r.removeFromTop (2);
            avRateSlider .setBounds (r.removeFromTop (rowH)); r.removeFromTop (2);
            avSweepSlider.setBounds (r.removeFromTop (rowH)); r.removeFromTop (2);
            avTypeCombo  .setBounds (r.removeFromTop (rowH));
        };

        interpCombo.addItem ("None",   1);
        interpCombo.addItem ("Linear", 2);
        interpCombo.addItem ("Cubic",  3);
        interpCombo.addItem ("Sinc16", 4);
        interpCombo.onChange = [this] {
            node.setInterpMode ((SamplerNode::InterpMode) (interpCombo.getSelectedId() - 1));
        };
        addAndMakeVisible (interpCombo);

        status.setJustificationType (Justification::centredLeft);
        status.setColour (Label::textColourId, Colour { 0xff'b0'b0'b0 });
        status.setFont (FontOptions (Font::getDefaultMonospacedFontName(), 11.0f, Font::plain));
        addAndMakeVisible (status);

        setOpaque (true);
        setSize (1040, 600);
        refreshInstLabel();
        rebuildEnvelopeViews();
        switchPage (Page::kBank);
        refresh();
        startTimerHz (8);
    }

    ~SamplerNodeEditor() override { stopTimer(); }

    void paint (Graphics& g) override
    {
        g.fillAll (Colour { 0xff'18'18'18 });
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (8);

        /* === Nav strip: Bank | Inst | Sample | FX + instrument combo. === */
        auto navRow = r.removeFromTop (26);
        const int navBtnW = 56;
        bankNavBtn  .setBounds (navRow.removeFromLeft (navBtnW)); navRow.removeFromLeft (2);
        instNavBtn  .setBounds (navRow.removeFromLeft (navBtnW)); navRow.removeFromLeft (2);
        sampleNavBtn.setBounds (navRow.removeFromLeft (navBtnW)); navRow.removeFromLeft (2);
        fxNavBtn    .setBounds (navRow.removeFromLeft (navBtnW)); navRow.removeFromLeft (12);
        instAddBtn .setBounds (navRow.removeFromRight (24));
        navRow.removeFromRight (2);
        instDelBtn .setBounds (navRow.removeFromRight (24));
        navRow.removeFromRight (8);
        instNextBtn.setBounds (navRow.removeFromRight (24));
        navRow.removeFromRight (2);
        instPrevBtn.setBounds (navRow.removeFromRight (24));
        navRow.removeFromRight (4);
        instLabel  .setBounds (navRow);
        r.removeFromTop (6);

        /* === Body area for the active page. === */
        const auto body = r;

        /* Inst / Sample / FX pages fill the body when active. */
        instPage_  .setBounds (body);
        samplePage_.setBounds (body);
        fxPage_    .setBounds (body);

        if (currentPage_ != Page::kBank)
            return;

        /* === Bank page: slot list fills full height on the left, right
         * pane gets per-slot params, large waveform, tabbed envelopes,
         * interp + status pinned at the bottom. === */
        auto bankR = body;

        /* Left column: load/clear pinned at bottom, slot list fills above. */
        /* Bank list is wider now — it hosts the per-row bus badge
         * (right edge) in addition to slot number + key range + name. */
        auto leftCol = bankR.removeFromLeft (380);
        auto leftFooter = leftCol.removeFromBottom (24);
        clearBtn.setBounds (leftFooter.removeFromRight (50));
        leftCol.removeFromBottom (6);
        slotList.setBounds (leftCol);

        bankR.removeFromLeft (12);

        /* Right column: footer (interp + status) pinned at bottom. */
        auto rightFooter = bankR.removeFromBottom (32);
        rightFooter.removeFromBottom (4);
        auto interpRow = rightFooter.removeFromTop (26);
        interpCombo.setBounds (interpRow.removeFromLeft (140));
        interpRow.removeFromLeft (8);
        status.setBounds (interpRow);

        /* Right column: per-slot param sliders at top. */
        const int rowH = 22;
        rootSlider .setBounds (bankR.removeFromTop (rowH)); bankR.removeFromTop (2);
        relSlider  .setBounds (bankR.removeFromTop (rowH)); bankR.removeFromTop (2);
        fineSlider .setBounds (bankR.removeFromTop (rowH)); bankR.removeFromTop (2);
        volSlider  .setBounds (bankR.removeFromTop (rowH)); bankR.removeFromTop (2);
        panSlider  .setBounds (bankR.removeFromTop (rowH)); bankR.removeFromTop (2);

        /* Per-bus master gain strip pinned to the BOTTOM (under the
         * preview).  Reserves rowH per bus + small gaps; preview takes
         * the rest. */
        constexpr int kBusN = SamplerNode::kNumBuses;
        const int gainsH = kBusN * rowH + (kBusN - 1) * 2 + 6;
        auto gainsR = bankR.removeFromBottom (gainsH);
        gainsR.removeFromTop (6);
        for (int b = 0; b < kBusN; ++b)
        {
            busGainSliders[b].setBounds (gainsR.removeFromTop (rowH));
            if (b + 1 < kBusN) gainsR.removeFromTop (2);
        }

        /* Waveform preview takes the rest of the right column. */
        waveformView.setBounds (bankR);
    }

    /** Switch the visible body page and update tab toggle state. */
    void switchPage (Page p)
    {
        currentPage_ = p;
        const bool bank   = p == Page::kBank;
        const bool inst   = p == Page::kInst;
        const bool sample = p == Page::kSample;
        const bool fx     = p == Page::kFX;

        bankNavBtn  .setToggleState (bank,   dontSendNotification);
        instNavBtn  .setToggleState (inst,   dontSendNotification);
        sampleNavBtn.setToggleState (sample, dontSendNotification);
        fxNavBtn    .setToggleState (fx,     dontSendNotification);

        slotList     .setVisible (bank);
        clearBtn     .setVisible (bank);
        rootSlider   .setVisible (bank);
        relSlider    .setVisible (bank);
        fineSlider   .setVisible (bank);
        volSlider    .setVisible (bank);
        panSlider    .setVisible (bank);
        for (auto& s : busGainSliders) s.setVisible (bank);
        waveformView .setVisible (bank);
        interpCombo  .setVisible (bank);
        status       .setVisible (bank);
        /* loopCombo + tabBar are no longer added to the Bank page — see
         * note in the ctor where the addAndMakeVisible was removed. */

        instPage_   .setVisible (inst);
        samplePage_ .setVisible (sample);
        fxPage_     .setVisible (fx);

        if (inst)   instPage_.refresh();
        if (sample) samplePage_.refresh();
        resized();
    }

    /* === ListBoxModel ================================================= */

    static constexpr int kBusBadgeWidth = 56;

    int getNumRows() override { return SamplerInstrument::kNumSlots; }

    /** Left-click on the bus badge cycles to the next bus.
     *  Right-click opens a small popup to pick any of Bus 1..N
     *  directly.  Click outside the badge falls through to normal
     *  row selection (handled by the base ListBox). */
    void listBoxItemClicked (int row, const MouseEvent& e) override
    {
        auto inst = node.getInstrument (activeInstrument);
        auto* slot = inst ? inst->getSlot (row) : nullptr;
        if (slot == nullptr) return;

        const int rowW = slotList.getWidth();
        const int badgeLeft = rowW - kBusBadgeWidth - 4;
        if (e.x < badgeLeft) return;

        if (e.mods.isPopupMenu())
        {
            PopupMenu m;
            for (int b = 0; b < SamplerNode::kNumBuses; ++b)
                m.addItem (b + 1, "Bus " + String (b + 1), true,
                           slot->busIndex == b);
            m.showMenuAsync (PopupMenu::Options(),
                             [this, row, inst] (int picked) {
                                 if (picked > 0)
                                 {
                                     if (auto* s = inst->getSlot (row))
                                         s->busIndex = picked - 1;
                                     slotList.repaint();
                                 }
                             });
        }
        else
        {
            slot->busIndex = (slot->busIndex + 1) % SamplerNode::kNumBuses;
            slotList.repaint();
        }
    }

    void paintListBoxItem (int row, Graphics& g, int width, int height,
                           bool rowIsSelected) override
    {
        if (rowIsSelected)
        {
            g.setColour (Colour { 0xff'40'80'b0 }.withAlpha (0.4f));
            g.fillRect (0, 0, width, height);
        }

        auto inst = node.getInstrument (activeInstrument);
        const auto* slot = inst ? inst->getSlot (row) : nullptr;
        const bool hasSample = slot && slot->isLoaded();

        /* Slot-color stripe down the left edge — matches keymap palette
         * so the eye can connect bank row ↔ tinted keys. */
        g.setColour (hasSample ? slotPaletteColour (row, 0.65f, 0.85f)
                               : Colour { 0xff'2a'2a'2a });
        g.fillRect (0, 0, 6, height);

        g.setFont (FontOptions (Font::getDefaultMonospacedFontName(), 12.0f, Font::plain));

        g.setColour (slot != nullptr ? Colour { 0xff'd4'd4'd4 } : Colour { 0xff'5a'5a'5a });
        g.drawText (String::formatted ("%02d", row + 1), 12, 0, 28, height,
                    Justification::centredLeft);

        int lo = -1, hi = -1;
        if (inst && slot)
        {
            for (int n = 0; n < 128; ++n)
            {
                if (inst->slotForNote (n) == row)
                {
                    if (lo < 0) lo = n;
                    hi = n;
                }
            }
        }
        const String rng = (lo >= 0 && hi >= 0)
                              ? (midiNoteName (lo) + "  " + midiNoteName (hi))
                              : String ("        ");
        g.setColour (Colour { 0xff'd0'80'40 });
        g.drawText (rng, 44, 0, 80, height, Justification::centredLeft);

        /* Reserve a compact "Bus N" badge on the right edge — same
         * width as kBusBadgeWidth (used in listBoxItemClicked for
         * hit-test).  Name column shrinks to leave room. */
        const int badgeW = kBusBadgeWidth;
        const int nameRight = width - badgeW - 6;

        g.setColour (slot != nullptr ? Colour { 0xff'b0'b0'b0 } : Colour { 0xff'40'40'40 });
        g.drawText (slot != nullptr ? slot->name : String (juce::CharPointer_UTF8 ("\xe2\x80\x94")),
                    130, 0, juce::jmax (0, nameRight - 130), height,
                    Justification::centredLeft);

        if (slot != nullptr)
        {
            const int bIdx = juce::jlimit (0, SamplerNode::kNumBuses - 1, slot->busIndex);
            auto badgeR = juce::Rectangle<int> (width - badgeW - 4, 2,
                                                 badgeW, height - 4);
            g.setColour (Colour { 0xff'2a'2a'2a });
            g.fillRoundedRectangle (badgeR.toFloat(), 3.0f);
            g.setColour (Colour { 0xff'd0'80'40 });
            g.drawRoundedRectangle (badgeR.toFloat(), 3.0f, 1.0f);
            g.setColour (Colours::white);
            g.setFont (FontOptions (Font::getDefaultMonospacedFontName(), 11.0f, Font::bold));
            g.drawText ("Bus " + String (bIdx + 1), badgeR,
                        Justification::centred);
        }
    }

    void selectedRowsChanged (int lastRowSelected) override
    {
        if (lastRowSelected >= 0)
        {
            activeSlot = lastRowSelected;
            refresh();
        }
    }

    void listBoxItemDoubleClicked (int row, const MouseEvent&) override
    {
        activeSlot = row;
        slotList.selectRow (row);
        onLoad();    /* pulls from DiskOpService selection if any */
    }

private:
    /** A simple Component that calls a std::function on resize.  Used as
     *  the tabbed-content container so we can lay out child controls. */
    struct ResizableWrap : public Component
    {
        std::function<void()> onResized;
        void resized() override { if (onResized) onResized(); }
    };

    void timerCallback() override { refresh(); }

    void configureParamSlider (Slider& s, const String& label, double lo, double hi,
                               double step, std::function<void (double)> on)
    {
        s.setSliderStyle (Slider::LinearBar);
        s.setRange (lo, hi, step);
        s.setTextValueSuffix ("  " + label);
        s.onValueChange = [&s, on] { on (s.getValue()); };
        s.setColour (Slider::backgroundColourId, Colour { 0xff'24'24'24 });
        s.setColour (Slider::trackColourId,      Colour { 0xff'4a'7a'b5 });
        addAndMakeVisible (s);
    }

    SamplerSampleSlot* currentSlot()
    {
        if (auto inst = node.getInstrument (activeInstrument))
            return inst->getSlot (activeSlot);
        return nullptr;
    }

    /** Compose the label shown for `instrumentIndex`.  Reads from the
     *  live SamplerNode (no caching) so any Disk Op mutation -- a
     *  bank rename or a slot load -- is reflected the moment this
     *  is called (see refresh()'s diff-gated dispatch).
     *
     *  Display rules:
     *    - Bank has a user-set name      -> "NNN <bank-name>"
     *    - Bank has samples but no name  -> "NNN <first-loaded-slot-name>"
     *    - Bank empty                    -> "NNN (empty)"
     */
    String composeInstLabel (int instrumentIndex) const
    {
        String label = String::formatted ("%03d  ", instrumentIndex + 1);
        if (instrumentIndex < 0 || instrumentIndex >= node.getNumInstruments())
            return label + "(empty)";

        auto inst = node.getInstrument (instrumentIndex);
        if (inst == nullptr) return label + "(empty)";

        if (inst->name.isNotEmpty())
            return label + inst->name;

        /* No bank name set but the user may have loaded samples
         * directly.  Surface the first loaded slot's name so the
         * dropdown / nav label isn't misleading. */
        for (int i = 0; i < SamplerInstrument::kNumSlots; ++i)
        {
            if (auto* s = inst->getSlot (i))
                if (s->name.isNotEmpty() || s->isLoaded())
                    return label + s->name;
        }
        return label + "(empty)";
    }

    /** Push the current bank label into the toolbar Label component.
     *  Cheap; called on every refresh tick after the diff-gate
     *  decides something changed (count, names, or slot mutations). */
    void refreshInstLabel()
    {
        instLabel.setText (composeInstLabel (activeInstrument),
                           dontSendNotification);
        instPrevBtn.setEnabled (activeInstrument > 0);
        instNextBtn.setEnabled (activeInstrument < SamplerNode::kMaxInstruments - 1);
    }

    void rebuildEnvelopeViews()
    {
        instPage_.rebuildEnvViews();
        volEnvView.reset();
        panEnvView.reset();
        if (auto inst = node.getInstrument (activeInstrument))
        {
            volEnvView.reset (new FT2EnvelopeView (
                inst->volumeEnv, "Volume Env",
                [this] { /* changes are direct on the struct; just repaint */ }));
            panEnvView.reset (new FT2EnvelopeView (
                inst->panEnv,    "Pan Env",
                [this] { }));
            volEnvWrap.removeAllChildren();
            panEnvWrap.removeAllChildren();
            volEnvWrap.addAndMakeVisible (volEnvView.get());
            panEnvWrap.addAndMakeVisible (panEnvView.get());
            volEnvWrap.addAndMakeVisible (volTabAddBtn);
            volEnvWrap.addAndMakeVisible (volTabDelBtn);
            panEnvWrap.addAndMakeVisible (panTabAddBtn);
            panEnvWrap.addAndMakeVisible (panTabDelBtn);
            volEnvWrap.onResized = [this] {
                auto r = volEnvWrap.getLocalBounds().reduced (2);
                auto btnRow = r.removeFromBottom (22);
                volTabAddBtn.setBounds (btnRow.removeFromLeft (60));
                btnRow.removeFromLeft (4);
                volTabDelBtn.setBounds (btnRow.removeFromLeft (60));
                r.removeFromBottom (3);
                if (volEnvView) volEnvView->setBounds (r);
            };
            panEnvWrap.onResized = [this] {
                auto r = panEnvWrap.getLocalBounds().reduced (2);
                auto btnRow = r.removeFromBottom (22);
                panTabAddBtn.setBounds (btnRow.removeFromLeft (60));
                btnRow.removeFromLeft (4);
                panTabDelBtn.setBounds (btnRow.removeFromLeft (60));
                r.removeFromBottom (3);
                if (panEnvView) panEnvView->setBounds (r);
            };
            volEnvWrap.resized();
            panEnvWrap.resized();

            fadeoutSlider.setValue ((double) inst->fadeoutRate, dontSendNotification);
            avDepthSlider.setValue ((double) inst->autoVib.depth, dontSendNotification);
            avRateSlider .setValue ((double) inst->autoVib.rate,  dontSendNotification);
            avSweepSlider.setValue ((double) inst->autoVib.sweep, dontSendNotification);
            avTypeCombo  .setSelectedId ((int) inst->autoVib.type + 1, dontSendNotification);
        }
    }

    /* Bank-page envelope point add/del — mirrors SamplerInstPage's
     * Add/Del so envelope editing works from the Vol/Pan tab too. */
    void addEnvPointBank (bool isVol)
    {
        auto inst = node.getInstrument (activeInstrument);
        if (inst == nullptr) return;
        FT2Envelope& e = isVol ? inst->volumeEnv : inst->panEnv;
        if (e.length >= 12) return;
        if (e.length == 0) { e.points[0] = { 0, 64 }; e.length = 1; }
        const int prevX = e.points[e.length - 1].x;
        const int prevY = e.points[e.length - 1].y;
        const int newX  = juce::jmin (324, prevX + 20);
        e.points[e.length] = { (int16_t) newX, (int16_t) prevY };
        e.length = (uint8_t) (e.length + 1);
        if (volEnvView) volEnvView->repaint();
        if (panEnvView) panEnvView->repaint();
        instPage_.refresh();
    }
    void delEnvPointBank (bool isVol)
    {
        auto inst = node.getInstrument (activeInstrument);
        if (inst == nullptr) return;
        FT2Envelope& e = isVol ? inst->volumeEnv : inst->panEnv;
        if (e.length <= 2) return;
        e.length = (uint8_t) (e.length - 1);
        if (e.sustainPoint >= e.length) e.sustainPoint = (uint8_t) (e.length - 1);
        if (e.loopStart   >= e.length) e.loopStart   = (uint8_t) (e.length - 1);
        if (e.loopEnd     >= e.length) e.loopEnd     = (uint8_t) (e.length - 1);
        if (volEnvView) volEnvView->repaint();
        if (panEnvView) panEnvView->repaint();
        instPage_.refresh();
    }

    void onLoad()
    {
        /* Pull the currently-selected sample from the app-wide Disk Op
         * service.  The user picks a file in the Disk Op nav page
         * (F5 / View → Disk Op), then comes back here and clicks Load.
         * Mode is forced to Sample so the Disk Op shows the audio-files
         * filter on the next visit. */
        auto& svc = DiskOpService::get();
        svc.setMode (DiskOpService::Mode::kSample);

        const auto f = svc.getSelectedFile();
        if (! f.existsAsFile())
        {
            status.setText ("No sample selected — pick one in Disk Op",
                            dontSendNotification);
            return;
        }
        if (node.loadSampleToSlot (activeInstrument, activeSlot, f))
            status.setText ("Loaded " + f.getFileName(), dontSendNotification);
        else
            status.setText ("Failed: " + f.getFileName(), dontSendNotification);
        refresh();
    }

    void refresh()
    {
        auto inst = node.getInstrument (activeInstrument);
        const int slotListSel = slotList.getSelectedRow();
        if (slotListSel != activeSlot)
            slotList.selectRow (activeSlot, true, false);

        auto* slot = currentSlot();
        if (slot != nullptr)
        {
            rootSlider.setValue ((double) slot->rootNote,     dontSendNotification);
            relSlider .setValue ((double) slot->relativeNote, dontSendNotification);
            fineSlider.setValue ((double) slot->finetune,     dontSendNotification);
            volSlider .setValue ((double) slot->volume,       dontSendNotification);
            panSlider .setValue ((double) slot->panning,      dontSendNotification);
            loopCombo .setSelectedId ((int) slot->loopMode + 1, dontSendNotification);
        }
        /* Master bus gains live on the node, not the slot — always sync
         * (independent of which slot is selected). */
        for (int b = 0; b < SamplerNode::kNumBuses; ++b)
            busGainSliders[b].setValue ((double) node.busGain[b], dontSendNotification);
        interpCombo.setSelectedId ((int) node.getInterpMode() + 1, dontSendNotification);

        const int numLoaded = inst ? inst->numLoaded() : 0;
        const int numInst   = node.getNumInstruments();
        const SamplerSampleSlot* slotPtr = slot;
        const bool listDirty = (numLoaded != lastNumLoaded_)
                            || (slotPtr   != lastSlotPtr_)
                            || (activeSlot != lastActiveSlot_)
                            || (activeInstrument != lastActiveInstrument_);
        if (listDirty)
        {
            slotList.updateContent();
            slotList.repaint();
        }

        if (slotPtr != lastSlotPtr_)
            waveformView.repaint();

        /* Bank label refreshes unconditionally each tick (cheap: one
         * setText with dontSendNotification on equal text is a no-op
         * inside juce::Label).  This covers all the live-sync cases
         * the user listed:
         *   - Disk Op rename bank -> instLabel picks up new name
         *   - Disk Op load sample -> first-slot-name surfaces
         *   - addInstrument / removeInstrument -> activeInstrument
         *     stays valid + label reflects current bank
         * The lastInstNames_ array is no longer needed for combo
         * rebuild gating, but is kept here for the moment in case a
         * follow-up commit needs it (TODO remove after verification). */
        juce::ignoreUnused (lastInstNames_, lastNumInstruments_);
        refreshInstLabel();

        const int voices = node.getNumVoices();
        const String slotName = (slot != nullptr) ? slot->name : String ("(empty)");
        if (numLoaded != lastNumLoaded_
         || voices    != lastVoices_
         || activeSlot != lastActiveSlot_
         || slotName  != lastSlotName_
         || activeInstrument != lastActiveInstrument_)
        {
            status.setText (String::formatted ("Ins %02d / Slot %02d: %s | %d/%d slots | %d ins | %d voices",
                                                activeInstrument + 1, activeSlot + 1,
                                                slotName.toRawUTF8(),
                                                numLoaded, SamplerInstrument::kNumSlots,
                                                numInst, voices),
                            dontSendNotification);
        }

        lastNumLoaded_       = numLoaded;
        lastSlotPtr_         = slotPtr;
        lastActiveSlot_      = activeSlot;
        lastActiveInstrument_ = activeInstrument;
        lastVoices_          = voices;
        lastNumInstruments_  = numInst;
        lastSlotName_        = slotName;
    }

    int                  lastNumLoaded_       { -1 };
    int                  lastVoices_          { -1 };
    int                  lastActiveSlot_      { -1 };
    int                  lastActiveInstrument_{ -1 };
    int                  lastNumInstruments_  { -1 };
    juce::StringArray    lastInstNames_;     // catches in-place renames
    const SamplerSampleSlot* lastSlotPtr_     { nullptr };
    String               lastSlotName_;

    SamplerNode& node;

    /* Bank navigation -- replaces a ComboBox with a fixed label +
     * prev/next nudge buttons, matching the tracker editor's OCT /
     * STEP / TRK / PAT toolbar pattern.  The label shows
     *   "NNN BankName"  (bank has a user-set name)
     *   "NNN <first-slot-name>"  (samples loaded but bank not named)
     *   "NNN (empty)"   (no samples, no name) */
    Label        instLabel;
    TextButton   instPrevBtn, instNextBtn;
    TextButton   instAddBtn,  instDelBtn;

    ListBox      slotList;
    TextButton   clearBtn;

    Slider       rootSlider, relSlider, fineSlider, volSlider, panSlider;
    ComboBox     loopCombo;
    /* Per-bus MASTER gain sliders — 4 of them, below the preview.
     * Drive SamplerNode::busGain[]; independent of slot selection.
     * Per-slot bus assignment lives in the bank-list row badge —
     * paintListBoxItem + listBoxItemClicked. */
    Slider       busGainSliders[SamplerNode::kNumBuses];
    SampleWaveformView waveformView;

    TabbedComponent tabBar { TabbedButtonBar::TabsAtTop };
    ResizableWrap   volEnvWrap, panEnvWrap, autoVibWrap;
    TextButton      volTabAddBtn, volTabDelBtn, panTabAddBtn, panTabDelBtn;
    std::unique_ptr<FT2EnvelopeView> volEnvView, panEnvView;

    Slider       fadeoutSlider, avDepthSlider, avRateSlider, avSweepSlider;
    ComboBox     avTypeCombo;

    ComboBox     interpCombo;
    Label        status;

    int activeSlot = 0;
    int activeInstrument = 0;

    /* Paged container — nav state + sub-pages. */
    Page currentPage_ = Page::kBank;
    TextButton bankNavBtn, instNavBtn, sampleNavBtn, fxNavBtn;
    SamplerInstPage   instPage_;
    SamplerSamplePage samplePage_;
    SamplerFXPage     fxPage_;
};

AudioProcessorEditor* SamplerNode::createEditor()
{
    return new SamplerNodeEditor (*this);
}


/* =========================================================================== */

/* Multi-bus output layout: kNumBuses stereo aux sends, NO separate
 * Main — Bus 1 is what JUCE/Element treats as the main bus (first
 * output) but the user-facing label is just "Bus 1".  Each slot
 * picks ONE bus (busIndex) and a level (busLevel) for that bus.
 *
 * Inlined (rather than a free helper) because BusesProperties is
 * `protected` in juce::AudioProcessor and only accessible from inside
 * a subclass body. */
SamplerNode::SamplerNode()
    : BaseProcessor ([] {
          /* withOutput's third arg is `isActivatedByDefault`, NOT
           * "is main bus".  Pass `true` for every bus so they all
           * spawn enabled — JUCE's first bus is implicitly the
           * "main" anyway.  Earlier i==1 conditional left Bus 2/3/4
           * disabled at construction, which made the graph node
           * report getTotalNumOutputChannels==2 (just Bus 1) — the
           * bug you saw on screen. */
          BusesProperties bp;
          for (int i = 1; i <= kNumBuses; ++i)
              bp = bp.withOutput ("Bus " + String (i),
                                  AudioChannelSet::stereo(),
                                  true);
          return bp;
      }())
{
    ensureMixerInterpolationTablesReady();
    /* mixFuncDispatch[] is patched with SIMD variants on CPUs that support
     * them; ft2_mix_init is idempotent so multiple SamplerNode ctors are
     * safe.  Single global state, but only write-once writes (memcpy +
     * fixed function pointers), so the second/Nth call is a no-op. */
    ft2_mix_init();
    formatManager.registerBasicFormats();
    channelBinding.fill (-1);

    synth.reset (new ChannelTrackingSynth (*this));

    instruments.reserve (16);
    instruments.push_back (new SamplerInstrument());
    rebuildVoicePool();

    synth->addSound (new Ft2SamplerSound (*this));
}

SamplerNode::~SamplerNode()
{
    synth->clearVoices();
    synth->clearSounds();
}

void SamplerNode::fillInPluginDescription (PluginDescription& desc) const
{
    desc.fileOrIdentifier   = EL_NODE_ID_SAMPLER;
    desc.name               = "Sampler";
    desc.descriptiveName    = "Multi-sample instrument (MIDI-in / " +
                              String (kNumBuses) + " stereo bus outputs)";
    desc.numInputChannels   = 0;
    /* Total output channel count = 2 (stereo) × kNumBuses.  This is
     * what Element's internal format reader uses to decide how many
     * output ports to wire on each instantiation — keeping it in
     * lockstep with the BusesProperties layout above is what makes
     * the graph node show the right number of pads. */
    desc.numOutputChannels  = 2 * kNumBuses;
    desc.hasSharedContainer = false;
    desc.isInstrument       = true;
    desc.category           = "Instrument";
    desc.manufacturerName   = EL_NODE_FORMAT_AUTHOR;
    desc.pluginFormatName   = EL_NODE_FORMAT_NAME;
    desc.version            = "0.3.0";
    desc.uniqueId           = EL_NODE_UID_SAMPLER;
}

void SamplerNode::prepareToPlay (double sampleRate, int maxBlockSize)
{
    currentSampleRate = sampleRate;
    synth->setCurrentPlaybackSampleRate (sampleRate);
    /* Worker scratch buffers are sized to maxBlockSize so a render in a
     * single sub-block fits without further allocation.  ChannelTrackingSynth
     * downcast: SamplerNode owns the synth and only ever constructs a
     * ChannelTrackingSynth in its ctor — see SamplerNode(). */
    if (auto* cts = dynamic_cast<ChannelTrackingSynth*> (synth.get()))
    {
        /* Worker scratch covers every aux bus (each stereo) — voices
         * write to whichever channels their per-bus busSend picks.
         * Total = 2 * kNumBuses; sized once at prepareToPlay. */
        cts->prepareWorkers (2 * kNumBuses, juce::jmax (64, maxBlockSize));
    }
}

void SamplerNode::releaseResources()
{
    synth->allNotesOff (0, true);
    if (auto* cts = dynamic_cast<ChannelTrackingSynth*> (synth.get()))
        cts->stopWorkers();
}

void SamplerNode::processBlock (AudioBuffer<float>& audio_, MidiBuffer& midi)
{
    audio_.clear();

    /* Scan for MIDI program-change events and update channel→instrument
     * binding before renderNextBlock dispatches notes.  PC value 0..127
     * maps directly to instrument index.  If the index exceeds the
     * loaded count, fall back to the channel default. */
    for (const auto& meta : midi)
    {
        const auto msg = meta.getMessage();
        if (! msg.isProgramChange()) continue;
        const int ch = msg.getChannel();         /* 1..16 */
        const int pc = msg.getProgramChangeNumber();
        if (pc >= 0 && pc < (int) instruments.size())
            bindChannelToInstrument (ch, pc);
        else
            bindChannelToInstrument (ch, -1);   /* default mapping */
    }

    synth->renderNextBlock (audio_, midi, 0, audio_.getNumSamples());
}

bool SamplerNode::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    /* Main + every aux must be stereo.  Returning false for any
     * non-stereo arrangement keeps the layout immutable so audio-thread
     * channel-index arithmetic in renderNextBlock is always safe. */
    if (layouts.outputBuses.size() != 1 + kNumBuses) return false;
    for (auto& set : layouts.outputBuses)
        if (set != AudioChannelSet::stereo()) return false;
    return true;
}

void SamplerNode::rebuildVoicePool()
{
    synth->clearVoices();
    for (int i = 0; i < numVoices; ++i)
        synth->addVoice (new Ft2SamplerVoice (*this));
}

/* ----- sampleLock discipline ------------------------------------------ *
 *
 *  All reads + writes of `instruments` and `channelBinding` go through
 *  sampleLock — INCLUDING the audio thread.  Without this, a UI-side
 *  mutation (addInstrument / removeInstrument / setStateInformation)
 *  races against a voice note-on reading the vector → torn Ptr / dead
 *  storage.  See [[bulletproof-audit-after-changes]].
 *
 *  Audio-thread contention is bounded because every UI-side mutation
 *  is short — file I/O for loadSampleToSlot happens via prepareSlot()
 *  BEFORE the lock is taken; only the buffer-swap commitSlot() runs
 *  inside.  Other mutations are O(n) over ≤128 entries with no
 *  allocation in the hot path.
 *
 *  UI accessor variants (getInstrument, getNumInstruments) lock too
 *  so editor reads see consistent state during the same broadcast.
 */

int SamplerNode::getNumInstruments() const
{
    const ScopedLock sl (sampleLock);
    return (int) instruments.size();
}

SamplerInstrument::Ptr SamplerNode::getInstrument (int index) const
{
    const ScopedLock sl (sampleLock);
    if (index < 0 || index >= (int) instruments.size()) return nullptr;
    return instruments[(size_t) index];
}

SamplerInstrument::Ptr SamplerNode::addInstrument()
{
    ScopedLock sl (sampleLock);
    if ((int) instruments.size() >= kMaxInstruments) return nullptr;
    auto inst = SamplerInstrument::Ptr (new SamplerInstrument());
    instruments.push_back (inst);
    return inst;
}

void SamplerNode::removeInstrument (int index)
{
    ScopedLock sl (sampleLock);
    if (index < 0 || index >= (int) instruments.size()) return;
    if (instruments.size() <= 1) return; /* always keep at least one */

    /* Drop mono-mode state keyed by the about-to-be-freed raw pointer. */
    SamplerInstrument* doomed = instruments[(size_t) index].get();
    if (auto* cts = dynamic_cast<ChannelTrackingSynth*> (synth.get()))
        cts->forgetInstrument (doomed);

    instruments.erase (instruments.begin() + index);
    /* Re-map channel bindings that pointed past the removed index. */
    for (auto& b : channelBinding)
    {
        if (b == (int8_t) index) b = -1;
        else if (b > (int8_t) index) --b;
    }
}

SamplerInstrument::Ptr SamplerNode::getInstrumentForChannel (int channel1to16) const
{
    /* Audio-thread read — short critical section, uncontended 99.9%
     * of the time.  Returns a Ptr by value (refcount bumped under
     * the lock), so the caller holds a live instrument even after
     * we release. */
    const ScopedLock sl (sampleLock);
    if (instruments.empty()) return nullptr;

    const int ch = juce::jlimit (1, 16, channel1to16) - 1;
    int idx = channelBinding[(size_t) ch];
    if (idx < 0) idx = ch;   /* default mapping */
    if (idx >= (int) instruments.size()) idx = 0;
    if (idx < 0 || idx >= (int) instruments.size()) return nullptr;
    return instruments[(size_t) idx];
}

void SamplerNode::bindChannelToInstrument (int channel1to16, int instrumentIndex)
{
    const ScopedLock sl (sampleLock);
    const int ch = juce::jlimit (1, 16, channel1to16) - 1;
    if (instrumentIndex < 0 || instrumentIndex >= (int) instruments.size())
        channelBinding[(size_t) ch] = -1;
    else
        channelBinding[(size_t) ch] = (int8_t) instrumentIndex;
}

int SamplerNode::getChannelBinding (int channel1to16) const
{
    const ScopedLock sl (sampleLock);
    const int ch = juce::jlimit (1, 16, channel1to16) - 1;
    return (int) channelBinding[(size_t) ch];
}

bool SamplerNode::loadSampleToSlot (int instrumentIndex, int slot, const File& file)
{
    /* Two-phase: read+decode the file (potentially seconds for big
     * WAVs) WITHOUT holding sampleLock — only the final commit
     * touches the instrument slot under the lock.  Keeps the audio
     * thread's critical sections short. */
    SamplerInstrument::Ptr inst;
    {
        const ScopedLock sl (sampleLock);
        if (instrumentIndex < 0
            || instrumentIndex >= (int) instruments.size())
            return false;
        inst = instruments[(size_t) instrumentIndex];
    }
    if (inst == nullptr) return false;

    auto loaded = inst->prepareSlot (file, formatManager);
    if (loaded == nullptr) return false;

    const ScopedLock sl (sampleLock);
    return inst->commitSlot (slot, std::move (loaded));
}

void SamplerNode::rebuildInstrument()
{
    ScopedLock sl (sampleLock);
    instruments.clear();
    instruments.push_back (new SamplerInstrument());
    channelBinding.fill (-1);
    synth->clearSounds();
    synth->addSound (new Ft2SamplerSound (*this));
}

void SamplerNode::setNumVoices (int n)
{
    numVoices = jlimit (1, 64, n);
    rebuildVoicePool();
}

void SamplerNode::setInterpMode (InterpMode m) { interpMode = m; }
void SamplerNode::setAdsr (AdsrParams p)
{
    adsrParams = p;
    adsrParams.attack  = jlimit (0.f, 30.f, adsrParams.attack);
    adsrParams.decay   = jlimit (0.f, 30.f, adsrParams.decay);
    adsrParams.sustain = jlimit (0.f,  1.f, adsrParams.sustain);
    adsrParams.release = jlimit (0.f, 30.f, adsrParams.release);
}

int SamplerNode::getSamplesPerEnvTick() const noexcept
{
    return juce::jmax (1, (int) (currentSampleRate / (double) kEnvTickRateHz));
}

std::vector<int> SamplerNode::collectPlayheadsForSlot (const SamplerSampleSlot* slot) const
{
    std::vector<int> out;
    if (slot == nullptr) return out;
    auto& s = *synth;
    const int nv = s.getNumVoices();
    out.reserve ((size_t) nv);
    for (int i = 0; i < nv; ++i)
    {
        if (auto* v = dynamic_cast<Ft2SamplerVoice*> (s.getVoice (i)))
        {
            if (v->isPlayingActive() && v->getCurrentSlot() == slot)
                out.push_back (v->getCurrentSamplePos());
        }
    }
    return out;
}

int SamplerNode::getMixFuncIndexForCurrentMode (bool loop, bool pingpong) const
{
    const int base    = kBase16Ramped;
    const int interp  = interpOffset ((int) interpMode);
    const int loopOff = pingpong ? 2 : (loop ? 1 : 0);
    return base + interp + loopOff;
}

void SamplerNode::getStateInformation (MemoryBlock& dest)
{
    ValueTree tree ("sampler");
    tree.setProperty ("numVoices",  numVoices,         nullptr);
    tree.setProperty ("interpMode", (int) interpMode,  nullptr);
    tree.setProperty ("adsrAtt",  adsrParams.attack,  nullptr);
    tree.setProperty ("adsrDec",  adsrParams.decay,   nullptr);
    tree.setProperty ("adsrSus",  adsrParams.sustain, nullptr);
    tree.setProperty ("adsrRel",  adsrParams.release, nullptr);

    /* Per-bus master gains live on the node (not per-slot). */
    for (int b = 0; b < kNumBuses; ++b)
        tree.setProperty (Identifier ("busGain" + String (b + 1)),
                          (double) busGain[b], nullptr);

    for (int b = 0; b < 16; ++b)
        tree.setProperty (Identifier ("bind" + String (b)), (int) channelBinding[(size_t) b], nullptr);

    for (size_t ii = 0; ii < instruments.size(); ++ii)
    {
        auto inst = instruments[ii];
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

        /* Persist the 128-byte MIDI keymap as a CSV of slot indices.
         * Older saves without this property fall back to autoSpread
         * during reload. */
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

void SamplerNode::setStateInformation (const void* data, int size)
{
    if (data == nullptr || size <= 0) return;
    const auto tree = ValueTree::readFromGZIPData (data, (size_t) size);
    if (! tree.isValid() || tree.getType() != Identifier ("sampler")) return;

    /* The audio thread reads instruments[] without locking — clearing
     * + repopulating the vector here can otherwise expose torn state
     * (0-size window, in-flight reallocation moving Ptr storage) to
     * a voice startNote call.  Hold sampleLock around the structural
     * mutation; the empty() guard inside getInstrumentForChannel is
     * the belt-and-suspenders fallback when this lock isn't held
     * (e.g., legacy callers). */
    ScopedLock sl (sampleLock);

    numVoices  = (int) tree.getProperty ("numVoices", 16);
    interpMode = (InterpMode) (int) tree.getProperty ("interpMode", (int) kInterpLinear);
    adsrParams.attack  = (float) (double) tree.getProperty ("adsrAtt", 0.005);
    adsrParams.decay   = (float) (double) tree.getProperty ("adsrDec", 0.05);
    adsrParams.sustain = (float) (double) tree.getProperty ("adsrSus", 1.0);
    adsrParams.release = (float) (double) tree.getProperty ("adsrRel", 0.10);

    /* Per-bus master gains.  Default to 1.0 if missing (old sessions). */
    for (int b = 0; b < kNumBuses; ++b)
        busGain[b] = (float) (double) tree.getProperty (
            Identifier ("busGain" + String (b + 1)), 1.0);

    rebuildVoicePool();

    for (int b = 0; b < 16; ++b)
        channelBinding[(size_t) b] = (int8_t) (int) tree.getProperty (Identifier ("bind" + String (b)), -1);

    /* Reset instrument table from saved children. */
    instruments.clear();

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
        /* Default ON for sessions that pre-date this field — matches
         * the SamplerInstrument ctor default and is the "intended
         * usage" per the design discussion. */
        inst->envSampleRelative = (bool) instTree.getProperty ("envSampleRel", true);

        auto readEnv = [&](FT2Envelope& e, const String& prefix)
        {
            /* All envelope indices are clamped to the points[] capacity
             * (12 slots, see FT2Envelope) on load so a corrupt session
             * file can't push later paint / tick code past the array. */
            const int rawLen  = (int) instTree.getProperty (Identifier (prefix + "Len"),   0);
            const int rawSus  = (int) instTree.getProperty (Identifier (prefix + "Sus"),   0);
            const int rawLoS  = (int) instTree.getProperty (Identifier (prefix + "LoopS"), 0);
            const int rawLoE  = (int) instTree.getProperty (Identifier (prefix + "LoopE"), 0);
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
            if (e.length > (uint8_t) n) e.length = (uint8_t) n;   /* don't outrun parsed points */
        };
        readEnv (inst->volumeEnv, "ve");
        readEnv (inst->panEnv,    "pe");

        for (int c = 0; c < instTree.getNumChildren(); ++c)
        {
            const auto slotTree = instTree.getChild (c);
            if (slotTree.getType() != Identifier ("slot")) continue;
            const int idx = (int) slotTree.getProperty ("idx", 0);
            if (idx < 0 || idx >= SamplerInstrument::kNumSlots) continue;

            /* ALWAYS allocate a slot for any persisted slot tree.
             * Without this, the old codepath silently dropped saved
             * slot metadata because SamplerInstrument's slots[] is
             * default-null after construction, so getSlot(idx)
             * returned nullptr and the metadata setters never ran.
             * That's why sessions reloaded with empty banks. */
            auto fresh = std::make_unique<SamplerSampleSlot>();

            /* If a source path was saved (commits >= 309e5e24, now
             * re-implemented), check existence with the native POSIX
             * ::access syscall -- matches Disk Op's own native-path
             * convention.  juce::File only enters the picture when
             * we actually need to decode the audio (which goes
             * through juce::AudioFormatManager either way). */
            const std::string savedPath =
                slotTree.getProperty ("sourceFile", "").toString().toRawUTF8();

            /* Native POSIX existence check.  No juce::File ops on the
             * fs side -- those go through wineserver on winelib.  We
             * pass the path to prepareSlot which does its own native
             * ::open/::read for the decode-source bytes. */
            if (! savedPath.empty() && ::access (savedPath.c_str(), R_OK) == 0)
            {
                /* juce::File construction is a pure string wrap (no fs
                 * call) -- prepareSlot extracts the path string + name
                 * back out and does all real I/O via ::open. */
                const File f { String (savedPath) };
                if (auto loaded = inst->prepareSlot (f, formatManager))
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
                    fresh->sourceFile = savedPath;   // decode failed -- keep path for diag
                }
            }
            else
            {
                fresh->sourceFile = savedPath;   // path tracked even if file missing
            }

            /* Apply the saved metadata on top of (possibly reloaded) audio. */
            fresh->name         = slotTree.getProperty ("name", "").toString();
            fresh->rootNote     = (int) slotTree.getProperty ("rootNote", 60);
            fresh->relativeNote = (int) slotTree.getProperty ("relativeNote", 0);
            fresh->finetune     = (int) slotTree.getProperty ("finetune", 0);
            fresh->volume       = (float) (double) slotTree.getProperty ("volume", 1.0);
            fresh->panning      = (float) (double) slotTree.getProperty ("pan", 0.5);
            fresh->loopMode     = (SamplerLoopMode) (int) slotTree.getProperty ("loopMode", 0);
            fresh->loopStart    = (int) slotTree.getProperty ("loopStart",  0);
            fresh->loopLength   = (int) slotTree.getProperty ("loopLength", 0);

            /* Bus assignment.  Read busIndex if present; older
             * intermediate formats (busSend1..N from the multi-
             * send experiment / busAssign+busWet) get folded into
             * busIndex with a best-effort mapping.  Default if
             * none present: Bus 1 (from ctor). */
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

            /* commitSlot publishes the slot under the (already held)
             * sampleLock.  inst is a fresh SamplerInstrument so this
             * is the first commit -- no audio-thread contention. */
            inst->commitSlot (idx, std::move (fresh));
        }

        /* Restore the saved keymap if present, AFTER all slots are
         * committed (commitSlot auto-spreads on first-load if
         * !keymapUserModified, so this needs to fire last to override
         * the auto-spread). */
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

        instruments.push_back (inst);
    }

    if (instruments.empty())
        instruments.push_back (new SamplerInstrument());

    synth->clearSounds();
    synth->addSound (new Ft2SamplerSound (*this));
}

} // namespace element
