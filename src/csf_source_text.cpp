#include "csf/source_text.hpp"

#include <algorithm>
#include <bit>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>

namespace csf {
namespace {

bool bare_start(const unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

bool bare_continue(const unsigned char c) {
    return bare_start(c) || (c >= '0' && c <= '9');
}

// Label words may start with '.' and contain non-ASCII letters (e.g. DAÑO).
bool label_word_char(const unsigned char c) {
    return bare_continue(c) || c == '.' || c >= 0x80;
}

bool is_bare_string(const std::string_view value) {
    if (value.empty() || !bare_start(static_cast<unsigned char>(value.front()))) return false;
    return std::ranges::all_of(value, [](const char c) {
        return bare_continue(static_cast<unsigned char>(c));
    });
}

// Returns the quoted form of raw Windows-1252 bytes, without the final NUL
// unless the value lacks one (then the `~` suffix marks it). The zero-length
// value, which is how shipped files store empty strings, is plain "".
std::string quote(const std::string_view raw) {
    if (raw.empty()) return "\"\"";
    const bool terminated = !raw.empty() && raw.back() == '\0';
    const auto content = terminated ? raw.substr(0, raw.size() - 1) : raw;
    std::string out = "\"";
    std::string pending;
    const auto flush = [&] {
        out += windows_1252_to_utf8(pending);
        pending.clear();
    };
    for (const char c : content) {
        const auto byte = static_cast<unsigned char>(c);
        const char* escape = nullptr;
        switch (byte) {
        case '\\': escape = "\\\\"; break;
        case '"': escape = "\\\""; break;
        case '\n': escape = "\\n"; break;
        case '\r': escape = "\\r"; break;
        case '\t': escape = "\\t"; break;
        default: break;
        }
        if (escape) {
            flush();
            out += escape;
        } else if (byte < 0x20 || byte == 0x7F || byte == 0x81 || byte == 0x8D || byte == 0x8F ||
                   byte == 0x90 || byte == 0x9D) {
            // Control bytes and the five undefined Windows-1252 positions stay
            // explicit so that the text is unambiguous in any editor.
            flush();
            char buffer[8];
            std::snprintf(buffer, sizeof buffer, "\\x%02X", byte);
            out += buffer;
        } else {
            pending.push_back(c);
        }
    }
    flush();
    out.push_back('"');
    if (!terminated) out.push_back('~');
    return out;
}

std::string label_text(const std::string& raw) {
    const bool terminated = !raw.empty() && raw.back() == '\0';
    const std::string_view name =
        terminated ? std::string_view(raw).substr(0, raw.size() - 1) : std::string_view(raw);
    const auto first = name.empty() ? '\0' : name.front();
    const bool word = terminated && !name.empty() && !(first >= '0' && first <= '9') &&
                      std::ranges::all_of(name, [](const char c) {
                          return label_word_char(static_cast<unsigned char>(c));
                      });
    if (word && first == '.' && name.size() > 1) return windows_1252_to_utf8(name);
    if (word && first != '.') return windows_1252_to_utf8(name) + ":";
    return quote(raw) + ":";
}

std::string real_text(const std::uint32_t bits) {
    const auto value = std::bit_cast<float>(bits);
    char buffer[64];
    if (!std::isfinite(value)) {
        std::snprintf(buffer, sizeof buffer, "%%%08x", bits);
        return buffer;
    }
    const auto [end, error] = std::to_chars(buffer, buffer + sizeof buffer, value);
    std::string text(buffer, error == std::errc{} ? end : buffer);
    if (text.find_first_of(".eE") == std::string::npos) text += ".0";
    return text;
}

bool instruction_like(const TreeNode& node) {
    if (node.kind != ValueKind::group || node.label || node.children.empty()) return false;
    const auto& opcode = node.children.front();
    if (opcode.kind != ValueKind::string || opcode.label || !opcode.as_string() ||
        !is_bare_string(*opcode.as_string()))
        return false;
    const auto no_labels = [](const auto& self, const TreeNode& value) -> bool {
        if (value.label) return false;
        return std::ranges::all_of(value.children,
                                   [&](const TreeNode& child) { return self(self, child); });
    };
    return no_labels(no_labels, node);
}

bool block_like(const TreeNode& node) {
    if (node.kind != ValueKind::group || (!node.is(".ACCIONES") && !node.is(".CONDICIONES")))
        return false;
    return std::ranges::all_of(node.children, instruction_like);
}

std::string upper_opcode(const TreeNode& instruction) {
    std::string key(*instruction.children.front().as_string());
    std::ranges::transform(key, key.begin(), [](const unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return key;
}

void print(std::string& out, const TreeNode& node, std::size_t indent);

void print_inline(std::string& out, const TreeNode& node) {
    if (node.label) {
        out += label_text(*node.label);
        out.push_back(' ');
    }
    switch (node.kind) {
    case ValueKind::integer:
        out += std::to_string(static_cast<std::int32_t>(node.raw));
        return;
    case ValueKind::real:
        out += real_text(node.raw);
        return;
    case ValueKind::string: {
        const bool terminated = !node.text.empty() && node.text.back() == '\0';
        const auto content = std::string_view(node.text).substr(0, node.text.size() - (terminated ? 1 : 0));
        if (terminated && is_bare_string(content)) out += content;
        else out += quote(node.text);
        return;
    }
    default:
        break;
    }
    out.push_back(node.kind == ValueKind::group ? '(' : '[');
    for (std::size_t i = 0; i < node.children.size(); ++i) {
        if (i) out.push_back(' ');
        print_inline(out, node.children[i]);
    }
    out.push_back(node.kind == ValueKind::group ? ')' : ']');
}

bool short_container(const TreeNode& node) {
    if (!node.is_container()) return true;
    if (node.children.size() > 8) return false;
    return std::ranges::all_of(node.children, [](const TreeNode& child) {
        return !child.is_container() && !child.label;
    });
}

void print_block(std::string& out, const TreeNode& node, const std::size_t indent) {
    out += label_text(*node.label);
    out += " {\n";
    std::size_t depth = 0;
    for (const auto& instruction : node.children) {
        const auto key = upper_opcode(instruction);
        const bool closer = key == "ENDIF" || key == "WEND" || key == "ENDFOR" || key == "ELSE";
        if (closer && depth > 0) --depth;
        out.append((indent + 1 + depth) * 2, ' ');
        for (std::size_t i = 0; i < instruction.children.size(); ++i) {
            if (i) out.push_back(' ');
            print_inline(out, instruction.children[i]);
        }
        out.push_back('\n');
        if (key == "IF" || key == "WHILE" || key == "FOREACH" || key == "ELSE") ++depth;
    }
    out.append(indent * 2, ' ');
    out.push_back('}');
}

void print(std::string& out, const TreeNode& node, const std::size_t indent) {
    if (block_like(node)) {
        print_block(out, node, indent);
        return;
    }
    if (short_container(node)) {
        print_inline(out, node);
        return;
    }
    if (node.label) {
        out += label_text(*node.label);
        out.push_back(' ');
    }
    out.push_back(node.kind == ValueKind::group ? '(' : '[');
    out.push_back('\n');
    for (const auto& child : node.children) {
        out.append((indent + 1) * 2, ' ');
        print(out, child, indent + 1);
        out.push_back('\n');
    }
    out.append(indent * 2, ' ');
    out.push_back(node.kind == ValueKind::group ? ')' : ']');
}

enum class TokenKind { end, newline, open, close, at, word, label, quoted, integer, real };

struct Token {
    TokenKind kind{TokenKind::end};
    char bracket{};
    std::string text; // raw Windows-1252 for words, labels and strings
    std::uint32_t raw{};
    std::size_t line{}, column{};
};

class Lexer {
public:
    explicit Lexer(const std::string_view text) : text_(text) {}

    Token next() {
        skip_blank();
        Token token;
        token.line = line_;
        token.column = column();
        if (at_ >= text_.size()) return token;
        const char c = text_[at_];
        if (c == '\n') {
            advance();
            token.kind = TokenKind::newline;
            return token;
        }
        if (c == '(' || c == '[' || c == '{') {
            advance();
            token.kind = TokenKind::open;
            token.bracket = c;
            return token;
        }
        if (c == ')' || c == ']' || c == '}') {
            advance();
            token.kind = TokenKind::close;
            token.bracket = c;
            return token;
        }
        if (c == '@') {
            advance();
            token.kind = TokenKind::at;
            return token;
        }
        if (c == '"') return quoted(token);
        if (c == '%') return raw_real(token);
        if (c == '-' || c == '+' || (c >= '0' && c <= '9')) return number(token);
        if (label_word_char(static_cast<unsigned char>(c))) return word(token);
        fail(token, std::string("Unexpected character '") + c + "'");
    }

    [[noreturn]] static void fail(const Token& at, const std::string& message) {
        throw SourceTextError(at.line, at.column, message);
    }

private:
    void advance() {
        if (text_[at_] == '\n') {
            ++line_;
            line_start_ = at_ + 1;
        }
        ++at_;
    }

    [[nodiscard]] std::size_t column() const { return at_ - line_start_ + 1; }

    void skip_blank() {
        while (at_ < text_.size()) {
            const char c = text_[at_];
            if (c == ' ' || c == '\t' || c == '\r') {
                advance();
            } else if (c == '#') {
                while (at_ < text_.size() && text_[at_] != '\n') advance();
            } else {
                break;
            }
        }
    }

    Token quoted(Token token) {
        advance();
        std::string utf8;
        std::string raw;
        const auto flush = [&] {
            const auto converted = utf8_to_windows_1252(utf8);
            if (!converted) fail(token, "String contains a character outside Windows-1252");
            raw += *converted;
            utf8.clear();
        };
        while (true) {
            if (at_ >= text_.size() || text_[at_] == '\n') fail(token, "Unterminated string");
            const char c = text_[at_];
            if (c == '"') {
                advance();
                break;
            }
            if (c != '\\') {
                utf8.push_back(c);
                advance();
                continue;
            }
            advance();
            if (at_ >= text_.size()) fail(token, "Unterminated escape");
            const char e = text_[at_];
            advance();
            flush();
            switch (e) {
            case '\\': raw.push_back('\\'); break;
            case '"': raw.push_back('"'); break;
            case 'n': raw.push_back('\n'); break;
            case 'r': raw.push_back('\r'); break;
            case 't': raw.push_back('\t'); break;
            case 'x': {
                if (at_ + 2 > text_.size()) fail(token, "Incomplete \\x escape");
                unsigned value{};
                const auto digits = text_.substr(at_, 2);
                const auto [end, error] =
                    std::from_chars(digits.data(), digits.data() + 2, value, 16);
                if (error != std::errc{} || end != digits.data() + 2) fail(token, "Invalid \\x escape");
                raw.push_back(static_cast<char>(value));
                advance();
                advance();
                break;
            }
            default:
                fail(token, std::string("Unknown escape \\") + e);
            }
        }
        flush();
        bool terminated = true;
        if (at_ < text_.size() && text_[at_] == '~') {
            advance();
            terminated = false;
        }
        // Shipped files store the empty string as zero bytes, so "" means that.
        if (terminated && !raw.empty()) raw.push_back('\0');
        token.text = std::move(raw);
        token.kind = TokenKind::quoted;
        if (at_ < text_.size() && text_[at_] == ':') {
            advance();
            token.kind = TokenKind::label;
        }
        return token;
    }

    Token raw_real(Token token) {
        advance();
        const auto start = at_;
        while (at_ < text_.size() && std::isxdigit(static_cast<unsigned char>(text_[at_]))) advance();
        const auto digits = text_.substr(start, at_ - start);
        std::uint32_t bits{};
        const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), bits, 16);
        if (digits.size() != 8 || error != std::errc{} || end != digits.data() + digits.size())
            fail(token, "A raw real needs exactly eight hexadecimal digits");
        token.kind = TokenKind::real;
        token.raw = bits;
        return token;
    }

    Token number(Token token) {
        const auto start = at_;
        while (at_ < text_.size()) {
            const char c = text_[at_];
            if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '-' || c == '+')
                advance();
            else
                break;
        }
        auto literal = text_.substr(start, at_ - start);
        if (!literal.empty() && literal.front() == '+') literal.remove_prefix(1);
        if (literal.find_first_of(".eE") != std::string_view::npos) {
            float value{};
            const auto [end, error] =
                std::from_chars(literal.data(), literal.data() + literal.size(), value);
            if (error != std::errc{} || end != literal.data() + literal.size())
                fail(token, "Invalid real " + std::string(literal));
            token.kind = TokenKind::real;
            token.raw = std::bit_cast<std::uint32_t>(value);
            return token;
        }
        std::int64_t value{};
        const auto [end, error] = std::from_chars(literal.data(), literal.data() + literal.size(), value);
        if (error != std::errc{} || end != literal.data() + literal.size() ||
            value < std::numeric_limits<std::int32_t>::min() ||
            value > std::numeric_limits<std::uint32_t>::max())
            fail(token, "Invalid 32-bit integer " + std::string(literal));
        token.kind = TokenKind::integer;
        token.raw = static_cast<std::uint32_t>(value);
        return token;
    }

    Token word(Token token) {
        const auto start = at_;
        while (at_ < text_.size() && label_word_char(static_cast<unsigned char>(text_[at_]))) advance();
        const auto utf8 = text_.substr(start, at_ - start);
        const auto raw = utf8_to_windows_1252(utf8);
        if (!raw) fail(token, "Word contains a character outside Windows-1252");
        token.text = *raw;
        bool colon = false;
        if (at_ < text_.size() && text_[at_] == ':') {
            advance();
            colon = true;
        }
        if (colon || (!token.text.empty() && token.text.front() == '.')) {
            token.kind = TokenKind::label;
            token.text.push_back('\0');
        } else {
            token.kind = TokenKind::word;
        }
        return token;
    }

    std::string_view text_;
    std::size_t at_{}, line_{1}, line_start_{};
};

class Parser {
public:
    explicit Parser(const std::string_view text) : lexer_(text) { advance(); }

