// SPDX-License-Identifier: GPL-2.0-or-later
//
// Originally from NON-DAW (Jonathan Moore Liles, 2008),
// timeline/src/Engine/Record_DS.H -- GPLv2-or-later.  Adapted for
// Element-NSPA with the following changes:
//   - Records to an Audio_File_SF directly rather than through NON's
//     Track::Capture indirection.  Caller (AudioClipNode in Phase 4)
//     supplies the destination juce::File + format + samplerate; we
//     open the file via Audio_File_SF::create on first audio block
//     and finalise on stop.
//   - process() receives the input juce::AudioBuffer slice as a
//     parameter; no Track::input[i].buffer reach-through.
//   - Punch-in / punch-out logic is deferred to a v2 of this class.
//     v1 ships single-take capture with explicit start / stop only;
//     timeline-audio-design.md Phase 4 only requires basic record +
//     commit, and the punch state machine is the largest source of
//     bugs in NON's original Record_DS code path.

#pragma once

#include "services/audiostreaming/disk_stream.hpp"

#include <atomic>

namespace element {

class Audio_File_SF;

class Record_DS final : public Disk_Stream
{
public:
    /** Construct + spawn the IO thread.  Does NOT yet create the
     *  output Audio_File_SF -- that happens lazily inside disk_thread
     *  on the first block written, so we can fail-fast at create()
     *  rather than mid-record if the path is bad.  Returns nullptr if
     *  the basename / format combination is invalid. */
    static std::unique_ptr<Record_DS> create (const juce::File&   outputBasename,
                                              const juce::String& format,
                                              nframes_t           file_samplerate,
                                              float               frame_rate,
                                              nframes_t           block_size,
                                              int                 channels);

    ~Record_DS() override;

    /** Begin capture.  `start_source_frame` is the frame value the IO
     *  thread will tag the first written sample with (typically 0 for
     *  a fresh file).  No effect if already recording. */
    void start (nframes_t start_source_frame) noexcept;

    /** Schedule a clean stop -- the IO thread drains pending audio
     *  data from the rings, finalises the Audio_File_SF, and exits.
     *  Idempotent.  Audio-thread-safe to call (non-blocking). */
    void stop() noexcept;

    /** Audio-thread side: push one block of input audio into the
     *  ringbuffers.  No-op while not recording.  Returns the number
     *  of frames consumed (== numFrames unless ring full -- xrun). */
    nframes_t process (const juce::AudioBuffer<float>& in,
                       int                             inStartSample,
                       nframes_t                       numFrames) noexcept;

    /** True between start() and the IO thread's finalise step. */
    bool recording() const noexcept { return _recording.load (std::memory_order_acquire); }

    /** Total frames captured this session.  Updated by the IO thread
     *  as it writes; reads from any thread are best-effort. */
    nframes_t frames_written() const noexcept { return _frames_written.load (std::memory_order_relaxed); }

    /** Path to the finalised capture file.  Stable string -- AudioFileSource
     *  registration uses this after stop() observes recording()==false. */
    juce::File finalised_file() const noexcept { return finalised_path_; }

private:
    Record_DS (const juce::File&   outputBasename,
               const juce::String& format,
               nframes_t           file_samplerate,
               float               frame_rate,
               nframes_t           block_size,
               int                 channels);

    void disk_thread() override;
    void flush() override { base_flush (false); }

    /** IO-thread side: drain one block from the rings, interleave
     *  channels, write to the Audio_File_SF (opening it lazily on the
     *  first call). */
    bool write_block (sample_t* interleave_buf, nframes_t nframes) noexcept;

    juce::File             output_basename_;     // basename without ext
    juce::String           format_;              // matches Audio_File_SF::supported_formats
    nframes_t              file_samplerate_;     // sf_info.samplerate at create-time

    /* Capture file -- owned by the IO thread (created on first write,
     * closed on stop).  Audio thread never touches this. */
    Audio_File_SF*         af_ { nullptr };

    /* Final path after close.  Written once by the IO thread before
     * exit; read by other threads once recording() == false. */
    juce::File             finalised_path_;

    std::atomic<bool>      _recording      { false };
    std::atomic<bool>      _stop_requested { false };
    std::atomic<nframes_t> _frames_written { 0 };

    JUCE_DECLARE_NON_COPYABLE (Record_DS)
};

} // namespace element
