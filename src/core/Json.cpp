// EdgeDock Studio :: core/Json.cpp
#include "core/Json.h"

#include "core/TextConv.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace edgedock::json {
namespace {

const Value kNullValue;

std::string NumberToString(double value) {
    if (std::isnan(value) || std::isinf(value)) return "0";
    if (value == static_cast<double>(static_cast<int64_t>(value)) && std::fabs(value) < 1e15) {
        char buffer[32] = {};
        std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(value));
        return std::string(buffer);
    }
    char buffer[64] = {};
    std::snprintf(buffer, sizeof(buffer), "%.17g", value);
    return std::string(buffer);
}

void AppendUtf8Codepoint(std::string& out, unsigned int cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

class Parser {
public:
    Parser(const std::string& text) : text_(text) {}

    bool ParseDocument(Value& out, std::string& error) {
        SkipWhitespace();
        if (!ParseValue(out)) {
            error = error_.empty() ? std::string("JSON inválido") : error_;
            return false;
        }
        SkipWhitespace();
        if (pos_ != text_.size()) {
            error = "contenido extra tras el valor JSON";
            return false;
        }
        return true;
    }

private:
    bool Fail(const char* message, size_t at) {
        char buffer[160] = {};
        std::snprintf(buffer, sizeof(buffer), "JSON: %s (offset %llu)", message,
                      static_cast<unsigned long long>(at));
        error_ = buffer;
        return false;
    }

    void SkipWhitespace() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    bool Match(char expected) {
        if (pos_ < text_.size() && text_[pos_] == expected) {
            ++pos_;
            return true;
        }
        return false;
    }

    bool ParseValue(Value& out) {
        SkipWhitespace();
        if (pos_ >= text_.size()) return Fail("fin de entrada inesperado", pos_);
        const char c = text_[pos_];
        switch (c) {
            case '{': return ParseObject(out);
            case '[': return ParseArray(out);
            case '"': {
                std::string s;
                if (!ParseString(s)) return false;
                out = Value(std::move(s));
                return true;
            }
            case 't':
                if (text_.compare(pos_, 4, "true") == 0) {
                    pos_ += 4;
                    out = Value(true);
                    return true;
                }
                return Fail("literal inválido", pos_);
            case 'f':
                if (text_.compare(pos_, 5, "false") == 0) {
                    pos_ += 5;
                    out = Value(false);
                    return true;
                }
                return Fail("literal inválido", pos_);
            case 'n':
                if (text_.compare(pos_, 4, "null") == 0) {
                    pos_ += 4;
                    out = Value();
                    return true;
                }
                return Fail("literal inválido", pos_);
            default:
                return ParseNumber(out);
        }
    }

    bool ParseNumber(Value& out) {
        const size_t start = pos_;
        if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) ++pos_;
        bool anyDigit = false;
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
                if (c >= '0' && c <= '9') anyDigit = true;
                ++pos_;
            } else {
                break;
            }
        }
        if (!anyDigit) return Fail("número inválido", start);
        const std::string token = text_.substr(start, pos_ - start);
        char* end = nullptr;
        const double value = std::strtod(token.c_str(), &end);
        if (end == nullptr || *end != '\0') return Fail("número inválido", start);
        out = Value(value);
        return true;
    }

    bool ParseString(std::string& out) {
        if (!Match('"')) return Fail("se esperaba una cadena", pos_);
        out.clear();
        while (pos_ < text_.size()) {
            const unsigned char c = static_cast<unsigned char>(text_[pos_++]);
            if (c == '"') return true;
            if (c == '\\') {
                if (pos_ >= text_.size()) return Fail("escape truncado", pos_);
                const char esc = text_[pos_++];
                switch (esc) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        unsigned int cp = 0;
                        if (pos_ + 4 > text_.size()) return Fail("escape \\u truncado", pos_);
                        for (int i = 0; i < 4; ++i) {
                            const char hex = text_[pos_++];
                            unsigned int digit = 0;
                            if (hex >= '0' && hex <= '9') {
                                digit = static_cast<unsigned int>(hex - '0');
                            } else if (hex >= 'a' && hex <= 'f') {
                                digit = static_cast<unsigned int>(hex - 'a' + 10);
                            } else if (hex >= 'A' && hex <= 'F') {
                                digit = static_cast<unsigned int>(hex - 'A' + 10);
                            } else {
                                return Fail("dígito hexadecimal inválido", pos_ - 1);
                            }
                            cp = (cp << 4) | digit;
                        }
                        // Par subrogado -> punto de código completo.
                        if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 6 <= text_.size() &&
                            text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
                            unsigned int low = 0;
                            size_t probe = pos_ + 2;
                            bool ok = true;
                            for (int i = 0; i < 4 && ok; ++i) {
                                const char hex = text_[probe + static_cast<size_t>(i)];
                                unsigned int digit = 0;
                                if (hex >= '0' && hex <= '9') {
                                    digit = static_cast<unsigned int>(hex - '0');
                                } else if (hex >= 'a' && hex <= 'f') {
                                    digit = static_cast<unsigned int>(hex - 'a' + 10);
                                } else if (hex >= 'A' && hex <= 'F') {
                                    digit = static_cast<unsigned int>(hex - 'A' + 10);
                                } else {
                                    ok = false;
                                }
                                low = (low << 4) | digit;
                            }
                            if (ok && low >= 0xDC00 && low <= 0xDFFF) {
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                                pos_ += 6;
                            }
                        }
                        AppendUtf8Codepoint(out, cp);
                        break;
                    }
                    default:
                        return Fail("escape desconocido", pos_ - 1);
                }
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
        return Fail("cadena sin cerrar", pos_);
    }

    bool ParseArray(Value& out) {
        Match('[');
        out = Value::MakeArray();
        SkipWhitespace();
        if (Match(']')) return true;
        while (true) {
            Value item;
            if (!ParseValue(item)) return false;
            out.Push(std::move(item));
            SkipWhitespace();
            if (Match(',')) {
                SkipWhitespace();
                continue;
            }
            if (Match(']')) return true;
            return Fail("se esperaba ',' o ']'", pos_);
        }
    }

    bool ParseObject(Value& out) {
        Match('{');
        out = Value::MakeObject();
        SkipWhitespace();
        if (Match('}')) return true;
        while (true) {
            SkipWhitespace();
            std::string key;
            if (!ParseString(key)) return false;
            SkipWhitespace();
            if (!Match(':')) return Fail("se esperaba ':'", pos_);
            Value item;
            if (!ParseValue(item)) return false;
            out.Set(key, std::move(item));
            SkipWhitespace();
            if (Match(',')) continue;
            if (Match('}')) return true;
            return Fail("se esperaba ',' o '}'", pos_);
        }
    }

    const std::string& text_;
    size_t pos_ = 0;
    std::string error_;
};

} // namespace

