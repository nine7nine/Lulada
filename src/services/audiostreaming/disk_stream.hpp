// SPDX-License-Identifier: GPL-2.0-or-later
//
// Originally from NON-DAW (Jonathan Moore Liles, 2008),
// timeline/src/Engine/Disk_Stream.H -- GPLv2-or-later.  Adapted for
// Element-NSPA with the following changes:
//   - Replaced `jack_ringbuffer_t*` per-channel rings with
//     juce::AbstractFifo + std::vector<sample_t> storage (lock-free
//     SPSC; same shape TrackerNode's launch FIFO already uses, no
//     JACK runtime dep).
//   - Replaced POSIX sem_t IO-thread wakeup with juce::WaitableEvent.
//     JUCE-NSPA swaps its underlying primitive to librtpi PiMutex +
//     PiCond (FUTEX_WAIT_REQUEUE_PI) when compiled with __WINE__
//     defined -- Element-NSPA's canonical wineg++ build hits this.
//     The signal path is a direct Linux kernel futex syscall, so the
//     audio-thread signal() boosts the IO-thread waiter via PI with
//     no wineserver involvement.
//   - Replaced NON's pthread Thread wrapper with juce::Thread.
//   - Dropped the `Track* _track` + `Audio_Sequence*` indirection
//     entirely.  Playback_DS / Record_DS receive the audio buffer
//     slice as a process() argument and stream against an
//     AudioFileSource directly -- the JUCE/Element graph model already
//     hands buffers in, so the JACK-port indirection isn't needed.
//   - process() removed from this abstract base; subclasses define
//     direction-specific signatures (output vs input buffer).

#pragma once

#include "services/audiostreaming/audio_file_types.hpp"

#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>

#include <atomic>
#include <memory>
#include <vector>

namespace element {

class Disk_Stream : public juce::Thread
{
public:
    /** Total streaming buffer per channel, in seconds.  Configure at
     *  startup before constructing any Disk_Stream.  Default 2.0 s
     *  matches NON's setting and is comfortable for spinning disks. */
    static float  seconds_to_buffer;

    /** Upper bound on the IO-thread read/write chunk size, in kibibytes.
     *  The thread groups blocks into chunks of roughly this size to keep
     *  the libsndfile call rate down on small audio blocks. */
    static std::size_t disk_io_kbytes;

    Disk_Stream (const juce::String& threadName,
                 float               frame_rate,
                 nframes_t           nframes,
                 int                 channels);

    ~Disk_Stream() override;

    /** Reconfigure for a new audio block size.  Stops and restarts the
     *  IO thread if it was running.  Audio-thread-safe to call only
     *  when the host has paused the audio callback. */
    void resize_buffers (nframes_t nframes);

    /** XRuns observed by process() -- block requested data but the
     *  ringbuffer was empty (playback) or full (record). */
    int xruns() const noexcept { return _xruns.load (std::memory_order_relaxed); }

    /** Ringbuffer fill percentage, averaged across channels.  100 = full,
     *  0 = empty.  Polled by Playback_DS::seek_pending() to throttle
     *  seeks against the available headroom. */
    int buffer_percent() const noexcept;

    /** Signals the IO thread, waits for it to observe _terminate and
     *  exit, then joins. */
    void shutdown();

protected:
    /** Number of per-channel ringbuffers.  Same as `channels` ctor arg. */
    int channels() const noexcept { return (int) _rb.size(); }

    /** Subclass IO-thread loop -- runs on the juce::Thread we inherit
     *  from; called via run().  Must check `_terminate.load()` and
     *  return promptly when it transitions to true. */
    virtual void disk_thread() = 0;

    /** Flush the ringbuffers + reset wakeup state.  Called by the
     *  subclass at the start of disk_thread or after a seek.  Wrap
     *  via base_flush() rather than calling directly. */
    virtual void flush() = 0;

    /** Resets all SPSC fifos + the wakeup event.  is_output=true means
     *  the audio thread has emptied everything and the IO thread should
     *  wake immediately to refill (playback); is_output=false means
     *  the IO thread should sleep until the audio thread starts
     *  producing (record). */
    void base_flush (bool is_output);

    /** Audio-thread side: tell the IO thread we just produced (record)
     *  or consumed (playback) a block.  juce::WaitableEvent::signal
     *  is librtpi-backed (PiCond + brief PiMutex lock) when the JUCE
     *  module is compiled with __WINE__ -- Element-NSPA's canonical
     *  wineg++ build.  Direct Linux kernel futex; no wineserver. */
    void block_processed() noexcept { _blocks.signal(); }

    /** IO-thread side: block until the audio thread signals (or
     *  shutdown was requested).  Returns false if _terminate is set. */
    bool wait_for_block() noexcept;

    /** Spawn the IO thread.  Subclasses typically call this from their
     *  ctor after they've finished initialising direction-specific
     *  state. */
    void run_io();

    /** Detach the IO thread without joining (used during destruct when
     *  a shutdown() race would deadlock).  Internal use. */
    void detach_io();

    // ---- IO-thread loop entry point -----------------------------------
    void run() override final { disk_thread(); }

    // ---- Ringbuffer accessors for subclasses --------------------------
    juce::AbstractFifo& fifo (int ch) noexcept { return *_rb[(std::size_t) ch]; }
    sample_t*           storage (int ch) noexcept { return _rb_storage[(std::size_t) ch].data(); }
    std::size_t         storage_size() const noexcept { return _rb_storage.empty() ? 0u
                                                                                  : _rb_storage[0].size(); }

    // ---- Sizing / state available to subclasses -----------------------
    nframes_t              _nframes;          // audio block size
    float                  _frame_rate;       // session sample rate (Hz)
    nframes_t              _total_blocks;     // ring capacity in blocks
    nframes_t              _disk_io_blocks;   // grouping per disk transfer

    // Current absolute playback / record frame (subclass-managed).
    std::atomic<nframes_t> _frame { 0 };

    // Seek request -- subclass-managed, audio-thread writes / IO-thread
    // reads.  juce::WaitableEvent::signal still required to wake the
    // IO thread after writing.
    std::atomic<nframes_t> _seek_frame   { 0 };
    std::atomic<bool>      _pending_seek { false };

    // Shutdown signal -- audio thread (or destructor) sets, IO thread
    // observes via wait_for_block / its own poll.
    std::atomic<bool>      _terminate { false };

    // XRun counter -- subclass increments on under/overflow.
    std::atomic<int>       _xruns { 0 };

private:
    void resize_buffers_locked (nframes_t nframes, int channels);

    // Per-channel SPSC fifos.  unique_ptr because juce::AbstractFifo is
    // non-copyable / non-movable (it holds atomic indices).
    std::vector<std::unique_ptr<juce::AbstractFifo>> _rb;
    std::vector<std::vector<sample_t>>               _rb_storage;

    // Wakeup primitive.  Under JUCE-NSPA + __WINE__ this is a librtpi
    // PiMutex + PiCond (FUTEX_WAIT_REQUEUE_PI -- direct Linux kernel
    // futex, PI-aware, no wineserver).  Otherwise plain std::mutex +
    // std::condition_variable.
    juce::WaitableEvent _blocks { false /*not manual-reset*/ };

    JUCE_DECLARE_NON_COPYABLE (Disk_Stream)
};

} // namespace element
