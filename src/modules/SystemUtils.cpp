// EdgeDock Studio :: modules/SystemUtils.cpp
#include "modules/SystemUtils.h"

#include "core/AppPaths.h"
#include "core/TextConv.h"

#include <windows.h>
#include <psapi.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <set>
#include <unordered_set>

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

namespace edgedock::sysutil {
namespace {

constexpr double kSpeakingWordsPerMinute = 150.0;

std::wstring FormatSecondsHuman(double seconds) {
    if (seconds < 1.0) return L"<1 s";
    const int total = static_cast<int>(seconds + 0.5);
    const int minutes = total / 60;
    const int rest = total % 60;
    if (minutes == 0) return text::Format(L"%d s", rest);
    if (minutes < 60) return text::Format(L"%d min %d s", minutes, rest);
    return text::Format(L"%d h %d min", minutes / 60, minutes % 60);
}

std::wstring DirNameOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

LaunchResult MakeResult(HINSTANCE instance) {
    LaunchResult result;
    const INT_PTR code = reinterpret_cast<INT_PTR>(instance);
    result.errorCode = static_cast<unsigned long>(code);
    result.ok = code > 32;
    if (result.ok) return result;

    switch (code) {
        case 0: result.message = L"sin memoria o recursos"; break;
        case ERROR_FILE_NOT_FOUND: result.message = L"no se encontró el archivo"; break;
        case ERROR_PATH_NOT_FOUND: result.message = L"no se encontró la ruta"; break;
        case ERROR_BAD_FORMAT: result.message = L"ejecutable con formato inválido"; break;
        case SE_ERR_ACCESSDENIED: result.message = L"acceso denegado"; break;
        case SE_ERR_ASSOCINCOMPLETE: result.message = L"asociación de archivo incompleta"; break;
        case SE_ERR_NOASSOC: result.message = L"sin programa asociado"; break;
        default: result.message = text::Format(L"fallo de ShellExecute (%lu)", result.errorCode); break;
    }
    return result;
}

} // namespace

// -----------------------------------------------------------------------------
// Contador en vivo
// -----------------------------------------------------------------------------

std::wstring TextStats::Summary() const {
    if (empty) return L"sin texto";
    return text::Format(L"%llu palabras \u00B7 %llu caracteres (%llu sin espacios) \u00B7 %s de lectura",
                        static_cast<unsigned long long>(words),
                        static_cast<unsigned long long>(chars),
                        static_cast<unsigned long long>(charsNoSpaces),
                        FormatSecondsHuman(readingSeconds).c_str());
}

std::wstring TextStats::Compact() const {
    if (empty) return L"0w";
    if (readingSeconds < 60.0) {
        return text::Format(L"%lluw \u00B7 %lluc \u00B7 <1m", static_cast<unsigned long long>(words),
                            static_cast<unsigned long long>(chars));
    }
    return text::Format(L"%lluw \u00B7 %lluc \u00B7 %llum", static_cast<unsigned long long>(words),
                        static_cast<unsigned long long>(chars),
                        static_cast<unsigned long long>(readingSeconds / 60.0));
}

TextStats AnalyzeText(const std::wstring& text, double wordsPerMinute) {
    TextStats stats;
    stats.chars = text.size();
    stats.codePoints = text::DisplayLength(text);
    stats.empty = text::Trim(text).empty();
    if (stats.empty) return stats;

    if (wordsPerMinute < 1.0) wordsPerMinute = 250.0;

    bool inWord = false;
    bool inParagraph = false;
    bool pendingParagraphBreak = false;
    int sentenceCount = 0;

    for (size_t i = 0; i < text.size(); ++i) {
        const wchar_t c = text[i];
        if (c == L'\r') continue;

        if (text::IsSpace(c)) {
            inWord = false;
            if (c == L'\n') {
                if (inParagraph) {
                    // Una línea vacía (dos saltos seguidos) separa párrafos reales.
                    pendingParagraphBreak = true;
                }
                inParagraph = false;
            } else {
                pendingParagraphBreak = false;
            }
            continue;
        }

        stats.charsNoSpaces++;
        if (!inWord) {
            ++stats.words;
            inWord = true;
        }
        if (pendingParagraphBreak) {
            ++stats.paragraphs;
            pendingParagraphBreak = false;
        }
        inParagraph = true;

        if (c == L'.' || c == L'!' || c == L'?' || c == L'\u2026') {
            // Solo cuenta como fin de frase si le sigue un espacio o el final del texto.
            const bool nextIsBreak = (i + 1 >= text.size()) || text::IsSpace(text[i + 1]) || text[i + 1] == L'"' ||
                                     text[i + 1] == L')';
            if (nextIsBreak) ++sentenceCount;
        }
    }
    if (inParagraph) ++stats.paragraphs;

    stats.sentences = static_cast<size_t>(sentenceCount);
    stats.lines = text::SplitLines(text).size();
    stats.readingSeconds = (static_cast<double>(stats.words) / wordsPerMinute) * 60.0;
    stats.speakingSeconds = (static_cast<double>(stats.words) / kSpeakingWordsPerMinute) * 60.0;
    return stats;
}

// -----------------------------------------------------------------------------
// Lanzador
// -----------------------------------------------------------------------------

LaunchResult Launch(const std::wstring& target, const std::wstring& parameters,
                    const std::wstring& workingDirectory, int showCommand) {
    if (target.empty()) {
        return LaunchResult{false, ERROR_INVALID_PARAMETER, L"destino vacío"};
    }

    const std::wstring expanded = paths::ExpandEnvironment(target);
    const std::wstring lower = text::ToLower(expanded);

    // Los scripts de Python pasan por el intérprete para no depender de la asociación.
    if (text::EndsWith(lower, L".py") || text::EndsWith(lower, L".pyw")) {
        return LaunchPythonScript(expanded, parameters);
    }

    const std::wstring workDir = workingDirectory.empty()
                                     ? paths::ModuleDirectory()
                                     : paths::ExpandEnvironment(workingDirectory);
    HINSTANCE instance = ::ShellExecuteW(nullptr, L"open", expanded.c_str(),
                                         parameters.empty() ? nullptr : parameters.c_str(),
                                         workDir.c_str(), showCommand);
    return MakeResult(instance);
}

LaunchResult OpenFolder(const std::wstring& path) {
    const std::wstring expanded = paths::ExpandEnvironment(path);
    if (!FileOrFolderExists(expanded)) {
        return LaunchResult{false, ERROR_PATH_NOT_FOUND, L"la carpeta no existe"};
    }
    HINSTANCE instance = ::ShellExecuteW(nullptr, L"open", L"explorer.exe", nullptr,
                                         expanded.c_str(), SW_SHOWNORMAL);
    return MakeResult(instance);
}

LaunchResult RevealInExplorer(const std::wstring& path) {
    const std::wstring expanded = paths::ExpandEnvironment(path);
    if (!FileOrFolderExists(expanded)) {
        return LaunchResult{false, ERROR_PATH_NOT_FOUND, L"la ruta no existe"};
    }
    const std::wstring params = L"/select,\"" + expanded + L"\"";
    HINSTANCE instance = ::ShellExecuteW(nullptr, L"open", L"explorer.exe", params.c_str(), nullptr,
                                         SW_SHOWNORMAL);
    return MakeResult(instance);
}

LaunchResult OpenUrl(const std::wstring& url) {
    if (url.empty()) return LaunchResult{false, ERROR_INVALID_PARAMETER, L"URL vacía"};
    std::wstring target = url;
    if (!text::Contains(target, L"://") && !text::StartsWithNoCase(target, L"www.") &&
        !text::StartsWithNoCase(target, L"mailto:")) {
        target = L"https://" + target;
    }
    HINSTANCE instance = ::ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr,
                                         SW_SHOWNORMAL);
    return MakeResult(instance);
}