Value Value::MakeArray() {
    Value v;
    v.type_ = Type::Array;
    return v;
}

Value Value::MakeObject() {
    Value v;
    v.type_ = Type::Object;
    return v;
}

bool Value::BoolOr(bool fallback) const { return type_ == Type::Bool ? bool_ : fallback; }

double Value::NumberOr(double fallback) const { return type_ == Type::Number ? num_ : fallback; }

int64_t Value::IntOr(int64_t fallback) const {
    return type_ == Type::Number ? static_cast<int64_t>(num_) : fallback;
}

std::string Value::StringOr(const std::string& fallback) const {
    return type_ == Type::String ? str_ : fallback;
}

std::wstring Value::WideStringOr(const std::wstring& fallback) const {
    return type_ == Type::String ? text::FromUtf8(str_) : fallback;
}

size_t Value::Size() const {
    if (type_ == Type::Array) return arr_.size();
    if (type_ == Type::Object) return obj_.size();
    return 0;
}

void Value::Push(Value value) {
    if (type_ != Type::Array) {
        type_ = Type::Array;
        str_.clear();
        obj_.clear();
    }
    arr_.push_back(std::move(value));
}

void Value::Set(const std::string& key, Value value) {
    if (type_ != Type::Object) {
        type_ = Type::Object;
        str_.clear();
        arr_.clear();
    }
    for (auto& pair : obj_) {
        if (pair.first == key) {
            pair.second = std::move(value);
            return;
        }
    }
    obj_.emplace_back(key, std::move(value));
}

