#pragma once
// EdgeDock Studio :: modules/ScratchpadManager.h
// Notas rápidas en .md con pestañas: pestaña activa en un EDIT nativo multilínea,
// autoguardado asíncrono en el ThreadPoolN4120 (dos slots PMR con coalescencia, cero heap)
// y creación de notas a partir de texto/archivos soltados por OLE drag & drop.

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

#include "core/Config.h"
#include "core/PmrPool.h"

namespace edgedock::modules {

struct ScratchNote {
    std::wstring id;       // Nombre base sin extensión
    std::wstring path;     // Ruta completa del .md
    std::wstring title;    // Primera línea con contenido
    std::wstring preview;  // Segunda línea, recortada
    int64_t modifiedAt = 0;
    size_t bytes = 0;
};

class ScratchpadManager {
public:
    ScratchpadManager() = default;
    ~ScratchpadManager();
    ScratchpadManager(const ScratchpadManager&) = delete;
    ScratchpadManager& operator=(const ScratchpadManager&) = delete;

    // `host` recibe los mensajes de aviso (nota guardada / lista cambiada).
    bool Initialize(HWND host, Config& config);
    void Shutdown();
    void SetHost(HWND host) { host_ = host; }

    static constexpr UINT kMsgNoteSaved = WM_APP + 0x80;
    static constexpr UINT kMsgNoteListChanged = WM_APP + 0x81;

    // --- Pestañas -----------------------------------------------------------------
    void Refresh();
    size_t NoteCount() const { return notes_.size(); }
    const ScratchNote* NoteAt(size_t index) const;
    int ActiveIndex() const { return activeIndex_; }
    bool Open(size_t index);
    bool NewNote();
    bool DeleteNote(size_t index);
    bool RenameActive(const std::wstring& title);

    // --- Editor -------------------------------------------------------------------
    HWND EnsureEditor();                 // crea el EDIT multilínea dentro del host
    void LayoutEditor(const RECT& clientRect, int dpi);
    void ShowEditor(bool visible);
    HWND EditorHandle() const { return editor_; }
    void FocusEditor();

    // --- Autoguardado -------------------------------------------------------------
    void MarkDirty();                    // llamado desde EN_CHANGE
    bool FlushNow();                     // guardado inmediato en el hilo de UI (al cerrar)
    uint64_t SavedGeneration() const { return savedGeneration_.load(std::memory_order_relaxed); }
    size_t SavedCount() const { return savedCount_.load(std::memory_order_relaxed); }
    size_t PendingWrites() const { return pendingWrites_.load(std::memory_order_relaxed); }
    bool Dirty() const { return dirty_.load(std::memory_order_relaxed); }
    int64_t LastSaveUnix() const { return lastSaveUnix_; }

    // --- Drag & drop --------------------------------------------------------------
    bool CreateNoteFromText(const wchar_t* text, size_t length, std::wstring* createdPath);
    bool CreateNoteFromFile(const std::wstring& sourcePath, std::wstring* createdPath);
    void NotifyDropActive(bool active, unsigned long effect);
    bool DropActive() const { return dropActive_; }
    unsigned long DropEffect() const { return dropEffect_; }

    const std::wstring& Folder() const { return folder_; }
    size_t TotalBytes() const;
    const std::wstring& LastStatus() const { return lastStatus_; }

private:
    // Slot de publicación sin heap: el hilo de UI escribe, un worker lee y libera.
    struct SaveSlot {
        std::pmr::wstring text{std::pmr::polymorphic_allocator<wchar_t>{&mem::PoolResource::Global()}};
        std::wstring path;
        std::atomic<bool> busy{false};
        std::atomic<uint64_t> generation{0};
    };

    static constexpr size_t kSlotCount = 2;
    static constexpr const wchar_t* kTimerClassName = L"EdgeDockStudio.ScratchTimer";

    bool CreateTimerWindow();
    void StartDebounce();
    void StopDebounce();
    bool CaptureEditor(SaveSlot& slot);
    bool PublishSave();
    static void SaveTask(void* a, void* b, void* c, void* d);
    void CompleteSave(size_t slotIndex);
    bool WriteNoteFile(const std::wstring& path, std::wstring_view text);
    std::wstring BuildNotePath(const std::wstring& hint) const;
    void LoadNotesFromDisk();
    bool LoadNoteText(const std::wstring& path, std::wstring& out) const;
    static std::wstring TitleOf(const std::wstring& text);
    static std::wstring PreviewOf(const std::wstring& text);

    HWND host_ = nullptr;
    HWND editor_ = nullptr;
    HWND timerWindow_ = nullptr;
    Config* config_ = nullptr;
    std::wstring folder_;
    std::vector<ScratchNote> notes_;
    int activeIndex_ = -1;

    SaveSlot slots_[kSlotCount];
    std::atomic<uint64_t> generation_{0};
    std::atomic<uint64_t> savedGeneration_{0};
    std::atomic<size_t> savedCount_{0};
    std::atomic<size_t> pendingWrites_{0};
    std::atomic<bool> dirty_{false};
    int64_t lastSaveUnix_ = 0;
    HFONT editorFont_ = nullptr;
    bool debounceRunning_ = false;
    bool dropActive_ = false;
    unsigned long dropEffect_ = 0;
    std::wstring lastStatus_;
};

} // namespace edgedock::modules
