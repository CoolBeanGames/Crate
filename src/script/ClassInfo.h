#pragma once
#include "script/Ast.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace crate::script {

// Runtime view of a parsed cScript class: owns the AST and provides fast
// method/field lookup with base-class fallback.
struct ClassInfo {
    std::string name;
    std::string base = "Actor";
    bool isStatic = false;
    bool isAbstract = false;

    // Bumped on every recompile so live ScriptComponents/objects know to reset.
    uint32_t generation = 0;

    std::unique_ptr<ClassDecl> decl;          // owns fields + functions AST
    const ClassInfo* baseClass = nullptr;     // resolved script base (null if native)

    std::unordered_map<std::string, const FunctionDecl*> ownFunctions;

    void indexFunctions() {
        ownFunctions.clear();
        if (!decl)
            return;
        for (const auto& fn : decl->functions)
            ownFunctions[fn.name] = &fn;
    }

    // Find a function by name walking up the base chain. `definedIn` receives the
    // class the function was found on (for `this.base` dispatch).
    const FunctionDecl* findFunction(const std::string& n, const ClassInfo** definedIn = nullptr) const {
        auto it = ownFunctions.find(n);
        if (it != ownFunctions.end() && !it->second->isAbstract) {
            if (definedIn)
                *definedIn = this;
            return it->second;
        }
        if (baseClass)
            return baseClass->findFunction(n, definedIn);
        // an abstract-only definition still counts as "known" for diagnostics
        if (it != ownFunctions.end()) {
            if (definedIn)
                *definedIn = this;
            return it->second;
        }
        return nullptr;
    }

    bool isA(const std::string& typeName) const {
        if (name == typeName || base == typeName)
            return true;
        for (const ClassInfo* c = baseClass; c; c = c->baseClass)
            if (c->name == typeName || c->base == typeName)
                return true;
        return false;
    }
};

} // namespace crate::script