LaunchResult EditTextFile(const std::wstring& path) {
    const std::wstring expanded = paths::ExpandEnvironment(path);
    if (!FileOrFolderExists(expanded)) {
        return LaunchResult{false, ERROR_FILE_NOT_FOUND, L"el archivo no existe"};
    }
    HINSTANCE instance = ::ShellExecuteW(nullptr, L"edit", expanded.c_str(), nullptr, nullptr,
                                         SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(instance) > 32) return MakeResult(instance);
    // Varios editores no registran el verbo "edit": se cae a "open".
    return MakeResult(::ShellExecuteW(nullptr, L"open", expanded.c_str(), nullptr, nullptr,
                                      SW_SHOWNORMAL));
}

std::wstring ResolvePythonInterpreter() {
    static const wchar_t* kCandidates[] = {L"py.exe", L"python.exe", L"python3.exe"};
    static std::wstring cached;
    static bool resolved = false;
    static std::mutex mutex;

    std::lock_guard<std::mutex> guard(mutex);
    if (resolved) return cached;

    for (const wchar_t* candidate : kCandidates) {
        wchar_t buffer[MAX_PATH * 2] = {};
        const DWORD length = ::SearchPathW(nullptr, candidate, nullptr,
                                           static_cast<DWORD>(std::size(buffer)), buffer, nullptr);
        if (length > 0 && length < std::size(buffer)) {
            cached.assign(buffer, length);
            break;
        }
    }
    resolved = true;
    return cached;
}

