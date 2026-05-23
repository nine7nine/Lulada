// SPDX-License-Identifier: GPL-2.0-or-later
//
// Originally from NON-DAW (Jonathan Moore Liles, 2008),
// timeline/src/Engine/Record_DS.C -- GPLv2-or-later.  Adapted for
// Element-NSPA; see record_ds.hpp for the list of changes.

#include "services/audiostreaming/record_ds.hpp"
#include "services/audiostreaming/audio_file_sf.hpp"

#include <juce_core/juce_core.h>

#include <cstring>

namespace element {

std::unique_ptr<Record_DS>
Record_DS::create (const juce::File&   outputBasename,
                   const juce::String& format,
                   nframes_t           file_samplerate,
                   float               frame_rate,
                   nframes_t           block_size,
                   int                 channels)
{
    if (outputBasename.getFullPathName().isEmpty() || channels <= 0)
        return nullptr;

    /* Validate format up-front.  Audio_File_SF::create returns sentinel
     * (Audio_File_SF*) 1 on unknown format -- we don't want to find
     * that out only when the audio thread first writes. */
    if (Audio_File::find_format (Audio_File_SF::supported_formats,
                                 format.toRawUTF8()) == nullptr)
    {
        juce::Logger::writeToLog (
            juce::String ("Record_DS::create: unknown format ") + format);
        return nullptr;
    }

    return std::unique_ptr<Record_DS> (
        new Record_DS (outputBasename, format, file_samplerate,
                       frame_rate, block_size, channels));
}

Record_DS::Record_DS (const juce::File&   outputBasename,
                      const juce::String& format,
                      nframes_t           file_samplerate,
                      float               frame_rate,
                      nframes_t           block_size,
                      int                 channels)
    : Disk_Stream ("ElementRecordDS", frame_rate, block_size, channels),
      output_basename_ (outputBasename),
      format_ (format),
      file_samplerate_ (file_samplerate)
{
    run_io();
}

Record_DS::~Record_DS()
{
    if (_recording.load (std::memory_order_acquire))
        stop();
    shutdown();

    /* Defensive: if the IO thread didn't finalise (e.g. shutdown
     * before stop()), close the file here so we don't leak the fd. */
    if (af_ != nullptr)
    {
        af_->finalize();
        delete af_;
        af_ = nullptr;
    }
}

void
Record_DS::start (nframes_t start_source_frame) noexcept
{
    if (_recording.load (std::memory_order_acquire))
        return;

    _frame.store (start_source_frame, std::memory_order_relaxed);
    _frames_written.store (0, std::memory_order_relaxed);
    _stop_requested.store (false, std::memory_order_relaxed);
    _recording.store (true, std::memory_order_release);
    block_processed();   // wake IO thread so it starts polling immediately
}

void
Record_DS::stop() noexcept
{
    _stop_requested.store (true, std::memory_order_release);
    block_processed();   // wake IO thread so it observes the flag promptly
}

nframes_t
Record_DS::process (const juce::AudioBuffer<float>& in,
                    int                             inStartSample,
                    nframes_t                       numFrames) noexcept
{
    if (! _recording.load (std::memory_order_acquire))
        return 0;

    const int ch = juce::jmin (channels(), in.getNumChannels());

    for (int c = 0; c < ch; ++c)
    {
        auto& f = fifo (c);
        if ((nframes_t) f.getFreeSpace() < numFrames)
        {
            _xruns.fetch_add (1, std::memory_order_relaxed);
            continue;
        }

        int s1, sz1, s2, sz2;
        f.prepareToWrite ((int) numFrames, s1, sz1, s2, sz2);
        const float* src = in.getReadPointer (c, inStartSample);
        sample_t*    dst = storage (c);
        if (sz1 > 0) std::memcpy (dst + s1,   src,        (std::size_t) sz1 * sizeof (float));
        if (sz2 > 0) std::memcpy (dst + s2,   src + sz1,  (std::size_t) sz2 * sizeof (float));
        f.finishedWrite ((int) numFrames);
    }

    block_processed();
    return numFrames;
}

bool
Record_DS::write_block (sample_t* interleave_buf, nframes_t nframes) noexcept
{
    /* Lazy-create the Audio_File_SF on first write.  Doing it here
     * (rather than at construct time) keeps a "create then immediately
     * stop without recording" path from leaving a zero-byte file on
     * disk. */
    if (af_ == nullptr)
    {
        Audio_File_SF* sf = Audio_File_SF::create (
            output_basename_.getFullPathName().toRawUTF8(),
            file_samplerate_, channels(), format_.toRawUTF8());

        /* (Audio_File_SF*) 1 is the unknown-format sentinel; create()
         * gates against that, so reaching here with sf == 1 means the
         * format table changed under us.  Treat as fatal-for-capture. */
        if (sf == nullptr || sf == reinterpret_cast<Audio_File_SF*> (1))
        {
            juce::Logger::writeToLog (
                juce::String ("Record_DS::write_block: Audio_File_SF::create failed for ")
                + output_basename_.getFullPathName());
            return false;
        }

        af_ = sf;
        /* af_->filename() is the absolute path including the appended
         * extension -- record that for finalised_file(). */
        finalised_path_ = juce::File (juce::String (af_->filename()));
    }

    const int ch = channels();

    /* Drain one block per channel from rings, interleave into the
     * write buffer.  All channels move in lockstep under SPSC. */
    for (int c = 0; c < ch; ++c)
    {
        auto& f = fifo (c);
        if ((nframes_t) f.getNumReady() < nframes)
        {
            /* Short read -- ring drained mid-write.  Pad with silence
             * for the channels still readable; this happens at stop
             * time when the audio thread has already stopped producing. */
            int avail = f.getNumReady();
            if (avail > 0)
            {
                int s1, sz1, s2, sz2;
                f.prepareToRead (avail, s1, sz1, s2, sz2);
                sample_t* src = storage (c);
                for (int i = 0; i < sz1; ++i)
                    interleave_buf[(std::size_t) i * (std::size_t) ch + (std::size_t) c] = src[s1 + i];
                for (int i = 0; i < sz2; ++i)
                    interleave_buf[(std::size_t) (sz1 + i) * (std::size_t) ch + (std::size_t) c] = src[s2 + i];
                f.finishedRead (avail);
            }
            /* Zero the rest of this channel. */
            for (nframes_t i = (nframes_t) avail; i < nframes; ++i)
                interleave_buf[(std::size_t) i * (std::size_t) ch + (std::size_t) c] = 0.0f;
            continue;
        }

        int s1, sz1, s2, sz2;
        f.prepareToRead ((int) nframes, s1, sz1, s2, sz2);
        sample_t* src = storage (c);

        /* Interleave: place sample i of channel c at slot
         * i * ch + c.  Two segments handled separately. */
        for (int i = 0; i < sz1; ++i)
            interleave_buf[(std::size_t) i * (std::size_t) ch + (std::size_t) c] = src[s1 + i];
        for (int i = 0; i < sz2; ++i)
            interleave_buf[(std::size_t) (sz1 + i) * (std::size_t) ch + (std::size_t) c] = src[s2 + i];

        f.finishedRead ((int) nframes);
    }

    const nframes_t w = af_->write (interleave_buf, nframes);
    _frames_written.fetch_add (w, std::memory_order_relaxed);
    _frame.fetch_add (w, std::memory_order_relaxed);
    return true;
}

void
Record_DS::disk_thread()
{
    /* Single block worth of interleaved data per IO chunk -- mirrors
     * NON's `_disk_io_blocks = 1` override for the record path.
     * Capture latency is dominated by the audio block size, not the
     * IO grouping, so larger chunks would only delay the write. */
    juce::HeapBlock<sample_t> interleave_buf (
        (std::size_t) _nframes * (std::size_t) channels(), true);

    base_flush (false);

    /* Wait until start() arms us. */
    while (! _terminate.load (std::memory_order_acquire))
    {
        if (_recording.load (std::memory_order_acquire))
            break;
        if (! wait_for_block())
            return;
    }

    while (! _terminate.load (std::memory_order_acquire))
    {
        if (! wait_for_block())
            break;

        /* Drain any pending blocks while data is available -- a single
         * audio-thread signal can correspond to multiple blocks in
         * flight when the IO thread was descheduled briefly. */
        bool drained_one = false;
        while ((nframes_t) fifo (0).getNumReady() >= _nframes)
        {
            if (! write_block (interleave_buf.getData(), _nframes))
            {
                /* Audio_File_SF::create failure -- abort capture. */
                _recording.store (false, std::memory_order_release);
                return;
            }
            drained_one = true;
        }

        /* Stop handling: drain the tail end (sub-block remainder) and
         * finalise.  We only enter this branch once the audio thread
         * has stopped producing, so getNumReady() is monotonically
         * non-increasing. */
        if (_stop_requested.load (std::memory_order_acquire))
        {
            const int leftover = fifo (0).getNumReady();
            if (leftover > 0)
            {
                /* Write the partial trailing block. */
                write_block (interleave_buf.getData(), (nframes_t) leftover);
            }

            if (af_ != nullptr)
            {
                af_->finalize();
                delete af_;
                af_ = nullptr;
            }

            _recording.store (false, std::memory_order_release);
            return;
        }

        if (! drained_one)
        {
            /* Spurious wake -- nothing yet.  Loop back and re-wait.
             * (Don't burn CPU if the audio thread hasn't produced.) */
        }
    }

    /* Terminated without a clean stop -- close the file defensively. */
    if (af_ != nullptr)
    {
        af_->finalize();
        delete af_;
        af_ = nullptr;
    }
    _recording.store (false, std::memory_order_release);
}

} // namespace element
