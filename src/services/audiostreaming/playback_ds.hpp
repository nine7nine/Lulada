// SPDX-License-Identifier: GPL-2.0-or-later
//
// Originally from NON-DAW (Jonathan Moore Liles, 2008),
// timeline/src/Engine/Playback_DS.H -- GPLv2-or-later.  Adapted for
// Element-NSPA with the following changes:
//   - No `Track*` indirection -- streams against an
//     AudioFileSource::Ptr the caller passes in and writes process()'s
//     output into a juce::AudioBuffer slice supplied by AudioClipNode
//     (later step in Phase 3).
//   - Each Playback_DS owns its own Audio_File handle (opened fresh
//     via Audio_File::from_file) so independent playback heads on the
//     same underlying file don't fight over libsndfile's seek
//     position.  NON's _open_files cache lived inside Audio_File and
//     is dropped on the Element side (SourceRegistry handles
//     AudioFileSource dedup at the metadata layer).
//   - Mute / solo gating moves out -- AudioClipNode's render path is
//     where mute/solo policy lives.

#pragma once

#include "services/audiostreaming/disk_stream.hpp"
#include "services/sources/audiofilesource.hpp"

#include <atomic>

namespace element {

class Audio_File;

class Playback_DS final : public Disk_Stream
{
public:
    /** Construct + spawn the IO thread.  `source` must be a registered
     *  AudioFileSource; we open our own Audio_File against
     *  source.file() so the playback head is independent.  Returns
     *  nullptr if the file fails to open.
     *
     *  When `looped` is true, the IO thread wraps back to
     *  `loopStartFrame` once it reaches end-of-source instead of
     *  stalling at EOF.  The default (false) plays straight through
     *  source-end and then silence. */
    static std::unique_ptr<Playback_DS> create (AudioFileSource::Ptr source,
                                                float                frame_rate,
                                                nframes_t            block_size,
                                                int                  channels,
                                                bool                 looped         = false,
                                                nframes_t            loopStartFrame = 0);

    ~Playback_DS() override;

    /** Audio-thread side.  Requests a seek to absolute source-frame
     *  offset.  Re-evaluation happens in the next disk_thread loop
     *  iteration -- the IO thread flushes the rings + re-reads
     *  starting at the requested frame. */
    void seek (nframes_t frame) noexcept;

    /** Returns true while a seek is pending OR the rings are < 50%
     *  full -- callers (the timeline scheduler) use this to back off
     *  rapid back-to-back seeks. */
    bool seek_pending() const noexcept;

    /** Delay compensation, applied to the source-frame the IO thread
     *  reads from.  Useful when the source is being mixed against
     *  delay-introducing plugins downstream of the AudioClipNode. */
    void undelay (nframes_t frames) noexcept { _undelay.store (frames, std::memory_order_relaxed); }

    /** Audio-thread side: drain one block from the rings into `out`.
     *  Out must have at least `channels()` channels available.
     *  Returns the number of frames produced (== block_size unless an
     *  xrun happens, in which case it still returns block_size but
     *  fills with silence and increments xruns()). */
    nframes_t process (juce::AudioBuffer<float>& out,
                       int                       outStartSample,
                       nframes_t                 numFrames) noexcept;

    /** Currently-positioned source frame (post-undelay).  For
     *  diagnostics / playhead UI. */
    nframes_t current_frame() const noexcept { return _frame.load (std::memory_order_relaxed); }

    /** Source identity -- AudioClipNode uses this to dedupe / look up
     *  the originating region. */
    const AudioFileSource::Ptr& source() const noexcept { return source_; }

private:
    Playback_DS (AudioFileSource::Ptr source,
                 Audio_File*          handle,
                 float                frame_rate,
                 nframes_t            block_size,
                 int                  channels,
                 bool                 looped,
                 nframes_t            loopStartFrame);

    void disk_thread() override;
    void flush() override { base_flush (true); }

    /** IO-thread side: pull one IO-chunk (`_disk_io_blocks` blocks
     *  worth of interleaved audio) from the Audio_File into the
     *  per-channel rings. */
    void read_block (sample_t* interleaved_buf, sample_t* deinterleave_buf,
                     nframes_t total_frames) noexcept;

    AudioFileSource::Ptr   source_;
    Audio_File*            file_     { nullptr };   // owned -- ::release() in dtor
    std::atomic<nframes_t> _undelay  { 0 };

    /* Loop config -- set at create() time, immutable for the life of
     * the streamer.  When loop_ is true, read_block wraps the
     * source-frame counter to loopStart_ on EOF instead of stalling. */
    bool       loop_       { false };
    nframes_t  loopStart_  { 0 };
    nframes_t  sourceLen_  { 0 };   /* cached source length in frames */

    JUCE_DECLARE_NON_COPYABLE (Playback_DS)
};

} // namespace element
