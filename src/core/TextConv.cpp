// EdgeDock Studio :: core/TextConv.cpp
#include "core/TextConv.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cwctype>
#include <stdexcept>

namespace edgedock::text {

std::string ToUtf8(std::wstring_view wide) {
    if (wide.empty()) return std::string();
    const int srcLen = static_cast<int>(wide.size());
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), srcLen, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return std::string();
    std::string out(static_cast<size_t>(needed), '\0');
    const int written = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), srcLen, out.data(), needed, nullptr, nullptr);
    if (written <= 0) return std::string();
    out.resize(static_cast<size_t>(written));
    return out;
}

std::wstring FromUtf8(std::string_view utf8) {
    if (utf8.empty()) return std::wstring();
    const int srcLen = static_cast<int>(utf8.size());
    const int needed = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), srcLen, nullptr, 0);
    if (needed <= 0) {
        // Entrada no válida en UTF-8: se devuelve como ANSI extendido en lugar de perder
        // por completo el contenido de origen.
        const int ansiNeeded = ::MultiByteToWideChar(CP_ACP, 0, utf8.data(), srcLen, nullptr, 0);
        if (ansiNeeded <= 0) return std::wstring();
        std::wstring ansi(static_cast<size_t>(ansiNeeded), L'\0');
        ::MultiByteToWideChar(CP_ACP, 0, utf8.data(), srcLen, ansi.data(), ansiNeeded);
        return ansi;
    }
    std::wstring out(static_cast<size_t>(needed), L'\0');
    const int written = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), srcLen, out.data(), needed);
    if (written <= 0) return std::wstring();
    out.resize(static_cast<size_t>(written));
    return out;
}

std::wstring ToLower(std::wstring_view s) {
    std::wstring out(s);
    if (!out.empty()) {
        ::CharLowerBuffW(out.data(), static_cast<DWORD>(out.size()));
    }
    return out;
}

std::wstring ToUpper(std::wstring_view s) {
    std::wstring out(s);
    if (!out.empty()) {
        ::CharUpperBuffW(out.data(), static_cast<DWORD>(out.size()));
    }
    return out;
}

bool IsSpace(wchar_t c) {
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'\f' || c == L'\v' ||
           c == 0x00A0 /* NBSP */ || c == 0x2028 || c == 0x2029 || c == 0xFEFF;
}

bool IsDigit(wchar_t c) {
    return ::iswdigit(static_cast<wint_t>(c)) != 0;
}

bool IsAlpha(wchar_t c) {
    return ::iswalpha(static_cast<wint_t>(c)) != 0;
}

std::wstring TrimLeft(std::wstring_view s) {
    size_t i = 0;
    while (i < s.size() && IsSpace(s[i])) ++i;
    return std::wstring(s.substr(i));
}

std::wstring TrimRight(std::wstring_view s) {
    size_t n = s.size();
    while (n > 0 && IsSpace(s[n - 1])) --n;
    return std::wstring(s.substr(0, n));
}

std::wstring Trim(std::wstring_view s) {
    size_t i = 0;
    size_t n = s.size();
    while (i < n && IsSpace(s[i])) ++i;
    while (n > i && IsSpace(s[n - 1])) --n;
    return std::wstring(s.substr(i, n - i));
}

bool EqualsNoCase(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) return false;
    if (a.empty()) return true;
    return ::CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
                                  b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

