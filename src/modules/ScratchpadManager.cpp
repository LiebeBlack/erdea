// EdgeDock Studio :: modules/ScratchpadManager.cpp
#include "modules/ScratchpadManager.h"

#include "core/AppPaths.h"
#include "core/TextConv.h"
#include "core/ThreadPoolN4120.h"

#include <commctrl.h>

#include <algorithm>
#include <cstring>

#pragma comment(lib, "comctl32.lib")

namespace edgedock::modules {
namespace {

constexpr UINT_PTR kDebounceTimer = 0x11;

} // namespace

ScratchpadManager::~ScratchpadManager() {
    Shutdown();
}

bool ScratchpadManager::Initialize(HWND host, Config& config) {
    host_ = host;
    config_ = &config;
    folder_ = config.EffectiveNotesFolder();
    if (!paths::EnsureDirectory(folder_)) {
        paths::AppendLog(L"Scratchpad: no se pudo preparar la carpeta de notas");
    }
    if (!CreateTimerWindow()) {
        paths::AppendLog(L"Scratchpad: sin ventana de temporizador, el autoguardado se degrada a s\u00EDncrono");
    }
    LoadNotesFromDisk();
    lastStatus_ = text::Format(L"%llu notas \u00B7 %s", static_cast<unsigned long long>(notes_.size()),
                               folder_.c_str());
    return true;
}

void ScratchpadManager::Shutdown() {
    // Cierre seguro: nada pendiente queda sin escribir.
    FlushNow();
    if (timerWindow_ != nullptr) {
        ::KillTimer(timerWindow_, kDebounceTimer);
        ::DestroyWindow(timerWindow_);
        timerWindow_ = nullptr;
    }
    if (editor_ != nullptr) {
        ::DestroyWindow(editor_);
        editor_ = nullptr;
    }
    if (editorFont_ != nullptr) {
        ::DeleteObject(editorFont_);
        editorFont_ = nullptr;
    }
}

// -----------------------------------------------------------------------------
// Ventana de temporizador (antirrebote del autoguardado)
// -----------------------------------------------------------------------------

bool ScratchpadManager::CreateTimerWindow() {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = [](HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) -> LRESULT {
            if (message == WM_NCCREATE) {
                auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
                ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
                return ::DefWindowProcW(hwnd, message, wParam, lParam);
            }
            auto* self = reinterpret_cast<ScratchpadManager*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (self == nullptr) return ::DefWindowProcW(hwnd, message, wParam, lParam);

            if (message == WM_TIMER && wParam == kDebounceTimer) {
                self->StopDebounce();
                self->PublishSave();
                return 0;
            }
            return ::DefWindowProcW(hwnd, message, wParam, lParam);
        };
        windowClass.hInstance = ::GetModuleHandleW(nullptr);
        windowClass.lpszClassName = kTimerClassName;
        registered = ::RegisterClassExW(&windowClass) != FALSE ||
                     ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }
    if (!registered) return false;

    timerWindow_ = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kTimerClassName, L"",
                                     WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                                     ::GetModuleHandleW(nullptr), this);
    return timerWindow_ != nullptr;
}

void ScratchpadManager::StartDebounce() {
    if (timerWindow_ == nullptr) {
        PublishSave();   // sin temporizador (raro): se guarda ya
        return;
    }
    const UINT delay = config_ != nullptr && config_->noteAutosaveMs > 0 ? config_->noteAutosaveMs : 350;
    ::SetTimer(timerWindow_, kDebounceTimer, delay, nullptr);
    debounceRunning_ = true;
}

void ScratchpadManager::StopDebounce() {
    if (timerWindow_ != nullptr && debounceRunning_) {
        ::KillTimer(timerWindow_, kDebounceTimer);
    }
    debounceRunning_ = false;
}

void ScratchpadManager::MarkDirty() {
    dirty_.store(true, std::memory_order_relaxed);
    // Cada pulsación reprograma el temporizador: se guarda cuando el usuario deja de escribir.
    if (timerWindow_ != nullptr) {
        ::SetTimer(timerWindow_, kDebounceTimer,
                   config_ != nullptr && config_->noteAutosaveMs > 0 ? config_->noteAutosaveMs : 350,
                   nullptr);
        debounceRunning_ = true;
    } else {
        PublishSave();
    }
}

// -----------------------------------------------------------------------------
// Captura y publicación
// -----------------------------------------------------------------------------

