#pragma once
// EdgeDock Studio :: core/AppPaths.h
// Rutas de la aplicación (%APPDATA%\EdgeDock), creación de directorios y sellos de tiempo.

#include <cstdint>
#include <string>

namespace edgedock::paths {

// --- Directorios y archivos de la aplicación ------------------------------------
std::wstring AppDataRoot();      // %APPDATA%\EdgeDock
std::wstring NotesDir();         // %APPDATA%\EdgeDock\Notes
std::wstring ConfigFile();       // %APPDATA%\EdgeDock\config.json
std::wstring DatabaseFile();     // %APPDATA%\EdgeDock\clips.db
std::wstring LogFile();          // %APPDATA%\EdgeDock\edgedock.log
std::wstring ModuleDirectory();  // Carpeta del ejecutable

// --- Sistema de archivos -------------------------------------------------------
bool EnsureDirectory(const std::wstring& dir);
bool FileExists(const std::wstring& path);
bool DirectoryExists(const std::wstring& path);
bool DeleteFileIfExists(const std::wstring& path);
uint64_t FileSizeBytes(const std::wstring& path);
std::wstring ExpandEnvironment(const std::wstring& value);
std::wstring Join(const std::wstring& base, const std::wstring& child);
std::wstring FileNameOf(const std::wstring& path);
std::wstring ExtensionOf(const std::wstring& path);   // en minúsculas, con punto
std::wstring SanitizeFileName(std::wstring_view name);
bool WriteTextFile(const std::wstring& path, const std::string& utf8Content);
bool ReadTextFile(const std::wstring& path, std::string& outUtf8);
bool WriteTextFileAtomic(const std::wstring& path, const std::string& utf8Content);

// --- Sellos de tiempo ----------------------------------------------------------
int64_t NowUnixSeconds();
std::wstring TimestampForFile();                                  // 2026-09-25_2113-07
std::wstring FormatLocalTime(int64_t unixSeconds, const wchar_t* fmt); // L"%H:%M" / L"%d/%m/%Y %H:%M"
std::wstring RelativeTime(int64_t unixSeconds);                   // "ahora", "hace 5 min", "12/09"

// --- Ayuda de diagnóstico ------------------------------------------------------
void AppendLog(const std::wstring& line);

} // namespace edgedock::paths
