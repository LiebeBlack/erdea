// EdgeDock Studio :: modules/TextProcessor.cpp
#include "modules/TextProcessor.h"

#include "core/TextConv.h"

#include <algorithm>
#include <cwctype>
#include <memory>
#include <mutex>
#include <regex>
#include <set>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

namespace edgedock::modules {
namespace {

// --- Patrones (ECMAScript) ------------------------------------------------------
const wchar_t* const kUrlPattern =
    LR"((?:https?|ftp|file)://[^\s<>"']+|www\.[^\s<>"']+)";
const wchar_t* const kEmailPattern =
    LR"([A-Za-z0-9._%+\-]+@[A-Za-z0-9.\-]+\.[A-Za-z]{2,})";
const wchar_t* const kIpPattern =
    LR"(\b(?:\d{1,3}\.){3}\d{1,3}\b)";
const wchar_t* const kPhonePattern =
    LR"(\+?\d[\d\s\-().]{6,}\d)";
const wchar_t* const kPathPattern =
    LR"([A-Za-z]:\\[^<>:"|?*\r\n]+|\\\\[A-Za-z0-9_.\-]+\\[^<>:"|?*\r\n]+)";
const wchar_t* const kHashtagPattern =
    LR"(#[0-9A-Za-z_\u00C0-\u024F]{2,})";
const wchar_t* const kNumberPattern =
    LR"(\b\d+(?:[.,]\d+)?\b)";
const wchar_t* const kHtmlTagPattern =
    LR"(<[^>]{1,240}>)";
const wchar_t* const kMarkdownLinkPattern =
    LR"(\[([^\]]*)\]\(([^)]*)\))";

std::wstring RemoveTrailingPunctuation(std::wstring value) {
    static const wchar_t* kTrailing = L".,;:!?)]}\"'";
    while (!value.empty()) {
        const wchar_t last = value.back();
        bool strip = false;
        for (const wchar_t* p = kTrailing; *p != L'\0'; ++p) {
            if (last == *p) {
                strip = true;
                break;
            }
        }
        if (!strip) break;
        value.pop_back();
    }
    return value;
}

int DigitCount(std::wstring_view value) {
    int count = 0;
    for (const wchar_t c : value) {
        if (c >= L'0' && c <= L'9') ++count;
    }
    return count;
}

bool HasWhitespace(std::wstring_view value) {
    for (const wchar_t c : value) {
        if (text::IsSpace(c)) return true;
    }
    return false;
}

std::wstring CollapseInner(std::wstring_view line, int* counter) {
    std::wstring out;
    out.reserve(line.size());
    int run = 0;
    for (const wchar_t c : line) {
        if (c == L' ' || c == L'\t') {
            ++run;
            continue;
        }
        if (run > 1 && counter != nullptr) ++(*counter);
        if (run > 0) out.push_back(L' ');
        run = 0;
        out.push_back(c);
    }
    return out;
}

std::wstring LeadingWhitespace(std::wstring_view line) {
    size_t i = 0;
    while (i < line.size() && (line[i] == L' ' || line[i] == L'\t')) ++i;
    return std::wstring(line.substr(0, i));
}

bool EndsWithSoftPunctuation(wchar_t c) {
    return c == L',' || c == L';' || c == L':' || c == L'/' || c == L'-' || c == L'\u2014' || c == L'\u2013';
}

bool IsOnlyDigits(std::wstring_view value) {
    if (value.empty()) return false;
    for (const wchar_t c : value) {
        if (c < L'0' || c > L'9') return false;
    }
    return true;
}

bool IsRomanNumeralLine(std::wstring_view value) {
    if (value.empty() || value.size() > 7) return false;
    const std::wstring upper = text::ToUpper(value);
    for (const wchar_t c : upper) {
        if (c != L'I' && c != L'V' && c != L'X' && c != L'L' && c != L'C' && c != L'D' && c != L'M') {
            return false;
        }
    }
    return true;
}

bool IsPageNumberLine(const std::wstring& line) {
    const std::wstring trimmed = text::Trim(line);
    if (trimmed.empty()) return false;
    if (IsOnlyDigits(trimmed) && trimmed.size() <= 5) return true;

    // "- 12 -", "— 12 —", "[12]"
    std::wstring core = trimmed;
    const wchar_t edges[] = {L'-', L'\u2013', L'\u2014', L'[', L'('};
    bool stripped = false;
    for (const wchar_t edge : edges) {
        if (!core.empty() && core.front() == edge) {
            core = text::Trim(core.substr(1));
            stripped = true;
        }
    }
    if (!core.empty() && (core.back() == L'-' || core.back() == L'\u2013' || core.back() == L'\u2014' ||
                          core.back() == L']' || core.back() == L')')) {
        core = text::Trim(core.substr(0, core.size() - 1));
        stripped = true;
    }
    if (stripped && IsOnlyDigits(core) && core.size() <= 5) return true;

    const std::wstring lower = text::ToLower(trimmed);
    if (text::StartsWith(lower, L"página ") || text::StartsWith(lower, L"pagina ") ||
        text::StartsWith(lower, L"pág. ") || text::StartsWith(lower, L"pag. ")) {
        return true;
    }
    if (text::StartsWith(lower, L"page ") && lower.find(L' ') != std::wstring::npos) {
        return true;
    }
    return IsRomanNumeralLine(trimmed);
}

bool IsBulletLine(const std::wstring& line) {
    const std::wstring trimmed = text::Trim(line);
    if (trimmed.empty()) return false;
    const wchar_t* kBullets[] = {L"- ", L"* ", L"+ ", L"•", L"·", L"○", L"●", L"▪", L"▫", L"→", L"»", L"\u2023"};
    for (const wchar_t* bullet : kBullets) {
        if (text::StartsWith(trimmed, bullet)) return true;
    }
    // Listas numeradas: "1. ", "1) ", "(1) ", "1.2 ", "a) ".
    size_t i = 0;
    if (trimmed[0] == L'(') ++i;
    size_t digits = 0;
    while (i < trimmed.size() && text::IsDigit(trimmed[i])) {
        ++i;
        ++digits;
    }
    if (digits > 0 && i < trimmed.size()) {
        const wchar_t next = trimmed[i];
        if ((next == L'.' || next == L')' || next == L'\u00BA') && i + 1 < trimmed.size() && trimmed[i + 1] == L' ') {
            return true;
        }
    }
    return false;
}

bool IsHeadingLine(const std::wstring& line) {
    const std::wstring trimmed = text::Trim(line);
    if (trimmed.empty() || trimmed.size() > 72) return false;
    if (text::StartsWith(trimmed, L"#")) return true;
    if (!LooksLikeSentenceEnd(trimmed.back()) && trimmed.size() >= 3) {
        int letters = 0;
        int uppercase = 0;
        for (const wchar_t c : trimmed) {
            if (text::IsAlpha(c)) {
                ++letters;
                if (::iswupper(static_cast<wint_t>(c)) != 0) ++uppercase;
            }
        }
        if (letters >= 3 && uppercase == letters) return true;
        if (IsRomanNumeralLine(trimmed) && trimmed.size() <= 5) return false;
    }
    return false;
}

bool StartsWithLowercase(const std::wstring& line) {
    for (const wchar_t c : line) {
        if (!text::IsAlpha(c)) continue;
        return ::iswlower(static_cast<wint_t>(c)) != 0;
    }
    return false;
}

template <typename T>
std::vector<T> FindAll(const std::wstring& text, const wchar_t* pattern, bool unique) {
    std::vector<T> results;
    if (text.empty()) return results;
    static std::unordered_map<std::wstring, std::shared_ptr<std::wregex>> cache;
    static std::mutex cacheMutex;

    std::shared_ptr<std::wregex> regex;
    {
        std::lock_guard<std::mutex> guard(cacheMutex);
        const auto it = cache.find(pattern);
        if (it != cache.end()) {
            regex = it->second;
        } else {
            try {
                regex = std::make_shared<std::wregex>(pattern,
                                                     std::regex::ECMAScript | std::regex::optimize);
            } catch (const std::regex_error&) {
                regex = std::make_shared<std::wregex>(L"(?!)");   // patrón inválido: nunca casa
            }
            cache.emplace(pattern, regex);
        }
    }

    std::unordered_set<std::wstring> seen;
    for (std::wsregex_iterator it(text.begin(), text.end(), *regex), end; it != end; ++it) {
        std::wstring match = (*it)[0].str();
        if constexpr (std::is_same_v<T, std::wstring>) {
            // Recorte de puntuación final típica de copias de PDF.
            match = RemoveTrailingPunctuation(match);
        } else {
            match = text::Trim(match);
        }
        if (match.empty()) continue;
        if (unique) {
            const std::wstring key = text::ToLower(match);
            if (!seen.insert(key).second) continue;
        }
        if constexpr (std::is_same_v<T, std::wstring>) {
            results.push_back(std::move(match));
        } else {
            results.push_back(match);
        }
    }
    return results;
}

bool IsValidIp(const std::wstring& value) {
    int octet = 0;
    int parts = 0;
    for (const wchar_t c : value) {
        if (c == L'.') {
            if (octet > 255) return false;
            octet = 0;
            ++parts;
        } else if (c >= L'0' && c <= L'9') {
            octet = octet * 10 + (c - L'0');
        } else {
            return false;
        }
    }
    return parts == 3 && octet <= 255;
}

bool IsPlausiblePhone(const std::wstring& value) {
    const int digits = DigitCount(value);
    if (digits < 7 || digits > 15) return false;
    // Descarta rangos de fechas y números decimales con separadores de miles.
    if (value.find(L'.') != std::wstring::npos && value.find(L',') != std::wstring::npos) return false;
    return true;
}

std::vector<std::wstring> SplitWordsRaw(std::wstring_view text) {
    std::vector<std::wstring> words;
    std::wstring current;
    const size_t size = text.size();

    auto flush = [&]() {
        if (!current.empty()) {
            words.push_back(current);
            current.clear();
        }
    };

    for (size_t i = 0; i < size; ++i) {
        const wchar_t c = text[i];
        const bool separator = c == L' ' || c == L'\t' || c == L'\n' || c == L'\r' || c == L'_' || c == L'-' ||
                               c == L'.' || c == L'/' || c == L'\\' || c == L'+' || c == L'@' || c == L':' ||
                               c == L',' || c == L';' || c == L'|' || c == L'#' || c == L'\u00A0';
        if (separator) {
            flush();
            continue;
        }
        const bool isUpper = ::iswupper(static_cast<wint_t>(c)) != 0;
        const bool isLower = ::iswlower(static_cast<wint_t>(c)) != 0;
        const bool isDigit = c >= L'0' && c <= L'9';

        if (!current.empty()) {
            const wchar_t prev = current.back();
            const bool prevUpper = ::iswupper(static_cast<wint_t>(prev)) != 0;
            const bool prevLower = ::iswlower(static_cast<wint_t>(prev)) != 0;
            const bool prevDigit = prev >= L'0' && prev <= L'9';

            // camelCase / PascalCase: frontera minúscula->mayúscula.
            bool boundary = isUpper && prevLower;
            // Siglas: "HTTPServer" -> "HTTP" + "Server".
            if (!boundary && isUpper && prevUpper && i + 1 < size) {
                const wchar_t next = text[i + 1];
                boundary = ::iswlower(static_cast<wint_t>(next)) != 0;
            }
            // Frontera letra<->dígito.
            if (!boundary && ((isDigit && (prevLower || prevUpper)) || ((isLower || isUpper) && prevDigit))) {
                boundary = true;
            }
            if (boundary) flush();
        }
        current.push_back(c);
    }
    flush();
    return words;
}

std::wstring CapitalizeWord(const std::wstring& word) {
    if (word.empty()) return word;
    std::wstring out = text::ToLower(word);
    if (!out.empty()) {
        out[0] = static_cast<wchar_t>(::towupper(static_cast<wint_t>(out[0])));
    }
    return out;
}

std::wstring NormalizeQuotesImpl(std::wstring_view input) {
    std::wstring out;
    out.reserve(input.size());
    for (const wchar_t c : input) {
        switch (c) {
            case L'\u201C': case L'\u201D': case L'\u201E': case L'\u201F':
            case L'\u00AB': case L'\u00BB': case L'\u2033':
                out.push_back(L'"');
                break;
            case L'\u2018': case L'\u2019': case L'\u201A': case L'\u201B':
            case L'\u2032':
                out.push_back(L'\'');
                break;
            case L'\u00A0': case L'\u2007': case L'\u202F':
                out.push_back(L' ');
                break;
            case L'\u2010': case L'\u2011':
                out.push_back(L'-');
                break;
            case L'\u2013':
                out.push_back(L'-');
                break;
            case L'\u2014':
                out.push_back(L'-');
                break;
            case L'\u00AD': case L'\u200B': case L'\uFEFF':
                break;   // guion suave, ancho cero y BOM: fuera
            default:
                out.push_back(c);
        }
    }
    return out;
}

std::wstring DecodeHtmlEntities(std::wstring text) {
    struct Entity { const wchar_t* from; const wchar_t* to; };
    static const Entity kEntities[] = {
        {L"&nbsp;", L" "}, {L"&amp;", L"&"}, {L"&lt;", L"<"}, {L"&gt;", L">"},
        {L"&quot;", L"\""}, {L"&#39;", L"'"}, {L"&apos;", L"'"},
        {L"&mdash;", L"-"}, {L"&ndash;", L"-"}, {L"&hellip;", L"..."}, {L"&middot;", L"\u00B7"},
    };
    for (const auto& entity : kEntities) {
        text = text::ReplaceAll(text, entity.from, entity.to);
    }
    return text;
}

} // namespace

