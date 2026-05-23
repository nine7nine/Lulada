// SPDX-License-Identifier: GPL-2.0-or-later
//
// Originally from NON-DAW (Jonathan Moore Liles, 2008),
// timeline/src/Engine/Audio_File_SF.H -- GPLv2-or-later.  Adapted for
// Element-NSPA: namespace + includes only.  Same libsndfile-backed
// implementation.  libsndfile is POSIX-native (no juce::File, no
// wineserver round-trips) -- satisfies the Linux-native I/O rule in
// timeline-audio-design.md Section 0a.

#pragma once

#include "services/audiostreaming/audio_file.hpp"

#include <sndfile.h>

namespace element {

class Audio_File_SF : public Audio_File
{
public:
    static const Audio_File::format_desc supported_formats[];

    static Audio_File_SF* from_file (const char* filename);

    /* Creates a new soundfile for writing (capture path).  `filename`
     * is taken without extension; the matching `format` entry from
     * `supported_formats` selects the encoding and extension.
     *
     * Return values mirror NON's:
     *   - non-null Audio_File_SF*  -> ready for write()
     *   - nullptr                  -> sf_open failed (disk full,
     *                                 permission denied, etc.)
     *   - (Audio_File_SF*) 1       -> format name not recognised
     */
    static Audio_File_SF* create (const char* filename,
                                  nframes_t   samplerate,
                                  int         channels,
                                  const char* format);

    ~Audio_File_SF() override
    {
        close();
    }

    bool      open() override;
    void      close() override;
    void      seek (nframes_t offset) override;
    nframes_t read (sample_t* buf, int channel, nframes_t len) override;
    nframes_t read (sample_t* buf, int channel,
                    nframes_t start, nframes_t len) override;
    nframes_t write (sample_t* buf, nframes_t nframes) override;

private:
    Audio_File_SF() noexcept
        : _in (nullptr),
          _current_read (0)
    {}

    SNDFILE*           _in;

    /* libsndfile lacks a no-op seek; cache the last read position so
     * we only emit sf_seek when the offset actually changes. */
    volatile nframes_t _current_read;
};

} // namespace element
