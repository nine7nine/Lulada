// Copyright 2023 Kushview, LLC <info@kushview.net>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <element/application.hpp>

#if defined (__WINE__)
// Diagnostic: log how Element actually exits.  We've eliminated every
// architectural hypothesis for the plugin crash but the process still
// dies without leaving a clear cause.  These handlers tell us whether
// exit is via std::terminate (C++ unhandled exception), a unix signal
// (SEGV / ABORT / TERM), or a clean atexit (normal shutdown — meaning
// the plugin crash is causing Element to gracefully quit somehow).
//
// Output: /tmp/element-exit.log (line-buffered so we get the message
// even if process is killed before fflush).
#include <cstdio>
#include <csignal>
#include <cstdlib>
#include <exception>

namespace {
    FILE* g_exit_log = nullptr;

    void exit_log_init() noexcept
    {
        g_exit_log = std::fopen ("/tmp/element-exit.log", "w");
        if (g_exit_log != nullptr)
            std::setvbuf (g_exit_log, nullptr, _IOLBF, 0);
    }

    void exit_log_write (const char* tag, const char* detail) noexcept
    {
        if (g_exit_log == nullptr) return;
        std::fprintf (g_exit_log, "[%s] %s\n", tag, detail);
        std::fflush (g_exit_log);
    }

    void on_signal (int sig) noexcept
    {
        const char* name = "?";
        switch (sig) {
            case SIGSEGV: name = "SIGSEGV"; break;
            case SIGABRT: name = "SIGABRT"; break;
            case SIGTERM: name = "SIGTERM"; break;
            case SIGHUP:  name = "SIGHUP";  break;
            case SIGINT:  name = "SIGINT";  break;
            case SIGFPE:  name = "SIGFPE";  break;
            case SIGBUS:  name = "SIGBUS";  break;
            case SIGPIPE: name = "SIGPIPE"; break;
            default:      name = "OTHER";   break;
        }
        exit_log_write ("SIGNAL", name);
        // Re-raise with default handler so we still see a core dump if any.
        std::signal (sig, SIG_DFL);
        std::raise (sig);
    }

    void on_terminate() noexcept
    {
        const char* what_msg = "<no current exception>";
        try
        {
            if (auto eptr = std::current_exception())
            {
                std::rethrow_exception (eptr);
            }
        }
        catch (const std::exception& e) { what_msg = e.what(); }
        catch (...)                     { what_msg = "<non-std::exception>"; }
        exit_log_write ("TERMINATE", what_msg);
        std::abort();  // produces SIGABRT → signal handler → log → core
    }

    void on_atexit() noexcept
    {
        exit_log_write ("ATEXIT", "process returning normally");
    }

    struct WinelibExitDiagnostics
    {
        WinelibExitDiagnostics() noexcept
        {
            exit_log_init();
            exit_log_write ("START", "Element launched");
            std::signal (SIGSEGV, on_signal);
            std::signal (SIGABRT, on_signal);
            std::signal (SIGTERM, on_signal);
            std::signal (SIGHUP,  on_signal);
            std::signal (SIGINT,  on_signal);
            std::signal (SIGFPE,  on_signal);
            std::signal (SIGBUS,  on_signal);
            // NOTE: SIGPIPE intentionally not caught — wine + JACK use it.
            std::set_terminate (on_terminate);
            std::atexit (on_atexit);
        }
    };
    static WinelibExitDiagnostics winelib_exit_diagnostics {};
}
#endif

#if defined (__WINE__)
// Winelib host: replicate yabridge-host's pre-plugin-load init.
// Source of truth is yabridge/src/wine-host/host.cpp lines 95-112.
//
// Both calls are required BEFORE any Windows VST3 plugin's static init
// runs.  Without them, u-he plugins (and others) initialize their COM
// state against an uninitialized OLE subsystem, and their internal
// boost::thread pools fail pthread_create for SCHED_FIFO threads
// because the parent process's priority class hasn't been promoted to
// REALTIME — the failure manifests downstream as exception code 0x6bf
// from Wine's RPC subsystem on a plugin-internal thread.
//
// Done at namespace scope so the static initializer runs before main()
// and before any JUCE static init that might construct plugin scanners.
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C" {
    int   __stdcall OleInitialize    (void* reserved);
    int   __stdcall SetPriorityClass (void* hProcess, unsigned dwPriorityClass);
    void* __stdcall GetCurrentProcess(void);
}