// -----------------------------------------------------------------------------
// Extracción
// -----------------------------------------------------------------------------

std::vector<std::wstring> FindMatches(std::wstring_view text, const std::wstring& pattern) {
    return FindAll<std::wstring>(std::wstring(text), pattern.c_str(), true);
}

std::vector<std::wstring> ExtractUrls(std::wstring_view text) {
    return FindAll<std::wstring>(std::wstring(text), kUrlPattern, true);
}

std::vector<std::wstring> ExtractEmails(std::wstring_view text) {
    return FindAll<std::wstring>(std::wstring(text), kEmailPattern, true);
}

std::vector<std::wstring> ExtractIpAddresses(std::wstring_view text) {
    const auto raw = FindAll<std::wstring>(std::wstring(text), kIpPattern, true);
    std::vector<std::wstring> out;
    for (const auto& value : raw) {
        if (IsValidIp(value)) out.push_back(value);
    }
    return out;
}

std::vector<std::wstring> ExtractPhoneNumbers(std::wstring_view text) {
    const auto raw = FindAll<std::wstring>(std::wstring(text), kPhonePattern, true);
    std::vector<std::wstring> out;
    for (const auto& value : raw) {
        if (IsPlausiblePhone(value)) out.push_back(text::Trim(value));
    }
    return out;
}

