# SPDX-FileCopyrightText: 2026 Kushview, LLC
# SPDX-License-Identifier: GPL-3.0-or-later

# Element static library
add_library(element STATIC)
add_library(kv::element ALIAS element)

set_target_properties(element PROPERTIES 
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

target_include_directories(element
    PUBLIC
        "${GENERATED_INCLUDE_DIR}"
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/src/lua/sol"
        "${PROJECT_SOURCE_DIR}/src/lua/src"
        "${PROJECT_SOURCE_DIR}/deps/clap-juce-extensions/clap-libs/clap/include"
        "${PROJECT_SOURCE_DIR}/deps/clap-juce-extensions/clap-libs/clap-helpers/include"
        "${Boost_INCLUDE_DIRS}"       
)

# Re-glob marker: bump this file's mtime to force CMake to re-scan
# after adding new sources under src/.  Updated 2026-05-28 for F2.1
# tracker loop wiring -- new test/tracker_loop_tests.cpp + accessor.
file(GLOB_RECURSE ELEMENT_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.c")

target_sources(element PRIVATE ${ELEMENT_SOURCES})

if(APPLE)
    target_sources(element PRIVATE 
        "${CMAKE_CURRENT_SOURCE_DIR}/ui/nsviewwithparent.mm"
        "${CMAKE_CURRENT_SOURCE_DIR}/urlhandler.mm"
    )
endif()

target_compile_definitions(element 
    PUBLIC BOOST_ALL_NO_LIB=1)

target_compile_options(element PRIVATE ${SUIL_CFLAGS_OTHER})

if(JACK_FOUND)
    message(STATUS "JACK audio driver enabled")
    target_compile_definitions(element PUBLIC ELEMENT_USE_JACK=1)
    target_include_directories(element PUBLIC ${JACK_INCLUDE_DIRS})
endif()

# libsndfile -- backs services/audiostreaming/audio_file_sf.{hpp,cpp},
# the NON-ported disk streaming path that AudioClipNode consumes for
# timeline audio playback / recording.  POSIX-native (no juce::File,
# no wineserver round-trips) per timeline-audio-design.md Section 0a.
if(PkgConfig_FOUND)
    pkg_check_modules(SNDFILE REQUIRED sndfile)
    target_include_directories(element PUBLIC ${SNDFILE_INCLUDE_DIRS})
    target_link_libraries(element PUBLIC ${SNDFILE_LIBRARIES})
    target_link_directories(element PUBLIC ${SNDFILE_LIBRARY_DIRS})
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(element PRIVATE -Wno-sign-conversion -Wno-switch-enum -Wno-attributes)
elseif(MSVC)
    # MSVC doesn't support -Wno-sign-conversion; disable a relevant warning if needed
    # e.g. /wd4244 or /wd4267 depending on the warning you want to suppress
    target_compile_options(element PRIVATE /wd4244)
endif()


target_compile_definitions(element
    PUBLIC
        JUCE_MODAL_LOOPS_PERMITTED=1
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=1
        JUCE_USE_XRENDER=1
        JUCE_LOAD_CURL_SYMBOLS_LAZILY=1
        JUCE_PLUGINHOST_LV2=1
        JUCE_PLUGINHOST_VST3=1
        JUCE_PLUGINHOST_AU=1
        JUCE_VST3_CAN_REPLACE_VST2=0
        # Element: JACK-only audio + MIDI on this fork — JUCE's ALSA
        # backend (audio device + ALSA-seq MIDI) is compiled out
        # entirely.  Native JACK handles both directions with sample-
        # accurate, RT-inline delivery.  See engine/jack.cpp +
        # engine/midiengine.cpp for the JACK pipeline.
        JUCE_ALSA=0
        JUCE_USE_ALSA=0)

target_link_libraries(element
    PUBLIC
        juce::juce_core
        juce::juce_gui_basics
        juce::juce_gui_extra
        juce::juce_opengl
        juce::juce_osc
        juce::juce_dsp
        juce::juce_data_structures
        juce::juce_audio_basics
        juce::juce_audio_devices
        juce::juce_audio_processors
        juce::juce_audio_formats
        juce::juce_audio_utils
        juce::juce_recommended_config_flags
        element-binarydata
        element-luascripts
        element-luamods
        sol2::sol2
    )

if(ELEMENT_ENABLE_UPDATER)
    message(STATUS "Element updater enabled")
    target_compile_definitions(element PUBLIC ELEMENT_UPDATER=1)
endif()

if(ELEMENT_LOCAL_AUTH)
    message(STATUS "Element local auth endpoints enabled")
    target_compile_definitions(element PRIVATE ELEMENT_LOCAL_AUTH=1)
endif()

if(ELEMENT_ENABLE_LTO)
    message("Link Time Optimization enabled")
    target_link_libraries(element PUBLIC juce::juce_recommended_lto_flags)
endif()

if(WIN32 AND ELEMENT_ENABLE_ASIO)
    message("ASIO audio device support enabled")
    target_include_directories(element PUBLIC "${ASIOSDK_SOURCE_DIR}/common")
    target_compile_definitions(element PUBLIC JUCE_ASIO=1)
endif()

# Winelib host: enable JUCE's ASIO driver support so the audio callback
# thread is a real Win32 thread (proper TEB / COM apartment / TLS init)
# instead of a JUCE-promoted pthread.  Solves the pthread-calls-Win32-
# plugin context-mismatch class of failures (the same one yabridge
# documents via its Win32Thread wrapper in wine-host/utils.h).
#
# On Linux, JACK lands the audio through wineasio.dll or the locally
# built nspaasio.dll, both of which register under HKLM\Software\ASIO
# and JUCE's ASIO driver enumerator picks up.  We use JUCE's bundled
# ASIO SDK headers (native/asio/iasiodrv.h) — no external Steinberg SDK
# download required.  JUCE_ASIO_USE_EXTERNAL_SDK stays at its default 0.
if(CMAKE_CXX_COMPILER MATCHES "wineg\\+\\+")
    message(STATUS "Winelib ASIO support enabled (JUCE bundled SDK)")
    target_compile_definitions(element PUBLIC JUCE_ASIO=1)
endif()

# CLAP host support — only enabled under winelib, where JUCE-NSPA's
# CLAPPluginFormat loads Windows .clap PE DLLs via Wine's LoadLibraryW.
# Native Linux CLAP hosting is handled by Element's own CLAPProvider
# (engine/clapprovider.cpp), which the context.cpp wire-up gates off
# under __WINE__ so we don't double-register the format.
if(CMAKE_CXX_COMPILER MATCHES "wineg\\+\\+")
    message(STATUS "Winelib CLAP host support enabled (JUCE-NSPA CLAPPluginFormat)")
    target_compile_definitions(element PUBLIC JUCE_PLUGINHOST_CLAP=1)
endif()

# VST2 enable host support
#
# Under winelib, always enable VST2 host support and point at the
# system-installed Steinberg VST2 SDK 2.4 (Arch's `vst2sdk` package,
# installed at /usr/src/vst2sdk/).  We can't ship our own copy of
# Steinberg's SDK — its license forbids redistribution — and we can't
# extend a clean-room replacement like vestige with Steinberg-derived
# symbols without tainting the clean-room status.  System install is
# the only viable path for JUCE-as-host coverage (vestige is too thin;
# JUCE references ERect, effVendorSpecific, kVstTransportRecording,
# VstTimeInfo::smpteFrameRate, etc.).
if(CMAKE_CXX_COMPILER MATCHES "wineg\\+\\+")
    set(ELEMENT_ENABLE_VST2 ON CACHE BOOL "Auto-enabled under winelib (Steinberg VST2 SDK 2.4)" FORCE)
    set(ELEMENT_VST2_SDK_PATH "/usr/src/vst2sdk"
        CACHE PATH "Auto-set under winelib to Arch's vst2sdk package (Steinberg 2.4)" FORCE)
endif()

if(ELEMENT_ENABLE_VST2)
    message(STATUS "VST2 Plugin support enabled (SDK path: ${ELEMENT_VST2_SDK_PATH})")
    juce_set_vst2_sdk_path("${ELEMENT_VST2_SDK_PATH}")
    target_compile_definitions(element PUBLIC JUCE_PLUGINHOST_VST=1)
    target_link_libraries(element PUBLIC juce_vst2_sdk)
endif()

# MSVC 14.44+ constexpr mutex constructor workaround (debug only).
# See CMakeLists.txt in the root for explanation.
if(MSVC)
    target_compile_definitions(element PRIVATE
        $<$<CONFIG:Debug>:_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR>)
endif()