LaunchResult LaunchPythonScript(const std::wstring& scriptPath, const std::wstring& arguments) {
    const std::wstring expanded = paths::ExpandEnvironment(scriptPath);
    if (!FileOrFolderExists(expanded)) {
        return LaunchResult{false, ERROR_FILE_NOT_FOUND, L"el script no existe"};
    }
    const std::wstring interpreter = ResolvePythonInterpreter();
    const std::wstring params = L"\"" + expanded + L"\"" + (arguments.empty() ? L"" : L" " + arguments);

    if (!interpreter.empty()) {
        HINSTANCE instance = ::ShellExecuteW(nullptr, L"open", interpreter.c_str(), params.c_str(),
                                             DirNameOf(expanded).c_str(), SW_SHOWNORMAL);
        LaunchResult result = MakeResult(instance);
        if (result.ok) return result;
    }
    // Sin intérprete en el PATH se delega en la asociación del sistema.
    return MakeResult(::ShellExecuteW(nullptr, L"open", expanded.c_str(),
                                      arguments.empty() ? nullptr : arguments.c_str(),
                                      DirNameOf(expanded).c_str(), SW_SHOWNORMAL));
}

bool FileOrFolderExists(const std::wstring& path) {
    if (path.empty()) return false;
    return ::GetFileAttributesW(paths::ExpandEnvironment(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

// -----------------------------------------------------------------------------
// Focus Assist (modo silencioso)
// -----------------------------------------------------------------------------
namespace {

const wchar_t* const kQuietKey =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\CloudStore\\Store\\DefaultAccount\\Current\\"
    L"default$windows.data.notifications.quiethourssettings\\window.data.notifications.quiethourssettings";
const wchar_t* const kQuietValueName = L"Data";
const wchar_t* const kToastKey = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\PushNotifications";
const wchar_t* const kToastValueName = L"ToastEnabled";
constexpr DWORD kRegistryAccess = KEY_READ | KEY_WRITE | KEY_WOW64_64KEY;

bool ReadBinaryValue(const wchar_t* key, const wchar_t* valueName, std::vector<uint8_t>& out) {
    out.clear();
    HKEY handle = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, kRegistryAccess, &handle) != ERROR_SUCCESS) return false;
    DWORD type = 0;
    DWORD size = 0;
    LSTATUS status = ::RegQueryValueExW(handle, valueName, nullptr, &type, nullptr, &size);
    if (status == ERROR_SUCCESS && type == REG_BINARY && size > 0) {
        out.resize(size);
        status = ::RegQueryValueExW(handle, valueName, nullptr, &type, out.data(), &size);
        if (status == ERROR_SUCCESS) {
            out.resize(size);
        } else {
            out.clear();
        }
    }
    ::RegCloseKey(handle);
    return !out.empty();
}

bool WriteBinaryValue(const wchar_t* key, const wchar_t* valueName, const std::vector<uint8_t>& data) {
    HKEY handle = nullptr;
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, nullptr, REG_OPTION_NON_VOLATILE, kRegistryAccess,
                          nullptr, &handle, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const LSTATUS status = ::RegSetValueExW(handle, valueName, 0, REG_BINARY, data.data(),
                                            static_cast<DWORD>(data.size()));
    ::RegCloseKey(handle);
    return status == ERROR_SUCCESS;
}

bool DeleteValue(const wchar_t* key, const wchar_t* valueName) {
    HKEY handle = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, kRegistryAccess, &handle) != ERROR_SUCCESS) return false;
    const LSTATUS status = ::RegDeleteValueW(handle, valueName);
    ::RegCloseKey(handle);
    return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
}

