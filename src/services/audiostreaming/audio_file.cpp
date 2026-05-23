// SPDX-License-Identifier: GPL-2.0-or-later
//
// Originally from NON-DAW (Jonathan Moore Liles, 2008),
// timeline/src/Engine/Audio_File.C -- GPLv2-or-later.  Adapted for
// Element-NSPA; see audio_file.hpp for the list of changes.

#include "services/audiostreaming/audio_file.hpp"
#include "services/audiostreaming/audio_file_sf.hpp"

#include <juce_core/juce_core.h>

#include <cstdlib>
#include <cstring>
#include <strings.h>   // POSIX strcasecmp -- not in <cstring>'s std:: scope

namespace element {

Audio_File::~Audio_File()
{
    if (_filename != nullptr)
        std::free (_filename);
    if (_path != nullptr)
        std::free (_path);
}

const Audio_File::format_desc*
Audio_File::find_format (const format_desc* fd, const char* name)
{
    for (; fd->name; ++fd)
        if (std::strcmp (fd->name, name) == 0)
            return fd;
    return nullptr;
}

void
Audio_File::all_supported_formats (std::list<const char*>& formats)
{
    const format_desc* fd = Audio_File_SF::supported_formats;
    for (; fd->name; ++fd)
        formats.push_back (fd->name);
}

Audio_File*
Audio_File::from_file (const char* filename)
{
    if (filename == nullptr || *filename == '\0')
        return nullptr;

    if (Audio_File* a = Audio_File_SF::from_file (filename))
        return a;

    return nullptr;
}

Audio_File*
Audio_File::duplicate() noexcept
{
    /* libsndfile seeks cheaply on WAV-class files but expensively on
     * OGG / FLAC; NON's original code opened a fresh fd for the
     * poor-seeker formats to avoid serialising disk threads on a
     * single seek-heavy fd.  Element preserves that policy. */
    auto is_poor_seeker = [] (const char* fn) noexcept
    {
        if (fn == nullptr) return false;
        const auto len = std::strlen (fn);
        if (len > 4 && ::strcasecmp (fn + len - 4, ".ogg")  == 0) return true;
        if (len > 5 && ::strcasecmp (fn + len - 5, ".flac") == 0) return true;
        return false;
    };

    if (is_poor_seeker (_filename))
        return from_file (_filename);

    ++_refs;
    return this;
}

void
Audio_File::release() noexcept
{
    if (--_refs == 0)
        delete this;
}

} // namespace element