    std::vector<TreeNode> values_until_end() {
        std::vector<TreeNode> result;
        while (true) {
            skip_newlines();
            if (current_.kind == TokenKind::end) return result;
            result.push_back(value());
        }
    }

private:
    void advance() { current_ = lexer_.next(); }

    void skip_newlines() {
        while (current_.kind == TokenKind::newline) advance();
    }

    TreeNode value() {
        skip_newlines();
        std::optional<std::string> label;
        if (current_.kind == TokenKind::label) {
            label = std::move(current_.text);
            advance();
            skip_newlines();
        }
        TreeNode node = item();
        node.label = std::move(label);
        return node;
    }

    TreeNode item() {
        TreeNode node;
        switch (current_.kind) {
        case TokenKind::integer:
            node.kind = ValueKind::integer;
            node.raw = current_.raw;
            advance();
            return node;
        case TokenKind::real:
            node.kind = ValueKind::real;
            node.raw = current_.raw;
            advance();
            return node;
        case TokenKind::word:
            node.kind = ValueKind::string;
            node.text = std::move(current_.text);
            node.text.push_back('\0');
            advance();
            return node;
        case TokenKind::quoted:
            node.kind = ValueKind::string;
            node.text = std::move(current_.text);
            advance();
            return node;
        case TokenKind::open:
            if (current_.bracket == '{') return block();
            return container();
        case TokenKind::end:
            Lexer::fail(current_, "Unexpected end of text; a value was expected");
        default:
            Lexer::fail(current_, "A value was expected");
        }
    }