bool ReadDwordValue(const wchar_t* key, const wchar_t* valueName, uint32_t& out) {
    HKEY handle = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, kRegistryAccess, &handle) != ERROR_SUCCESS) return false;
    DWORD type = 0;
    DWORD size = sizeof(DWORD);
    DWORD data = 0;
    const LSTATUS status = ::RegQueryValueExW(handle, valueName, nullptr, &type,
                                              reinterpret_cast<LPBYTE>(&data), &size);
    ::RegCloseKey(handle);
    if (status != ERROR_SUCCESS || type != REG_DWORD) return false;
    out = data;
    return true;
}

bool WriteDwordValue(const wchar_t* key, const wchar_t* valueName, uint32_t data) {
    HKEY handle = nullptr;
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, nullptr, REG_OPTION_NON_VOLATILE, kRegistryAccess,
                          nullptr, &handle, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const LSTATUS status = ::RegSetValueExW(handle, valueName, 0, REG_DWORD,
                                            reinterpret_cast<const BYTE*>(&data), sizeof(DWORD));
    ::RegCloseKey(handle);
    return status == ERROR_SUCCESS;
}

// El blob de CloudStore es opaco: se localiza el "slot de perfil" como el último DWORD
// poco-endiano con valor 0x0A (off), 0x10 (solo prioritarias) o 0x11 (solo alarmas)
// precedido de un byte nulo. Todo cambio se verifica contra el shell y se revierte si
// el estado observado no cambia.
uint8_t ProfileByteFor(FocusPriority priority) {
    switch (priority) {
        case FocusPriority::PriorityOnly: return 0x10;
        case FocusPriority::AlarmsOnly: return 0x11;
        case FocusPriority::Off: default: return 0x0A;
    }
}

FocusPriority PriorityFromProfileByte(uint8_t value) {
    if (value == 0x10) return FocusPriority::PriorityOnly;
    if (value == 0x11) return FocusPriority::AlarmsOnly;
    return FocusPriority::Off;
}

bool FindProfileSlot(const std::vector<uint8_t>& blob, size_t& offset) {
    bool found = false;
    for (size_t i = 1; i + 4 <= blob.size(); ++i) {
        if (blob[i - 1] != 0x00) continue;
        if (blob[i + 1] != 0x00 || blob[i + 2] != 0x00 || blob[i + 3] != 0x00) continue;
        const uint8_t candidate = blob[i];
        if (candidate == 0x0A || candidate == 0x10 || candidate == 0x11) {
            offset = i;
            found = true;
        }
    }
    return found;
}

} // namespace

bool IsQuietStateActive() {
    QUERY_USER_NOTIFICATION_STATE state = QUNS_ACCEPTS_NOTIFICATIONS;
    if (FAILED(::SHQueryUserNotificationState(&state))) return false;
    return state == QUNS_QUIET_TIME;
}

