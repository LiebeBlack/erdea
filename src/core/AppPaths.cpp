// EdgeDock Studio :: core/AppPaths.cpp
#include "core/AppPaths.h"

#include "core/TextConv.h"

#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <ctime>
#include <mutex>

namespace edgedock::paths {
namespace {

std::wstring KnownFolder(REFKNOWNFOLDERID id, const wchar_t* fallbackEnv) {
    PWSTR raw = nullptr;
    std::wstring result;
    if (SUCCEEDED(::SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &raw)) && raw != nullptr) {
        result.assign(raw);
        ::CoTaskMemFree(raw);
    } else {
        result = ExpandEnvironment(std::wstring(L"%") + fallbackEnv + L"%");
    }
    if (!result.empty() && result.back() == L'\\') result.pop_back();
    return result;
}

std::wstring SubDir(const std::wstring& base, const wchar_t* leaf) {
    std::wstring dir = base.empty() ? std::wstring(leaf) : (base + L'\\' + leaf);
    EnsureDirectory(dir);
    return dir;
}

std::mutex g_logMutex;

} // namespace

bool EnsureDirectory(const std::wstring& dir) {
    if (dir.empty()) return false;
    const DWORD attrs = ::GetFileAttributesW(dir.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES) return (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    return ::CreateDirectoryW(dir.c_str(), nullptr) != FALSE || ::GetLastError() == ERROR_ALREADY_EXISTS;
}

std::wstring AppDataRoot() {
    static const std::wstring cached = []() -> std::wstring {
        std::wstring base = KnownFolder(FOLDERID_RoamingAppData, L"APPDATA");
        if (base.empty()) base = L".";
        std::wstring dir = base + L"\\EdgeDock";
        EnsureDirectory(dir);
        return dir;
    }();
    return cached;
}

std::wstring NotesDir() {
    static const std::wstring cached = []() -> std::wstring {
        return SubDir(AppDataRoot(), L"Notes");
    }();
    return cached;
}

std::wstring ConfigFile() { return AppDataRoot() + L"\\config.json"; }
std::wstring DatabaseFile() { return AppDataRoot() + L"\\clips.db"; }
std::wstring LogFile() { return AppDataRoot() + L"\\edgedock.log"; }

std::wstring ModuleDirectory() {
    wchar_t buffer[MAX_PATH * 4] = {};
    const DWORD len = ::GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    if (len == 0) return L".";
    std::wstring path(buffer, len);
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring(L".") : path.substr(0, slash);
}

bool FileExists(const std::wstring& path) {
    if (path.empty()) return false;
    const DWORD attrs = ::GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool DirectoryExists(const std::wstring& path) {
    if (path.empty()) return false;
    const DWORD attrs = ::GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool DeleteFileIfExists(const std::wstring& path) {
    if (FileExists(path)) return ::DeleteFileW(path.c_str()) != FALSE;
    return false;
}

uint64_t FileSizeBytes(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
    LARGE_INTEGER size;
    size.HighPart = static_cast<LONG>(data.nFileSizeHigh);
    size.LowPart = data.nFileSizeLow;
    return static_cast<uint64_t>(size.QuadPart);
}

std::wstring ExpandEnvironment(const std::wstring& value) {
    if (value.empty()) return std::wstring();
    const DWORD needed = ::ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
    if (needed == 0) return value;
    std::wstring out(static_cast<size_t>(needed), L'\0');
    const DWORD written = ::ExpandEnvironmentStringsW(value.c_str(), out.data(), needed);
    if (written == 0) return value;
    out.resize(static_cast<size_t>(written > 0 ? written - 1 : 0));
    return out;
}

std::wstring Join(const std::wstring& base, const std::wstring& child) {
    if (base.empty()) return child;
    if (child.empty()) return base;
    if (base.back() == L'\\' || base.back() == L'/') return base + child;
    return base + L'\\' + child;
}

std::wstring FileNameOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring ExtensionOf(const std::wstring& path) {
    const size_t dot = path.find_last_of(L'.');
    const size_t slash = path.find_last_of(L"\\/");
    if (dot == std::wstring::npos) return std::wstring();
    if (slash != std::wstring::npos && dot < slash) return std::wstring();
    return text::ToLower(path.substr(dot));
}

std::wstring SanitizeFileName(std::wstring_view name) {
    const wchar_t* kInvalid = L"\\/:*?\"<>|";
    std::wstring out;
    out.reserve(name.size());
    for (const wchar_t c : name) {
        bool bad = c < 0x20;
        for (const wchar_t* p = kInvalid; *p != L'\0' && !bad; ++p) {
            if (c == *p) bad = true;
        }
        out.push_back(bad ? L'_' : c);
    }
    out = text::Trim(out);
    while (!out.empty() && (out.back() == L'.' || out.back() == L' ')) out.pop_back();
    if (out.size() > 96) out.resize(96);
    if (out.empty()) out = L"nota";
    return out;
}

bool WriteTextFile(const std::wstring& path, const std::string& utf8Content) {
    if (path.empty()) return false;
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    if (!utf8Content.empty()) {
        DWORD written = 0;
        ok = ::WriteFile(file, utf8Content.data(), static_cast<DWORD>(utf8Content.size()), &written, nullptr) != FALSE &&
             written == utf8Content.size();
    }
    if (ok) ::FlushFileBuffers(file);
    ::CloseHandle(file);
    return ok;
}

bool WriteTextFileAtomic(const std::wstring& path, const std::string& utf8Content) {
    if (path.empty()) return false;
    const std::wstring temp = path + L".tmp";
    if (!WriteTextFile(temp, utf8Content)) {
        DeleteFileIfExists(temp);
        return false;
    }
    if (::MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        DeleteFileIfExists(temp);
        return false;
    }
    return true;
}

bool ReadTextFile(const std::wstring& path, std::string& outUtf8) {
    outUtf8.clear();
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(file, &size)) {
        ::CloseHandle(file);
        return false;
    }
    if (size.QuadPart > 32 * 1024 * 1024) {  // 32 MB es más que suficiente para config/notes
        ::CloseHandle(file);
        return false;
    }
    const size_t length = static_cast<size_t>(size.QuadPart);
    outUtf8.resize(length);
    bool ok = true;
    if (length > 0) {
        DWORD read = 0;
        ok = ::ReadFile(file, outUtf8.data(), static_cast<DWORD>(length), &read, nullptr) != FALSE;
        if (ok) outUtf8.resize(read);
    }
    ::CloseHandle(file);
    return ok;
}

int64_t NowUnixSeconds() {
    FILETIME ft{};
    ::GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER value;
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    return static_cast<int64_t>(value.QuadPart / 10000000ULL) - 11644473600LL;
}

std::wstring FormatLocalTime(int64_t unixSeconds, const wchar_t* fmt) {
    const time_t raw = static_cast<time_t>(unixSeconds);
    tm local{};
    if (::localtime_s(&local, &raw) != 0) return std::wstring();
    wchar_t buffer[128] = {};
    if (::wcsftime(buffer, std::size(buffer), fmt, &local) == 0) return std::wstring();
    return std::wstring(buffer);
}

std::wstring TimestampForFile() {
    const time_t raw = static_cast<time_t>(NowUnixSeconds());
    tm local{};
    if (::localtime_s(&local, &raw) != 0) return L"nota";
    wchar_t buffer[64] = {};
    ::wcsftime(buffer, std::size(buffer), L"%Y-%m-%d_%H%M-%S", &local);
    return std::wstring(buffer);
}

std::wstring RelativeTime(int64_t unixSeconds) {
    const int64_t now = NowUnixSeconds();
    const int64_t delta = now - unixSeconds;
    if (delta < 0) return L"ahora";
    if (delta < 45) return L"ahora";
    if (delta < 3600) {
        return text::Format(L"hace %lld min", static_cast<long long>(delta / 60));
    }
    if (delta < 86400) {
        return text::Format(L"hace %lld h", static_cast<long long>(delta / 3600));
    }
    if (delta < 86400 * 7) {
        return text::Format(L"hace %lld d", static_cast<long long>(delta / 86400));
    }
    return FormatLocalTime(unixSeconds, L"%d/%m %H:%M");
}

void AppendLog(const std::wstring& line) {
    std::lock_guard<std::mutex> guard(g_logMutex);
    const std::wstring path = LogFile();
    const std::wstring stamped = text::Format(L"[%s] %s\r\n",
                                              FormatLocalTime(NowUnixSeconds(), L"%Y-%m-%d %H:%M:%S").c_str(),
                                              line.c_str());
    const std::string utf8 = text::ToUtf8(stamped);
    HANDLE file = ::CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    ::WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    ::CloseHandle(file);
}

} // namespace edgedock::paths