std::vector<std::wstring> ExtractWindowsPaths(std::wstring_view text) {
    const auto raw = FindAll<std::wstring>(std::wstring(text), kPathPattern, true);
    std::vector<std::wstring> out;
    for (const auto& value : raw) {
        const std::wstring trimmed = text::Trim(value);
        if (trimmed.size() >= 4) out.push_back(trimmed);
    }
    return out;
}

std::vector<std::wstring> ExtractHashtags(std::wstring_view text) {
    return FindAll<std::wstring>(std::wstring(text), kHashtagPattern, true);
}

std::vector<std::wstring> ExtractNumbers(std::wstring_view text) {
    return FindAll<std::wstring>(std::wstring(text), kNumberPattern, true);
}

bool LooksLikeSentenceEnd(wchar_t last) {
    const wchar_t* kEnders = L".!?\u2026:\"')\u201D\u2019]}\u00BB";
    for (const wchar_t* p = kEnders; *p != L'\0'; ++p) {
        if (last == *p) return true;
    }
    return false;
}

bool LooksLikeCodeLine(std::wstring_view line) {
    const std::wstring trimmed = text::Trim(line);
    if (trimmed.empty()) return false;
    const std::wstring lower = text::ToLower(trimmed);

    static const wchar_t* kPrefixes[] = {
        L"#include", L"#define", L"#!", L"import ", L"from ", L"using ", L"package ", L"def ", L"class ",
        L"function ", L"public ", L"private ", L"protected ", L"static ", L"const ", L"let ", L"var ",
        L"return ", L"if (", L"if(", L"for (", L"for(", L"while (", L"while(", L"switch (", L"catch (",
        L"try {", L"else {", L"}", L") {", L"};", L"</", L"<!--", L"//", L"/*", L"* ", L"-->",
        L"select ", L"insert ", L"update ", L"create table", L"curl ", L"sudo ", L"docker ", L"git ",
    };
    for (const wchar_t* prefix : kPrefixes) {
        if (text::StartsWith(lower, prefix)) return true;
    }
    if (trimmed.back() == L';' || trimmed.back() == L'{' || trimmed.back() == L'}') return true;
    if ((trimmed.front() == L'}' || trimmed.front() == L')') && trimmed.size() <= 3) return true;

    static const wchar_t* kMarkers[] = {L"=>", L"->", L"::", L":=", L"</", L"/>", L"&&", L"||", L"!=",
                                        L"==", L"++", L"--", L"${", L"</div>", L"</p>", L"<div", L"<span"};
    for (const wchar_t* marker : kMarkers) {
        if (text::Contains(trimmed, marker)) return true;
    }
    return false;
}

