// EdgeDock Studio :: core/Config.cpp
#include "core/Config.h"

#include "core/AppPaths.h"
#include "core/Json.h"
#include "core/TextConv.h"

#include <windows.h>

#include <cstdlib>

namespace edgedock {
namespace {

json::Value WideArrayToJson(const std::vector<std::wstring>& values) {
    json::Value array = json::Value::MakeArray();
    for (const auto& value : values) array.Push(json::Value(text::ToUtf8(value)));
    return array;
}

std::vector<std::wstring> JsonToWideArray(const json::Value& value, const std::vector<std::wstring>& fallback) {
    if (!value.IsArray() || value.Size() == 0) return fallback;
    std::vector<std::wstring> out;
    out.reserve(value.Size());
    for (size_t i = 0; i < value.Size(); ++i) {
        const std::wstring item = value[i].WideStringOr(std::wstring());
        if (!item.empty()) out.push_back(item);
    }
    return out.empty() ? fallback : out;
}

UINT VirtualKeyFromName(const std::wstring& raw) {
    const std::wstring name = text::ToUpper(text::Trim(raw));
    if (name.empty()) return 0;
    if (name.size() == 1) {
        const wchar_t c = name[0];
        if ((c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9')) return static_cast<UINT>(c);
    }
    if (name[0] == L'F' && name.size() <= 3) {
        const int number = _wtoi(name.c_str() + 1);
        if (number >= 1 && number <= 24) return static_cast<UINT>(VK_F1 + number - 1);
    }
    struct NamedKey { const wchar_t* name; UINT vk; };
    static const NamedKey kNames[] = {
        {L"SPACE", VK_SPACE}, {L"ESPACIO", VK_SPACE}, {L"ENTER", VK_RETURN}, {L"RETURN", VK_RETURN},
        {L"TAB", VK_TAB}, {L"ESC", VK_ESCAPE}, {L"ESCAPE", VK_ESCAPE}, {L"INS", VK_INSERT},
        {L"INSERT", VK_INSERT}, {L"DEL", VK_DELETE}, {L"DELETE", VK_DELETE}, {L"HOME", VK_HOME},
        {L"END", VK_END}, {L"PGUP", VK_PRIOR}, {L"PGDN", VK_NEXT}, {L"LEFT", VK_LEFT},
        {L"RIGHT", VK_RIGHT}, {L"UP", VK_UP}, {L"DOWN", VK_DOWN}, {L"BACKSPACE", VK_BACK},
        {L"BS", VK_BACK}, {L"IMPECR", VK_SNAPSHOT}, {L"PRINTSCREEN", VK_SNAPSHOT},
    };
    for (const auto& entry : kNames) {
        if (name == entry.name) return entry.vk;
    }
    return 0;
}

} // namespace

std::wstring Config::EffectiveNotesFolder() const {
    if (!notesFolder.empty()) {
        const std::wstring expanded = paths::ExpandEnvironment(notesFolder);
        paths::EnsureDirectory(expanded);
        return expanded;
    }
    return paths::NotesDir();
}

Config Config::Default() {
    return Config{};
}

std::wstring ConfigPath() {
    return paths::ConfigFile();
}

Config LoadConfig() {
    Config cfg = Config::Default();
    std::string raw;
    if (!paths::ReadTextFile(ConfigPath(), raw) || raw.empty()) return cfg;

    std::string error;
    const json::Value root = json::Value::Parse(raw, &error);
    if (!root.IsObject()) {
        if (!error.empty()) paths::AppendLog(text::Format(L"config.json inválido: %s", text::FromUtf8(error).c_str()));
        return cfg;
    }

    cfg.version = static_cast<int>(root["version"].IntOr(cfg.version));
    cfg.panelWidthDip = static_cast<float>(root["panel_width_dip"].NumberOr(cfg.panelWidthDip));
    cfg.collapsedDip = static_cast<float>(root["collapsed_dip"].NumberOr(cfg.collapsedDip));
    cfg.startExpanded = root["start_expanded"].BoolOr(cfg.startExpanded);
    cfg.autoCollapseOnBlur = root["auto_collapse"].BoolOr(cfg.autoCollapseOnBlur);
    cfg.collapseDelayMs = static_cast<int>(root["collapse_delay_ms"].IntOr(cfg.collapseDelayMs));
    cfg.animationMs = static_cast<int>(root["animation_ms"].IntOr(cfg.animationMs));
    cfg.topMost = root["top_most"].BoolOr(cfg.topMost);

    cfg.historyLimit = static_cast<int>(root["history_limit"].IntOr(cfg.historyLimit));
    cfg.historyKeepDays = static_cast<int>(root["history_keep_days"].IntOr(cfg.historyKeepDays));
    cfg.maxPreviewChars = static_cast<int>(root["max_preview_chars"].IntOr(cfg.maxPreviewChars));
    cfg.captureImages = root["capture_images"].BoolOr(cfg.captureImages);
    cfg.captureFiles = root["capture_files"].BoolOr(cfg.captureFiles);
    cfg.dedupeClips = root["dedupe_clips"].BoolOr(cfg.dedupeClips);

    cfg.hotkeyToggle = root["hotkey_toggle"].WideStringOr(cfg.hotkeyToggle);
    cfg.hotkeyFocus = root["hotkey_focus"].WideStringOr(cfg.hotkeyFocus);
    cfg.hotkeyCaptureNote = root["hotkey_capture_note"].WideStringOr(cfg.hotkeyCaptureNote);

    cfg.focusAssistOnHotkey = root["focus_assist_on_hotkey"].BoolOr(cfg.focusAssistOnHotkey);
    cfg.focusAssistPriority = static_cast<int>(root["focus_assist_priority"].IntOr(cfg.focusAssistPriority));
    cfg.suspendCpuHogsOnFocus = root["suspend_cpu_hogs"].BoolOr(cfg.suspendCpuHogsOnFocus);
    cfg.cpuHogs = JsonToWideArray(root["cpu_hogs"], cfg.cpuHogs);

    cfg.theme = root["theme"].WideStringOr(cfg.theme);
    cfg.monospaceBody = root["monospace_body"].BoolOr(cfg.monospaceBody);
    cfg.noteAutosaveMs = static_cast<int>(root["note_autosave_ms"].IntOr(cfg.noteAutosaveMs));
    cfg.notesFolder = root["notes_folder"].WideStringOr(cfg.notesFolder);

    // Saneamiento defensivo: el panel nunca puede ser más ancho que una pantalla razonable.
    if (cfg.panelWidthDip < 260.0f) cfg.panelWidthDip = 260.0f;
    if (cfg.panelWidthDip > 900.0f) cfg.panelWidthDip = 900.0f;
    if (cfg.collapsedDip < 1.0f) cfg.collapsedDip = 1.0f;
    if (cfg.collapsedDip > 8.0f) cfg.collapsedDip = 8.0f;
    if (cfg.historyLimit < 50) cfg.historyLimit = 50;
    if (cfg.historyLimit > 5000) cfg.historyLimit = 5000;
    if (cfg.animationMs < 40) cfg.animationMs = 40;
    if (cfg.animationMs > 900) cfg.animationMs = 900;
    if (cfg.collapseDelayMs < 100) cfg.collapseDelayMs = 100;
    if (cfg.collapseDelayMs > 4000) cfg.collapseDelayMs = 4000;
    if (cfg.focusAssistPriority < 0 || cfg.focusAssistPriority > 2) cfg.focusAssistPriority = 1;
    if (cfg.noteAutosaveMs < 80) cfg.noteAutosaveMs = 80;
    if (cfg.noteAutosaveMs > 5000) cfg.noteAutosaveMs = 5000;
    return cfg;
}

bool SaveConfig(const Config& cfg) {
    json::Value root = json::Value::MakeObject();
    root.Set("version", json::Value(cfg.version));
    root.Set("panel_width_dip", json::Value(static_cast<double>(cfg.panelWidthDip)));
    root.Set("collapsed_dip", json::Value(static_cast<double>(cfg.collapsedDip)));
    root.Set("start_expanded", json::Value(cfg.startExpanded));
    root.Set("auto_collapse", json::Value(cfg.autoCollapseOnBlur));
    root.Set("collapse_delay_ms", json::Value(cfg.collapseDelayMs));
    root.Set("animation_ms", json::Value(cfg.animationMs));
    root.Set("top_most", json::Value(cfg.topMost));
    root.Set("history_limit", json::Value(cfg.historyLimit));
    root.Set("history_keep_days", json::Value(cfg.historyKeepDays));
    root.Set("max_preview_chars", json::Value(cfg.maxPreviewChars));
    root.Set("capture_images", json::Value(cfg.captureImages));
    root.Set("capture_files", json::Value(cfg.captureFiles));
    root.Set("dedupe_clips", json::Value(cfg.dedupeClips));
    root.Set("hotkey_toggle", json::Value(text::ToUtf8(cfg.hotkeyToggle)));
    root.Set("hotkey_focus", json::Value(text::ToUtf8(cfg.hotkeyFocus)));
    root.Set("hotkey_capture_note", json::Value(text::ToUtf8(cfg.hotkeyCaptureNote)));
    root.Set("focus_assist_on_hotkey", json::Value(cfg.focusAssistOnHotkey));
    root.Set("focus_assist_priority", json::Value(cfg.focusAssistPriority));
    root.Set("suspend_cpu_hogs", json::Value(cfg.suspendCpuHogsOnFocus));
    root.Set("cpu_hogs", WideArrayToJson(cfg.cpuHogs));
    root.Set("theme", json::Value(text::ToUtf8(cfg.theme)));
    root.Set("monospace_body", json::Value(cfg.monospaceBody));
    root.Set("note_autosave_ms", json::Value(cfg.noteAutosaveMs));
    root.Set("notes_folder", json::Value(text::ToUtf8(cfg.notesFolder)));
    return paths::WriteTextFileAtomic(ConfigPath(), root.Dump(2) + "\n");
}

bool ParseHotkey(const std::wstring& text, UINT& modifiers, UINT& virtualKey) {
    modifiers = 0;
    virtualKey = 0;
    if (text.empty()) return false;

    const std::vector<std::wstring> tokens = text::Split(text, L'+');
    size_t index = 0;
    for (; index < tokens.size(); ++index) {
        const std::wstring token = text::ToUpper(text::Trim(tokens[index]));
        if (token.empty()) continue;
        if (token == L"CTRL" || token == L"CONTROL") {
            modifiers |= MOD_CONTROL;
        } else if (token == L"ALT") {
            modifiers |= MOD_ALT;
        } else if (token == L"SHIFT" || token == L"MAYUS") {
            modifiers |= MOD_SHIFT;
        } else if (token == L"WIN" || token == L"WINDOWS") {
            modifiers |= MOD_WIN;
        } else {
            virtualKey = VirtualKeyFromName(token);
            break;
        }
    }
    if (virtualKey == 0 && !tokens.empty()) {
        virtualKey = VirtualKeyFromName(text::ToUpper(text::Trim(tokens.back())));
    }
    if (virtualKey == 0) {
        // Atajo inválido: se degrada al valor por defecto en vez de dejar el panel sin tecla.
        modifiers = MOD_CONTROL | MOD_ALT;
        virtualKey = 'D';
        return false;
    }
    if (modifiers == 0 && virtualKey >= VK_F1 && virtualKey <= VK_F24) {
        return true;   // F1..F24 sin modificadores es válido en Windows
    }
    return true;
}

std::wstring FormatHotkey(const std::wstring& accelerator) {
    UINT modifiers = 0;
    UINT vk = 0;
    if (!ParseHotkey(accelerator, modifiers, vk)) return L"(inválido)";
    std::wstring out;
    if (modifiers & MOD_CONTROL) out += L"Ctrl+";
    if (modifiers & MOD_WIN) out += L"Win+";
    if (modifiers & MOD_ALT) out += L"Alt+";
    if (modifiers & MOD_SHIFT) out += L"Shift+";
    if (vk >= 'A' && vk <= 'Z') {
        out.push_back(static_cast<wchar_t>(vk));
    } else if (vk >= '0' && vk <= '9') {
        out.push_back(static_cast<wchar_t>(vk));
    } else if (vk >= VK_F1 && vk <= VK_F24) {
        out += text::Format(L"F%u", vk - VK_F1 + 1);
    } else {
        out += text::Format(L"0x%02X", vk);
    }
    return out;
}

} // namespace edgedock
