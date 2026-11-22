// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The dlopen/LoadLibrary load shim shared by the three vendor engines (Phase 4c
// design §4, P-VENDOR, D3 in-process). A loaded runtime is a plain void* handle;
// every symbol is resolved by name into a typed function pointer and null-checked
// before use. No vendor header, .lib or `find_package` is ever involved — the
// engines compile as plain C++ and only reach a vendor SDK at run time, from the
// user's own install (design §4, §13). Header-only (inline) so the three engines
// share one load/resolve path with no extra translation unit.

#include <QDir>
#include <QFileInfo>
#include <QString>

#include <cstdint>
#include <utility>

#if defined(Q_OS_WIN)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace sb::npu::engine_internal {

// The last load/resolve error as a user-facing string ("dlerror" text on POSIX,
// the Win32 code + message on Windows). Empty when there is no error.
inline QString lastLoadError()
{
#if defined(Q_OS_WIN)
    // A readable Win32 message for the last error, or the bare code as a fallback.
    const DWORD code = GetLastError();
    wchar_t *buffer = nullptr;
    const DWORD n = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<wchar_t *>(&buffer), 0, nullptr);
    QString out;
    if (n && buffer) {
        out = QString::fromWCharArray(buffer, int(n)).trimmed();
        LocalFree(buffer);
    }
    return out.isEmpty() ? QStringLiteral("Win32 error %1").arg(code) : out;
#else
    const char *msg = dlerror();
    return msg ? QString::fromUtf8(msg) : QString();
#endif
}

// An opened shared library, closed on destruction. Load errors are captured in
// *error (the caller's out-string) rather than thrown; isOpen() is false after a
// failed load. Move-only (the raw handle must never be double-closed).
class Library
{
public:
    // Loads `absPath`. On failure isOpen() is false and *error (if set) names the
    // library and the reason. Never throws. `globalScope` (Linux only) loads with
    // RTLD_GLOBAL so a SUBSEQUENTLY loaded module can bind to this one — the GenAI
    // shim's undefined ov::genai::* symbols resolve against the user's
    // openvino_genai module only when it is in the global scope
    // (engine_openvino.cpp loadGenAi). Ignored on Windows.
    Library(const QString &absPath, QString *error, bool globalScope = false) : m_path(absPath)
    {
#if defined(Q_OS_WIN)
        Q_UNUSED(globalScope);
        // Syntheon.exe runs SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS),
        // so plain LoadLibraryW(absPath) only searches the exe folder + System32,
        // never dirname(absPath) — vendor sibling DLLs would fail with error 126.
        // Load with LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR (the loaded DLL's own dir) so
        // a runtime's DLL dependencies resolve beside it. Win32 rejects '/'-seps
        // with ERROR_INVALID_PARAMETER, so normalise to a native absolute path.
        const QString native = QDir::toNativeSeparators(QFileInfo(absPath).absoluteFilePath());
        m_handle = reinterpret_cast<void *>(LoadLibraryExW(reinterpret_cast<const wchar_t *>(native.utf16()),
                                                            nullptr,
                                                            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR
                                                                | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS));
#else
        const int flags = RTLD_NOW | (globalScope ? RTLD_GLOBAL : RTLD_LOCAL);
        m_handle = dlopen(absPath.toUtf8().constData(), flags);
#endif
        if (!m_handle && error) {
            const QString reason = lastLoadError();
            *error = reason.isEmpty()
                ? QStringLiteral("failed to load %1").arg(absPath)
                : QStringLiteral("failed to load %1: %2").arg(absPath, reason);
        }
    }
    ~Library() { close(); }
    Library(const Library &) = delete;
    Library &operator=(const Library &) = delete;
    Library(Library &&other) noexcept : m_handle(other.m_handle), m_path(std::move(other.m_path))
    {
        other.m_handle = nullptr;
    }
    Library &operator=(Library &&other) noexcept
    {
        if (this != &other) {
            close();
            m_handle = other.m_handle;
            m_path = std::move(other.m_path);
            other.m_handle = nullptr;
        }
        return *this;
    }

    bool isOpen() const { return m_handle != nullptr; }
    const QString &path() const { return m_path; }

    // Resolves `name` into the raw symbol pointer, null-checked. On a missing
    // symbol sets *error (if non-null) with a clear message and returns nullptr.
    // The caller casts via resolve<T>() below; nothing here calls a symbol.
    void *sym(const char *name, QString *error) const
    {
#if defined(Q_OS_WIN)
        // Clear any stale error, then resolve from the module's base address.
        SetLastError(0);
        void *p = nullptr;
        if (m_handle)
            p = reinterpret_cast<void *>(GetProcAddress(reinterpret_cast<HMODULE>(m_handle), name));
#else
        // dlerror() must be cleared first, then read only if the symbol is missing.
        void *p = nullptr;
        if (m_handle) {
            dlerror();
            p = dlsym(m_handle, name);
            const char *msg = dlerror();
            if (msg && error) {
                *error = QStringLiteral("failed to resolve %1 in %2: %3")
                             .arg(QString::fromLatin1(name), m_path, QString::fromUtf8(msg));
                return nullptr;
            }
        }
#endif
        if (!p && error) {
#if defined(Q_OS_WIN)
            const QString reason = lastLoadError();
            *error = reason.isEmpty()
                ? QStringLiteral("failed to resolve %1 in %2")
                      .arg(QString::fromLatin1(name), m_path)
                : QStringLiteral("failed to resolve %1 in %2: %3")
                      .arg(QString::fromLatin1(name), m_path, reason);
#else
            // A null symbol with no dlerror text (the RTLD_DEFAULT dlsym(NULL) case
            // doesn't apply here — a non-empty handle always sets dlerror).
            *error = QStringLiteral("failed to resolve %1 in %2")
                         .arg(QString::fromLatin1(name), m_path);
#endif
        }
        return p;
    }

private:
    void close()
    {
        if (!m_handle)
            return;
#if defined(Q_OS_WIN)
        FreeLibrary(reinterpret_cast<HMODULE>(m_handle));
#else
        dlclose(m_handle);
#endif
        m_handle = nullptr;
    }

    void *m_handle = nullptr;
    QString m_path;
};

namespace detail {
// An untyped function pointer, for the cast in resolve().
template <typename F>
F castFromVoid(void *p)
{
    // dlsym/GetProcAddress return a void*, POSIX guarantees it can be cast to a
    // function pointer; ISO C++ rejects a direct void*→function-pointer cast, so
    // route through std::uintptr_t (the warning-free idiom every dlopen shim uses).
    return reinterpret_cast<F>(reinterpret_cast<std::uintptr_t>(p));
}
} // namespace detail

// Resolves `lib`-exported `name` into a function pointer of type F. Returns
// nullptr (and sets *error) when the library is closed or the symbol is missing.
template <typename F>
F resolve(const Library &lib, const char *name, QString *error)
{
    void *p = lib.sym(name, error);
    return p ? detail::castFromVoid<F>(p) : nullptr;
}

} // namespace sb::npu::engine_internal
