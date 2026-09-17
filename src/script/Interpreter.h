#pragma once
#include "script/ClassInfo.h"
#include "script/Runtime.h"
#include "script/Value.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace crate {
class Actor;
}

namespace crate::script {

// RuntimeError now lives in Runtime.h (crate::script::RuntimeError), shared
// with the code that will eventually compile scripts to native C++; kept
// visible here via the #include above so existing callers of Interpreter.h
// don't need to change.

// Shared services the interpreter needs: the type table and I/O hooks.
struct ScriptContext {
    const std::unordered_map<std::string, std::unique_ptr<ClassInfo>>* types = nullptr;
    std::function<void(const std::string&)> print;  // -> game console
    std::function<void(const std::string&)> warn;

    // Resolve a component of the given script type on an actor (for
    // this.actor.get_component(type_of(SomeScript))). Set by ScriptSystem.
    std::function<std::shared_ptr<ScriptObject>(crate::Actor*, const std::string&)> getComponent;

    // Resolve the singleton instance of a `static class` by name (or null).
    std::function<std::shared_ptr<ScriptObject>(const std::string&)> getStatic;

    // Persistent signal-carrying object for an input button (Input.get_button).
    std::function<std::shared_ptr<ScriptObject>(const std::string&)> inputButton;
    // Raw input queries: 0 = pressed, 1 = just_pressed, 2 = just_released,
    // 3..5 = axis x/y/(unused). name in arg.
    std::function<double(const std::string&, int)> inputQuery;

    // Scene instancing (Scenes task): loads a saved .cscene at the given
    // path and attaches it as a new child under `parent`, returning the new
    // instance root (or null on failure). Backs Value.instantiate() -- a
    // "Scene"-typed field holds the path as a plain string, and calling
    // .instantiate() on it goes through here rather than a direct
    // Scene::instantiateUnder() call, since that lives in crate_core and
    // this ScriptContext is also reachable from a generated per-namespace
    // DLL (crate_script_runtime only).
    std::function<crate::Actor*(const std::string& path, crate::Actor* parent)> instantiateScene;

    const ClassInfo* findType(const std::string& n) const {
        if (!types)
            return nullptr;
        auto it = types->find(n);
        return it == types->end() ? nullptr : it->second.get();
    }
};

// One instance == one script object living on an actor (or a static singleton).
class Interpreter {
public:
    Interpreter(ScriptContext* ctx, std::shared_ptr<ScriptObject> self);

    // Initialise fields from their declared defaults (call once).
    void constructFields();

    // Call a method by name; missing method is a no-op unless `required`.
    Value call(const std::string& method, std::vector<Value> args = {}, bool required = false);

    bool hasMethod(const std::string& name) const;

    std::shared_ptr<ScriptObject> object() const { return self_; }

    // Construct a plain script-class instance (no actor). Used for `new`-less
    // class references / statics.
    static std::shared_ptr<ScriptObject> instantiate(ScriptContext* ctx, const ClassInfo* cls,
                                                     crate::Actor* owner);

    // Call a method by name on an ARBITRARY interpreted ScriptObject (not
    // necessarily this Interpreter's own self_) -- walks obj->cls's base
    // chain via findFunction(), non-resumable (do_async in the called
    // method runs synchronously to completion, exactly like any other
    // internal/cross-object call -- see Interpreter::call()'s own
    // resumable=true being reserved for the top-level hook-invocation entry
    // point only). Public so crate::script::callObjectMethod
    // (ObjectDispatch.h, Phase 9a) can dispatch to an interpreted target
    // without duplicating this logic. `viaBase=true` starts the lookup at
    // obj->cls->baseClass instead of obj->cls itself (this.base.method()).
    Value callMethodOn(std::shared_ptr<ScriptObject> obj, const std::string& method,
                       std::vector<Value> args, int line, bool viaBase);

private:
    struct Scope {
        std::unordered_map<std::string, Value> vars;
    };
    struct ReturnSignal {
        Value value;
    };
    struct BreakSignal {};
    struct ContinueSignal {};

    ScriptContext* ctx_;
    std::shared_ptr<ScriptObject> self_;
    std::vector<Scope> scopes_;
    const ClassInfo* dispatchClass_ = nullptr; // for `this.base`

    // do_async (frame-stepped loop) state for the current top-level call.
    bool resumable_ = false;      // this call may suspend/resume a do_async
    int resumeIndex_ = -1;        // resume at the Nth top-level do_async (-1 = none)
    bool yielded_ = false;        // set when a do_async suspended this frame
    int yieldIndex_ = 0;

    Value runFunction(const FunctionDecl& fn, const ClassInfo* definedIn, std::vector<Value>& args,
                      bool resumable = false);
    void execBlock(const std::vector<StmtPtr>& body);
    void execStmt(const Stmt& s);
    Value eval(const Expr& e);

    Value evalBinary(const Expr& e);
    // Arithmetic core shared by binary expressions and compound assignment.
    // Handles numbers, string concat (+), and Vector2/Vector3 math.
    Value arith(Tok op, const Value& a, const Value& b, int line);
    Value evalCall(const Expr& e);
    Value evalMember(const Expr& e);
    Value* lvalue(const Expr& e);       // resolves an assignable slot
    void assign(const Expr& target, Value v);

    Value* findVar(const std::string& name);
    Value builtinCall(const std::string& name, std::vector<Value>& args, int line);
    Value mathCall(const std::string& fn, std::vector<Value>& args, int line); // Math.*

    // Signals (Godot-style): connect / disconnect / emit / is_connected.
    Value signalCall(const Value& sig, const std::string& method, std::vector<Value> args, int line);
    Value invokeCallable(const Value& fn, std::vector<Value> args, int line);
    void emitSignal(const std::shared_ptr<ScriptObject>& owner, const std::string& name,
                    std::vector<Value> args, int line);
    Value actorMember(crate::Actor* a, const std::string& name, int line);
};

} // namespace crate::script
