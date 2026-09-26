#pragma once
// EdgeDock Studio :: core/TextConv.h
// Conversión UTF-8 <-> UTF-16 y utilidades de cadena sin dependencias externas.

#include <string>
#include <string_view>
#include <vector>

namespace edgedock::text {

// --- Conversión de codificación -------------------------------------------------
std::string  ToUtf8(std::wstring_view wide);
std::wstring FromUtf8(std::string_view utf8);

// --- Caja y clasificación (CharUpperBuffW / CharLowerBuffW: respeta el locale) ---
std::wstring ToLower(std::wstring_view s);
std::wstring ToUpper(std::wstring_view s);
bool IsSpace(wchar_t c);
bool IsDigit(wchar_t c);
bool IsAlpha(wchar_t c);

// --- Recorte ------------------------------------------------------------------
std::wstring Trim(std::wstring_view s);
std::wstring TrimLeft(std::wstring_view s);
std::wstring TrimRight(std::wstring_view s);

// --- Comparación y búsqueda ----------------------------------------------------
bool EqualsNoCase(std::wstring_view a, std::wstring_view b);
bool StartsWith(std::wstring_view s, std::wstring_view prefix);
bool StartsWithNoCase(std::wstring_view s, std::wstring_view prefix);
bool EndsWith(std::wstring_view s, std::wstring_view suffix);
bool Contains(std::wstring_view hay, std::wstring_view needle);
bool ContainsNoCase(std::wstring_view hay, std::wstring_view needle);
size_t FindNoCase(std::wstring_view hay, std::wstring_view needle);

// --- Manipulación -------------------------------------------------------------
std::wstring ReplaceAll(std::wstring_view s, std::wstring_view from, std::wstring_view to);
std::vector<std::wstring> SplitLines(std::wstring_view s);
std::vector<std::wstring> Split(std::wstring_view s, wchar_t sep);
std::wstring Join(const std::vector<std::wstring>& parts, std::wstring_view sep);
std::wstring CollapseSpaces(std::wstring_view s);          // espacios/tabs múltiples -> uno
std::wstring StripControl(std::wstring_view s);            // fuera caracteres de control
std::wstring TruncateEllipsis(std::wstring_view s, size_t maxChars);
std::wstring Format(const wchar_t* fmt, ...);              // printf-style sobre wchar_t
std::wstring FormatV(const wchar_t* fmt, va_list args);
std::wstring HexDigits(unsigned long long value, size_t minDigits);

// Longitud visual aproximada: cuenta caracteres UTF-16 fuera del rango surrogate.
size_t DisplayLength(std::wstring_view s);

} // namespace edgedock::text
