#include "json.h"

#include <charconv>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace ve::json {

const Value::Object& Value::object() const { return std::get<Object>(data); }
Value::Object& Value::object() { return std::get<Object>(data); }
const Value::Array& Value::array() const { return std::get<Array>(data); }
const std::string& Value::string() const { return std::get<std::string>(data); }
std::int64_t Value::integer() const {
    if (const auto* value = std::get_if<std::int64_t>(&data)) return *value;
    throw std::runtime_error("JSON value is not an integer");
}
double Value::number() const {
    if (const auto* value = std::get_if<double>(&data)) return *value;
    if (const auto* value = std::get_if<std::int64_t>(&data)) return static_cast<double>(*value);
    throw std::runtime_error("JSON value is not numeric");
}
bool Value::boolean() const { return std::get<bool>(data); }
const Value& Value::at(std::string_view key) const {
    const auto& values = object();
    const auto iterator = values.find(std::string(key));
    if (iterator == values.end()) throw std::runtime_error("missing JSON field: " + std::string(key));
    return iterator->second;
}
const Value* Value::find(std::string_view key) const {
    const auto& values = object();
    const auto iterator = values.find(std::string(key));
    return iterator == values.end() ? nullptr : &iterator->second;
}

namespace {

class Parser {
public:
    explicit Parser(std::string_view input) : input_(input) {}

    Value run() {
        auto result = value();
        whitespace();
        if (position_ != input_.size()) fail("trailing content");
        return result;
    }

private:
    std::string_view input_;
    std::size_t position_{0};

