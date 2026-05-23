// SPDX-License-Identifier: GPL-2.0-or-later
//
// Originally from NON-DAW (Jonathan Moore Liles, 2008),
// timeline/src/Engine/Audio_File_SF.C -- GPLv2-or-later.  Adapted for
// Element-NSPA: namespace + includes only; the read/seek/write
// surface is structurally identical to NON's libsndfile backend.
// Stripped: Peak_Writer hook in write() (replaced by JUCE
// AudioThumbnail in the UI).

#include "services/audiostreaming/audio_file_sf.hpp"

#include <juce_core/juce_core.h>

#include <cassert>
#include <cstdlib>
#include <cstring>

namespace element {

const Audio_File::format_desc Audio_File_SF::supported_formats[] =
{
    { "Wav 24",   "wav",  SF_FORMAT_WAV  | SF_FORMAT_PCM_24 | SF_ENDIAN_FILE, 0 },
    { "Wav 16",   "wav",  SF_FORMAT_WAV  | SF_FORMAT_PCM_16 | SF_ENDIAN_FILE, 0 },
    { "Wav f32",  "wav",  SF_FORMAT_WAV  | SF_FORMAT_FLOAT  | SF_ENDIAN_FILE, 0 },
    { "W64 24",   "w64",  SF_FORMAT_W64  | SF_FORMAT_PCM_24 | SF_ENDIAN_FILE, 0 },
    { "W64 16",   "w64",  SF_FORMAT_W64  | SF_FORMAT_PCM_16 | SF_ENDIAN_FILE, 0 },
    { "W64 f32",  "w64",  SF_FORMAT_W64  | SF_FORMAT_FLOAT  | SF_ENDIAN_FILE, 0 },
    { "Au 24",    "au",   SF_FORMAT_AU   | SF_FORMAT_PCM_24 | SF_ENDIAN_FILE, 0 },
    { "Au 16",    "au",   SF_FORMAT_AU   | SF_FORMAT_PCM_16 | SF_ENDIAN_FILE, 0 },
    { "FLAC",     "flac", SF_FORMAT_FLAC | SF_FORMAT_PCM_24,                  0 },
#ifdef HAVE_SF_FORMAT_VORBIS
    { "Vorbis q10", "ogg", SF_FORMAT_OGG | SF_FORMAT_VORBIS, 10 },
    { "Vorbis q6",  "ogg", SF_FORMAT_OGG | SF_FORMAT_VORBIS, 6  },
    { "Vorbis q3",  "ogg", SF_FORMAT_OGG | SF_FORMAT_VORBIS, 3  },
#endif
    { nullptr, nullptr, 0, 0 }
};

Audio_File_SF*
Audio_File_SF::from_file (const char* filename)
{
    SF_INFO si;
    std::memset (&si, 0, sizeof (si));

    SNDFILE* in = sf_open (filename, SFM_READ, &si);
    if (in == nullptr)
        return nullptr;

    auto* c = new Audio_File_SF;

    c->_current_read = 0;
    c->_filename     = strdup (filename);
    c->_path         = strdup (filename);
    c->_length       = (nframes_t) si.frames;
    c->_samplerate   = (nframes_t) si.samplerate;
    c->_channels     = si.channels;
    c->_in           = in;

    return c;
}

Audio_File_SF*
Audio_File_SF::create (const char* filename,
                       nframes_t   samplerate,
                       int         channels,
                       const char* format)
{
    const Audio_File::format_desc* fd
        = Audio_File::find_format (Audio_File_SF::supported_formats, format);

    if (fd == nullptr)
        return (Audio_File_SF*) 1;

    SF_INFO si;
    std::memset (&si, 0, sizeof (si));
    si.samplerate = (int) samplerate;
    si.channels   = channels;
    si.format     = (int) fd->id;

    /* Append the format's extension if the caller did not. */
    char* name = nullptr;
    if (asprintf (&name, "%s.%s", filename, fd->extension) < 0 || name == nullptr)
        return nullptr;

    SNDFILE* out = sf_open (name, SFM_WRITE, &si);
    if (out == nullptr)
    {
        juce::Logger::writeToLog (
            juce::String ("Audio_File_SF::create: sf_open failed for ") + name);
        std::free (name);
        return nullptr;
    }

    if (std::strcmp (fd->extension, "ogg") == 0)
    {
        /* libsndfile uses [0..1] quality scale; map back from NON's
         * 1..11 step indexing. */
        double quality = (fd->quality + 1) / (double) 11.0;
        sf_command (out, SFC_SET_VBR_ENCODING_QUALITY, &quality, sizeof (double));
    }

    auto* c = new Audio_File_SF;
    c->_filename   = name;        /* takes ownership */
    c->_path       = strdup (name);
    c->_length     = 0;
    c->_samplerate = samplerate;
    c->_channels   = channels;
    c->_in         = out;

    return c;
}

bool
Audio_File_SF::open()
{
    SF_INFO si;
    std::memset (&si, 0, sizeof (si));

    assert (_in == nullptr);

    _in = sf_open (_path, SFM_READ, &si);
    if (_in == nullptr)
        return false;

    _current_read = 0;
    _length       = (nframes_t) si.frames;
    _samplerate   = (nframes_t) si.samplerate;
    _channels     = si.channels;
    return true;
}

void
Audio_File_SF::close()
{
    if (_in != nullptr)
        sf_close (_in);
    _in = nullptr;
}

void
Audio_File_SF::seek (nframes_t offset)
{
    const juce::ScopedLock sl (lock_);

    if (offset != _current_read)
    {
        sf_seek (_in, (sf_count_t) offset, SEEK_SET | SFM_READ);
        _current_read = offset;
    }
}

nframes_t
Audio_File_SF::read (sample_t* buf, int channel, nframes_t len)
{
    if (len > 256u * 100u)
    {
        juce::Logger::writeToLog (
            juce::String::formatted (
                "Audio_File_SF::read: suspiciously large read request (%lu frames)",
                (unsigned long) len));
    }

    const juce::ScopedLock sl (lock_);

    nframes_t rlen = 0;

    if (_channels == 1 || channel == -1)
    {
        rlen = (nframes_t) sf_readf_float (_in, buf, (sf_count_t) len);
    }
    else
    {
        sample_t* tmp = new sample_t[len * (nframes_t) _channels];
        rlen = (nframes_t) sf_readf_float (_in, tmp, (sf_count_t) len);

        /* Deinterleave the requested channel into the caller's buf. */
        for (nframes_t i = (nframes_t) channel;
             i < rlen * (nframes_t) _channels;
             i += (nframes_t) _channels)
        {
            *(buf++) = tmp[i];
        }

        delete[] tmp;
    }

    _current_read += rlen;
    return rlen;
}

nframes_t
Audio_File_SF::read (sample_t* buf, int channel, nframes_t start, nframes_t len)
{
    const juce::ScopedLock sl (lock_);
    seek (start);
    return read (buf, channel, len);
}

nframes_t
Audio_File_SF::write (sample_t* buf, nframes_t nframes)
{
    const juce::ScopedLock sl (lock_);
    const nframes_t l = (nframes_t) sf_writef_float (_in, buf, (sf_count_t) nframes);
    _length += l;
    return l;
}

} // namespace element