void Value::Remove(const std::string& key) {
    if (type_ != Type::Object) return;
    for (size_t i = 0; i < obj_.size(); ++i) {
        if (obj_[i].first == key) {
            obj_.erase(obj_.begin() + static_cast<ptrdiff_t>(i));
            return;
        }
    }
}

Value& Value::operator[](size_t index) {
    if (type_ != Type::Array) {
        type_ = Type::Array;
        arr_.clear();
        obj_.clear();
        str_.clear();
    }
    if (index >= arr_.size()) arr_.resize(index + 1);
    return arr_[index];
}

const Value& Value::operator[](size_t index) const {
    if (type_ == Type::Array && index < arr_.size()) return arr_[index];
    return kNullValue;
}

Value& Value::operator[](const std::string& key) {
    if (type_ != Type::Object) {
        type_ = Type::Object;
        str_.clear();
        arr_.clear();
    }
    for (auto& pair : obj_) {
        if (pair.first == key) return pair.second;
    }
    obj_.emplace_back(key, Value());
    return obj_.back().second;
}

const Value& Value::operator[](const std::string& key) const {
    if (type_ == Type::Object) {
        for (const auto& pair : obj_) {
            if (pair.first == key) return pair.second;
        }
    }
    return kNullValue;
}

bool Value::Has(const std::string& key) const {
    if (type_ != Type::Object) return false;
    for (const auto& pair : obj_) {
        if (pair.first == key) return true;
    }
    return false;
}

std::vector<std::string> Value::Keys() const {
    std::vector<std::string> keys;
    if (type_ != Type::Object) return keys;
    keys.reserve(obj_.size());
    for (const auto& pair : obj_) keys.push_back(pair.first);
    return keys;
}

std::string EscapeString(const std::string& raw) {
    std::string out;
    out.reserve(raw.size() + 8);
    for (const unsigned char c : raw) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buffer[8] = {};
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                    out += buffer;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    return out;
}

void Value::Serialize(std::string& out, int indent, int depth) const {
    const bool pretty = indent > 0;
    const std::string pad(pretty ? static_cast<size_t>(indent * (depth + 1)) : 0, ' ');
    const std::string padEnd(pretty ? static_cast<size_t>(indent * depth) : 0, ' ');

    switch (type_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += bool_ ? "true" : "false"; break;
        case Type::Number: out += NumberToString(num_); break;
        case Type::String:
            out.push_back('"');
            out += EscapeString(str_);
            out.push_back('"');
            break;
        case Type::Array: {
            if (arr_.empty()) {
                out += "[]";
                break;
            }
            out += "[";
            for (size_t i = 0; i < arr_.size(); ++i) {
                if (i != 0) out += ",";
                if (pretty) {
                    out += "\n";
                    out += pad;
                }
                arr_[i].Serialize(out, indent, depth + 1);
            }
            if (pretty) {
                out += "\n";
                out += padEnd;
            }
            out += "]";
            break;
        }
        case Type::Object: {
            if (obj_.empty()) {
                out += "{}";
                break;
            }
            out += "{";
            for (size_t i = 0; i < obj_.size(); ++i) {
                if (i != 0) out += ",";
                if (pretty) {
                    out += "\n";
                    out += pad;
                }
                out.push_back('"');
                out += EscapeString(obj_[i].first);
                out += pretty ? "\": " : "\":";
                obj_[i].second.Serialize(out, indent, depth + 1);
            }
            if (pretty) {
                out += "\n";
                out += padEnd;
            }
            out += "}";
            break;
        }
    }
}

std::string Value::Dump(int indent) const {
    std::string out;
    out.reserve(256);
    Serialize(out, indent, 0);
    return out;
}

Value Value::Parse(const std::string& text, std::string* error) {
    Value result;
    std::string localError;
    Parser parser(text);
    if (parser.ParseDocument(result, localError)) {
        if (error != nullptr) error->clear();
        return result;
    }
    if (error != nullptr) *error = localError;
    return Value();
}

} // namespace edgedock::json
