#pragma once
// EdgeDock Studio :: core/Config.h
// Configuración persistente en %APPDATA%\EdgeDock\config.json (JSON propio, sin dependencias).

#include <string>
#include <vector>

namespace edgedock {

struct Config {
    int version = 1;

    // --- Geometría y comportamiento del panel --------------------------------------
    float panelWidthDip = 430.0f;   // Ancho desplegado en DIP
    float collapsedDip = 2.0f;      // Franja visible contraída (línea de 2 px)
    bool startExpanded = false;
    bool autoCollapseOnBlur = true;
    int collapseDelayMs = 550;      // Gracia antes de replegarse al salir el ratón
    int animationMs = 190;          // Duración de la animación de despliegue
    bool topMost = true;

    // --- Portapapeles --------------------------------------------------------------
    int historyLimit = 600;         // Clips servidos en memoria
    int historyKeepDays = 30;       // Antigüedad máxima de los clips no anclados
    int maxPreviewChars = 1200;     // Recorte del texto guardado por clip
    bool captureImages = true;
    bool captureFiles = true;
    bool dedupeClips = true;

    // --- Atajos globales -----------------------------------------------------------
    std::wstring hotkeyToggle = L"Ctrl+Alt+D";
    std::wstring hotkeyFocus = L"Ctrl+Alt+F";
    std::wstring hotkeyCaptureNote = L"Ctrl+Alt+S";

    // --- Modo silencioso / CPU ------------------------------------------------------
    bool focusAssistOnHotkey = true;
    int focusAssistPriority = 1;    // 0 = desactivado, 1 = solo prioritarias, 2 = solo alarmas
    bool suspendCpuHogsOnFocus = false;
    std::vector<std::wstring> cpuHogs = {L"OneDrive.exe", L"Teams.exe"};

    // --- Aspecto -------------------------------------------------------------------
    std::wstring theme = L"cyan";   // cyan | purple | amber
    bool monospaceBody = false;

    // --- Notas ---------------------------------------------------------------------
    int noteAutosaveMs = 350;       // Retardo del autoguardado asíncrono
    std::wstring notesFolder;       // Vacío = %APPDATA%\EdgeDock\Notes

    std::wstring EffectiveNotesFolder() const;
    static Config Default();
};

std::wstring ConfigPath();
Config LoadConfig();
bool SaveConfig(const Config& cfg);
std::wstring FormatHotkey(const std::wstring& accelerator);
// Traduce "Ctrl+Alt+D" a (MOD_CONTROL|MOD_ALT, 'D') para RegisterHotKey.
bool ParseHotkey(const std::wstring& text, unsigned int& modifiers, unsigned int& virtualKey);

} // namespace edgedock
