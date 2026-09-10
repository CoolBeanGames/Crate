#pragma once
#include "script/ClassInfo.h"
#include "script/Value.h"

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace crate {
class Actor;
}

namespace crate::script {

struct RuntimeError : std::runtime_error {
    int line;
    RuntimeError(std::string msg, int ln) : std::runtime_error(std::move(msg)), line(ln) {}
};

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

    Value runFunction(const FunctionDecl& fn, const ClassInfo* definedIn, std::vector<Value>& args);
    void execBlock(const std::vector<StmtPtr>& body);
    void execStmt(const Stmt& s);
    Value eval(const Expr& e);

    Value evalBinary(const Expr& e);
    Value evalCall(const Expr& e);
    Value evalMember(const Expr& e);
    Value* lvalue(const Expr& e);       // resolves an assignable slot
    void assign(const Expr& target, Value v);

    Value* findVar(const std::string& name);
    Value builtinCall(const std::string& name, std::vector<Value>& args, int line);
    Value actorMember(crate::Actor* a, const std::string& name, int line);
    Value callMethodOn(std::shared_ptr<ScriptObject> obj, const std::string& method,
                       std::vector<Value> args, int line, bool viaBase);
};

// Build a Vector2/Vector3 value.
Value makeVector(const std::string& kind, double x, double y, double z);

} // namespace crate::script