    [[noreturn]] void fail(std::string_view message) const {
        throw std::runtime_error("JSON parse error at byte " + std::to_string(position_) + ": " +
                                 std::string(message));
    }
    void whitespace() {
        while (position_ < input_.size() &&
               (input_[position_] == ' ' || input_[position_] == '\n' ||
                input_[position_] == '\r' || input_[position_] == '\t')) {
            ++position_;
        }
    }
    bool consume(char expected) {
        whitespace();
        if (position_ < input_.size() && input_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }
    Value value() {
        whitespace();
        if (position_ >= input_.size()) fail("expected value");
        switch (input_[position_]) {
        case '{': return object();
        case '[': return array();
        case '"': return Value(string());
        case 't': literal("true"); return Value(true);
        case 'f': literal("false"); return Value(false);
        case 'n': literal("null"); return Value(nullptr);
        default:
            if (input_[position_] == '-' ||
                (input_[position_] >= '0' && input_[position_] <= '9')) return number();
            fail("unexpected token");
        }
    }
    void literal(std::string_view expected) {
        if (input_.substr(position_, expected.size()) != expected) fail("invalid literal");
        position_ += expected.size();
    }
    Value object() {
        ++position_;
        Value::Object result;
        if (consume('}')) return result;
        while (true) {
            whitespace();
            if (position_ >= input_.size() || input_[position_] != '"') fail("expected object key");
            auto key = string();
            if (!consume(':')) fail("expected colon");
            if (!result.emplace(std::move(key), value()).second) fail("duplicate object key");
            if (consume('}')) break;
            if (!consume(',')) fail("expected comma");
        }
        return result;
    }
    Value array() {
        ++position_;
        Value::Array result;
        if (consume(']')) return result;
        while (true) {
            result.push_back(value());
            if (consume(']')) break;
            if (!consume(',')) fail("expected comma");
        }
        return result;
    }
    static void appendUtf8(std::string& output, unsigned codepoint) {
        if (codepoint <= 0x7fU) output.push_back(static_cast<char>(codepoint));
        else if (codepoint <= 0x7ffU) {
            output.push_back(static_cast<char>(0xc0U | (codepoint >> 6U)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
        } else {
            output.push_back(static_cast<char>(0xe0U | (codepoint >> 12U)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
        }
    }
    std::string string() {
        ++position_;
        std::string result;
        while (position_ < input_.size()) {
            const char current = input_[position_++];
            if (current == '"') return result;
            if (static_cast<unsigned char>(current) < 0x20U) fail("control character in string");
            if (current != '\\') {
                result.push_back(current);
                continue;
            }
            if (position_ >= input_.size()) fail("unfinished escape");
            switch (input_[position_++]) {
            case '"': result.push_back('"'); break;
            case '\\': result.push_back('\\'); break;
            case '/': result.push_back('/'); break;
            case 'b': result.push_back('\b'); break;
            case 'f': result.push_back('\f'); break;
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            case 'u': {
                if (position_ + 4 > input_.size()) fail("short unicode escape");
                unsigned codepoint = 0;
                for (int index = 0; index < 4; ++index) {
                    const char digit = input_[position_++];
                    codepoint <<= 4U;
                    if (digit >= '0' && digit <= '9') codepoint += static_cast<unsigned>(digit - '0');
                    else if (digit >= 'a' && digit <= 'f') codepoint += static_cast<unsigned>(digit - 'a' + 10);
                    else if (digit >= 'A' && digit <= 'F') codepoint += static_cast<unsigned>(digit - 'A' + 10);
                    else fail("invalid unicode escape");
                }
                appendUtf8(result, codepoint);
                break;
            }
            default: fail("invalid escape");
            }
        }
        fail("unterminated string");
    }
    Value number() {
        const auto begin = position_;
        if (input_[position_] == '-') ++position_;
        while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') ++position_;
        bool floating = false;
        if (position_ < input_.size() && input_[position_] == '.') {
            floating = true; ++position_;
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') ++position_;
        }
        if (position_ < input_.size() && (input_[position_] == 'e' || input_[position_] == 'E')) {
            floating = true; ++position_;
            if (position_ < input_.size() && (input_[position_] == '+' || input_[position_] == '-')) ++position_;
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') ++position_;
        }
        const auto token = input_.substr(begin, position_ - begin);
        if (!floating) {
            std::int64_t integer = 0;
            const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), integer);
            if (error == std::errc{} && end == token.data() + token.size()) return Value(integer);
        }
        double decimal = 0.0;
        const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), decimal);
        if (error != std::errc{} || end != token.data() + token.size() || !std::isfinite(decimal)) fail("invalid number");
        return Value(decimal);
    }
};

std::string escaped(const std::string& input) {
    std::ostringstream output;
    output << '"';
    for (const unsigned char value : input) {
        switch (value) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\b': output << "\\b"; break;
        case '\f': output << "\\f"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (value < 0x20U) output << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(value) << std::dec;
            else output << static_cast<char>(value);
        }
    }
    output << '"';
    return output.str();
}

void write(const Value& value, std::ostringstream& output, int indent, int depth) {
    const auto newline = [&] { if (indent > 0) output << '\n' << std::string(static_cast<std::size_t>((depth + 1) * indent), ' '); };
    if (std::holds_alternative<std::nullptr_t>(value.data)) output << "null";
    else if (const auto* boolean = std::get_if<bool>(&value.data)) output << (*boolean ? "true" : "false");
    else if (const auto* integer = std::get_if<std::int64_t>(&value.data)) output << *integer;
    else if (const auto* decimal = std::get_if<double>(&value.data)) output << std::setprecision(17) << *decimal;
    else if (const auto* string = std::get_if<std::string>(&value.data)) output << escaped(*string);
    else if (const auto* array = std::get_if<Value::Array>(&value.data)) {
        output << '[';
        for (std::size_t index = 0; index < array->size(); ++index) {
            if (index != 0) output << ',';
            newline(); write((*array)[index], output, indent, depth + 1);
        }
        if (!array->empty() && indent > 0) output << '\n' << std::string(static_cast<std::size_t>(depth * indent), ' ');
        output << ']';
    } else {
        const auto& object = std::get<Value::Object>(value.data);
        output << '{';
        std::size_t index = 0;
        for (const auto& [key, member] : object) {
            if (index++ != 0) output << ',';
            newline(); output << escaped(key) << (indent > 0 ? ": " : ":");
            write(member, output, indent, depth + 1);
        }
        if (!object.empty() && indent > 0) output << '\n' << std::string(static_cast<std::size_t>(depth * indent), ' ');
        output << '}';
    }
}

} // namespace

Value parse(std::string_view source) { return Parser(source).run(); }
std::string dump(const Value& value, int indent) {
    std::ostringstream output;
    write(value, output, indent, 0);
    output << '\n';
    return output.str();
}

} // namespace ve::json