bool GetFocusAssist(FocusPriority& priority) {
    const bool quiet = IsQuietStateActive();

    FocusPriority fromBlob = FocusPriority::Off;
    bool blobKnown = false;
    std::vector<uint8_t> blob;
    if (ReadBinaryValue(kQuietKey, kQuietValueName, blob)) {
        size_t offset = 0;
        if (FindProfileSlot(blob, offset)) {
            fromBlob = PriorityFromProfileByte(blob[offset]);
            blobKnown = true;
        }
    }

    uint32_t toastsEnabled = 1;
    const bool toastsMuted = ReadDwordValue(kToastKey, kToastValueName, toastsEnabled) && toastsEnabled == 0;

    if (blobKnown) {
        priority = fromBlob;
    } else if (quiet || toastsMuted) {
        priority = FocusPriority::PriorityOnly;   // interpretación conservadora
    } else {
        priority = FocusPriority::Off;
    }
    return true;
}

bool SetFocusAssist(FocusPriority priority, FocusState& state) {
    state = FocusState{};
    GetFocusAssist(state.previous);

    std::vector<uint8_t> blob;
    if (ReadBinaryValue(kQuietKey, kQuietValueName, blob)) {
        state.hadBlob = true;
        state.previousBlob = blob;
    }

    bool patched = false;
    if (state.hadBlob) {
        size_t offset = 0;
        if (FindProfileSlot(blob, offset)) {
            blob[offset] = ProfileByteFor(priority);
            patched = WriteBinaryValue(kQuietKey, kQuietValueName, blob);
            if (patched) {
                const bool quiet = IsQuietStateActive();
                const bool expected = priority != FocusPriority::Off;
                if (quiet != expected) {
                    // El ajuste no produjo el efecto buscado: rollback inmediato.
                    WriteBinaryValue(kQuietKey, kQuietValueName, state.previousBlob);
                    patched = false;
                }
            }
        }
    }

    if (patched) {
        state.registryPatched = true;
        state.active = priority != FocusPriority::Off;
        return true;
    }

    if (priority == FocusPriority::Off) {
        RestoreFocusAssist(state);
        state.active = false;
        return true;
    }

    // Reserva soportada por el sistema: silenciar los avisos (toast) por completo.
    uint32_t previousToast = 1;
    state.toastValueExisted = ReadDwordValue(kToastKey, kToastValueName, previousToast);
    state.previousToastEnabled = state.toastValueExisted ? previousToast : 1;
    if (!WriteDwordValue(kToastKey, kToastValueName, 0)) return false;
    state.toastsDisabled = true;
    state.active = true;
    return true;
}

bool RestoreFocusAssist(const FocusState& state) {
    bool ok = true;
    if (state.registryPatched) {
        ok = state.hadBlob ? WriteBinaryValue(kQuietKey, kQuietValueName, state.previousBlob)
                           : DeleteValue(kQuietKey, kQuietValueName);
    }
    if (state.toastsDisabled) {
        const bool restored = state.toastValueExisted
                                  ? WriteDwordValue(kToastKey, kToastValueName, state.previousToastEnabled)
                                  : DeleteValue(kToastKey, kToastValueName);
        ok = ok && restored;
    }
    return ok;
}

std::wstring FocusPriorityLabel(FocusPriority priority) {
    switch (priority) {
        case FocusPriority::Off: return L"Desactivado";
        case FocusPriority::PriorityOnly: return L"Solo prioritarias";
        case FocusPriority::AlarmsOnly: return L"Solo alarmas";
    }
    return L"Desconocido";
}