namespace {
    constexpr unsigned WINELIB_REALTIME_PRIORITY_CLASS = 0x00000100u;

    // Wineserver creates its master socket at
    // /tmp/.wine-<uid>/server-<x-y>/socket.  Returns true if any such
    // socket exists for the current effective uid — i.e., a wineserver
    // is already running for this user.
    bool wineserver_socket_present() noexcept
    {
        const std::string dir = "/tmp/.wine-" + std::to_string (::geteuid());
        DIR* d = ::opendir (dir.c_str());
        if (d == nullptr)
            return false;

        bool found = false;
        while (dirent* ent = ::readdir (d))
        {
            if (std::strncmp (ent->d_name, "server-", 7) != 0)
                continue;
            const std::string sock = dir + "/" + ent->d_name + "/socket";
            struct stat st {};
            if (::stat (sock.c_str(), &st) == 0)
            {
                found = true;
                break;
            }
        }
        ::closedir (d);
        return found;
    }

    // Pre-warm wineserver before the first Wine API call from Element.
    // Mirrors yabridge's ensure_wineserver_warm — fast-path no-op when
    // wineserver is already alive for this uid; otherwise forks
    // `wineserver -p` (persistent / daemonize) and polls up to ~2 s for
    // the master socket to appear.  Sidesteps an NSPA-RT cold-spawn
    // race where the first wine client launched from Element's process
    // tree exits silently against an un-initialized wineserver — vanilla
    // wine init is slow enough that the race window doesn't manifest,
    // but RT-accelerated startup exposes it.
    //
    // TODO: this is a defensive warmup, not a root-cause fix.  The race
    // is only triggered by Element (both Linux-native and winelib);
    // every other DAW we've tested loads plugins fine against a cold
    // wineserver.  Two leading hypotheses worth a focused strace
    // session: (a) JUCE's static-init bursts the first Wine call out
    // faster than wineserver's own init window, (b) the SetPriorityClass
    // call below promotes the parent to SCHED_FIFO before libwine's
    // auto-spawn-and-wait completes, starving wineserver's startup.
    // Keep the warmup either way — it's idempotent and matches the
    // pattern yabridge ships for the same prerequisite-before-first-
    // call discipline.
    //
    // `wineserver -p` is idempotent: when one is already running, the
    // bind() on the master socket fails and the second instance exits
    // cleanly.  Fast-path skips the spawn altogether in the warm case.
    void ensure_wineserver_warm() noexcept
    {
        if (wineserver_socket_present())
            return;

        const pid_t child = ::fork();
        if (child < 0)
            return;
        if (child == 0)
        {
            ::execlp ("wineserver", "wineserver", "-p",
                      static_cast<char*> (nullptr));
            ::_exit (127);
        }

        // -p daemonizes via fork, so our direct child exits fast.
        ::waitpid (child, nullptr, 0);

        using namespace std::chrono_literals;
        for (int i = 0; i < 40; ++i)
        {
            if (wineserver_socket_present())
                return;
            std::this_thread::sleep_for (50ms);
        }
    }

    struct WinelibPluginHostInit
    {
        WinelibPluginHostInit() noexcept
        {
            // Wineserver alive first — every Win32 call below needs it,
            // and on NSPA RT kernels the cold-spawn race silently
            // kills our first wine client before SetPriorityClass
            // returns.
            ensure_wineserver_warm();
            // Order matches yabridge-host.cpp.  SetPriorityClass first so
            // any thread spawned during OleInitialize already inherits
            // the realtime class.
            SetPriorityClass (GetCurrentProcess(), WINELIB_REALTIME_PRIORITY_CLASS);
            OleInitialize (nullptr);
        }
    };
    static WinelibPluginHostInit winelib_plugin_host_init {};
}
#endif

START_JUCE_APPLICATION (element::Application)
