#pragma once
// EdgeDock Studio :: core/Json.h
// DOM JSON mínimo (sin dependencias) usado para config.json y exportaciones de clips.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace edgedock::json {

class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Value() = default;
    Value(bool v) : type_(Type::Bool), bool_(v) {}
    Value(int v) : type_(Type::Number), num_(static_cast<double>(v)) {}
    Value(int64_t v) : type_(Type::Number), num_(static_cast<double>(v)) {}
    Value(double v) : type_(Type::Number), num_(v) {}
    Value(const char* v) : type_(Type::String), str_(v ? v : "") {}
    Value(std::string v) : type_(Type::String), str_(std::move(v)) {}

    static Value MakeArray();
    static Value MakeObject();

    Type type() const { return type_; }
    bool IsNull() const { return type_ == Type::Null; }
    bool IsBool() const { return type_ == Type::Bool; }
    bool IsNumber() const { return type_ == Type::Number; }
    bool IsString() const { return type_ == Type::String; }
    bool IsArray() const { return type_ == Type::Array; }
    bool IsObject() const { return type_ == Type::Object; }

    bool BoolOr(bool fallback) const;
    double NumberOr(double fallback) const;
    int64_t IntOr(int64_t fallback) const;
    std::string StringOr(const std::string& fallback) const;
    std::wstring WideStringOr(const std::wstring& fallback) const;

    size_t Size() const;
    void Push(Value value);
    void Set(const std::string& key, Value value);
    void Remove(const std::string& key);

    Value& operator[](size_t index);
    const Value& operator[](size_t index) const;
    Value& operator[](const std::string& key);
    const Value& operator[](const std::string& key) const;

    bool Has(const std::string& key) const;
    std::vector<std::string> Keys() const;

    std::string Dump(int indent = 2) const;
    static Value Parse(const std::string& text, std::string* error = nullptr);

private:
    void Serialize(std::string& out, int indent, int depth) const;

    Type type_ = Type::Null;
    bool bool_ = false;
    double num_ = 0.0;
    std::string str_;
    std::vector<Value> arr_;
    std::vector<std::pair<std::string, Value>> obj_;
};

std::string EscapeString(const std::string& raw);

} // namespace edgedock::json