// -----------------------------------------------------------------------------
// Suspensión de procesos (NtSuspendProcess / NtResumeProcess)
// -----------------------------------------------------------------------------
namespace {

using NtSuspendProcessFn = LONG(NTAPI*)(HANDLE);
using NtResumeProcessFn = LONG(NTAPI*)(HANDLE);

struct NtProcessApi {
    NtSuspendProcessFn suspend = nullptr;
    NtResumeProcessFn resume = nullptr;
    bool available() const { return suspend != nullptr && resume != nullptr; }
};

const NtProcessApi& NtApi() {
    static const NtProcessApi api = []() {
        NtProcessApi result;
        HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
        if (ntdll == nullptr) ntdll = ::LoadLibraryW(L"ntdll.dll");
        if (ntdll != nullptr) {
            result.suspend = reinterpret_cast<NtSuspendProcessFn>(::GetProcAddress(ntdll, "NtSuspendProcess"));
            result.resume = reinterpret_cast<NtResumeProcessFn>(::GetProcAddress(ntdll, "NtResumeProcess"));
        }
        return result;
    }();
    return api;
}

// SeDebugPrivilege permite suspender procesos ajenos cuando el usuario es administrador;
// si no lo es, el intento falla con acceso denegado y se reporta sin efectos colaterales.
void TryEnableDebugPrivilege() {
    static std::once_flag once;
    std::call_once(once, []() {
        HANDLE token = nullptr;
        if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return;
        TOKEN_PRIVILEGES privileges{};
        privileges.PrivilegeCount = 1;
        privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        if (::LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &privileges.Privileges[0].Luid)) {
            ::AdjustTokenPrivileges(token, FALSE, &privileges, sizeof(privileges), nullptr, nullptr);
        }
        ::CloseHandle(token);
    });
}

HANDLE OpenTargetProcess(unsigned long processId) {
    if (processId == 0) return nullptr;
    HANDLE process = ::OpenProcess(PROCESS_SUSPEND_RESUME | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process == nullptr) {
        TryEnableDebugPrivilege();
        process = ::OpenProcess(PROCESS_SUSPEND_RESUME | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    }
    return process;
}

bool MatchesProcessName(const std::wstring& processName, const std::wstring& candidate) {
    if (text::EqualsNoCase(processName, candidate)) return true;
    if (text::EndsWith(candidate, L".exe")) return false;
    return text::EqualsNoCase(processName, candidate + L".exe");
}

} // namespace

bool SuspendProcessById(unsigned long processId) {
    const NtProcessApi& api = NtApi();
    if (!api.available()) return false;
    HANDLE process = OpenTargetProcess(processId);
    if (process == nullptr) return false;
    const LONG status = api.suspend(process);
    ::CloseHandle(process);
    return status >= 0;
}

bool ResumeProcessById(unsigned long processId) {
    const NtProcessApi& api = NtApi();
    if (!api.available()) return false;
    HANDLE process = OpenTargetProcess(processId);
    if (process == nullptr) return false;
    const LONG status = api.resume(process);
    ::CloseHandle(process);
    return status >= 0;
}

std::vector<SuspendedProcess> SuspendProcessesByNames(const std::vector<std::wstring>& names) {
    std::vector<SuspendedProcess> suspended;
    if (names.empty()) return suspended;
    const NtProcessApi& api = NtApi();
    if (!api.available()) return suspended;

    const unsigned long selfProcessId = ::GetCurrentProcessId();
    HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return suspended;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (::Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == 0 || entry.th32ProcessID == selfProcessId) continue;
            const std::wstring processName(entry.szExeFile);
            bool wanted = false;
            for (const auto& candidate : names) {
                if (MatchesProcessName(processName, candidate)) {
                    wanted = true;
                    break;
                }
            }
            if (!wanted) continue;

            HANDLE process = OpenTargetProcess(entry.th32ProcessID);
            if (process == nullptr) continue;
            if (api.suspend(process) >= 0) {
                suspended.push_back(SuspendedProcess{entry.th32ProcessID, processName, false});
            }
            ::CloseHandle(process);
        } while (::Process32NextW(snapshot, &entry));
    }
    ::CloseHandle(snapshot);
    return suspended;
}

size_t ResumeProcesses(std::vector<SuspendedProcess>& processes) {
    size_t resumed = 0;
    for (auto& process : processes) {
        if (process.resumed) continue;
        if (ResumeProcessById(process.processId)) {
            process.resumed = true;
            ++resumed;
        }
    }
    return resumed;
}