std::vector<std::wstring> ExtractCodeBlocks(std::wstring_view text) {
    std::vector<std::wstring> blocks;
    const std::vector<std::wstring> lines = text::SplitLines(text);
    if (lines.empty()) return blocks;

    std::vector<bool> used(lines.size(), false);

    // 1) Bloques delimitados explícitamente con vallas ```.
    for (size_t i = 0; i < lines.size(); ++i) {
        if (!text::StartsWith(text::Trim(lines[i]), L"```")) continue;
        std::vector<std::wstring> body;
        size_t j = i + 1;
        for (; j < lines.size(); ++j) {
            used[j] = true;
            if (text::StartsWith(text::Trim(lines[j]), L"```")) break;
            body.push_back(lines[j]);
        }
        used[i] = true;
        if (j < lines.size()) used[j] = true;
        while (!body.empty() && text::Trim(body.front()).empty()) body.erase(body.begin());
        while (!body.empty() && text::Trim(body.back()).empty()) body.pop_back();
        if (!body.empty()) blocks.push_back(text::Join(body, L"\n"));
        i = j;
    }

    // 2) Bloques heurísticos: dos o más líneas consecutivas con sintaxis de código.
    std::vector<std::wstring> group;
    size_t start = 0;
    auto flushGroup = [&]() {
        if (group.size() >= 2) blocks.push_back(text::Join(group, L"\n"));
        group.clear();
    };
    for (size_t i = 0; i < lines.size(); ++i) {
        if (used[i]) {
            flushGroup();
            continue;
        }
        const std::wstring raw = lines[i];
        const bool indented = raw.size() >= static_cast<size_t>(4) &&
                              (raw[0] == L' ' || raw[0] == L'\t');
        if (LooksLikeCodeLine(raw) || (indented && !text::Trim(raw).empty())) {
            if (group.empty()) start = i;
            group.push_back(raw);
        } else {
            flushGroup();
        }
    }
    flushGroup();
    (void)start;
    return blocks;
}

