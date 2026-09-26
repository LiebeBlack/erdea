#pragma once
// EdgeDock Studio :: data/ClipDatabase.h
// Historial multi-nivel sobre SQLite embebido (amalgama en third_party/sqlite3) con índice
// FTS5 para búsqueda instantánea, clips anclados y poda por antigüedad.

#include <cstdint>
#include <string>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace edgedock::data {

enum class ClipKind : int {
    Text = 0,
    Url = 1,
    Email = 2,
    Code = 3,
    FilePath = 4,
    ImagePath = 5,
    Files = 6,
};

const wchar_t* ClipKindLabel(ClipKind kind);
ClipKind ClassifyText(const std::wstring& text);

struct ClipEntry {
    int64_t id = 0;
    ClipKind kind = ClipKind::Text;
    std::wstring text;          // Contenido (recortado según max_preview_chars)
    std::wstring source;        // Proceso que lo copió (p. ej. "chrome.exe")
    std::wstring hash;          // FNV-1a de 64 bits en hexadecimal (dedupe)
    int64_t createdAt = 0;      // Unix seconds
    int64_t updatedAt = 0;
    int64_t hits = 1;           // Veces que se volvió a copiar el mismo contenido
    bool pinned = false;        // Clip anclado: nunca se poda ni se sobrescribe

    bool IsEmpty() const { return text.empty(); }
    size_t Length() const { return text.size(); }
};

class ClipDatabase {
public:
    ClipDatabase() = default;
    ~ClipDatabase();
    ClipDatabase(const ClipDatabase&) = delete;
    ClipDatabase& operator=(const ClipDatabase&) = delete;

    bool Open(const std::wstring& path);
    void Close();
    bool IsOpen() const { return db_ != nullptr; }

    // Inserta o incrementa el contador de un clip ya existente. Devuelve el id, o -1 si falló.
    int64_t InsertOrBump(const ClipEntry& entry, bool dedupe);
    bool SetPinned(int64_t id, bool pinned);
    bool Touch(int64_t id);
    bool UpdateText(int64_t id, const std::wstring& text);
    bool Delete(int64_t id);
    bool DeleteAllUnpinned();
    // Conserva los `keepCount` más recientes y todo lo anclado; borra lo más antiguo que
    // `keepDays`. Devuelve cuántas filas se eliminaron.
    int PruneUnpinned(int keepCount, int keepDays, int64_t nowSeconds);

    std::vector<ClipEntry> Load(int limit);
    std::vector<ClipEntry> Search(const std::wstring& query, int limit);

    int CountAll() const;
    int CountPinned() const;
    int64_t DatabaseSizeBytes() const;
    const std::wstring& Path() const { return path_; }
    std::wstring LastError() const { return lastError_; }

private:
    bool Exec(const char* sql);
    bool Prepare(const char* sql, sqlite3_stmt** statement) const;
    bool Migrate();
    void SetError(const wchar_t* context);

    sqlite3* db_ = nullptr;
    std::wstring path_;
    std::wstring lastError_;
};

std::wstring MakeClipHash(const std::wstring& text);

} // namespace edgedock::data