std::vector<std::wstring> ListRunningProcessNames() {
    std::vector<std::wstring> names;
    HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return names;

    std::set<std::wstring> unique;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (::Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID != 0) unique.insert(text::ToLower(entry.szExeFile));
        } while (::Process32NextW(snapshot, &entry));
    }
    ::CloseHandle(snapshot);
    names.assign(unique.begin(), unique.end());
    return names;
}

// -----------------------------------------------------------------------------
// Métricas del sistema
// -----------------------------------------------------------------------------

SystemSnapshot SampleSystem() {
    static std::mutex mutex;
    static FILETIME previousIdle{};
    static FILETIME previousKernel{};
    static FILETIME previousUser{};
    static bool hasPrevious = false;

    std::lock_guard<std::mutex> guard(mutex);
    SystemSnapshot snapshot;

    FILETIME idle{};
    FILETIME kernel{};
    FILETIME user{};
    if (::GetSystemTimes(&idle, &kernel, &user)) {
        const auto toU64 = [](const FILETIME& value) -> uint64_t {
            ULARGE_INTEGER combined;
            combined.LowPart = value.dwLowDateTime;
            combined.HighPart = value.dwHighDateTime;
            return combined.QuadPart;
        };
        if (hasPrevious) {
            const uint64_t idleDelta = toU64(idle) - toU64(previousIdle);
            const uint64_t kernelDelta = toU64(kernel) - toU64(previousKernel);
            const uint64_t userDelta = toU64(user) - toU64(previousUser);
            const uint64_t total = kernelDelta + userDelta;   // el kernel ya incluye el idle
            if (total > 0) {
                const double busy = 100.0 * (1.0 - static_cast<double>(idleDelta) / static_cast<double>(total));
                snapshot.cpuPercent = std::clamp(busy, 0.0, 100.0);
            }
        }
        previousIdle = idle;
        previousKernel = kernel;
        previousUser = user;
        hasPrevious = true;
    }

    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (::GlobalMemoryStatusEx(&memory)) {
        snapshot.totalMemoryBytes = memory.ullTotalPhys;
        snapshot.availableMemoryBytes = memory.ullAvailPhys;
        snapshot.usedMemoryBytes = memory.ullTotalPhys - memory.ullAvailPhys;
    }

    PERFORMANCE_INFORMATION performance{};
    performance.cb = sizeof(performance);
    if (::GetPerformanceInfo(&performance, sizeof(performance))) {
        snapshot.processCount = performance.ProcessCount;
        snapshot.threadCount = performance.ThreadCount;
    }

    snapshot.uptimeSeconds = ::GetTickCount64() / 1000ULL;
    return snapshot;
}

// -----------------------------------------------------------------------------
// Formato
// -----------------------------------------------------------------------------

std::wstring FormatBytes(uint64_t bytes) {
    constexpr double kUnit = 1024.0;
    if (bytes < static_cast<uint64_t>(kUnit)) return text::Format(L"%llu B", static_cast<unsigned long long>(bytes));
    const double kib = static_cast<double>(bytes) / kUnit;
    if (kib < kUnit) return text::Format(L"%.1f KB", kib);
    const double mib = kib / kUnit;
    if (mib < kUnit) return text::Format(L"%.1f MB", mib);
    const double gib = mib / kUnit;
    if (gib < kUnit) return text::Format(L"%.2f GB", gib);
    return text::Format(L"%.2f TB", gib / kUnit);
}

std::wstring FormatDurationShort(double seconds) {
    if (seconds < 1.0) return L"<1 s";
    const int total = static_cast<int>(seconds + 0.5);
    const int hours = total / 3600;
    const int minutes = (total % 3600) / 60;
    const int rest = total % 60;
    if (hours > 0) return text::Format(L"%dh %02dm", hours, minutes);
    if (minutes > 0) return text::Format(L"%dm %02ds", minutes, rest);
    return text::Format(L"%ds", rest);
}

std::wstring FormatPercent(double value, int decimals) {
    if (decimals <= 0) return text::Format(L"%.0f%%", value);
    if (decimals == 1) return text::Format(L"%.1f%%", value);
    return text::Format(L"%.2f%%", value);
}

} // namespace edgedock::sysutil