bool ScratchpadManager::CaptureEditor(SaveSlot& slot) {
    if (editor_ == nullptr) return false;

    // Mensajes del EDIT estándar (no del rich edit): longitud + volcado directo al buffer
    // del slot PMR, sin pasar por cadenas intermedias ni por el heap.
    const LRESULT length = ::SendMessageW(editor_, WM_GETTEXTLENGTH, 0, 0);
    if (length <= 0) {
        slot.text.clear();
        return true;
    }

    slot.text.resize(static_cast<size_t>(length) + 1);
    const LRESULT copied = ::SendMessageW(editor_, WM_GETTEXT, static_cast<WPARAM>(length) + 1,
                                          reinterpret_cast<LPARAM>(slot.text.data()));
    slot.text.resize(copied > 0 ? static_cast<size_t>(copied) : 0);
    return true;
}

bool ScratchpadManager::PublishSave() {
    if (activeIndex_ < 0 || activeIndex_ >= static_cast<int>(notes_.size())) {
        dirty_.store(false, std::memory_order_relaxed);
        return false;
    }

    // Se busca un slot libre: si los dos están ocupados, el guardado se coalesce y queda
    // pendiente para la siguiente pulsación (nunca se pierde el último contenido).
    for (size_t attempt = 0; attempt < kSlotCount; ++attempt) {
        SaveSlot& slot = slots_[attempt];
        bool expected = false;
        if (!slot.busy.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) continue;

        if (!CaptureEditor(slot)) {
            slot.busy.store(false, std::memory_order_release);
            return false;
        }
        slot.path = notes_[static_cast<size_t>(activeIndex_)].path;
        slot.generation.store(generation_.fetch_add(1, std::memory_order_relaxed) + 1,
                              std::memory_order_release);

        pendingWrites_.fetch_add(1, std::memory_order_relaxed);
        dirty_.store(false, std::memory_order_relaxed);

        Task task = MakeTask(&ScratchpadManager::SaveTask, this,
                             reinterpret_cast<void*>(attempt), host_, nullptr);
        if (ThreadPoolN4120::Instance().Submit(task)) return true;

        // Pool saturado: se escribe ya en el hilo de UI para no perder la nota.
        CompleteSave(attempt);
        return true;
    }
    return false;
}

void ScratchpadManager::SaveTask(void* a, void* b, void* c, void* d) {
    (void)c;
    (void)d;
    auto* manager = static_cast<ScratchpadManager*>(a);
    const size_t slotIndex = reinterpret_cast<size_t>(b);
    if (manager == nullptr || slotIndex >= kSlotCount) return;
    manager->CompleteSave(slotIndex);
}

void ScratchpadManager::CompleteSave(size_t slotIndex) {
    if (slotIndex >= kSlotCount) return;
    SaveSlot& slot = slots_[slotIndex];
    const uint64_t generation = slot.generation.load(std::memory_order_acquire);

    // El archivo se escribe de forma atómica: temp + MoveFileEx, sin estados intermedios.
    const bool ok = WriteNoteFile(slot.path, slot.text);
    if (ok) {
        savedCount_.fetch_add(1, std::memory_order_relaxed);
        lastSaveUnix_ = paths::NowUnixSeconds();
    } else {
        dirty_.store(true, std::memory_order_relaxed);
        paths::AppendLog(L"Scratchpad: fallo al escribir " + slot.path);
    }
    savedGeneration_.store(generation, std::memory_order_release);
    pendingWrites_.fetch_sub(1, std::memory_order_relaxed);
    slot.busy.store(false, std::memory_order_release);

    lastStatus_ = ok ? text::Format(L"Guardado %s \u00B7 %llu escrituras",
                                    paths::FormatLocalTime(lastSaveUnix_, L"%H:%M:%S").c_str(),
                                    static_cast<unsigned long long>(savedCount_.load()))
                     : std::wstring(L"No se pudo guardar la nota");
    if (host_ != nullptr) ::PostMessageW(host_, kMsgNoteSaved, 0, 0);
}

bool ScratchpadManager::WriteNoteFile(const std::wstring& path, const std::wstring& text) {
    if (path.empty()) return false;
    // El .md se persiste en UTF-8 con escritura atómica (temp + MoveFileEx).
    return paths::WriteTextFileAtomic(path, text::ToUtf8(text));
}

