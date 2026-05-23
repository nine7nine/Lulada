// SPDX-License-Identifier: GPL-2.0-or-later
//
// Originally from NON-DAW (Jonathan Moore Liles, 2008),
// timeline/src/Engine/Audio_File.H -- GPLv2-or-later.  Adapted for
// Element-NSPA (GPL-3-or-later combined work) with the following
// changes:
//   - Wrapped in `namespace element`.
//   - Replaced NON's Mutex base class with juce::CriticalSection
//     composition.  Under winelib (__WINE__) juce::CriticalSection is
//     backed by librtpi's pi_mutex_t (FUTEX_LOCK_PI with NSPA
//     recursive extension) -- inherits RT priority across audio /
//     disk-IO thread boundaries.  See JUCE-NSPA
//     modules/juce_core/threads/juce_CriticalSection.h:125-132 and
//     modules/juce_core/native/juce_winelib_rtpi.h.
//   - Stripped Peaks member + read_peaks() (JUCE AudioThumbnail
//     handles waveform caching in the Element UI layer).
//   - Stripped Audio_File_Dummy fallback; on open failure callers get
//     a null pointer.
//   - Stripped the static `_open_files` dedup map; Element's
//     SourceRegistry (services/sources/sourceregistry.hpp) handles
//     audio-file dedup by uuid at a higher level.
//   - Stripped Block_Timer diagnostic.
//   - Stripped the `sources/`-relative `path()` helper; Element always
//     passes absolute paths (juce::File::getFullPathName).

#pragma once

#include "services/audiostreaming/audio_file_types.hpp"

#include <juce_core/juce_core.h>

#include <list>

namespace element {

class Audio_File
{
public:
    /* Format descriptor.  Public so derived classes (Audio_File_SF)
     * can declare static arrays of supported formats. */
    struct format_desc
    {
        const char* name;
        const char* extension;
        unsigned long id;
        int quality;
    };

    Audio_File() noexcept
        : _filename (nullptr),
          _path     (nullptr),
          _length   (0),
          _samplerate (0),
          _channels (0),
          _refs     (1)
    {}

    virtual ~Audio_File();

    /* Dummy / placeholder source override.  Element-NSPA does not ship
     * the Audio_File_Dummy fallback; this returns false unconditionally
     * and exists only as the polymorphic hook callers expected. */
    virtual bool dummy() const noexcept { return false; }

    static void all_supported_formats (std::list<const char*>& formats);

    /* Attempts to open `filename` via the registered backend formats.
     * Returns nullptr on failure (unlike NON, which substituted an
     * Audio_File_Dummy). */
    static Audio_File* from_file (const char* filename);

    /* Refcount management.  Audio_File instances are shared between
     * regions; release() decrements and deletes on the last ref. */
    void release() noexcept;
    Audio_File* duplicate() noexcept;

    const char* filename() const noexcept { return _path; }
    const char* name()     const noexcept { return _filename; }
    nframes_t   length()   const noexcept { return _length; }
    int         channels() const noexcept { return _channels; }
    nframes_t   samplerate() const noexcept { return _samplerate; }

    virtual bool      open()                                                  = 0;
    virtual void      close()                                                 = 0;
    virtual void      seek (nframes_t offset)                                 = 0;
    virtual nframes_t read (sample_t* buf, int channel, nframes_t len)        = 0;
    virtual nframes_t read (sample_t* buf, int channel,
                            nframes_t start, nframes_t len)                   = 0;
    virtual nframes_t write (sample_t* buf, nframes_t len)                    = 0;

    /* Finalisation hook for derived writers.  NON used this to close
     * out the Peak_Writer; Element relies on JUCE AudioThumbnail so
     * the base implementation is a no-op. */
    virtual void finalize() {}

    /* Public so callers can validate a format string before opening a
     * capture stream (Record_DS::create gates on this). */
    static const format_desc* find_format (const format_desc* fd, const char* name);

protected:
    char*               _filename;     /* short name (basename) */
    char*               _path;         /* absolute path */

    volatile nframes_t  _length;       /* length of file in samples */
    nframes_t           _samplerate;
    int                 _channels;

    /* librtpi-backed under winelib (juce::CriticalSection ->
     * pi_mutex_t with FUTEX_LOCK_PI).  Used by Audio_File_SF to
     * serialise libsndfile calls between disk-IO and (rare) UI-thread
     * metadata reads. */
    mutable juce::CriticalSection lock_;

private:
    int _refs;

    Audio_File (const Audio_File&)            = delete;
    Audio_File& operator= (const Audio_File&) = delete;
};

} // namespace element
