#pragma once
// EdgeDock Studio :: modules/ClipboardHook.h
// Escucha WM_CLIPBOARDUPDATE (AddClipboardFormatListener), archiva cada copia en SQLite,
// mantiene el historial en memoria y resuelve la Búsqueda Instantánea sobre ese vector.

#include <windows.h>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "data/ClipDatabase.h"

namespace edgedock::modules {

using data::ClipEntry;
using data::ClipKind;

class ClipboardHook {
public:
    using Notify = std::function<void()>;

    ClipboardHook() = default;
    ~ClipboardHook();
    ClipboardHook(const ClipboardHook&) = delete;
    ClipboardHook& operator=(const ClipboardHook&) = delete;

    struct Settings {
        int historyLimit = 600;
        int maxPreviewChars = 1200;
        bool captureImages = true;
        bool captureFiles = true;
        bool dedupe = true;
    };

    bool Install(HWND owner, data::ClipDatabase* database, const Settings& settings);
    void Uninstall();
    bool Installed() const { return installed_; }
    void UpdateSettings(const Settings& settings);
    void SetNotify(Notify notify) { notify_ = std::move(notify); }

    // Llamar desde el WndProc al recibir WM_CLIPBOARDUPDATE. Devuelve true si entró un clip.
    bool HandleClipboardUpdate();
    // Recarga el historial completo desde la base de datos.
    void Refresh();

    // --- Historial en memoria + filtro --------------------------------------------
    const std::vector<ClipEntry>& History() const { return history_; }
    const std::vector<size_t>& Filtered() const { return filtered_; }
    size_t VisibleCount() const { return filtered_.size(); }
    const ClipEntry* Visible(size_t visibleIndex) const;
    size_t VisibleToHistory(size_t visibleIndex) const;
    size_t PinnedCount() const;

    void SetQuery(std::wstring_view query);
    const std::wstring& Query() const { return query_; }
    void ClearQuery() { SetQuery(std::wstring()); }

    bool TogglePin(size_t visibleIndex);
    bool DeleteAt(size_t visibleIndex);
    // Sustituye el contenido de un clip (usado por limpiador, case converter, etc.).
    bool ReplaceText(size_t visibleIndex, const std::wstring& newText, bool writeToClipboard);
    bool CopyToClipboard(size_t visibleIndex);

    int CapturedCount() const { return capturedCount_; }
    int SkippedCount() const { return skippedCount_; }
    std::wstring LastCaptureSummary() const { return lastCapture_; }

    // --- Utilidades de portapapeles (independientes de la instancia) ---------------
    static bool CopyTextToClipboard(HWND owner, const std::wstring& text);
    static bool ReadClipboardText(HWND owner, std::wstring& out);
    static std::wstring ForegroundProcessName();

private:
    void ApplyFilter();
    void RebuildFoldCache();
    void ReorderInMemory();
    void TrimMemory();
    // Inserción/borrado que mantienen history_ y folded_ sincronizados índice a índice.
    void InsertHistory(size_t index, const ClipEntry& entry);
    void EraseHistory(size_t index);
    size_t PinnedPrefixLength() const;
    bool CaptureText(const std::wstring& text, ClipKind kind, const std::wstring& source);
    bool CaptureFiles(HDROP drop);
    bool CaptureBitmap();
    bool CaptureRawFormat(UINT format, ClipKind kind, const wchar_t* label);
    void SortHistory();

    HWND owner_ = nullptr;
    data::ClipDatabase* db_ = nullptr;
    Settings settings_{};
    std::vector<ClipEntry> history_;
    std::vector<std::wstring> folded_;    // cache en minúsculas: filtro en tiempo real sin recalcular
    std::vector<size_t> filtered_;
    std::wstring query_;
    std::wstring foldedQuery_;
    bool installed_ = false;
    int capturedCount_ = 0;
    int skippedCount_ = 0;
    std::wstring lastCapture_;
    Notify notify_;
};

} // namespace edgedock::modules