bool ScratchpadManager::FlushNow() {
    if (editor_ == nullptr || activeIndex_ < 0) return true;
    StopDebounce();

    // Guardado síncrono del contenido actual (usado al cerrar el panel o la aplicación).
    std::wstring content;
    const LRESULT length = ::GetWindowTextLengthW(editor_);
    content.resize(static_cast<size_t>(length) + 1, L'\0');
    if (length > 0) ::GetWindowTextW(editor_, content.data(), static_cast<int>(length + 1));
    content.resize(static_cast<size_t>(length));
    return WriteNoteFile(notes_[static_cast<size_t>(activeIndex_)].path, content);
}

// -----------------------------------------------------------------------------
// Pestañas
// -----------------------------------------------------------------------------

void ScratchpadManager::Refresh() {
    LoadNotesFromDisk();
    if (host_ != nullptr) ::PostMessageW(host_, kMsgNoteListChanged, 0, 0);
}

void ScratchpadManager::LoadNotesFromDisk() {
    notes_.clear();
    if (folder_.empty()) return;

    WIN32_FIND_DATAW data{};
    const std::wstring pattern = paths::Join(folder_, L"*.md");
    HANDLE find = ::FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return;

    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
        ScratchNote note;
        note.id = data.cFileName;
        const size_t dot = note.id.find_last_of(L'.');
        if (dot != std::wstring::npos) note.id.resize(dot);
        note.path = paths::Join(folder_, data.cFileName);
        note.bytes = (static_cast<size_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;

        ULARGE_INTEGER stamp;
        stamp.LowPart = data.ftLastWriteTime.dwLowDateTime;
        stamp.HighPart = data.ftLastWriteTime.dwHighDateTime;
        note.modifiedAt = static_cast<int64_t>(stamp.QuadPart / 10000000ULL) - 11644473600LL;

        std::wstring text;
        if (LoadNoteText(note.path, text)) {
            note.title = TitleOf(text);
            note.preview = PreviewOf(text);
        }
        if (note.title.empty()) note.title = note.id;
        notes_.push_back(std::move(note));
    } while (::FindNextFileW(find, &data));
    ::FindClose(find);

    std::stable_sort(notes_.begin(), notes_.end(), [](const ScratchNote& left, const ScratchNote& right) {
        return left.modifiedAt > right.modifiedAt;
    });
    if (activeIndex_ >= static_cast<int>(notes_.size())) activeIndex_ = -1;
}

bool ScratchpadManager::LoadNoteText(const std::wstring& path, std::wstring& out) const {
    out.clear();
    std::string raw;
    if (!paths::ReadTextFile(path, raw)) return false;
    out = text::FromUtf8(raw);
    return true;
}

std::wstring ScratchpadManager::TitleOf(const std::wstring& text) {
    for (const std::wstring& line : text::SplitLines(text)) {
        std::wstring trimmed = text::Trim(line);
        while (!trimmed.empty() && trimmed[0] == L'#') trimmed = text::Trim(trimmed.substr(1));
        if (!trimmed.empty()) return text::TruncateEllipsis(trimmed, 42);
    }
    return std::wstring();
}

std::wstring ScratchpadManager::PreviewOf(const std::wstring& text) {
    bool seenTitle = false;
    for (const std::wstring& line : text::SplitLines(text)) {
        const std::wstring trimmed = text::Trim(line);
        if (trimmed.empty()) continue;
        if (!seenTitle) {
            seenTitle = true;
            continue;
        }
        return text::TruncateEllipsis(trimmed, 72);
    }
    return std::wstring();
}

const ScratchNote* ScratchpadManager::NoteAt(size_t index) const {
    return index < notes_.size() ? &notes_[index] : nullptr;
}

bool ScratchpadManager::Open(size_t index) {
    if (index >= notes_.size()) return false;
    if (!EnsureEditor()) return false;

    // Antes de cambiar de pestaña se vacía lo pendiente de la nota actual.
    if (activeIndex_ >= 0 && activeIndex_ < static_cast<int>(notes_.size()) && index != static_cast<size_t>(activeIndex_)) {
        FlushNow();
    }

    std::wstring text;
    LoadNoteText(notes_[index].path, text);
    ::SetWindowTextW(editor_, text.c_str());
    activeIndex_ = static_cast<int>(index);
    ::SendMessageW(editor_, EM_SETSEL, 0, 0);
    dirty_.store(false, std::memory_order_relaxed);

    // El orden por fecha de modificación se recoloca sin releer todos los archivos.
    if (index != 0) std::rotate(notes_.begin(), notes_.begin() + static_cast<ptrdiff_t>(index), notes_.begin() + static_cast<ptrdiff_t>(index + 1));
    activeIndex_ = 0;
    return true;
}

