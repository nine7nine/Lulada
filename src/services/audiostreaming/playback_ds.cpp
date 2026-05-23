// SPDX-License-Identifier: GPL-2.0-or-later
//
// Originally from NON-DAW (Jonathan Moore Liles, 2008),
// timeline/src/Engine/Playback_DS.C -- GPLv2-or-later.  Adapted for
// Element-NSPA; see playback_ds.hpp for the list of changes.

#include "services/audiostreaming/playback_ds.hpp"
#include "services/audiostreaming/audio_file.hpp"

#include <juce_core/juce_core.h>

#include <cstring>

namespace element {

std::unique_ptr<Playback_DS>
Playback_DS::create (AudioFileSource::Ptr source,
                     float                frame_rate,
                     nframes_t            block_size,
                     int                  channels,
                     bool                 looped,
                     nframes_t            loopStartFrame)
{
    if (source == nullptr)
        return nullptr;

    const juce::String path = source->file().getFullPathName();
    if (path.isEmpty())
        return nullptr;

    Audio_File* handle = Audio_File::from_file (path.toRawUTF8());
    if (handle == nullptr)
    {
        juce::Logger::writeToLog (
            juce::String ("Playback_DS::create: Audio_File::from_file failed for ") + path);
        return nullptr;
    }

    /* std::make_unique can't see the private ctor; use new directly. */
    return std::unique_ptr<Playback_DS> (
        new Playback_DS (std::move (source), handle, frame_rate, block_size, channels,
                         looped, loopStartFrame));
}

Playback_DS::Playback_DS (AudioFileSource::Ptr source,
                          Audio_File*          handle,
                          float                frame_rate,
                          nframes_t            block_size,
                          int                  channels,
                          bool                 looped,
                          nframes_t            loopStartFrame)
    : Disk_Stream ("ElementPlaybackDS", frame_rate, block_size, channels),
      source_ (std::move (source)),
      file_   (handle),
      loop_   (looped),
      loopStart_ (loopStartFrame),
      sourceLen_ (handle != nullptr ? handle->length() : 0)
{
    run_io();
}

Playback_DS::~Playback_DS()
{
    shutdown();
    if (file_ != nullptr)
    {
        file_->release();
        file_ = nullptr;
    }
}

bool
Playback_DS::seek_pending() const noexcept
{
    if (_pending_seek.load (std::memory_order_acquire))
        return true;
    return buffer_percent() < 50;
}

void
Playback_DS::seek (nframes_t frame) noexcept
{
    /* Best-effort: if a seek is already pending we replace it -- the
     * IO thread will see the latest _seek_frame on its next wakeup.
     * Multiple seeks-while-pending coalesce to the last one, which is
     * the right behaviour for rapid scrubbing UI. */
    _seek_frame.store (frame, std::memory_order_relaxed);
    _pending_seek.store (true, std::memory_order_release);
    block_processed();   // wake the IO thread
}

void
Playback_DS::read_block (sample_t* interleaved_buf,
                         sample_t* deinterleave_buf,
                         nframes_t total_frames) noexcept
{
    const int ch = channels();
    auto      frame = _frame.load (std::memory_order_relaxed)
                    + _undelay.load (std::memory_order_relaxed);

    /* Loop wrap-around (entry side): if we already passed EOF AND
     * looping is on, snap back to the loop start before reading.
     * Keeps multiple consecutive loops working without a special-case
     * seek path. */
    if (loop_ && sourceLen_ > 0 && frame >= sourceLen_)
    {
        const nframes_t past = frame - loopStart_;
        const nframes_t cycle = sourceLen_ > loopStart_ ? sourceLen_ - loopStart_ : 1;
        frame = loopStart_ + (past % cycle);
        _frame.store (frame - _undelay.load (std::memory_order_relaxed),
                      std::memory_order_relaxed);
    }

    /* Zero-fill in case of short read (EOF). */
    std::memset (interleaved_buf, 0,
                 (std::size_t) total_frames * (std::size_t) ch * sizeof (sample_t));

    if (file_ == nullptr)
        return;

    /* Audio_File_SF::read with channel == -1 returns interleaved data
     * across all channels.  The libsndfile-internal lock protects the
     * underlying seek + read pair (juce::ScopedLock on
     * Audio_File::lock_).  Threading: this runs only on our IO thread;
     * any other access to the same Audio_File* would be a programming
     * error (each Playback_DS owns its own handle). */
    nframes_t got = file_->read (interleaved_buf, -1, frame, total_frames);

    /* Loop wrap-around (split read): if the requested span straddles
     * EOF, fill the tail by seeking to loopStart_ and reading the
     * remainder.  Single-pass straddle handles per-block crossings;
     * if the loop body is shorter than total_frames the user gets
     * acceptable v1 behaviour (partial silence past one wrap) -- a
     * cleaner multi-pass fill is a follow-up. */
    if (loop_ && sourceLen_ > 0 && got < total_frames && got > 0)
    {
        const nframes_t remaining = total_frames - got;
        sample_t* tail = interleaved_buf
            + (std::size_t) got * (std::size_t) ch;
        const nframes_t wrapped = file_->read (tail, -1, loopStart_, remaining);
        got += wrapped;
        /* _frame after this block ends up at loopStart_ + wrapped,
         * computed by the loop-entry branch on the next iteration. */
        _frame.store (loopStart_ + wrapped - _undelay.load (std::memory_order_relaxed),
                      std::memory_order_relaxed);
    }

    /* Deinterleave into the per-channel rings, one block (= _nframes
     * frames) at a time.  Splitting on block boundaries matches the
     * audio thread's per-block consumer pattern -- the ring view it
     * sees never straddles deinterleave boundaries. */
    const nframes_t blocks_avail
        = (got + _nframes - 1) / _nframes;   // round up; trailing partial block is silence-padded

    for (nframes_t b = 0; b < blocks_avail; ++b)
    {
        const nframes_t base = b * _nframes;
        for (int c = 0; c < ch; ++c)
        {
            for (nframes_t i = 0; i < _nframes; ++i)
                deinterleave_buf[i] = interleaved_buf[(base + i) * (nframes_t) ch + (nframes_t) c];

            auto& f = fifo (c);
            int s1, sz1, s2, sz2;
            f.prepareToWrite ((int) _nframes, s1, sz1, s2, sz2);

            if ((int) (sz1 + sz2) < (int) _nframes)
            {
                /* Ring full -- can happen briefly on resize / seek
                 * transitions; the wait loop below should prevent this
                 * in steady state. */
                f.finishedWrite (sz1 + sz2);
                continue;
            }

            sample_t* dst = storage (c);
            if (sz1 > 0)
                std::memcpy (dst + s1, deinterleave_buf,
                             (std::size_t) sz1 * sizeof (sample_t));
            if (sz2 > 0)
                std::memcpy (dst + s2, deinterleave_buf + sz1,
                             (std::size_t) sz2 * sizeof (sample_t));

            f.finishedWrite ((int) _nframes);
        }
    }

    _frame.fetch_add (blocks_avail * _nframes, std::memory_order_relaxed);
}

void
Playback_DS::disk_thread()
{
    /* IO-chunk buffers -- sized for `_disk_io_blocks` blocks at a
     * time.  Allocated on the heap, not the stack, to keep this
     * thread's stack small under deep IO recursion paths. */
    const std::size_t interleaved_samples
        = (std::size_t) _nframes * (std::size_t) channels() * (std::size_t) _disk_io_blocks;
    juce::HeapBlock<sample_t> interleaved_buf (interleaved_samples, true);
    juce::HeapBlock<sample_t> deinterleave_buf ((std::size_t) _nframes, true);

    base_flush (true);   // empty rings + arm wake

    while (! _terminate.load (std::memory_order_acquire))
    {
        /* If a seek is pending, jump the source-frame and flush the
         * rings so the next read fills from the new position. */
        if (_pending_seek.load (std::memory_order_acquire))
        {
            _frame.store (_seek_frame.load (std::memory_order_relaxed),
                          std::memory_order_relaxed);
            _pending_seek.store (false, std::memory_order_release);
            base_flush (true);
            continue;
        }

        /* Wait until the audio thread tells us a block was consumed.
         * Spurious wakes are fine -- we just re-check pending_seek /
         * terminate / ring fullness and either fill or sleep again. */
        if (! wait_for_block())
            break;

        if (_terminate.load (std::memory_order_acquire))
            break;

        /* Refill the rings.  We pull one IO chunk per loop iteration;
         * the loop spins so a steady consumer keeps the rings topped
         * up without WaitableEvent thrash. */
        read_block (interleaved_buf.getData(),
                    deinterleave_buf.getData(),
                    _nframes * _disk_io_blocks);
    }
}

nframes_t
Playback_DS::process (juce::AudioBuffer<float>& out,
                      int                       outStartSample,
                      nframes_t                 numFrames) noexcept
{
    const int ch = juce::jmin (channels(), out.getNumChannels());

    for (int c = 0; c < ch; ++c)
    {
        auto& f = fifo (c);
        if ((nframes_t) f.getNumReady() < numFrames)
        {
            _xruns.fetch_add (1, std::memory_order_relaxed);
            out.clear (c, outStartSample, (int) numFrames);
            continue;
        }

        int s1, sz1, s2, sz2;
        f.prepareToRead ((int) numFrames, s1, sz1, s2, sz2);
        float*       dst = out.getWritePointer (c, outStartSample);
        const float* src = storage (c);
        if (sz1 > 0) std::memcpy (dst,        src + s1, (std::size_t) sz1 * sizeof (float));
        if (sz2 > 0) std::memcpy (dst + sz1,  src + s2, (std::size_t) sz2 * sizeof (float));
        f.finishedRead ((int) numFrames);
    }

    /* Tell the IO thread we have free space.  Single signal -- even
     * if the IO thread is already running it'll re-check ring fullness
     * on its next iteration. */
    block_processed();

    return numFrames;
}

} // namespace element