std::vector<std::wstring> Deduplicate(const std::vector<std::wstring>& values) {
    std::vector<std::wstring> out;
    std::unordered_set<std::wstring> seen;
    out.reserve(values.size());
    for (const auto& value : values) {
        if (seen.insert(text::ToLower(text::Trim(value))).second) out.push_back(value);
    }
    return out;
}

std::wstring JoinLines(const std::vector<std::wstring>& values) {
    return text::Join(values, L"\n");
}

size_t ExtractionResult::TotalFound() const {
    return urls.size() + emails.size() + codeBlocks.size() + ipAddresses.size() + phoneNumbers.size() +
           paths.size() + hashtags.size() + numbers.size();
}

bool ExtractionResult::Empty() const {
    return TotalFound() == 0;
}

std::wstring ExtractionResult::Summary() const {
    std::vector<std::wstring> parts;
    if (!urls.empty()) parts.push_back(text::Format(L"%llu URL", static_cast<unsigned long long>(urls.size())));
    if (!emails.empty()) parts.push_back(text::Format(L"%llu MAIL", static_cast<unsigned long long>(emails.size())));
    if (!codeBlocks.empty()) parts.push_back(text::Format(L"%llu CODE", static_cast<unsigned long long>(codeBlocks.size())));
    if (!ipAddresses.empty()) parts.push_back(text::Format(L"%llu IP", static_cast<unsigned long long>(ipAddresses.size())));
    if (!phoneNumbers.empty()) parts.push_back(text::Format(L"%llu TEL", static_cast<unsigned long long>(phoneNumbers.size())));
    if (!paths.empty()) parts.push_back(text::Format(L"%llu PATH", static_cast<unsigned long long>(paths.size())));
    if (!hashtags.empty()) parts.push_back(text::Format(L"%llu TAG", static_cast<unsigned long long>(hashtags.size())));
    if (!numbers.empty()) parts.push_back(text::Format(L"%llu NUM", static_cast<unsigned long long>(numbers.size())));
    if (parts.empty()) return L"sin coincidencias";
    return text::Join(parts, L" \u00B7 ");
}

ExtractionResult Extract(const std::wstring& text) {
    ExtractionResult result;
    result.urls = ExtractUrls(text);
    result.emails = ExtractEmails(text);
    result.codeBlocks = ExtractCodeBlocks(text);
    result.ipAddresses = ExtractIpAddresses(text);
    result.phoneNumbers = ExtractPhoneNumbers(text);
    result.paths = ExtractWindowsPaths(text);
    result.hashtags = ExtractHashtags(text);
    result.numbers = ExtractNumbers(text);

    // Las URLs también aparecen dentro de los emails; se eliminan duplicados cruzados.
    std::unordered_set<std::wstring> urlKeys;
    for (const auto& url : result.urls) urlKeys.insert(text::ToLower(url));
    for (const auto& path : result.paths) {
        if (urlKeys.insert(text::ToLower(path)).second) result.urls.push_back(path);
    }
    return result;
}

// -----------------------------------------------------------------------------
// Limpieza
// -----------------------------------------------------------------------------

