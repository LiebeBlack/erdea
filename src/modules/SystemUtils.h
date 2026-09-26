#pragma once
// EdgeDock Studio :: modules/SystemUtils.h
// Lanzador ShellExecuteW, contador en vivo (palabras/caracteres/tiempo de lectura),
// Focus Assist con verificación y rollback, y suspensión de procesos devoradores de CPU
// a través de NtSuspendProcess (ntdll, cargado dinámicamente).

#include <cstdint>
#include <string>
#include <vector>

namespace edgedock::sysutil {

// --- Contador en vivo -----------------------------------------------------------
struct TextStats {
    size_t chars = 0;             // Unidades UTF-16
    size_t codePoints = 0;        // Puntos de código reales
    size_t charsNoSpaces = 0;
    size_t words = 0;             // Separadas por espacios
    size_t sentences = 0;
    size_t lines = 0;
    size_t paragraphs = 0;
    double readingSeconds = 0.0;  // 250 palabras por minuto
    double speakingSeconds = 0.0; // 150 palabras por minuto
    bool empty = true;

    std::wstring Summary() const;   // "124 palabras · 812 caracteres · 3 min 15 s de lectura"
    std::wstring Compact() const;   // "124w 812c r3m"
};

TextStats AnalyzeText(const std::wstring& text, double wordsPerMinute = 250.0);

// --- Lanzador -------------------------------------------------------------------
struct LaunchResult {
    bool ok = false;
    unsigned long errorCode = 0;   // <= 32 proviene de ShellExecuteW
    std::wstring message;

    explicit operator bool() const { return ok; }
};

LaunchResult Launch(const std::wstring& target, const std::wstring& parameters = std::wstring(),
                    const std::wstring& workingDirectory = std::wstring(), int showCommand = 1);
LaunchResult OpenFolder(const std::wstring& path);
LaunchResult RevealInExplorer(const std::wstring& path);
LaunchResult OpenUrl(const std::wstring& url);
LaunchResult EditTextFile(const std::wstring& path);
std::wstring ResolvePythonInterpreter();
LaunchResult LaunchPythonScript(const std::wstring& scriptPath, const std::wstring& arguments = std::wstring());

// --- Focus Assist / Modo silencioso ---------------------------------------------
enum class FocusPriority { Off = 0, PriorityOnly = 1, AlarmsOnly = 2 };

struct FocusState {
    bool active = false;
    bool registryPatched = false;    // Se reescribió el blob de CloudStore
    bool toastsDisabled = false;     // Reserva: se silenciaron las notificaciones por completo
    FocusPriority previous = FocusPriority::Off;
    std::vector<uint8_t> previousBlob;
    bool hadBlob = false;
    bool toastValueExisted = false;
    uint32_t previousToastEnabled = 1;
};

bool IsQuietStateActive();
bool GetFocusAssist(FocusPriority& priority);
bool SetFocusAssist(FocusPriority priority, FocusState& state);
bool RestoreFocusAssist(const FocusState& state);
std::wstring FocusPriorityLabel(FocusPriority priority);

// --- Suspensión de procesos -----------------------------------------------------
struct SuspendedProcess {
    unsigned long processId = 0;
    std::wstring name;
    bool resumed = false;
};

bool SuspendProcessById(unsigned long processId);
bool ResumeProcessById(unsigned long processId);
std::vector<SuspendedProcess> SuspendProcessesByNames(const std::vector<std::wstring>& names);
size_t ResumeProcesses(std::vector<SuspendedProcess>& processes);
std::vector<std::wstring> ListRunningProcessNames();

// --- Métricas del sistema -------------------------------------------------------
struct SystemSnapshot {
    double cpuPercent = 0.0;
    uint64_t totalMemoryBytes = 0;
    uint64_t availableMemoryBytes = 0;
    uint64_t usedMemoryBytes = 0;
    uint64_t uptimeSeconds = 0;
    uint32_t processCount = 0;
    uint32_t threadCount = 0;
};

SystemSnapshot SampleSystem();

// --- Formato --------------------------------------------------------------------
std::wstring FormatBytes(uint64_t bytes);
std::wstring FormatDurationShort(double seconds);
std::wstring FormatPercent(double value, int decimals = 0);
bool FileOrFolderExists(const std::wstring& path);

} // namespace edgedock::sysutil