bool ScratchpadManager::NewNote() {
    if (!EnsureEditor()) return false;
    FlushNow();

    const std::wstring hint = text::Format(L"nota-%s", paths::TimestampForFile().c_str());
    const std::wstring path = BuildNotePath(hint);
    const std::wstring header = text::Format(L"# %s\n\n", hint.c_str());
    if (!WriteNoteFile(path, header)) {
        lastStatus_ = L"No se pudo crear la nota";
        return false;
    }

    ScratchNote note;
    note.id = paths::FileNameOf(path);
    const size_t dot = note.id.find_last_of(L'.');
    if (dot != std::wstring::npos) note.id.resize(dot);
    note.path = path;
    note.modifiedAt = paths::NowUnixSeconds();
    note.title = hint;
    note.bytes = (header.size() + 1) * sizeof(wchar_t);
    notes_.insert(notes_.begin(), std::move(note));
    activeIndex_ = 0;

    ::SetWindowTextW(editor_, header.c_str());
    ::SendMessageW(editor_, EM_SETSEL, static_cast<WPARAM>(header.size()), static_cast<LPARAM>(header.size()));
    dirty_.store(false, std::memory_order_relaxed);
    lastStatus_ = text::Format(L"Nota nueva \u00B7 %s", paths::FileNameOf(path).c_str());
    if (host_ != nullptr) ::PostMessageW(host_, kMsgNoteListChanged, 0, 0);
    return true;
}

bool ScratchpadManager::DeleteNote(size_t index) {
    if (index >= notes_.size()) return false;
    const std::wstring path = notes_[index].path;
    const bool removed = ::DeleteFileW(path.c_str()) != FALSE;
    if (!removed) {
        lastStatus_ = L"No se pudo borrar el archivo de la nota";
        return false;
    }
    notes_.erase(notes_.begin() + static_cast<ptrdiff_t>(index));
    if (index == static_cast<size_t>(activeIndex_) && !notes_.empty()) {
        Open(index < notes_.size() ? index : notes_.size() - 1);
    } else if (notes_.empty()) {
        activeIndex_ = -1;
        if (editor_ != nullptr) ::SetWindowTextW(editor_, L"");
    }
    lastStatus_ = L"Nota borrada";
    if (host_ != nullptr) ::PostMessageW(host_, kMsgNoteListChanged, 0, 0);
    return true;
}

bool ScratchpadManager::RenameActive(const std::wstring& title) {
    if (activeIndex_ < 0 || activeIndex_ >= static_cast<int>(notes_.size())) return false;
    const std::wstring safe = paths::SanitizeFileName(title);
    if (safe.empty()) return false;

    ScratchNote& note = notes_[static_cast<size_t>(activeIndex_)];
    const std::wstring target = paths::Join(folder_, safe + L".md");
    if (text::EqualsNoCase(note.path, target)) return true;
    if (paths::FileExists(target)) {
        lastStatus_ = L"Ya existe una nota con ese nombre";
        return false;
    }
    if (::MoveFileExW(note.path.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING) == FALSE) {
        lastStatus_ = L"No se pudo renombrar la nota";
        return false;
    }
    note.path = target;
    note.id = safe;
    note.title = safe;
    lastStatus_ = text::Format(L"Nota renombrada a %s.md", safe.c_str());
    if (host_ != nullptr) ::PostMessageW(host_, kMsgNoteListChanged, 0, 0);
    return true;
}

// -----------------------------------------------------------------------------
// Editor
// -----------------------------------------------------------------------------