std::wstring CleanText(const std::wstring& input, const CleanOptions& options, CleanStats* stats) {
    CleanStats local{};
    local.charsBefore = input.size();
    if (stats == nullptr) stats = &local;

    std::wstring work = input;
    if (options.normalizeQuotes) work = NormalizeQuotesImpl(work);
    work = text::ReplaceAll(work, L"\r\n", L"\n");
    work = text::ReplaceAll(work, L"\r", L"\n");
    if (options.stripHtml) work = StripHtmlTags(work);

    std::vector<std::wstring> rawLines = text::SplitLines(work);
    local.linesBefore = static_cast<int>(rawLines.size());

    // Cabeceras/pies repetidos: líneas cortas que aparecen 3 o más veces.
    std::unordered_set<std::wstring> repeated;
    if (options.removeRepeatedHeader) {
        std::unordered_map<std::wstring, int> counts;
        for (const auto& line : rawLines) {
            const std::wstring trimmed = text::Trim(line);
            if (trimmed.empty() || trimmed.size() > 80) continue;
            if (trimmed.find(L"http") != std::wstring::npos) continue;
            ++counts[trimmed];
        }
        for (const auto& entry : counts) {
            if (entry.second >= 3 && !LooksLikeSentenceEnd(entry.first.back()) && !IsBulletLine(entry.first)) {
                repeated.insert(entry.first);
            }
        }
    }

    std::vector<std::wstring> paragraphs;
    std::vector<std::wstring> current;
    int spaceCollapses = 0;

    auto isSkippable = [&](const std::wstring& trimmed) {
        if (options.removePageNumbers && IsPageNumberLine(trimmed)) {
            ++local.pageNumbersRemoved;
            return true;
        }
        if (options.removeRepeatedHeader && repeated.count(trimmed) != 0) {
            ++local.headersRemoved;
            return true;
        }
        return false;
    };

    auto flushParagraph = [&]() {
        if (current.empty()) return;
        paragraphs.push_back(text::Join(current, L"\n"));
        current.clear();
        ++local.paragraphs;
    };

    for (const std::wstring& rawLine : rawLines) {
        const std::wstring trimmed = text::Trim(rawLine);
        if (trimmed.empty()) {
            flushParagraph();
            continue;
        }
        if (isSkippable(trimmed)) continue;

        std::wstring line = CollapseInner(trimmed, &spaceCollapses);
        if (options.keepIndent) {
            const std::wstring indent = LeadingWhitespace(rawLine);
            if (!indent.empty()) line = indent + line;
        }
        if (options.forceLowercase) line = text::ToLower(line);

        const bool lineIsBullet = IsBulletLine(line);
        const bool lineIsHeading = IsHeadingLine(line);
        const bool lineIsCode = LooksLikeCodeLine(line);

        if (current.empty()) {
            current.push_back(line);
            continue;
        }

        const std::wstring& previous = current.back();
        const wchar_t previousLast = previous.empty() ? L'.' : previous.back();
        const bool previousIsBullet = IsBulletLine(previous);
        const bool previousIsCode = LooksLikeCodeLine(previous);
        const bool previousIsHeading = IsHeadingLine(previous);

        bool join = false;
        bool hyphenJoin = false;

        if (lineIsBullet || lineIsHeading || previousIsBullet || previousIsHeading) {
            join = false;   // las listas y los títulos siempre empiezan línea nueva
        } else if (options.fixHyphenation && (previousLast == L'-' || previousLast == L'\u2010' || previousLast == L'\u2011') &&
                   StartsWithLowercase(line)) {
            join = true;
            hyphenJoin = true;
        } else if (options.mergeBrokenLines && !lineIsCode && !previousIsCode) {
            const wchar_t firstChar = line.empty() ? L' ' : line[0];
            const bool nextStartsLower = ::iswlower(static_cast<wint_t>(firstChar)) != 0;
            if (EndsWithSoftPunctuation(previousLast)) {
                join = true;                       // coma/dos puntos: la frase continúa
            } else if (!LooksLikeSentenceEnd(previousLast)) {
                join = true;                       // línea sin cierre: corte de maquetación
            } else if (nextStartsLower) {
                // Terminó en punto pero la siguiente empieza en minúscula: PDF roto.
                join = true;
            }
        }

        if (join) {
            if (hyphenJoin) {
                current.back() = previous.substr(0, previous.size() - 1) + line;
                ++local.hyphenJoins;
            } else {
                current.back() = previous + L" " + line;
                ++local.linesJoined;
            }
        } else {
            current.push_back(line);
        }
    }
    flushParagraph();

    if (options.unwrapLists && !paragraphs.empty()) {
        for (auto& paragraph : paragraphs) {
            paragraph = text::ReplaceAll(paragraph, L"- ", L"");
            paragraph = text::ReplaceAll(paragraph, L"* ", L"");
        }
    }

    std::wstring result;
    if (paragraphs.empty()) {
        result.clear();
    } else if (options.keepParagraphs) {
        result = text::Join(paragraphs, L"\n\n");
    } else {
        result = text::Join(paragraphs, L" ");
        result = text::ReplaceAll(result, L"\n", L" ");
        while (text::Contains(result, L"  ")) result = text::ReplaceAll(result, L"  ", L" ");
    }

    local.spacesCollapsed = spaceCollapses;
    local.linesAfter = static_cast<int>(options.keepParagraphs
                                            ? std::count(result.begin(), result.end(), L'\n') + (result.empty() ? 0 : 1)
                                            : (result.empty() ? 0 : 1));
    local.charsAfter = result.size();
    local.emptyResult = text::Trim(result).empty();
    *stats = local;
    return result;
}

