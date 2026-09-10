#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace crate {
class Actor;
}

namespace crate::script {

struct ClassInfo;
struct ScriptObject;

// A dynamically typed cScript value. `var` variables and untyped expressions
// hold one of these; typed declarations are checked/coerced on assignment.
struct Value {
    enum class T { Null, Bool, Int, Float, Char, String, Array, Object, TypeRef, Actor };
    T t = T::Null;

    bool b = false;
    long long i = 0;
    double f = 0.0;
    std::string s; // String contents, Char (1 char), or TypeRef name

    std::shared_ptr<std::vector<Value>> arr;
    std::shared_ptr<ScriptObject> obj;
    crate::Actor* actor = nullptr;

    static Value Null_() { return {}; }
    static Value Bool(bool v) { Value x; x.t = T::Bool; x.b = v; return x; }
    static Value Int(long long v) { Value x; x.t = T::Int; x.i = v; return x; }
    static Value Float(double v) { Value x; x.t = T::Float; x.f = v; return x; }
    static Value Char(char v) { Value x; x.t = T::Char; x.s = std::string(1, v); return x; }
    static Value Str(std::string v) { Value x; x.t = T::String; x.s = std::move(v); return x; }
    static Value Type(std::string name) { Value x; x.t = T::TypeRef; x.s = std::move(name); return x; }
    static Value ActorRef(crate::Actor* a) { Value x; x.t = T::Actor; x.actor = a; return x; }
    static Value Arr(std::vector<Value> v = {}) {
        Value x;
        x.t = T::Array;
        x.arr = std::make_shared<std::vector<Value>>(std::move(v));
        return x;
    }
    static Value Obj(std::shared_ptr<ScriptObject> o) {
        Value x;
        x.t = T::Object;
        x.obj = std::move(o);
        return x;
    }

    bool truthy() const;
    double num() const;     // numeric coercion (Int/Float/Bool/Char)
    bool isNumeric() const { return t == T::Int || t == T::Float || t == T::Bool || t == T::Char; }
    std::string str() const;              // .str()  (human-readable)
    const char* typeName() const;         // for diagnostics
};

// A plain script object: either an instance of a cScript class (cls != null) or
// a built-in aggregate like Vector3 (builtin set, cls null).
struct ScriptObject {
    const ClassInfo* cls = nullptr;
    std::string builtin;                       // "Vector2" / "Vector3" when cls == null
    std::unordered_map<std::string, Value> fields;
    crate::Actor* owner = nullptr;             // owning actor for script components

    // do_async suspension: which top-level do_async of which method is paused.
    std::string asyncResumeFn;
    int asyncResumeIndex = 0;
};

} // namespace crate::script
