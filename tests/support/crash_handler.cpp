// SPDX-License-Identifier: GPL-3.0-or-later
//
// Linked into every test executable. A crashing test used to leave CI with an
// empty log (the process died with its buffered output). This prints what
// killed it to stderr, unbuffered, before the process goes:
//  - Windows: the unhandled exception code and address, and a stack of module
//    + offset frames;
//  - everywhere: std::terminate (an escaped exception) and abort().
// It changes nothing about how tests run.

#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <exception>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <crtdbg.h>
#endif

namespace {

#ifdef _WIN32
void printFrame(void *address)
{
    HMODULE module = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(address), &module)) {
        GetModuleFileNameA(module, name, sizeof name);
    }
    const char *base = name;
    for (const char *p = name; *p; ++p) {
        if (*p == '\\' || *p == '/')
            base = p + 1;
    }
    std::fprintf(stderr, "    %s+0x%llx (%p)\n", base,
                 static_cast<unsigned long long>(reinterpret_cast<char *>(address) - reinterpret_cast<char *>(module)), address);
}
#endif

void printStack()
{
#ifdef _WIN32
    void *frames[48];
    const USHORT n = CaptureStackBackTrace(1, 48, frames, nullptr);
    for (USHORT i = 0; i < n; ++i)
        printFrame(frames[i]);
#endif
}

#ifdef _WIN32
LONG WINAPI onException(EXCEPTION_POINTERS *info)
{
    std::fprintf(stderr, "\n*** UNHANDLED EXCEPTION 0x%08lX at %p\n", info->ExceptionRecord->ExceptionCode,
                 info->ExceptionRecord->ExceptionAddress);
    if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2) {
        std::fprintf(stderr, "    access violation: %s address %p\n",
                     info->ExceptionRecord->ExceptionInformation[0] == 0 ? "reading"
                     : info->ExceptionRecord->ExceptionInformation[0] == 1 ? "writing"
                                                                           : "executing",
                     reinterpret_cast<void *>(info->ExceptionRecord->ExceptionInformation[1]));
    }
    printFrame(info->ExceptionRecord->ExceptionAddress);
    printStack();
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

void onTerminate()
{
    std::fputs("\n*** std::terminate called (an exception escaped, or a noexcept function threw)\n", stderr);
    printStack();
    std::fflush(stderr);
    std::abort();
}

void onAbort(int)
{
    std::fputs("\n*** abort() called\n", stderr);
    printStack();
    std::fflush(stderr);
    std::signal(SIGABRT, SIG_DFL);
    std::abort();
}

struct Installer {
    Installer()
    {
        std::setvbuf(stderr, nullptr, _IONBF, 0);
        std::set_terminate(onTerminate);
        std::signal(SIGABRT, onAbort);
#ifdef _WIN32
        SetUnhandledExceptionFilter(onException);
        // No "program has stopped working" / CRT assert dialogs on a CI box.
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
        _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
        _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
    }
} installer;

} // namespace