// -----------------------------------------------------------------------------
// Caja
// -----------------------------------------------------------------------------

std::vector<std::wstring> SplitWords(std::wstring_view text) {
    return SplitWordsRaw(text);
}

std::wstring ToCamelCase(std::wstring_view text) {
    const auto words = SplitWordsRaw(text);
    std::wstring out;
    for (size_t i = 0; i < words.size(); ++i) {
        const std::wstring word = words[i];
        if (i == 0) {
            out += text::ToLower(word);
        } else {
            out += CapitalizeWord(word);
        }
    }
    return out;
}

std::wstring ToPascalCase(std::wstring_view text) {
    const auto words = SplitWordsRaw(text);
    std::wstring out;
    for (const auto& word : words) out += CapitalizeWord(word);
    return out;
}

std::wstring ToSnakeCase(std::wstring_view text) {
    const auto words = SplitWordsRaw(text);
    std::vector<std::wstring> lowered;
    lowered.reserve(words.size());
    for (const auto& word : words) lowered.push_back(text::ToLower(word));
    return text::Join(lowered, L"_");
}

std::wstring ToKebabCase(std::wstring_view text) {
    const auto words = SplitWordsRaw(text);
    std::vector<std::wstring> lowered;
    lowered.reserve(words.size());
    for (const auto& word : words) lowered.push_back(text::ToLower(word));
    return text::Join(lowered, L"-");
}

std::wstring ToConstantCase(std::wstring_view text) {
    const auto words = SplitWordsRaw(text);
    std::vector<std::wstring> uppered;
    uppered.reserve(words.size());
    for (const auto& word : words) uppered.push_back(text::ToUpper(word));
    return text::Join(uppered, L"_");
}

std::wstring ToTitleCase(std::wstring_view text) {
    const auto words = SplitWordsRaw(text);
    std::vector<std::wstring> parts;
    parts.reserve(words.size());
    for (const auto& word : words) parts.push_back(CapitalizeWord(word));
    return text::Join(parts, L" ");
}

std::wstring ToSentenceCase(std::wstring_view text) {
    const std::wstring lower = text::ToLower(text::NormalizeWhitespace(text));
    std::wstring out;
    out.reserve(lower.size());
    bool startOfSentence = true;
    for (const wchar_t c : lower) {
        if (startOfSentence && text::IsAlpha(c)) {
            out.push_back(static_cast<wchar_t>(::towupper(static_cast<wint_t>(c))));
            startOfSentence = false;
        } else {
            out.push_back(c);
            if (c == L'.' || c == L'!' || c == L'?' || c == L'\n') startOfSentence = true;
        }
    }
    return out;
}

std::wstring ApplyCase(std::wstring_view text, CaseStyle style) {
    switch (style) {
        case CaseStyle::Camel: return ToCamelCase(text);
        case CaseStyle::Pascal: return ToPascalCase(text);
        case CaseStyle::Snake: return ToSnakeCase(text);
        case CaseStyle::Kebab: return ToKebabCase(text);
        case CaseStyle::Constant: return ToConstantCase(text);
        case CaseStyle::Upper: return text::ToUpper(text);
        case CaseStyle::Lower: return text::ToLower(text);
        case CaseStyle::Title: return ToTitleCase(text);
        case CaseStyle::Sentence: return ToSentenceCase(text);
    }
    return std::wstring(text);
}

const wchar_t* CaseStyleLabel(CaseStyle style) {
    switch (style) {
        case CaseStyle::Camel: return L"camelCase";
        case CaseStyle::Pascal: return L"PascalCase";
        case CaseStyle::Snake: return L"snake_case";
        case CaseStyle::Kebab: return L"kebab-case";
        case CaseStyle::Constant: return L"CONSTANT_CASE";
        case CaseStyle::Upper: return L"MAYÚSCULAS";
        case CaseStyle::Lower: return L"minúsculas";
        case CaseStyle::Title: return L"Título Propio";
        case CaseStyle::Sentence: return L"Frase normal";
    }
    return L"texto";
}

// -----------------------------------------------------------------------------
// Formato
// -----------------------------------------------------------------------------

std::wstring NormalizeQuotes(std::wstring_view input) {
    return NormalizeQuotesImpl(input);
}

std::wstring NormalizeWhitespace(std::wstring_view input) {
    std::wstring out;
    out.reserve(input.size());
    int spaces = 0;
    int newlines = 0;
    for (const wchar_t c : input) {
        if (c == L'\r') continue;
        if (c == L' ' || c == L'\t') {
            ++spaces;
            continue;
        }
        if (c == L'\n') {
            spaces = 0;
            if (++newlines > 2) continue;
            out.push_back(L'\n');
            continue;
        }
        if (spaces > 0 && !out.empty() && out.back() != L'\n') out.push_back(L' ');
        spaces = 0;
        newlines = 0;
        out.push_back(c);
    }
    return out;
}

