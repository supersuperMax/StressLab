#include "fem/core/context/when.h"

#include "when_node.h"

#include <algorithm>
#include <string>
#include <utility>

namespace fem::context {

namespace {

using detail::WhenNode;
using Op = WhenNode::Op;

bool evaluate_node(const WhenNode& node, const WhenExpression::Lookup& lookup) {
    switch (node.op) {
        case Op::constant:
            return node.constant;
        case Op::key:
            return lookup(node.key).is_truthy();
        case Op::equals:
            return lookup(node.key) == node.literal;
        case Op::not_equals:
            return lookup(node.key) != node.literal;
        case Op::in: {
            const ContextValue& item = lookup(node.key);
            const ContextValue& list = lookup(node.list);
            if (item.kind() != ContextValue::Kind::string ||
                list.kind() != ContextValue::Kind::list) {
                return false;
            }
            const ContextValue::List& items = list.as_list();
            return std::find(items.begin(), items.end(), item.as_string()) != items.end();
        }
        case Op::negate:
            return !evaluate_node(node.operands.front(), lookup);
        case Op::all:
            return std::all_of(
                node.operands.begin(), node.operands.end(),
                [&](const WhenNode& operand) { return evaluate_node(operand, lookup); });
        case Op::any:
            return std::any_of(
                node.operands.begin(), node.operands.end(),
                [&](const WhenNode& operand) { return evaluate_node(operand, lookup); });
    }
    return false;
}

void collect_keys(const WhenNode& node, std::vector<std::string>& keys) {
    switch (node.op) {
        case Op::key:
        case Op::equals:
        case Op::not_equals:
            keys.push_back(node.key);
            break;
        case Op::in:
            keys.push_back(node.key);
            keys.push_back(node.list);
            break;
        default:
            for (const WhenNode& operand : node.operands) {
                collect_keys(operand, keys);
            }
            break;
    }
}

void append_node(std::string& out, const WhenNode& node);

void append_operand(std::string& out, const WhenNode& operand, bool parenthesize) {
    if (parenthesize) {
        out.push_back('(');
    }
    append_node(out, operand);
    if (parenthesize) {
        out.push_back(')');
    }
}

void append_node(std::string& out, const WhenNode& node) {
    switch (node.op) {
        case Op::constant:
            out += node.constant ? "true" : "false";
            break;
        case Op::key:
            out += node.key;
            break;
        case Op::equals:
        case Op::not_equals:
            out += node.key;
            out += node.op == Op::equals ? " == " : " != ";
            out += node.spelling;
            break;
        case Op::in:
            out += node.key;
            out += " in ";
            out += node.list;
            break;
        case Op::negate: {
            const WhenNode& operand = node.operands.front();
            out.push_back('!');
            append_operand(
                out, operand,
                operand.op != Op::constant && operand.op != Op::key && operand.op != Op::negate);
            break;
        }
        case Op::all:
        case Op::any: {
            // '&&' binds tighter than '||', and chains never hold chains of their own operator.
            const char* separator = node.op == Op::all ? " && " : " || ";
            bool first = true;
            for (const WhenNode& operand : node.operands) {
                if (!first) {
                    out += separator;
                }
                first = false;
                append_operand(out, operand, node.op == Op::all && operand.op == Op::any);
            }
            break;
        }
    }
}

}  // namespace

bool is_valid_key(std::string_view name) noexcept {
    if (name.empty() || !detail::is_key_start(name.front()) || name.back() == '.' ||
        name.find("..") != std::string_view::npos) {
        return false;
    }
    if (!std::all_of(name.begin(), name.end(), detail::is_key_char)) {
        return false;
    }
    return name != "true" && name != "false" && name != "in";
}

WhenExpression::WhenExpression(std::shared_ptr<const detail::WhenNode> root)
    : root_(std::move(root)) {
    collect_keys(*root_, keys_);
    std::sort(keys_.begin(), keys_.end());
    keys_.erase(std::unique(keys_.begin(), keys_.end()), keys_.end());
}

WhenExpression WhenExpression::parse(std::string_view text) {
    return WhenExpression(std::make_shared<const WhenNode>(detail::parse_when(text)));
}

bool WhenExpression::evaluate(const Lookup& lookup) const {
    return evaluate_node(*root_, lookup);
}

std::string WhenExpression::to_string() const {
    std::string out;
    append_node(out, *root_);
    return out;
}

}  // namespace fem::context
