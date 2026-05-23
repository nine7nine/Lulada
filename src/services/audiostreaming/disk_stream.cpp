// SPDX-License-Identifier: GPL-2.0-or-later
//
// Originally from NON-DAW (Jonathan Moore Liles, 2008),
// timeline/src/Engine/Disk_Stream.C -- GPLv2-or-later.  Adapted for
// Element-NSPA; see disk_stream.hpp for the list of changes.

#include "services/audiostreaming/disk_stream.hpp"

#include <juce_core/juce_core.h>

#include <cassert>

namespace element {

float       Disk_Stream::seconds_to_buffer = 2.0f;
std::size_t Disk_Stream::disk_io_kbytes    = 256;

Disk_Stream::Disk_Stream (const juce::String& threadName,
                          float               frame_rate,
                          nframes_t           nframes,
                          int                 channels)
    : juce::Thread (threadName),
      _nframes (nframes),
      _frame_rate (frame_rate),
      _total_blocks (0),
      _disk_io_blocks (0)
{
    jassert (channels > 0);
    resize_buffers_locked (nframes, channels);
}

Disk_Stream::~Disk_Stream()
{
    /* Subclasses are expected to call shutdown() in their dtor before
     * base destruction runs; if they didn't, do it here defensively
     * so we don't leak the running IO thread. */
    if (isThreadRunning())
    {
        _terminate.store (true, std::memory_order_release);
        _blocks.signal();
        stopThread (2000);
    }
}

void
Disk_Stream::base_flush (bool is_output)
{
    for (auto& f : _rb)
        f->reset();

    _blocks.reset();

    /* For playback (is_output==true) the ring starts empty -> the IO
     * thread should immediately wake and refill.  For record the ring
     * starts empty too but the IO thread should wait until the audio
     * thread starts producing -- in both cases the audio thread will
     * signal as soon as it has work for the IO side, so an immediate
     * wake on playback is the only directional difference and we
     * encode it as a one-shot signal. */
    if (is_output)
        _blocks.signal();
}

void
Disk_Stream::detach_io()
{
    _terminate.store (true, std::memory_order_release);
    _blocks.signal();
    /* juce::Thread doesn't expose pthread_detach; stopThread without
     * timeout would block.  We accept that detach + immediate destruct
     * is best-effort and rely on the shutdown() path for clean exits. */
}

void
Disk_Stream::shutdown()
{
    if (! isThreadRunning())
        return;

    _terminate.store (true, std::memory_order_release);

    /* Loop signalling until the thread observes _terminate.  Mirror
     * NON's "usleep + post" loop -- the IO thread may have been
     * blocked inside a libsndfile call, so a single signal can race. */
    while (isThreadRunning())
    {
        _blocks.signal();
        juce::Thread::sleep (10);
    }

    stopThread (2000);
}

bool
Disk_Stream::wait_for_block() noexcept
{
    if (_terminate.load (std::memory_order_acquire))
        return false;

    _blocks.wait();

    return ! _terminate.load (std::memory_order_acquire);
}

void
Disk_Stream::run_io()
{
    jassert (! isThreadRunning());
    startThread();
}

void
Disk_Stream::resize_buffers (nframes_t nframes)
{
    if (nframes == _nframes)
        return;

    const bool was_running = isThreadRunning();
    if (was_running)
        shutdown();

    resize_buffers_locked (nframes, channels());
    flush();

    if (was_running)
        run_io();
}

void
Disk_Stream::resize_buffers_locked (nframes_t nframes, int channels)
{
    _rb.clear();
    _rb_storage.clear();

    _nframes = nframes;
    _total_blocks = (nframes_t) (_frame_rate * seconds_to_buffer) / nframes;
    if (_total_blocks == 0) _total_blocks = 1;

    const std::size_t bufsize_samples = (std::size_t) _total_blocks
                                      * (std::size_t) nframes;

    if (disk_io_kbytes != 0)
    {
        const std::size_t total_bytes = bufsize_samples
                                      * sizeof (sample_t)
                                      * (std::size_t) channels;
        _disk_io_blocks = (nframes_t) (total_bytes / (disk_io_kbytes * 1024u));
        if (_disk_io_blocks == 0) _disk_io_blocks = 1;
    }
    else
    {
        _disk_io_blocks = 1;
    }

    _rb_storage.reserve ((std::size_t) channels);
    _rb.reserve         ((std::size_t) channels);

    for (int i = 0; i < channels; ++i)
    {
        _rb_storage.emplace_back (bufsize_samples, 0.0f);
        /* AbstractFifo capacity must match its backing storage. */
        _rb.emplace_back (std::make_unique<juce::AbstractFifo> ((int) bufsize_samples));
    }
}

int
Disk_Stream::buffer_percent() const noexcept
{
    if (_rb.empty())
        return 0;

    const int capacity = _rb[0]->getTotalSize();
    if (capacity <= 0) return 0;

    /* Fill % averaged across channels.  All channels move in lockstep
     * under SPSC so this is essentially channel 0's value. */
    juce::int64 ready_sum = 0;
    for (const auto& f : _rb)
        ready_sum += f->getNumReady();

    const auto avg = ready_sum / (juce::int64) _rb.size();
    return (int) ((avg * 100) / capacity);
}

} // namespace element
