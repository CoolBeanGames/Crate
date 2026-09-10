#include "script/Value.h"

#include <cmath>
#include <cstdio>

namespace crate::script {

bool Value::truthy() const {
    switch (t) {
        case T::Null: return false;
        case T::Bool: return b;
        case T::Int: return i != 0;
        case T::Float: return f != 0.0;
        case T::Char: return !s.empty() && s[0] != '\0';
        case T::String: return !s.empty();
        case T::Array: return arr && !arr->empty();
        default: return true;
    }
}

double Value::num() const {
    switch (t) {
        case T::Int: return double(i);
        case T::Float: return f;
        case T::Bool: return b ? 1.0 : 0.0;
        case T::Char: return s.empty() ? 0.0 : double((unsigned char)s[0]);
        default: return 0.0;
    }
}

static std::string trimFloat(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%g", v);
    return buf;
}

std::string Value::str() const {
    switch (t) {
        case T::Null: return "null";
        case T::Bool: return b ? "true" : "false";
        case T::Int: return std::to_string(i);
        case T::Float: return trimFloat(f);
        case T::Char: return s;
        case T::String: return s;
        case T::TypeRef: return "type(" + s + ")";
        case T::Actor: return "<actor>";
        case T::Array: {
            std::string out = "[";
            if (arr)
                for (size_t k = 0; k < arr->size(); ++k) {
                    if (k)
                        out += ", ";
                    out += (*arr)[k].str();
                }
            return out + "]";
        }
        case T::Object: {
            if (obj && !obj->builtin.empty()) {
                auto g = [&](const char* n) {
                    auto it = obj->fields.find(n);
                    return it == obj->fields.end() ? std::string("0") : it->second.str();
                };
                if (obj->builtin == "Vector2")
                    return "(" + g("x") + ", " + g("y") + ")";
                return "(" + g("x") + ", " + g("y") + ", " + g("z") + ")";
            }
            return "<object>";
        }
    }
    return "";
}

const char* Value::typeName() const {
    switch (t) {
        case T::Null: return "null";
        case T::Bool: return "bool";
        case T::Int: return "int";
        case T::Float: return "float";
        case T::Char: return "char";
        case T::String: return "string";
        case T::Array: return "array";
        case T::Object: return obj && !obj->builtin.empty() ? obj->builtin.c_str() : "object";
        case T::TypeRef: return "type";
        case T::Actor: return "Actor";
    }
    return "?";
}

} // namespace crate::script
