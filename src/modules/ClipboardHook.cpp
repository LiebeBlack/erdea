// EdgeDock Studio :: modules/ClipboardHook.cpp
#include "modules/ClipboardHook.h"

#include "core/AppPaths.h"
#include "core/TextConv.h"
#include "modules/TextProcessor.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cwctype>

namespace edgedock::modules {
namespace {

constexpr int kClipboardRetries = 12;
constexpr DWORD kClipboardRetrySleepMs = 10;

// CF_HTML y CF_RTF no son constantes del SDK: son formatos registrados por nombre.
UINT FormatHtml() {
    static const UINT id = ::RegisterClipboardFormatW(L"HTML Format");
    return id;
}

UINT FormatRtf() {
    static const UINT id = ::RegisterClipboardFormatW(L"Rich Text Format");
    return id;
}

// --- RTF -> texto plano ---------------------------------------------------------
wchar_t DecodeCp1252(unsigned char value) {
    // Rango 0x80-0x9F de CP1252 (el resto coincide con Latin-1).
    static const wchar_t kTable[32] = {
        L'\u20AC', L'\uFFFD', L'\u201A', L'\u0192', L'\u201E', L'\u2026', L'\u2020', L'\u2021',
        L'\u02C6', L'\u2030', L'\u0160', L'\u2039', L'\u0152', L'\uFFFD', L'\u017D', L'\uFFFD',
        L'\uFFFD', L'\u2018', L'\u2019', L'\u201C', L'\u201D', L'\u2022', L'\u2013', L'\u2014',
        L'\u02DC', L'\u2122', L'\u0161', L'\u203A', L'\u0153', L'\uFFFD', L'\u017E', L'\u0178'};
    if (value >= 0x80 && value <= 0x9F) return kTable[value - 0x80];
    return static_cast<wchar_t>(value);
}

std::wstring RtfToPlain(const std::string& rtf) {
    std::wstring out;
    out.reserve(rtf.size() / 2 + 16);
    int depth = 0;
    bool skipping = false;
    int skipDepth = 0;

    const size_t size = rtf.size();
    for (size_t i = 0; i < size; ++i) {
        const unsigned char raw = static_cast<unsigned char>(rtf[i]);
        if (raw == '{') {
            ++depth;
            if (skipping && skipDepth == 0) skipDepth = depth;
            continue;
        }
        if (raw == '}') {
            if (skipping && depth == skipDepth) {
                skipping = false;
                skipDepth = 0;
            }
            --depth;
            continue;
        }
        if (raw == '\\') {
            const size_t next = i + 1;
            if (next >= size) break;
            const unsigned char symbol = static_cast<unsigned char>(rtf[next]);
            if (!std::isalpha(symbol)) {
                if (symbol == '\'') {
                    if (next + 2 < size) {
                        const auto hexValue = [](char c) -> int {
                            if (c >= '0' && c <= '9') return c - '0';
                            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                            return -1;
                        };
                        const int hi = hexValue(rtf[next + 1]);
                        const int lo = hexValue(rtf[next + 2]);
                        if (hi >= 0 && lo >= 0 && !skipping) {
                            out.push_back(DecodeCp1252(static_cast<unsigned char>((hi << 4) | lo)));
                        }
                        i = next + 2;
                    } else {
                        i = next;
                    }
                    continue;
                }
                if ((symbol == '\\' || symbol == '{' || symbol == '}') && !skipping) {
                    out.push_back(static_cast<wchar_t>(symbol));
                }
                i = next;
                continue;
            }

            // Palabra de control: nombre + parámetro numérico opcional.
            size_t cursor = next;
            std::string word;
            while (cursor < size && std::isalpha(static_cast<unsigned char>(rtf[cursor]))) {
                word.push_back(rtf[cursor]);
                ++cursor;
            }
            bool hasParameter = false;
            long parameter = 0;
            bool negative = false;
            if (cursor < size && (rtf[cursor] == '-' || std::isdigit(static_cast<unsigned char>(rtf[cursor])))) {
                hasParameter = true;
                if (rtf[cursor] == '-') {
                    negative = true;
                    ++cursor;
                }
                while (cursor < size && std::isdigit(static_cast<unsigned char>(rtf[cursor]))) {
                    parameter = parameter * 10 + (rtf[cursor] - '0');
                    ++cursor;
                }
            }
            if (cursor < size && rtf[cursor] == ' ') ++cursor;
            i = cursor > 0 ? cursor - 1 : i;

            if (word == "*") {
                skipping = true;
                skipDepth = depth;
            } else if (word == "fonttbl" || word == "colortbl" || word == "stylesheet" || word == "info" ||
                       word == "pict" || word == "header" || word == "footer" || word == "generator") {
                skipping = true;
                skipDepth = depth;
            } else if (!skipping) {
                if (word == "par" || word == "line" || word == "sect" || word == "row" || word == "page") {
                    out.push_back(L'\n');
                } else if (word == "tab") {
                    out.push_back(L'\t');
                } else if (word == "u" && hasParameter) {
                    long value = negative ? parameter + 65536 : parameter;
                    out.push_back(static_cast<wchar_t>(value & 0xFFFF));
                    // El carácter Unicode va seguido de su reemplazo ANSI (\'hh): se salta.
                    size_t probe = i + 1;
                    while (probe < size && rtf[probe] != '\\') ++probe;
                    if (probe + 3 < size && rtf[probe + 1] == '\'') i = probe + 3;
                }
            }
            continue;
        }
        if (raw == '\r' || raw == '\n') continue;
        if (!skipping && depth > 0) {
            out.push_back(static_cast<wchar_t>(raw));
        }
    }

    // RTF escapa las comillas y suele dejar dobles espacios por los controles eliminados.
    out = text::ReplaceAll(out, L"  ", L" ");
    while (text::Contains(out, L"  ")) out = text::ReplaceAll(out, L"  ", L" ");
    return text::Trim(out);
}

// --- CF_HTML -> fragmento -------------------------------------------------------
std::wstring HtmlFragmentToText(const std::string& html) {
    size_t start = 0;
    size_t end = html.size();

    // El encabezado CF_HTML declara StartFragment/EndFragment en bytes.
    const auto findOffset = [&html](const char* key, size_t& target) {
        const size_t pos = html.find(key);
        if (pos == std::string::npos) return false;
        const size_t colon = html.find(':', pos);
        if (colon == std::string::npos) return false;
        size_t cursor = colon + 1;
        while (cursor < html.size() && (html[cursor] == ' ' || html[cursor] == '\t')) ++cursor;
        size_t value = 0;
        bool any = false;
        while (cursor < html.size() && std::isdigit(static_cast<unsigned char>(html[cursor]))) {
            value = value * 10 + static_cast<size_t>(html[cursor] - '0');
            ++cursor;
            any = true;
        }
        if (!any) return false;
        target = value;
        return true;
    };

    size_t startOffset = 0;
    size_t endOffset = 0;
    if (findOffset("StartFragment", startOffset)) start = std::min(startOffset, html.size());
    if (findOffset("EndFragment", endOffset)) end = std::min(endOffset, html.size());
    if (end <= start) {
        start = 0;
        end = html.size();
    }

    const std::string fragment = html.substr(start, end - start);
    // Se conserva el marcado: los clips de tipo CODE son la materia prima del panel TEXTO.
    return text::Trim(text::FromUtf8(fragment));
}

bool WriteBmpFile(const std::wstring& path, const std::vector<uint8_t>& dib) {
    if (dib.size() < sizeof(BITMAPINFOHEADER)) return false;
    const BITMAPINFOHEADER* header = reinterpret_cast<const BITMAPINFOHEADER*>(dib.data());

    const size_t headerSize = static_cast<size_t>(header->biSize) +
                              (header->biCompression == BI_BITFIELDS ? 12u : 0u);
    size_t paletteEntries = 0;
    if (header->biClrUsed != 0) {
        paletteEntries = header->biClrUsed;
    } else if (header->biBitCount <= 8) {
        paletteEntries = static_cast<size_t>(1) << header->biBitCount;
    }
    const size_t paletteBytes = paletteEntries * 4;

    BITMAPFILEHEADER fileHeader{};
    fileHeader.bfType = 0x4D42;   // 'BM'
    fileHeader.bfOffBits = static_cast<DWORD>(sizeof(BITMAPFILEHEADER) + headerSize + paletteBytes);
    fileHeader.bfSize = static_cast<DWORD>(sizeof(BITMAPFILEHEADER) + dib.size());
    fileHeader.bfReserved1 = 0;
    fileHeader.bfReserved2 = 0;

    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    DWORD written = 0;
    bool ok = ::WriteFile(file, &fileHeader, sizeof(fileHeader), &written, nullptr) != FALSE &&
              written == sizeof(fileHeader);
    if (ok) {
        ok = ::WriteFile(file, dib.data(), static_cast<DWORD>(dib.size()), &written, nullptr) != FALSE &&
             written == dib.size();
    }
    ::CloseHandle(file);
    if (!ok) paths::DeleteFileIfExists(path);
    return ok;
}

} // namespace

ClipboardHook::~ClipboardHook() {
    Uninstall();
}

bool ClipboardHook::Install(HWND owner, data::ClipDatabase* database, const Settings& settings) {
    if (owner == nullptr || database == nullptr) return false;
    owner_ = owner;
    db_ = database;
    settings_ = settings;
    if (!::AddClipboardFormatListener(owner_)) {
        paths::AppendLog(L"ClipboardHook: AddClipboardFormatListener falló");
        return false;
    }
    installed_ = true;
    Refresh();
    return true;
}

void ClipboardHook::Uninstall() {
    if (installed_ && owner_ != nullptr) {
        ::RemoveClipboardFormatListener(owner_);
    }
    installed_ = false;
}

void ClipboardHook::UpdateSettings(const Settings& settings) {
    settings_ = settings;
    if (settings_.historyLimit < 20) settings_.historyLimit = 20;
    if (settings_.maxPreviewChars < 200) settings_.maxPreviewChars = 200;
    TrimMemory();
    ApplyFilter();
}

void ClipboardHook::Refresh() {
    if (db_ == nullptr) return;
    history_ = db_->Load(settings_.historyLimit);
    RebuildFoldCache();
    ApplyFilter();
}

void ClipboardHook::RebuildFoldCache() {
    folded_.clear();
    folded_.reserve(history_.size());
    for (const auto& entry : history_) folded_.push_back(text::ToLower(entry.text));
}

void ClipboardHook::InsertHistory(size_t index, const ClipEntry& entry) {
    if (index > history_.size()) index = history_.size();
    history_.insert(history_.begin() + static_cast<ptrdiff_t>(index), entry);
    folded_.insert(folded_.begin() + static_cast<ptrdiff_t>(std::min(index, folded_.size())),
                   text::ToLower(entry.text));
}

void ClipboardHook::EraseHistory(size_t index) {
    if (index >= history_.size()) return;
    history_.erase(history_.begin() + static_cast<ptrdiff_t>(index));
    if (index < folded_.size()) folded_.erase(folded_.begin() + static_cast<ptrdiff_t>(index));
}

size_t ClipboardHook::PinnedPrefixLength() const {
    size_t count = 0;
    while (count < history_.size() && history_[count].pinned) ++count;
    return count;
}

void ClipboardHook::SortHistory() {
    std::vector<size_t> order(history_.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [this](size_t a, size_t b) {
        const ClipEntry& left = history_[a];
        const ClipEntry& right = history_[b];
        if (left.pinned != right.pinned) return left.pinned;
        return left.createdAt > right.createdAt;
    });

    std::vector<ClipEntry> sortedEntries;
    std::vector<std::wstring> sortedFolded;
    sortedEntries.reserve(history_.size());
    sortedFolded.reserve(folded_.size());
    for (const size_t index : order) {
        sortedEntries.push_back(history_[index]);
        sortedFolded.push_back(index < folded_.size() ? folded_[index] : text::ToLower(history_[index].text));
    }
    history_.swap(sortedEntries);
    folded_.swap(sortedFolded);
}

void ClipboardHook::ReorderInMemory() {
    if (history_.size() < 2) return;

    // Salida rápida: si el bloque anclado ya está delante, no se toca nada.
    bool alreadyOrdered = true;
    bool seenUnpinned = false;
    for (const auto& entry : history_) {
        if (entry.pinned && seenUnpinned) {
            alreadyOrdered = false;
            break;
        }
        if (!entry.pinned) seenUnpinned = true;
    }
    if (alreadyOrdered) return;

    // Partición estable: anclados delante, cada bloque conserva su orden por fecha.
    std::vector<ClipEntry> entries;
    std::vector<std::wstring> folds;
    entries.reserve(history_.size());
    folds.reserve(folded_.size());
    for (int pass = 0; pass < 2; ++pass) {
        const bool wantedPinned = (pass == 0);
        for (size_t i = 0; i < history_.size(); ++i) {
            if (history_[i].pinned != wantedPinned) continue;
            entries.push_back(history_[i]);
            folds.push_back(i < folded_.size() ? folded_[i] : text::ToLower(history_[i].text));
        }
    }
    if (entries.size() != history_.size()) return;
    history_.swap(entries);
    folded_.swap(folds);
}

void ClipboardHook::TrimMemory() {
    const size_t limit = static_cast<size_t>(settings_.historyLimit);
    while (history_.size() > limit) {
        // Los anclados nunca se descartan de la vista en memoria.
        size_t victim = history_.size();
        for (size_t i = history_.size(); i-- > 0;) {
            if (!history_[i].pinned) {
                victim = i;
                break;
            }
        }
        if (victim == history_.size()) break;
        EraseHistory(victim);
    }
}

void ClipboardHook::ApplyFilter() {
    filtered_.clear();
    filtered_.reserve(history_.size());
    if (history_.empty()) return;

    if (foldedQuery_.empty()) {
        for (size_t i = 0; i < history_.size(); ++i) filtered_.push_back(i);
        return;
    }
    for (size_t i = 0; i < history_.size(); ++i) {
        // folded_ ya está en minúsculas: la búsqueda es un find plano, sin volver a
        // normalizar en cada pulsación de tecla.
        const std::wstring& haystack = i < folded_.size() ? folded_[i] : history_[i].text;
        if (haystack.find(foldedQuery_) != std::wstring::npos) filtered_.push_back(i);
    }
}

void ClipboardHook::SetQuery(std::wstring_view query) {
    query_.assign(query);
    foldedQuery_ = text::ToLower(text::Trim(query_));
    ApplyFilter();
}

size_t ClipboardHook::VisibleToHistory(size_t visibleIndex) const {
    if (visibleIndex >= filtered_.size()) return static_cast<size_t>(-1);
    return filtered_[visibleIndex];
}

const ClipEntry* ClipboardHook::Visible(size_t visibleIndex) const {
    const size_t index = VisibleToHistory(visibleIndex);
    if (index == static_cast<size_t>(-1) || index >= history_.size()) return nullptr;
    return &history_[index];
}

size_t ClipboardHook::PinnedCount() const {
    size_t count = 0;
    for (const auto& entry : history_) {
        if (entry.pinned) ++count;
    }
    return count;
}

bool ClipboardHook::TogglePin(size_t visibleIndex) {
    const size_t index = VisibleToHistory(visibleIndex);
    if (index == static_cast<size_t>(-1) || history_[index].id <= 0) return false;
    ClipEntry& entry = history_[index];
    const bool target = !entry.pinned;
    if (db_ != nullptr && !db_->SetPinned(entry.id, target)) return false;
    entry.pinned = target;
    entry.updatedAt = paths::NowUnixSeconds();
    SortHistory();
    ApplyFilter();
    return true;
}

bool ClipboardHook::DeleteAt(size_t visibleIndex) {
    const size_t index = VisibleToHistory(visibleIndex);
    if (index == static_cast<size_t>(-1)) return false;
    const int64_t id = history_[index].id;
    if (db_ != nullptr && id > 0 && !db_->Delete(id)) return false;
    EraseHistory(index);
    ApplyFilter();
    return true;
}

bool ClipboardHook::ReplaceText(size_t visibleIndex, const std::wstring& newText, bool writeToClipboard) {
    const size_t index = VisibleToHistory(visibleIndex);
    if (index == static_cast<size_t>(-1)) return false;
    ClipEntry& entry = history_[index];

    std::wstring stored = newText;
    if (stored.size() > static_cast<size_t>(settings_.maxPreviewChars)) {
        stored.resize(static_cast<size_t>(settings_.maxPreviewChars));
        stored += L"\n\u2026 [recortado]";
    }
    entry.text = stored;
    entry.kind = data::ClassifyText(stored);
    entry.updatedAt = paths::NowUnixSeconds();
    if (index < folded_.size()) folded_[index] = text::ToLower(stored);
    if (db_ != nullptr && entry.id > 0) {
        if (!db_->UpdateText(entry.id, stored)) return false;
        db_->Touch(entry.id);
    }
    if (writeToClipboard) CopyTextToClipboard(owner_, stored);
    ApplyFilter();
    return true;
}

bool ClipboardHook::CopyToClipboard(size_t visibleIndex) {
    const ClipEntry* entry = Visible(visibleIndex);
    if (entry == nullptr || entry->text.empty()) return false;
    const bool ok = CopyTextToClipboard(owner_, entry->text);
    if (ok && db_ != nullptr && entry->id > 0) db_->Touch(entry->id);
    return ok;
}

// -----------------------------------------------------------------------------
// Captura
// -----------------------------------------------------------------------------

bool ClipboardHook::CaptureText(const std::wstring& text, ClipKind kind, const std::wstring& source) {
    if (db_ == nullptr) return false;

    // Normaliza el final: los programas suelen dejar NUL o CR sobrantes al copiar.
    std::wstring cleaned;
    cleaned.reserve(text.size());
    for (const wchar_t c : text) {
        if (c != L'\0') cleaned.push_back(c);
    }
    cleaned = text::Trim(cleaned);
    if (cleaned.empty()) {
        ++skippedCount_;
        return false;
    }

    const std::wstring hash = data::MakeClipHash(cleaned);
    if (settings_.dedupe && !history_.empty() && history_.front().hash == hash) {
        // La misma copia repetida no reinicia el historial: solo suma un uso.
        if (history_.front().id > 0) db_->Touch(history_.front().id);
        ++skippedCount_;
        return false;
    }

    std::wstring stored = cleaned;
    if (stored.size() > static_cast<size_t>(settings_.maxPreviewChars)) {
        stored.resize(static_cast<size_t>(settings_.maxPreviewChars));
        stored += L"\n\u2026 [recortado]";
    }

    ClipEntry entry;
    entry.kind = kind == ClipKind::Text ? data::ClassifyText(cleaned) : kind;
    entry.text = stored;
    entry.hash = hash;
    entry.source = source;
    entry.createdAt = paths::NowUnixSeconds();
    entry.updatedAt = entry.createdAt;
    entry.hits = 1;

    const int64_t id = db_->InsertOrBump(entry, settings_.dedupe);
    if (id < 0) {
        ++skippedCount_;
        return false;
    }
    entry.id = id;

    // Reubica el clip si ya estaba en memoria (dedupe): sube al frente conservando el ancla.
    bool merged = false;
    for (size_t i = 0; i < history_.size(); ++i) {
        if (history_[i].hash == hash) {
            entry.hits = history_[i].hits + 1;
            entry.pinned = history_[i].pinned;
            EraseHistory(i);
            merged = true;
            break;
        }
    }
    // Insertar justo después del bloque anclado deja el clip nuevo al frente de los
    // no anclados; ReorderInMemory reafirma la invariante por si hubo refresco de uno
    // ya existente (`merged`).
    InsertHistory(PinnedPrefixLength(), entry);
    if (merged) ReorderInMemory();
    TrimMemory();
    ApplyFilter();

    ++capturedCount_;
    lastCapture_ = text::Format(L"%s \u00B7 %llu ch", data::ClipKindLabel(entry.kind),
                                static_cast<unsigned long long>(cleaned.size()));
    return true;
}

bool ClipboardHook::CaptureFiles(HDROP drop) {
    if (drop == nullptr) return false;
    const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    if (count == 0) return false;

    std::vector<std::wstring> paths;
    paths.reserve(count);
    for (UINT i = 0; i < count && i < 64; ++i) {
        const UINT length = ::DragQueryFileW(drop, i, nullptr, 0);
        if (length == 0) continue;
        std::wstring path(length + 1, L'\0');
        const UINT copied = ::DragQueryFileW(drop, i, path.data(), static_cast<UINT>(path.size()));
        path.resize(copied);
        if (!path.empty()) paths.push_back(path);
    }
    if (paths.empty()) return false;

    if (paths.size() == 1) {
        const ClipKind kind = data::ClassifyText(paths[0]);
        return CaptureText(paths[0], kind == ClipKind::Text ? ClipKind::FilePath : kind,
                           ForegroundProcessName());
    }
    return CaptureText(text::Join(paths, L"\n"), ClipKind::Files, ForegroundProcessName());
}

bool ClipboardHook::CaptureBitmap() {
    if (!settings_.captureImages) return false;
    HANDLE handle = ::GetClipboardData(CF_DIB);
    if (handle == nullptr) return false;

    void* locked = ::GlobalLock(handle);
    if (locked == nullptr) return false;
    const SIZE_T size = ::GlobalSize(handle);
    if (size < sizeof(BITMAPINFOHEADER)) {
        ::GlobalUnlock(handle);
        return false;
    }

    std::vector<uint8_t> dib(static_cast<size_t>(size));
    std::memcpy(dib.data(), locked, static_cast<size_t>(size));
    ::GlobalUnlock(handle);

    const BITMAPINFOHEADER* header = reinterpret_cast<const BITMAPINFOHEADER*>(dib.data());
    const int width = static_cast<int>(header->biWidth);
    const int height = static_cast<int>(header->biHeight);

    // La imagen se persiste como BMP dentro de %APPDATA%\EdgeDock\Clips y el clip guarda
    // su ruta: así el historial sigue siendo texto puro y las imágenes no engordan la base.
    const std::wstring clipsDir = paths::AppDataRoot() + L"\\Clips";
    paths::EnsureDirectory(clipsDir);
    const std::wstring path = paths::Join(
        clipsDir, text::Format(L"img-%s-%ux%u.bmp", paths::TimestampForFile().c_str(),
                               static_cast<unsigned>(width < 0 ? -width : width),
                               static_cast<unsigned>(height < 0 ? -height : height)));
    if (!WriteBmpFile(path, dib)) {
        ++skippedCount_;
        return false;
    }
    return CaptureText(path, ClipKind::ImagePath, ForegroundProcessName());
}

bool ClipboardHook::CaptureRawFormat(UINT format, ClipKind kind, const wchar_t* label) {
    HANDLE handle = ::GetClipboardData(format);
    if (handle == nullptr) return false;
    void* locked = ::GlobalLock(handle);
    if (locked == nullptr) return false;
    const SIZE_T size = ::GlobalSize(handle);
    if (size == 0) {
        ::GlobalUnlock(handle);
        return false;
    }
    std::string payload(static_cast<const char*>(locked), static_cast<size_t>(size));
    ::GlobalUnlock(handle);

    std::wstring converted;
    if (format == FormatRtf()) {
        converted = RtfToPlain(payload);
    } else {
        converted = HtmlFragmentToText(payload);
    }
    if (text::Trim(converted).empty()) {
        return CaptureText(text::Format(L"[%s sin texto legible]", label), ClipKind::Text,
                           ForegroundProcessName());
    }
    return CaptureText(converted, kind, ForegroundProcessName());
}

bool ClipboardHook::HandleClipboardUpdate() {
    if (!installed_ || db_ == nullptr) return false;

    bool opened = false;
    for (int attempt = 0; attempt < kClipboardRetries && !opened; ++attempt) {
        opened = ::OpenClipboard(owner_) != FALSE;
        if (!opened) ::Sleep(kClipboardRetrySleepMs);
    }
    if (!opened) {
        ++skippedCount_;
        return false;
    }

    bool captured = false;
    if (settings_.captureFiles && ::IsClipboardFormatAvailable(CF_HDROP)) {
        captured = CaptureFiles(reinterpret_cast<HDROP>(::GetClipboardData(CF_HDROP)));
    }
    if (!captured && ::IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        std::wstring text;
        HANDLE handle = ::GetClipboardData(CF_UNICODETEXT);
        if (handle != nullptr) {
            const wchar_t* locked = static_cast<const wchar_t*>(::GlobalLock(handle));
            if (locked != nullptr) {
                const SIZE_T bytes = ::GlobalSize(handle);
                size_t length = bytes / sizeof(wchar_t);
                while (length > 0 && locked[length - 1] == L'\0') --length;
                text.assign(locked, length);
                ::GlobalUnlock(handle);
            }
        }
        if (!text.empty()) captured = CaptureText(text, data::ClassifyText(text), ForegroundProcessName());
    }
    if (!captured && settings_.captureImages && ::IsClipboardFormatAvailable(CF_DIB)) {
        captured = CaptureBitmap();
    }
    if (!captured && FormatHtml() != 0 && ::IsClipboardFormatAvailable(FormatHtml())) {
        captured = CaptureRawFormat(FormatHtml(), ClipKind::Code, L"HTML");
    }
    if (!captured && FormatRtf() != 0 && ::IsClipboardFormatAvailable(FormatRtf())) {
        captured = CaptureRawFormat(FormatRtf(), ClipKind::Text, L"RTF");
    }

    ::CloseClipboard();
    if (captured && notify_) notify_();
    return captured;
}

// -----------------------------------------------------------------------------
// Utilidades estáticas
// -----------------------------------------------------------------------------

bool ClipboardHook::CopyTextToClipboard(HWND owner, const std::wstring& text) {
    if (text.empty()) return false;

    bool opened = false;
    for (int attempt = 0; attempt < kClipboardRetries && !opened; ++attempt) {
        opened = ::OpenClipboard(owner) != FALSE;
        if (!opened) ::Sleep(kClipboardRetrySleepMs);
    }
    if (!opened) return false;

    bool ok = false;
    if (::EmptyClipboard() != FALSE) {
        const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (memory != nullptr) {
            void* locked = ::GlobalLock(memory);
            if (locked != nullptr) {
                std::memcpy(locked, text.c_str(), bytes);
                ::GlobalUnlock(memory);
                if (::SetClipboardData(CF_UNICODETEXT, memory) != nullptr) {
                    ok = true;
                    memory = nullptr;   // el portapapeles es ahora el propietario
                }
            }
            if (memory != nullptr) ::GlobalFree(memory);
        }
    }
    ::CloseClipboard();
    return ok;
}

bool ClipboardHook::ReadClipboardText(HWND owner, std::wstring& out) {
    out.clear();
    if (!::IsClipboardFormatAvailable(CF_UNICODETEXT)) return false;

    bool opened = false;
    for (int attempt = 0; attempt < kClipboardRetries && !opened; ++attempt) {
        opened = ::OpenClipboard(owner) != FALSE;
        if (!opened) ::Sleep(kClipboardRetrySleepMs);
    }
    if (!opened) return false;

    HANDLE handle = ::GetClipboardData(CF_UNICODETEXT);
    if (handle != nullptr) {
        const wchar_t* locked = static_cast<const wchar_t*>(::GlobalLock(handle));
        if (locked != nullptr) {
            const SIZE_T bytes = ::GlobalSize(handle);
            size_t length = bytes / sizeof(wchar_t);
            while (length > 0 && locked[length - 1] == L'\0') --length;
            out.assign(locked, length);
            ::GlobalUnlock(handle);
        }
    }
    ::CloseClipboard();
    return !out.empty();
}

std::wstring ClipboardHook::ForegroundProcessName() {
    HWND foreground = ::GetForegroundWindow();
    if (foreground == nullptr) return L"desconocido";

    DWORD processId = 0;
    ::GetWindowThreadProcessId(foreground, &processId);
    if (processId == 0) return L"desconocido";

    HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process == nullptr) return L"desconocido";

    wchar_t buffer[MAX_PATH * 2] = {};
    DWORD length = static_cast<DWORD>(std::size(buffer));
    std::wstring name = L"desconocido";
    if (::QueryFullProcessImageNameW(process, 0, buffer, &length)) {
        name = text::ToLower(paths::FileNameOf(std::wstring(buffer, length)));
    }
    ::CloseHandle(process);
    return name;
}

} // namespace edgedock::modules
