#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ve::json {

struct Value {
    using Object = std::map<std::string, Value>;
    using Array = std::vector<Value>;
    using Storage = std::variant<std::nullptr_t, bool, std::int64_t, double, std::string, Array, Object>;
    Storage data{nullptr};

    Value() = default;
    Value(std::nullptr_t) : data(nullptr) {}
    Value(bool value) : data(value) {}
    Value(std::int64_t value) : data(value) {}
    Value(std::int32_t value) : data(static_cast<std::int64_t>(value)) {}
    Value(std::uint64_t value) : data(static_cast<std::int64_t>(value)) {}
    Value(double value) : data(value) {}
    Value(std::string value) : data(std::move(value)) {}
    Value(const char* value) : data(std::string(value)) {}
    Value(Array value) : data(std::move(value)) {}
    Value(Object value) : data(std::move(value)) {}

    [[nodiscard]] const Object& object() const;
    [[nodiscard]] Object& object();
    [[nodiscard]] const Array& array() const;
    [[nodiscard]] const std::string& string() const;
    [[nodiscard]] std::int64_t integer() const;
    [[nodiscard]] double number() const;
    [[nodiscard]] bool boolean() const;
    [[nodiscard]] const Value& at(std::string_view key) const;
    [[nodiscard]] const Value* find(std::string_view key) const;
};

[[nodiscard]] Value parse(std::string_view source);
[[nodiscard]] std::string dump(const Value& value, int indent = 2);

} // namespace ve::json