HWND ScratchpadManager::EnsureEditor() {
    if (editor_ != nullptr) return editor_;
    if (host_ == nullptr) return nullptr;

    editor_ = ::CreateWindowExW(0, L"EDIT", L"",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE |
                                    ES_WANTRETURN | ES_AUTOVSCROLL | ES_NOHIDESEL,
                                0, 0, 10, 10, host_, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    if (editor_ == nullptr) {
        paths::AppendLog(L"Scratchpad: no se pudo crear el EDIT de notas");
        return nullptr;
    }

    const int fontHeight = -MulDiv(11, ::GetDpiForWindow(host_), 72);
    editorFont_ = ::CreateFontW(fontHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                FIXED_PITCH | FF_MODERN, L"Consolas");
    if (editorFont_ != nullptr) {
        ::SendMessageW(editor_, WM_SETFONT, reinterpret_cast<WPARAM>(editorFont_), TRUE);
    }
    ::SendMessageW(editor_, EM_SETLIMITTEXT, 1024u * 1024u, 0);
    ::SendMessageW(editor_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(10, 10));
    return editor_;
}

void ScratchpadManager::LayoutEditor(const RECT& clientRect, int dpi) {
    if (editor_ == nullptr) return;
    (void)dpi;
    const int editorWidth = std::max(10, static_cast<int>(clientRect.right - clientRect.left));
    const int editorHeight = std::max(10, static_cast<int>(clientRect.bottom - clientRect.top));
    ::SetWindowPos(editor_, nullptr, clientRect.left, clientRect.top, editorWidth, editorHeight,
                   SWP_NOZORDER | SWP_NOACTIVATE);
}

void ScratchpadManager::ShowEditor(bool visible) {
    if (editor_ == nullptr) return;
    ::ShowWindow(editor_, visible ? SW_SHOW : SW_HIDE);
}

void ScratchpadManager::FocusEditor() {
    if (EnsureEditor() != nullptr) ::SetFocus(editor_);
}

// -----------------------------------------------------------------------------
// Drag & drop
// -----------------------------------------------------------------------------

void ScratchpadManager::NotifyDropActive(bool active, unsigned long effect) {
    dropActive_ = active;
    dropEffect_ = effect;
    lastStatus_ = active ? L"Suelta el texto para crear una nota .md" : lastStatus_;
    if (host_ != nullptr) ::PostMessageW(host_, kMsgNoteListChanged, 0, 0);
}

bool ScratchpadManager::CreateNoteFromText(const wchar_t* text, size_t length, std::wstring* createdPath) {
    if (text == nullptr || length == 0) return false;

    std::wstring content(text, length);
    // Normaliza finales de línea para que el .md sea legible en cualquier editor.
    content = text::ReplaceAll(content, L"\r\n", L"\n");
    content = text::ReplaceAll(content, L"\r", L"\n");

    const std::wstring hint = TitleOf(content).empty() ? L"nota-soltada" : TitleOf(content);
    const std::wstring path = BuildNotePath(hint);
    const std::wstring payload = text::Format(L"# %s\n\n%s\n", hint.c_str(), content.c_str());
    if (!WriteNoteFile(path, payload)) {
        lastStatus_ = L"No se pudo escribir la nota soltada";
        return false;
    }
    if (createdPath != nullptr) *createdPath = path;

    ScratchNote note;
    note.id = paths::FileNameOf(path);
    const size_t dot = note.id.find_last_of(L'.');
    if (dot != std::wstring::npos) note.id.resize(dot);
    note.path = path;
    note.title = hint;
    note.preview = PreviewOf(payload);
    note.modifiedAt = paths::NowUnixSeconds();
    note.bytes = (payload.size() + 1) * sizeof(wchar_t);
    notes_.insert(notes_.begin(), std::move(note));
    activeIndex_ = 0;
    lastStatus_ = text::Format(L"Nota creada desde el arrastre \u00B7 %s", paths::FileNameOf(path).c_str());
    if (editor_ != nullptr) ::SetWindowTextW(editor_, payload.c_str());
    if (host_ != nullptr) ::PostMessageW(host_, kMsgNoteListChanged, 0, 0);
    return true;
}

bool ScratchpadManager::CreateNoteFromFile(const std::wstring& sourcePath, std::wstring* createdPath) {
    if (!paths::FileExists(sourcePath)) return false;

    std::string raw;
    if (!paths::ReadTextFile(sourcePath, raw)) {
        lastStatus_ = L"No se pudo leer el archivo soltado";
        return false;
    }
    const std::wstring wide = text::FromUtf8(raw);
    return CreateNoteFromText(wide.c_str(), wide.size(), createdPath);
}

std::wstring ScratchpadManager::BuildNotePath(const std::wstring& hint) const {
    std::wstring base = paths::SanitizeFileName(text::TruncateEllipsis(hint, 40));
    if (base.empty()) base = L"nota";
    std::wstring path = paths::Join(folder_, base + L".md");

    // Sin sobrescrituras: si el nombre está tomado se añade un sufijo numérico.
    for (int suffix = 2; paths::FileExists(path) && suffix < 999; ++suffix) {
        path = paths::Join(folder_, text::Format(L"%s-%d.md", base.c_str(), suffix));
    }
    return path;
}

size_t ScratchpadManager::TotalBytes() const {
    size_t total = 0;
    for (const auto& note : notes_) total += note.bytes;
    return total;
}

} // namespace edgedock::modules