std::wstring StripHtmlTags(std::wstring_view input) {
    const std::wstring text(input);
    std::wstring out = text;
    try {
        static const std::wregex tagRegex(kHtmlTagPattern);
        out = std::regex_replace(out, tagRegex, L"");
    } catch (const std::regex_error&) {
        // Sin regex no se puede filtrar: se devuelve el original en lugar de perder texto.
        return text;
    }
    out = DecodeHtmlEntities(out);
    static const wchar_t* kBlockTags[] = {L"</p>", L"</div>", L"</li>", L"<br>", L"<br/>", L"</tr>", L"</h1>",
                                          L"</h2>", L"</h3>"};
    for (const wchar_t* tag : kBlockTags) {
        out = text::ReplaceAll(out, tag, L"\n");
    }
    return out;
}

std::wstring StripFormatting(const std::wstring& input) {
    std::wstring out = input;
    try {
        static const std::wregex linkRegex(kMarkdownLinkPattern);
        out = std::regex_replace(out, linkRegex, L"$1 ($2)");
    } catch (const std::regex_error&) {
        // Se continúa con el texto sin transformar los enlaces.
    }

    static const wchar_t* kMarkers[] = {L"**", L"__", L"```", L"`", L"~~", L"#", L"> "};
    for (const wchar_t* marker : kMarkers) {
        out = text::ReplaceAll(out, marker, L"");
    }
    // Asteriscos y guiones bajos de énfasis sueltos.
    out = text::ReplaceAll(out, L"*", L"");
    out = text::ReplaceAll(out, L"_", L" ");
    out = text::ReplaceAll(out, L"  ", L" ");
    return text::Trim(out);
}

std::wstring ToSingleParagraph(const std::wstring& input, const CleanOptions& options) {
    CleanOptions opts = options;
    opts.keepParagraphs = false;
    CleanStats stats{};
    return CleanText(input, opts, &stats);
}

std::wstring ToCleanMarkdown(const std::wstring& input, const CleanOptions& options) {
    CleanOptions opts = options;
    opts.keepParagraphs = true;
    CleanStats stats{};
    const std::wstring cleaned = CleanText(input, opts, &stats);
    if (text::Trim(cleaned).empty()) return std::wstring();

    const std::vector<std::wstring> paragraphs = text::Split(cleaned, L'\n');
    std::vector<std::wstring> blocks;
    std::vector<std::wstring> buffer;
    bool inCode = false;

    auto flushBuffer = [&]() {
        if (buffer.empty()) return;
        blocks.push_back(text::Join(buffer, L"\n"));
        buffer.clear();
    };

    for (const std::wstring& line : paragraphs) {
        if (text::Trim(line).empty()) {
            flushBuffer();
            continue;
        }
        const bool codeLine = LooksLikeCodeLine(line);
        if (codeLine != inCode) {
            flushBuffer();
            if (codeLine) blocks.push_back(L"```");
            inCode = codeLine;
            if (!codeLine) blocks.push_back(L"```");
        }
        if (IsBulletLine(line)) {
            std::wstring content = text::Trim(line);
            while (!content.empty() && (content[0] == L'-' || content[0] == L'*' || content[0] == L'+' ||
                                        content[0] == L'\u2022' || content[0] == L'\u00B7')) {
                content = text::Trim(content.substr(1));
            }
            buffer.push_back(L"- " + content);
        } else if (IsHeadingLine(line)) {
            std::wstring content = text::Trim(line);
            while (!content.empty() && content[0] == L'#') content = text::Trim(content.substr(1));
            buffer.push_back(L"## " + content);
        } else {
            buffer.push_back(line);
        }
    }
    flushBuffer();
    if (inCode) blocks.push_back(L"```");
    return text::Join(blocks, L"\n\n");
}

std::wstring WrapForClipboard(std::wstring_view text, size_t maxLineChars) {
    if (maxLineChars < 20) maxLineChars = 20;
    std::vector<std::wstring> wrappedLines;
    for (const std::wstring& paragraph : text::SplitLines(text)) {
        if (paragraph.empty()) {
            wrappedLines.push_back(std::wstring());
            continue;
        }
        std::wstring current;
        for (const std::wstring& word : text::Split(paragraph, L' ')) {
            if (!current.empty() && current.size() + 1 + word.size() > maxLineChars) {
                wrappedLines.push_back(current);
                current.clear();
            }
            if (!current.empty()) current.push_back(L' ');
            current += word;
        }
        wrappedLines.push_back(current);
    }
    return text::Join(wrappedLines, L"\n");
}

} // namespace edgedock::modules
