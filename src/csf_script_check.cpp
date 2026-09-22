#include "csf/script_signatures.hpp"

#include <algorithm>
#include <unordered_map>

namespace csf {
namespace {

struct Key {
    std::string_view head;
    bool instruction{};
    friend bool operator==(const Key&, const Key&) = default;
};

struct KeyHash {
    std::size_t operator()(const Key& key) const noexcept {
        return std::hash<std::string_view>{}(key.head) ^ (key.instruction ? 0x9e3779b9U : 0U);
    }
};

const std::unordered_map<Key, const ScriptSignature*, KeyHash>& signature_map() {
    static const auto map = [] {
        std::unordered_map<Key, const ScriptSignature*, KeyHash> result;
        for (const auto& signature : script_signatures())
            result.emplace(Key{signature.head, signature.instruction}, &signature);
        return result;
    }();
    return map;
}

std::string_view shape_head(const TreeNode& node) {
    if (node.kind == ValueKind::group && !node.children.empty())
        if (const auto tag = node.children.front().as_string()) return *tag;
    switch (node.kind) {
    case ValueKind::group: return "group";
    case ValueKind::array: return "array";
    case ValueKind::integer: return "int";
    case ValueKind::real: return "real";
    case ValueKind::string: return "string";
    default: return "unknown";
    }
}

bool position_allows(std::string_view positions, const std::size_t index,
                     const std::string_view shape) {
    for (std::size_t i = 0; i < index; ++i) {
        const auto separator = positions.find(';');
        if (separator == std::string_view::npos) return false;
        positions.remove_prefix(separator + 1);
    }
    positions = positions.substr(0, positions.find(';'));
    while (!positions.empty()) {
        const auto bar = positions.find('|');
        if (positions.substr(0, bar) == shape) return true;
        if (bar == std::string_view::npos) break;
        positions.remove_prefix(bar + 1);
    }
    return false;
}

void check(const TreeNode& node, const bool instruction, std::string_view opcode,
           std::vector<ScriptCheck>& output) {
    if (node.kind != ValueKind::group || node.children.empty()) return;
    const auto head = node.children.front().as_string();
    if (!head) {
        if (instruction)
            output.push_back({"Instruction does not start with an opcode string", std::string(opcode)});
        return;
    }
    if (instruction) opcode = *head;
    const auto& map = signature_map();
    const auto found = map.find(Key{*head, instruction});
    const auto arguments = node.children.size() - 1;
    if (found == map.end()) {
        output.push_back({std::string(instruction ? "Opcode " : "Operand ") + std::string(*head) +
                              " never occurs in shipped scripts",
                          std::string(opcode)});
    } else {
        const auto& signature = *found->second;
        if (arguments < signature.minimum_arity || arguments > signature.maximum_arity) {
            output.push_back({std::string(*head) + " takes " +
                                  std::to_string(signature.minimum_arity) +
                                  (signature.minimum_arity == signature.maximum_arity
                                       ? std::string{}
                                       : "-" + std::to_string(signature.maximum_arity)) +
                                  " argument(s) in shipped scripts, not " + std::to_string(arguments),
                              std::string(opcode)});
        }
        for (std::size_t i = 0; i < arguments && i < signature.maximum_arity; ++i) {
            const auto shape = shape_head(node.children[i + 1]);
            if (!position_allows(signature.positions, i, shape))
                output.push_back({"Argument " + std::to_string(i + 1) + " of " + std::string(*head) +
                                      " is " + std::string(shape) +
                                      ", which shipped scripts never use there",
                                  std::string(opcode)});
        }
    }
    for (std::size_t i = 1; i < node.children.size(); ++i) check(node.children[i], false, opcode, output);
}

void walk(const TreeNode& node, std::vector<ScriptCheck>& output) {
    if (node.is(".ACCIONES") || node.is(".CONDICIONES")) {
        for (const auto& instruction : node.children) check(instruction, true, {}, output);
        return;
    }
    for (const auto& child : node.children) walk(child, output);
}

} // namespace

const ScriptSignature* find_script_signature(const std::string_view head,
                                             const bool instruction) noexcept {
    const auto& map = signature_map();
    const auto found = map.find(Key{head, instruction});
    return found == map.end() ? nullptr : found->second;
}

std::vector<ScriptCheck> check_script_against_signatures(const TreeNode& node) {
    std::vector<ScriptCheck> output;
    walk(node, output);
    return output;
}

} // namespace csf
