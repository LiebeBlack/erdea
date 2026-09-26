// EdgeDock Studio :: data/ClipDatabase.cpp
#include "data/ClipDatabase.h"

#include "core/AppPaths.h"
#include "core/TextConv.h"

#include <windows.h>

#include <sqlite3.h>

#include <algorithm>

namespace edgedock::data {
namespace {

// Hash determinista (FNV-1a de 64 bits) + longitud: detecta duplicados sin comparar
// megabytes de texto en cada pegado.
constexpr uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;

std::wstring EscapeFtsToken(std::wstring_view token) {
    std::wstring out;
    out.reserve(token.size() + 4);
    out.push_back(L'"');
    for (const wchar_t c : token) {
        if (c == L'"') out.push_back(L'"');   // comilla escapada por duplicación
        out.push_back(c);
    }
    out.push_back(L'"');
    out.push_back(L'*');                      // prefijo: "bus" encuentra "búsqueda"
    return out;
}

std::wstring BuildFtsQuery(std::wstring_view raw) {
    std::vector<std::wstring> tokens;
    std::wstring current;
    for (const wchar_t c : raw) {
        if (text::IsSpace(c) || c == L',' || c == L';' || c == L'/') {
            if (!current.empty()) tokens.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) tokens.push_back(current);

    std::wstring query;
    for (size_t i = 0; i < tokens.size() && i < 8; ++i) {
        if (tokens[i].size() < 2) continue;
        if (!query.empty()) query += L" AND ";
        query += EscapeFtsToken(tokens[i]);
    }
    return query;
}

class Statement {
public:
    Statement(sqlite3* db, const char* sql) {
        if (::sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
            stmt_ = nullptr;
        }
    }
    ~Statement() {
        if (stmt_ != nullptr) ::sqlite3_finalize(stmt_);
    }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    bool Valid() const { return stmt_ != nullptr; }
    sqlite3_stmt* Get() const { return stmt_; }

    void BindText(int index, const std::wstring& value) {
        ::sqlite3_bind_text16(stmt_, index, value.c_str(), static_cast<int>(value.size() * sizeof(wchar_t)),
                              SQLITE_TRANSIENT);
    }
    void BindInt64(int index, int64_t value) { ::sqlite3_bind_int64(stmt_, index, value); }
    void BindInt(int index, int value) { ::sqlite3_bind_int(stmt_, index, value); }

private:
    sqlite3_stmt* stmt_ = nullptr;
};

ClipKind ReadKind(sqlite3_stmt* stmt, int column) {
    const int raw = ::sqlite3_column_int(stmt, column);
    if (raw < static_cast<int>(ClipKind::Text) || raw > static_cast<int>(ClipKind::Files)) {
        return ClipKind::Text;
    }
    return static_cast<ClipKind>(raw);
}

std::wstring ReadText(sqlite3_stmt* stmt, int column) {
    const void* data = ::sqlite3_column_text16(stmt, column);
    const int bytes = ::sqlite3_column_bytes16(stmt, column);
    if (data == nullptr || bytes <= 0) return std::wstring();
    return std::wstring(reinterpret_cast<const wchar_t*>(data), static_cast<size_t>(bytes / sizeof(wchar_t)));
}

ClipEntry ReadRow(sqlite3_stmt* stmt) {
    ClipEntry entry;
    entry.id = ::sqlite3_column_int64(stmt, 0);
    entry.kind = ReadKind(stmt, 1);
    entry.text = ReadText(stmt, 2);
    entry.source = ReadText(stmt, 3);
    entry.hash = ReadText(stmt, 4);
    entry.createdAt = ::sqlite3_column_int64(stmt, 5);
    entry.updatedAt = ::sqlite3_column_int64(stmt, 6);
    entry.hits = ::sqlite3_column_int64(stmt, 7);
    entry.pinned = ::sqlite3_column_int(stmt, 8) != 0;
    return entry;
}

constexpr const char* kColumns = "id, kind, text, source, hash, created_at, updated_at, hits, pinned";

} // namespace

const wchar_t* ClipKindLabel(ClipKind kind) {
    switch (kind) {
        case ClipKind::Text: return L"TXT";
        case ClipKind::Url: return L"URL";
        case ClipKind::Email: return L"MAIL";
        case ClipKind::Code: return L"CODE";
        case ClipKind::FilePath: return L"PATH";
        case ClipKind::ImagePath: return L"IMG";
        case ClipKind::Files: return L"FILES";
    }
    return L"TXT";
}

ClipKind ClassifyText(const std::wstring& text) {
    const std::wstring trimmed = text::Trim(text);
    if (trimmed.empty()) return ClipKind::Text;

    const std::wstring lower = text::ToLower(trimmed);

    // Ruta de archivo: una sola línea con separadores y sin espacios internos.
    const bool singleLine = trimmed.find(L'\n') == std::wstring::npos;
    if (singleLine && (lower.find(L"\\\\") == 0 || lower.find(L":\\") != std::wstring::npos)) {
        const std::wstring extension = paths::ExtensionOf(trimmed);
        if (extension == L".png" || extension == L".jpg" || extension == L".jpeg" || extension == L".gif" ||
            extension == L".bmp" || extension == L".webp" || extension == L".svg" || extension == L".ico" ||
            extension == L".tif" || extension == L".tiff") {
            return ClipKind::ImagePath;
        }
        return ClipKind::FilePath;
    }

    // URLs: esquema explícito o www. al inicio.
    if (lower.find(L"http://") == 0 || lower.find(L"https://") == 0 || lower.find(L"ftp://") == 0 ||
        lower.find(L"file://") == 0 || lower.find(L"www.") == 0) {
        return singleLine ? ClipKind::Url : ClipKind::Text;
    }

    // Email: una sola línea con arroba y dominio con punto, sin espacios.
    if (singleLine && trimmed.find(L'@') != std::wstring::npos &&
        trimmed.find(L' ') == std::wstring::npos && trimmed.find(L'.') != std::wstring::npos) {
        const size_t at = trimmed.find(L'@');
        if (at > 0 && at + 3 < trimmed.size()) return ClipKind::Email;
    }

    // Código: varias señales de sintaxis juntas en un bloque multilínea.
    if (!singleLine) {
        int signals = 0;
        const wchar_t* markers[] = {L"{", L"}", L";", L"=>", L"</", L"#!/", L"def ", L"class ", L"function ",
                                    L"return ", L"import ", L"#include", L"const ", L"let ", L"void "};
        for (const wchar_t* marker : markers) {
            if (text::Contains(lower, marker)) ++signals;
        }
        if (signals >= 3) return ClipKind::Code;
    }
    return ClipKind::Text;
}

std::wstring MakeClipHash(const std::wstring& text) {
    uint64_t hash = kFnvOffset;
    for (const wchar_t c : text) {
        const uint64_t low = static_cast<uint64_t>(c & 0x00FF);
        const uint64_t high = static_cast<uint64_t>((c >> 8) & 0x00FF);
        hash ^= low;
        hash *= kFnvPrime;
        hash ^= high;
        hash *= kFnvPrime;
    }
    return text::HexDigits(hash, 16) + L"-" + text::HexDigits(text.size(), 1);
}

ClipDatabase::~ClipDatabase() {
    Close();
}

void ClipDatabase::SetError(const wchar_t* context) {
    lastError_ = text::Format(L"%s: %s", context,
                              db_ != nullptr ? text::FromUtf8(::sqlite3_errmsg(db_)).c_str() : L"sin base");
}

bool ClipDatabase::Exec(const char* sql) {
    if (db_ == nullptr) return false;
    char* error = nullptr;
    if (::sqlite3_exec(db_, sql, nullptr, nullptr, &error) != SQLITE_OK) {
        lastError_ = text::Format(L"SQL: %s", error != nullptr ? text::FromUtf8(error).c_str() : L"desconocido");
        if (error != nullptr) ::sqlite3_free(error);
        return false;
    }
    return true;
}

bool ClipDatabase::Prepare(const char* sql, sqlite3_stmt** statement) const {
    if (db_ == nullptr) return false;
    return ::sqlite3_prepare_v2(db_, sql, -1, statement, nullptr) == SQLITE_OK;
}

bool ClipDatabase::Migrate() {
    static const char* kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS clips (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    kind        INTEGER NOT NULL DEFAULT 0,
    text        TEXT    NOT NULL,
    source      TEXT    NOT NULL DEFAULT '',
    hash        TEXT    NOT NULL UNIQUE,
    created_at  INTEGER NOT NULL,
    updated_at  INTEGER NOT NULL,
    hits        INTEGER NOT NULL DEFAULT 1,
    pinned      INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_clips_created ON clips(created_at DESC);
CREATE INDEX IF NOT EXISTS idx_clips_pinned ON clips(pinned, created_at DESC);
CREATE VIRTUAL TABLE IF NOT EXISTS clips_fts USING fts5(
    text,
    content='clips',
    content_rowid='id',
    tokenize='unicode61 remove_diacritics 2'
);
CREATE TRIGGER IF NOT EXISTS clips_ai AFTER INSERT ON clips BEGIN
    INSERT INTO clips_fts(rowid, text) VALUES (new.id, new.text);
END;
CREATE TRIGGER IF NOT EXISTS clips_ad AFTER DELETE ON clips BEGIN
    INSERT INTO clips_fts(clips_fts, rowid, text) VALUES ('delete', old.id, old.text);
END;
CREATE TRIGGER IF NOT EXISTS clips_au AFTER UPDATE ON clips BEGIN
    INSERT INTO clips_fts(clips_fts, rowid, text) VALUES ('delete', old.id, old.text);
    INSERT INTO clips_fts(rowid, text) VALUES (new.id, new.text);
END;
PRAGMA user_version = 1;
)SQL";
    return Exec(kSchema);
}

bool ClipDatabase::Open(const std::wstring& path) {
    Close();
    path_ = path;
    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    if (::sqlite3_open_v2(text::ToUtf8(path).c_str(), &db_, flags, nullptr) != SQLITE_OK) {
        SetError(L"sqlite3_open_v2");
        if (db_ != nullptr) {
            ::sqlite3_close(db_);
            db_ = nullptr;
        }
        return false;
    }

    ::sqlite3_busy_timeout(db_, 3000);
    // WAL + NORMAL: escrituras instantáneas sin fsync por clip, ideal para un historial vivo.
    if (!Exec("PRAGMA journal_mode=WAL;") || !Exec("PRAGMA synchronous=NORMAL;") ||
        !Exec("PRAGMA temp_store=MEMORY;") || !Exec("PRAGMA cache_size=-8000;") ||
        !Exec("PRAGMA foreign_keys=ON;")) {
        paths::AppendLog(L"ClipDatabase: no se pudieron aplicar todos los PRAGMA");
    }
    if (!Migrate()) {
        paths::AppendLog(L"ClipDatabase: migración de esquema fallida -> " + lastError_);
        Close();
        return false;
    }
    return true;
}

void ClipDatabase::Close() {
    if (db_ != nullptr) {
        // Checkpoint final: deja el .db consistente sin depender del cierre del proceso.
        ::sqlite3_wal_checkpoint_v2(db_, nullptr, SQLITE_CHECKPOINT_TRUNCATE, nullptr, nullptr);
        ::sqlite3_close(db_);
        db_ = nullptr;
    }
}

int64_t ClipDatabase::InsertOrBump(const ClipEntry& entry, bool dedupe) {
    if (db_ == nullptr) return -1;

    std::wstring hash = entry.hash;
    if (hash.empty()) hash = MakeClipHash(entry.text);
    if (!dedupe) {
        // Con el dedupe desactivado cada pegado entra como fila propia.
        hash += L"~" + text::HexDigits(static_cast<unsigned long long>(paths::NowUnixSeconds()), 8) + L"~" +
                text::HexDigits(static_cast<unsigned long long>(::GetTickCount64()), 8);
    }

    const int64_t now = paths::NowUnixSeconds();
    const int64_t createdAt = entry.createdAt > 0 ? entry.createdAt : now;
    const int64_t updatedAt = entry.updatedAt > 0 ? entry.updatedAt : now;

    static const char* kUpsert =
        "INSERT INTO clips(kind, text, source, hash, created_at, updated_at, hits, pinned) "
        "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8) "
        "ON CONFLICT(hash) DO UPDATE SET "
        "  text = excluded.text, kind = excluded.kind, source = excluded.source, "
        "  created_at = excluded.created_at, updated_at = excluded.updated_at, "
        "  hits = clips.hits + 1, pinned = MAX(clips.pinned, excluded.pinned);";

    Statement upsert(db_, kUpsert);
    if (!upsert.Valid()) {
        SetError(L"prepare upsert");
        return -1;
    }
    upsert.BindInt(1, static_cast<int>(entry.kind));
    upsert.BindText(2, entry.text);
    upsert.BindText(3, entry.source);
    upsert.BindText(4, hash);
    upsert.BindInt64(5, createdAt);
    upsert.BindInt64(6, updatedAt);
    upsert.BindInt64(7, entry.hits > 0 ? entry.hits : 1);
    upsert.BindInt(8, entry.pinned ? 1 : 0);
    if (::sqlite3_step(upsert.Get()) != SQLITE_DONE) {
        SetError(L"step upsert");
        return -1;
    }

    Statement lookup(db_, "SELECT id FROM clips WHERE hash = ?1;");
    if (!lookup.Valid()) {
        SetError(L"prepare lookup");
        return -1;
    }
    lookup.BindText(1, hash);
    if (::sqlite3_step(lookup.Get()) != SQLITE_ROW) return -1;
    return ::sqlite3_column_int64(lookup.Get(), 0);
}

bool ClipDatabase::SetPinned(int64_t id, bool pinned) {
    Statement stmt(db_, "UPDATE clips SET pinned = ?1, updated_at = ?2 WHERE id = ?3;");
    if (!stmt.Valid()) return false;
    stmt.BindInt(1, pinned ? 1 : 0);
    stmt.BindInt64(2, paths::NowUnixSeconds());
    stmt.BindInt64(3, id);
    if (::sqlite3_step(stmt.Get()) != SQLITE_DONE) {
        SetError(L"set pinned");
        return false;
    }
    return true;
}

bool ClipDatabase::Touch(int64_t id) {
    Statement stmt(db_, "UPDATE clips SET hits = hits + 1 WHERE id = ?1;");
    if (!stmt.Valid()) return false;
    stmt.BindInt64(1, id);
    return ::sqlite3_step(stmt.Get()) == SQLITE_DONE;
}

bool ClipDatabase::UpdateText(int64_t id, const std::wstring& text) {
    Statement stmt(db_, "UPDATE clips SET text = ?1, kind = ?2, updated_at = ?3 WHERE id = ?4;");
    if (!stmt.Valid()) return false;
    stmt.BindText(1, text);
    stmt.BindInt(2, static_cast<int>(ClassifyText(text)));
    stmt.BindInt64(3, paths::NowUnixSeconds());
    stmt.BindInt64(4, id);
    if (::sqlite3_step(stmt.Get()) != SQLITE_DONE) {
        SetError(L"update text");
        return false;
    }
    return true;
}

bool ClipDatabase::Delete(int64_t id) {
    Statement stmt(db_, "DELETE FROM clips WHERE id = ?1;");
    if (!stmt.Valid()) return false;
    stmt.BindInt64(1, id);
    return ::sqlite3_step(stmt.Get()) == SQLITE_DONE;
}

bool ClipDatabase::DeleteAllUnpinned() {
    return Exec("DELETE FROM clips WHERE pinned = 0;");
}

int ClipDatabase::PruneUnpinned(int keepCount, int keepDays, int64_t nowSeconds) {
    if (db_ == nullptr) return 0;
    const int64_t cutoff = nowSeconds - static_cast<int64_t>(keepDays) * 86400LL;
    const int64_t floor = static_cast<int64_t>(keepCount < 0 ? 0 : keepCount);

    // Se borran los no anclados que queden por debajo del corte temporal Y fuera del
    // bloque de los `keepCount` más recientes. Las tres condiciones anteriores se
    // cubren con esta consulta: por antigüedad o por exceso de volumen.
    Statement stmt(db_,
                   "DELETE FROM clips WHERE pinned = 0 AND id NOT IN ("
                   "  SELECT id FROM clips WHERE pinned = 0 ORDER BY created_at DESC LIMIT ?1"
                   ") AND created_at < ?2;");
    if (!stmt.Valid()) return 0;
    stmt.BindInt64(1, floor);
    stmt.BindInt64(2, cutoff);
    if (::sqlite3_step(stmt.Get()) != SQLITE_DONE) {
        SetError(L"prune");
        return 0;
    }
    const int removed = ::sqlite3_changes(db_);
    if (removed > 0) {
        // Mantener el índice FTS compacto tras borrados masivos.
        Exec("INSERT INTO clips_fts(clips_fts) VALUES('optimize');");
    }
    return removed;
}

std::vector<ClipEntry> ClipDatabase::Load(int limit) {
    std::vector<ClipEntry> entries;
    if (db_ == nullptr) return entries;
    if (limit <= 0) limit = 200;

    const std::string sql = std::string("SELECT ") + kColumns + " FROM clips ORDER BY pinned DESC, created_at DESC LIMIT ?1;";
    Statement stmt(db_, sql.c_str());
    if (!stmt.Valid()) {
        SetError(L"load");
        return entries;
    }
    stmt.BindInt64(1, limit);
    entries.reserve(static_cast<size_t>(limit));
    while (::sqlite3_step(stmt.Get()) == SQLITE_ROW) {
        entries.push_back(ReadRow(stmt.Get()));
    }
    return entries;
}

std::vector<ClipEntry> ClipDatabase::Search(const std::wstring& query, int limit) {
    std::vector<ClipEntry> entries;
    if (db_ == nullptr) return entries;
    const std::wstring trimmed = text::Trim(query);
    if (trimmed.empty()) return Load(limit);
    if (limit <= 0) limit = 200;

    const std::wstring fts = BuildFtsQuery(trimmed);
    if (!fts.empty()) {
        const std::string sql = std::string("SELECT ") + kColumns +
                                " FROM clips c JOIN clips_fts f ON f.rowid = c.id "
                                "WHERE clips_fts MATCH ?1 ORDER BY c.pinned DESC, bm25(clips_fts) LIMIT ?2;";
        Statement stmt(db_, sql.c_str());
        if (stmt.Valid()) {
            stmt.BindText(1, fts);
            stmt.BindInt64(2, limit);
            while (::sqlite3_step(stmt.Get()) == SQLITE_ROW) {
                entries.push_back(ReadRow(stmt.Get()));
            }
            if (!entries.empty()) return entries;
        }
    }

    // Reserva para consultas que FTS no indexa (una sola letra, símbolos sueltos).
    const std::string likeSql = std::string("SELECT ") + kColumns +
                                " FROM clips WHERE text LIKE '%' || ?1 || '%' "
                                "ORDER BY pinned DESC, created_at DESC LIMIT ?2;";
    Statement fallback(db_, likeSql.c_str());
    if (!fallback.Valid()) return entries;
    fallback.BindText(1, trimmed);
    fallback.BindInt64(2, limit);
    while (::sqlite3_step(fallback.Get()) == SQLITE_ROW) {
        entries.push_back(ReadRow(fallback.Get()));
    }
    return entries;
}

int ClipDatabase::CountAll() const {
    sqlite3_stmt* stmt = nullptr;
    if (!Prepare("SELECT COUNT(*) FROM clips;", &stmt)) return 0;
    int count = 0;
    if (::sqlite3_step(stmt) == SQLITE_ROW) count = ::sqlite3_column_int(stmt, 0);
    ::sqlite3_finalize(stmt);
    return count;
}

int ClipDatabase::CountPinned() const {
    sqlite3_stmt* stmt = nullptr;
    if (!Prepare("SELECT COUNT(*) FROM clips WHERE pinned = 1;", &stmt)) return 0;
    int count = 0;
    if (::sqlite3_step(stmt) == SQLITE_ROW) count = ::sqlite3_column_int(stmt, 0);
    ::sqlite3_finalize(stmt);
    return count;
}

int64_t ClipDatabase::DatabaseSizeBytes() const {
    int64_t total = paths::FileSizeBytes(path_);
    total += paths::FileSizeBytes(path_ + L"-wal");
    total += paths::FileSizeBytes(path_ + L"-shm");
    return total;
}

} // namespace edgedock::data