    TreeNode container() {
        const auto open = current_;
        const char close = open.bracket == '(' ? ')' : ']';
        TreeNode node;
        node.kind = open.bracket == '(' ? ValueKind::group : ValueKind::array;
        advance();
        while (true) {
            skip_newlines();
            if (current_.kind == TokenKind::end) Lexer::fail(open, "Unclosed container");
            if (current_.kind == TokenKind::close) {
                if (current_.bracket != close)
                    Lexer::fail(current_, std::string("Expected '") + close + "'");
                advance();
                return node;
            }
            node.children.push_back(value());
        }
    }

    TreeNode block() {
        const auto open = current_;
        TreeNode node;
        node.kind = ValueKind::group;
        advance();
        while (true) {
            skip_newlines();
            if (current_.kind == TokenKind::end) Lexer::fail(open, "Unclosed instruction block");
            if (current_.kind == TokenKind::close) {
                if (current_.bracket != '}') Lexer::fail(current_, "Expected '}'");
                advance();
                return node;
            }
            if (current_.kind == TokenKind::at) {
                advance();
                node.children.push_back(value());
                continue;
            }
            TreeNode instruction;
            instruction.kind = ValueKind::group;
            while (current_.kind != TokenKind::newline && current_.kind != TokenKind::end &&
                   !(current_.kind == TokenKind::close && current_.bracket == '}')) {
                if (current_.kind == TokenKind::label)
                    Lexer::fail(current_, "Instruction items cannot be labelled");
                instruction.children.push_back(item());
            }
            node.children.push_back(std::move(instruction));
        }
    }

    Lexer lexer_;
    Token current_;
};

} // namespace

SourceTextError::SourceTextError(const std::size_t line, const std::size_t column,
                                 const std::string& message)
    : std::runtime_error("line " + std::to_string(line) + ", column " + std::to_string(column) +
                         ": " + message),
      line_(line), column_(column) {}

std::string to_source_text(const TreeNode& node, const std::size_t indent) {
    std::string out;
    print(out, node, indent);
    return out;
}

std::string to_source_text(const std::vector<TreeNode>& nodes) {
    std::string out;
    for (const auto& node : nodes) {
        print(out, node, 0);
        out.push_back('\n');
    }
    return out;
}

std::vector<TreeNode> parse_source_text(const std::string_view text) {
    return Parser(text).values_until_end();
}

TreeNode parse_source_value(const std::string_view text) {
    auto values = parse_source_text(text);
    if (values.size() != 1)
        throw SourceTextError(1, 1, "Expected exactly one value, found " +
                                        std::to_string(values.size()));
    return std::move(values.front());
}

} // namespace csf
