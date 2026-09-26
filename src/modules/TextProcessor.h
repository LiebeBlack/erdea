#pragma once
// EdgeDock Studio :: modules/TextProcessor.h
// Motor de texto: limpiador de copias procedentes de PDF/web, extractor por expresiones
// regulares (URLs, emails, bloques de código...) y conversor de estilos de caja.

#include <string>
#include <string_view>
#include <vector>

namespace edgedock::modules {

// --- Limpieza ------------------------------------------------------------------
struct CleanOptions {
    bool mergeBrokenLines = true;    // Une líneas cortadas por el diseño del PDF
    bool fixHyphenation = true;      // "infor-\nmación" -> "información"
    bool collapseSpaces = true;      // Espacios/tabuladores múltiples -> uno
    bool keepParagraphs = true;      // Un párrafo real (línea vacía) se conserva
    bool removePageNumbers = true;   // Descarta "12", "- 12 -", "Página 12 de 40"
    bool removeRepeatedHeader = true;// Descarta cabeceras/pies repetidos
    bool normalizeQuotes = true;     // Comillas tipográficas -> rectas, NBSP -> espacio
    bool stripHtml = false;          // Elimina etiquetas HTML si el origen fue web
    bool unwrapLists = false;        // Fusiona los ítems de lista en un párrafo
    bool forceLowercase = false;
    bool keepIndent = false;         // Conserva la indentación inicial de cada línea
};

struct CleanStats {
    size_t charsBefore = 0;
    size_t charsAfter = 0;
    int linesBefore = 0;
    int linesAfter = 0;
    int linesJoined = 0;
    int hyphenJoins = 0;
    int spacesCollapsed = 0;
    int pageNumbersRemoved = 0;
    int headersRemoved = 0;
    int paragraphs = 0;
    bool emptyResult = false;
};

std::wstring CleanText(const std::wstring& input, const CleanOptions& options = {}, CleanStats* stats = nullptr);

// --- Extracción ----------------------------------------------------------------
struct ExtractionResult {
    std::vector<std::wstring> urls;
    std::vector<std::wstring> emails;
    std::vector<std::wstring> codeBlocks;
    std::vector<std::wstring> ipAddresses;
    std::vector<std::wstring> phoneNumbers;
    std::vector<std::wstring> paths;
    std::vector<std::wstring> hashtags;
    std::vector<std::wstring> quotedLines;
    std::vector<std::wstring> numbers;

    size_t TotalFound() const;
    bool Empty() const;
    std::wstring Summary() const;   // "3 URL · 1 MAIL · 2 CODE"
};

ExtractionResult Extract(const std::wstring& text);
std::vector<std::wstring> ExtractUrls(std::wstring_view text);
std::vector<std::wstring> ExtractEmails(std::wstring_view text);
std::vector<std::wstring> ExtractCodeBlocks(std::wstring_view text);
std::vector<std::wstring> ExtractIpAddresses(std::wstring_view text);
std::vector<std::wstring> ExtractPhoneNumbers(std::wstring_view text);
std::vector<std::wstring> ExtractWindowsPaths(std::wstring_view text);
std::vector<std::wstring> ExtractHashtags(std::wstring_view text);
std::vector<std::wstring> ExtractNumbers(std::wstring_view text);
std::vector<std::wstring> FindMatches(std::wstring_view text, const std::wstring& pattern);
std::vector<std::wstring> Deduplicate(const std::vector<std::wstring>& values);
std::wstring JoinLines(const std::vector<std::wstring>& values);
bool LooksLikeCodeLine(std::wstring_view line);
bool LooksLikeSentenceEnd(wchar_t last);

// --- Caja ----------------------------------------------------------------------
enum class CaseStyle { Camel, Pascal, Snake, Kebab, Constant, Upper, Lower, Title, Sentence };

std::wstring ToCamelCase(std::wstring_view text);
std::wstring ToPascalCase(std::wstring_view text);
std::wstring ToSnakeCase(std::wstring_view text);
std::wstring ToKebabCase(std::wstring_view text);
std::wstring ToConstantCase(std::wstring_view text);
std::wstring ToTitleCase(std::wstring_view text);
std::wstring ToSentenceCase(std::wstring_view text);
std::wstring ApplyCase(std::wstring_view text, CaseStyle style);
const wchar_t* CaseStyleLabel(CaseStyle style);
std::vector<std::wstring> SplitWords(std::wstring_view text);

// --- Formato -------------------------------------------------------------------
std::wstring ToCleanMarkdown(const std::wstring& input, const CleanOptions& options = {});
std::wstring ToSingleParagraph(const std::wstring& input, const CleanOptions& options = {});
std::wstring StripFormatting(const std::wstring& input);
std::wstring StripHtmlTags(std::wstring_view input);
std::wstring NormalizeQuotes(std::wstring_view input);
std::wstring NormalizeWhitespace(std::wstring_view input);
std::wstring WrapForClipboard(std::wstring_view text, size_t maxLineChars = 100);

} // namespace edgedock::modules