bool StartsWith(std::wstring_view s, std::wstring_view prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool StartsWithNoCase(std::wstring_view s, std::wstring_view prefix) {
    if (s.size() < prefix.size()) return false;
    if (prefix.empty()) return true;
    return ::CompareStringOrdinal(s.data(), static_cast<int>(prefix.size()),
                                  prefix.data(), static_cast<int>(prefix.size()),
                                  TRUE) == CSTR_EQUAL;
}

bool EndsWith(std::wstring_view s, std::wstring_view suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool Contains(std::wstring_view hay, std::wstring_view needle) {
    if (needle.empty()) return true;
    return hay.find(needle) != std::wstring_view::npos;
}

size_t FindNoCase(std::wstring_view hay, std::wstring_view needle) {
    if (needle.empty()) return 0;
    const std::wstring lowHay = ToLower(hay);
    const std::wstring lowNeedle = ToLower(needle);
    return lowHay.find(lowNeedle);
}

bool ContainsNoCase(std::wstring_view hay, std::wstring_view needle) {
    return FindNoCase(hay, needle) != std::wstring::npos;
}

std::wstring ReplaceAll(std::wstring_view s, std::wstring_view from, std::wstring_view to) {
    if (from.empty()) return std::wstring(s);
    std::wstring out;
    out.reserve(s.size());
    size_t pos = 0;
    while (pos < s.size()) {
        if (s.size() - pos >= from.size() && s.compare(pos, from.size(), from) == 0) {
            out.append(to);
            pos += from.size();
        } else {
            out.push_back(s[pos]);
            ++pos;
        }
    }
    return out;
}

std::vector<std::wstring> SplitLines(std::wstring_view s) {
    std::vector<std::wstring> lines;
    std::wstring current;
    size_t i = 0;
    while (i < s.size()) {
        const wchar_t c = s[i];
        if (c == L'\r') {
            lines.push_back(current);
            current.clear();
            if (i + 1 < s.size() && s[i + 1] == L'\n') ++i;
            ++i;
        } else if (c == L'\n') {
            lines.push_back(current);
            current.clear();
            ++i;
        } else {
            current.push_back(c);
            ++i;
        }
    }
    lines.push_back(current);
    return lines;
}

std::vector<std::wstring> Split(std::wstring_view s, wchar_t sep) {
    std::vector<std::wstring> parts;
    std::wstring current;
    for (const wchar_t c : s) {
        if (c == sep) {
            parts.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    parts.push_back(current);
    return parts;
}

std::wstring Join(const std::vector<std::wstring>& parts, std::wstring_view sep) {
    std::wstring out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) out.append(sep);
        out.append(parts[i]);
    }
    return out;
}

std::wstring CollapseSpaces(std::wstring_view s) {
    std::wstring out;
    out.reserve(s.size());
    bool pendingSpace = false;
    for (const wchar_t c : s) {
        if (c == L'\r' || c == L'\n') {
            pendingSpace = false;
            out.push_back(c);
            continue;
        }
        if (c == L' ' || c == L'\t' || c == 0x00A0) {
            pendingSpace = !out.empty();
            continue;
        }
        if (pendingSpace) {
            out.push_back(L' ');
            pendingSpace = false;
        }
        out.push_back(c);
    }
    return out;
}

std::wstring StripControl(std::wstring_view s) {
    std::wstring out;
    out.reserve(s.size());
    for (const wchar_t c : s) {
        const bool isControl = (c < 0x20 && c != L'\r' && c != L'\n' && c != L'\t') || c == 0x7F;
        if (!isControl) out.push_back(c);
    }
    return out;
}

std::wstring TruncateEllipsis(std::wstring_view s, size_t maxChars) {
    if (s.size() <= maxChars) return std::wstring(s);
    if (maxChars <= 1) return std::wstring(L"\u2026");
    return std::wstring(s.substr(0, maxChars - 1)) + L"\u2026";
}

std::wstring FormatV(const wchar_t* fmt, va_list args) {
    va_list probe;
    va_copy(probe, args);
    const int needed = _vscwprintf(fmt, probe);
    va_end(probe);
    if (needed <= 0) return std::wstring();

    std::wstring out(static_cast<size_t>(needed) + 1, L'\0');
    const int written = _vsnwprintf_s(out.data(), out.size(), _TRUNCATE, fmt, args);
    if (written < 0) return std::wstring();
    out.resize(static_cast<size_t>(written));
    return out;
}

std::wstring Format(const wchar_t* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::wstring out = FormatV(fmt, args);
    va_end(args);
    return out;
}

std::wstring HexDigits(unsigned long long value, size_t minDigits) {
    static const wchar_t* kDigits = L"0123456789abcdef";
    std::wstring out;
    if (value == 0) {
        out.assign(minDigits > 0 ? minDigits : 1, L'0');
        return out;
    }
    while (value != 0) {
        out.push_back(kDigits[value & 0xF]);
        value >>= 4;
    }
    while (out.size() < minDigits) out.push_back(L'0');
    std::reverse(out.begin(), out.end());
    return out;
}

size_t DisplayLength(std::wstring_view s) {
    size_t count = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        const wchar_t c = s[i];
        if (c >= 0xD800 && c <= 0xDBFF) {
            if (i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) ++i;
        }
        ++count;
    }
    return count;
}

} // namespace edgedock::text
