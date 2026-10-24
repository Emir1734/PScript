#include "stdlib/native_common.h"
#include "stdlib/native_impl.h"

#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace pscript {
namespace {

constexpr std::size_t kMaxXmlDepth = 64;
constexpr std::size_t kMaxXmlNodes = 10000;

[[noreturn]] void xml_fail(std::string message) {
    native_value_error(std::move(message));
}

struct XmlNode {
    std::string tag;
    std::vector<std::pair<std::string, Value>> attributes;
    std::string text;
    std::vector<XmlNode> children;
};

Value xml_node_to_value(const XmlNode& node) {
    std::vector<Value> child_values;
    child_values.reserve(node.children.size());
    for (const XmlNode& child : node.children) {
        child_values.push_back(xml_node_to_value(child));
    }
    return Runtime::make_dict({{"tag", Runtime::make_string(node.tag)},
                               {"attributes", Runtime::make_dict(node.attributes)},
                               {"text", Runtime::make_string(node.text)},
                               {"children", Runtime::make_list(std::move(child_values))}});
}

class XmlParser {
public:
    explicit XmlParser(std::string_view input) : input_(input) {}

    XmlNode parse_document() {
        skip_ws();
        XmlNode root = parse_element(0);
        skip_ws();
        if (!input_.empty()) {
            xml_fail("unexpected trailing content");
        }
        return root;
    }

private:
    std::string_view input_;
    std::size_t nodes_ = 0;

    void skip_ws() {
        while (!input_.empty() && std::isspace(static_cast<unsigned char>(input_.front()))) {
            input_.remove_prefix(1);
        }
    }

    bool consume(char ch) {
        if (input_.empty() || input_.front() != ch) {
            return false;
        }
        input_.remove_prefix(1);
        return true;
    }

    void require(char ch) {
        if (!consume(ch)) {
            xml_fail("invalid xml");
        }
    }

    std::string read_name() {
        if (input_.empty() || (!std::isalpha(static_cast<unsigned char>(input_.front())) && input_.front() != '_')) {
            xml_fail("invalid tag name");
        }
        std::string name;
        while (!input_.empty()) {
            const char ch = input_.front();
            if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_' && ch != '-' && ch != ':') {
                break;
            }
            name.push_back(ch);
            input_.remove_prefix(1);
        }
        return name;
    }

    std::string read_until(char stop) {
        std::string out;
        while (!input_.empty() && input_.front() != stop) {
            out.push_back(input_.front());
            input_.remove_prefix(1);
        }
        return out;
    }

    XmlNode parse_element(std::size_t depth) {
        if (depth > kMaxXmlDepth) {
            xml_fail("xml depth limit exceeded");
        }
        if (++nodes_ > kMaxXmlNodes) {
            xml_fail("xml node limit exceeded");
        }
        require('<');
        if (consume('/')) {
            xml_fail("unexpected end tag");
        }
        if (consume('?')) {
            read_until('>');
            require('>');
            skip_ws();
            return parse_element(depth);
        }
        XmlNode node;
        node.tag = read_name();
        skip_ws();
        while (!input_.empty() && input_.front() != '>' && input_.front() != '/') {
            const std::string key = read_name();
            skip_ws();
            require('=');
            skip_ws();
            const char quote = input_.empty() ? '\0' : input_.front();
            if (quote != '"' && quote != '\'') {
                xml_fail("expected attribute quote");
            }
            input_.remove_prefix(1);
            const std::string value = read_until(quote);
            require(quote);
            node.attributes.emplace_back(key, Runtime::make_string(value));
            skip_ws();
        }
        if (consume('/')) {
            require('>');
            return node;
        }
        require('>');
        while (!input_.empty()) {
            if (input_.front() == '<') {
                if (input_.size() >= 2 && input_[1] == '/') {
                    break;
                }
                node.children.push_back(parse_element(depth + 1));
                continue;
            }
            node.text.push_back(input_.front());
            input_.remove_prefix(1);
        }
        require('<');
        require('/');
        const std::string closing = read_name();
        if (closing != node.tag) {
            xml_fail("mismatched closing tag");
        }
        skip_ws();
        require('>');
        return node;
    }
};

std::string xml_escape(std::string_view text) {
    std::string out;
    for (char ch : text) {
        switch (ch) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            default:
                out.push_back(ch);
                break;
        }
    }
    return out;
}

void stringify_node(const Value& node, std::string& out) {
    if (node.kind != ValueKind::Dict || !node.dict) {
        xml_fail("invalid xml node");
    }
    std::string tag;
    std::string text;
    const Value* attrs = nullptr;
    const Value* children = nullptr;
    for (const auto& entry : node.dict->entries) {
        if (entry.first == "tag" && entry.second.kind == ValueKind::String) {
            tag = entry.second.string;
        } else if (entry.first == "text" && entry.second.kind == ValueKind::String) {
            text = entry.second.string;
        } else if (entry.first == "attributes") {
            attrs = &entry.second;
        } else if (entry.first == "children") {
            children = &entry.second;
        }
    }
    out.push_back('<');
    out += tag;
    if (attrs && attrs->kind == ValueKind::Dict && attrs->dict) {
        for (const auto& attr : attrs->dict->entries) {
            out += ' ';
            out += attr.first;
            out += "=\"";
            out += xml_escape(attr.second.kind == ValueKind::String ? attr.second.string : Runtime::format(attr.second));
            out += '"';
        }
    }
    const bool has_children = children && children->kind == ValueKind::List && children->list &&
                              !children->list->items.empty();
    if (text.empty() && !has_children) {
        out += "/>";
        return;
    }
    out.push_back('>');
    out += xml_escape(text);
    if (has_children) {
        for (const Value& child : children->list->items) {
            stringify_node(child, out);
        }
    }
    out += "</";
    out += tag;
    out += '>';
}

}  // namespace

Value native_xml_parse(const std::vector<Value>& args) {
    native_require_argc("parse", args, 1);
    const std::string& text = native_require_string(args[0], "parse", "xml");
    XmlParser parser(text);
    return xml_node_to_value(parser.parse_document());
}

Value native_xml_stringify(const std::vector<Value>& args) {
    native_require_argc("stringify", args, 1);
    std::string out;
    stringify_node(args[0], out);
    return Runtime::make_string(out);
}

}  // namespace pscript
