#include "script/ScriptSystem.h"

#include "core/Log.h"
#include "scene/Actor.h"
#include "scene/ComponentRegistry.h"
#include "script/Format.h"
#include "script/Lexer.h"
#include "script/Parser.h"
#include "script/ScriptComponent.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace crate::script {
namespace fs = std::filesystem;

ScriptSystem& ScriptSystem::get() {
    static ScriptSystem instance;
    return instance;
}

ScriptSystem::ScriptSystem() {
    ctx_.types = &types_;
    ctx_.print = [](const std::string& s) { CR_GAME("script", s); };
    ctx_.warn = [](const std::string& s) { CR_WARN("script", s); };
    ctx_.getComponent = [this](crate::Actor* a, const std::string& typeName)
        -> std::shared_ptr<ScriptObject> {
        if (!a)
            return nullptr;
        for (const auto& c : a->components()) {
            auto* sc = dynamic_cast<ScriptComponent*>(c.get());
            if (sc && sc->classInfo() &&
                (sc->classInfo()->name == typeName || sc->classInfo()->isA(typeName)))
                return sc->object();
        }
        return nullptr;
    };
    ctx_.getStatic = [this](const std::string& name) -> std::shared_ptr<ScriptObject> {
        auto it = statics_.find(name);
        return it == statics_.end() ? nullptr : it->second;
    };
    rebuildTypeDocs();
}

bool ScriptSystem::compile(const std::string& source, std::string* errorOut, std::string* nameOut) {
    try {
        Lexer lex(source);
        Parser parser(lex.tokenize());
        std::unique_ptr<ClassDecl> decl = parser.parseClass();

        // Name rules: starts with an uppercase letter, no leading digit.
        const std::string& n = decl->name;
        if (n.empty() || std::isdigit((unsigned char)n[0]) || !std::isupper((unsigned char)n[0])) {
            if (errorOut)
                *errorOut = "class name '" + n + "' must start with an uppercase letter";
            return false;
        }

        auto ci = std::make_unique<ClassInfo>();
        ci->name = decl->name;
        ci->base = decl->base;
        ci->isStatic = decl->isStatic;
        ci->isAbstract = decl->isAbstract;
        ci->decl = std::move(decl);
        ci->indexFunctions();

        if (nameOut)
            *nameOut = ci->name;
        std::string cname = ci->name;
        bool wasStatic = ci->isStatic;
        types_[cname] = std::move(ci);
        resolveBases();
        // static / abstract classes cannot be added to actors as components
        if (!wasStatic && !types_[cname]->isAbstract)
            registerComponent(cname);
        if (wasStatic)
            rebuildStatic(cname);
        rebuildTypeDocs();
        if (errorOut)
            errorOut->clear();
        return true;
    } catch (const ParseError& e) {
        if (errorOut)
            *errorOut = "line " + std::to_string(e.line) + ": " + e.what();
        return false;
    } catch (const std::exception& e) {
        if (errorOut)
            *errorOut = e.what();
        return false;
    }
}

void ScriptSystem::resolveBases() {
    for (auto& [name, ci] : types_) {
        auto it = types_.find(ci->base);
        ci->baseClass = (it != types_.end() && it->second.get() != ci.get()) ? it->second.get()
                                                                             : nullptr;
    }
}

void ScriptSystem::registerComponent(const std::string& className) {
    ScriptContext* ctx = &ctx_;
    const ClassInfo* cls = types_.at(className).get();
    ComponentRegistry::get().add(className, "Scripts", [ctx, cls] {
        return std::make_unique<ScriptComponent>(ctx, cls);
    });
}

void ScriptSystem::rebuildStatic(const std::string& className) {
    auto it = types_.find(className);
    if (it == types_.end() || !it->second->isStatic)
        return;
    statics_[className] = Interpreter::instantiate(&ctx_, it->second.get(), nullptr);
}

void ScriptSystem::resetStatics() {
    for (const auto& [name, ci] : types_)
        if (ci->isStatic)
            rebuildStatic(name);
}

void ScriptSystem::startStatics() {
    resetStatics();
    for (auto& [name, obj] : statics_) {
        try {
            Interpreter(&ctx_, obj).call("start");
        } catch (const std::exception& ex) {
            CR_ERROR("script", name + ".start (static): " + ex.what());
        }
    }
}

void ScriptSystem::tickStatics(float dt) {
    for (auto& [name, obj] : statics_) {
        try {
            Interpreter(&ctx_, obj).call("update", {Value::Float(dt)});
        } catch (const std::exception& ex) {
            CR_ERROR("script", name + ".update (static): " + ex.what());
        }
    }
}

void ScriptSystem::physicsStatics(float dt) {
    for (auto& [name, obj] : statics_) {
        try {
            Interpreter(&ctx_, obj).call("physics_update", {Value::Float(dt)});
        } catch (const std::exception&) {
        }
    }
}

void ScriptSystem::loadFolder(const std::string& dir) {
    std::error_code ec;
    if (!fs::exists(dir, ec))
        return;
    for (const auto& entry : fs::recursive_directory_iterator(dir, ec)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".cscript")
            continue;
        std::ifstream in(entry.path(), std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        std::string src = reindent(ss.str());

        ScriptFile f;
        f.path = entry.path().string();
        f.source = src;
        std::string name, err;
        if (compile(src, &err, &name)) {
            f.name = name;
            CR_LOG("script", "Loaded script '" + name + "' from " +
                                 entry.path().filename().string());
        } else {
            f.name = entry.path().stem().string();
            f.error = err;
            CR_ERROR("script", "Failed to load " + entry.path().filename().string() + ": " + err);
        }
        // replace existing entry with the same path
        bool replaced = false;
        for (auto& existing : files_)
            if (existing.path == f.path) {
                existing = f;
                replaced = true;
            }
        if (!replaced)
            files_.push_back(std::move(f));
    }
}

ScriptSystem::ScriptFile* ScriptSystem::file(const std::string& name) {
    for (auto& f : files_)
        if (f.name == name)
            return &f;
    return nullptr;
}

bool ScriptSystem::saveFile(ScriptFile& f) {
    if (f.path.empty())
        return false;
    std::ofstream out(f.path, std::ios::binary);
    if (!out)
        return false;
    out << f.source;
    f.dirty = false;
    CR_LOG("script", "Saved " + fs::path(f.path).filename().string());
    return true;
}

void ScriptSystem::setSource(const std::string& name, std::string source) {
    ScriptFile* f = file(name);
    if (!f)
        return;
    f->source = std::move(source);
    f->dirty = true;
    std::string err, newName;
    if (compile(f->source, &err, &newName)) {
        f->error.clear();
        if (!newName.empty())
            f->name = newName;
    } else {
        f->error = err;
    }
}

const ScriptSystem::TypeDoc* ScriptSystem::typeDoc(const std::string& name) const {
    for (const auto& d : typeDocs_)
        if (d.name == name)
            return &d;
    return nullptr;
}

void ScriptSystem::rebuildTypeDocs() {
    typeDocs_.clear();
    auto add = [&](std::string name, std::string base, bool isScript,
                   std::vector<std::string> members) {
        typeDocs_.push_back({std::move(name), std::move(base), isScript, std::move(members)});
    };
    add("int", "", false, {"str"});
    add("float", "", false, {"str"});
    add("bool", "", false, {"str"});
    add("char", "", false, {});
    add("string", "", false, {"length"});
    add("array", "", false, {"length", "add", "str"});
    add("Vector2", "", false, {"x", "y", "str"});
    add("Vector3", "", false, {"x", "y", "z", "str"});
    add("Actor", "", false, {"name", "position", "rotation", "scale", "get_component"});
    add("Actor2D", "Actor", false, {"name", "position", "rotation", "scale", "get_component"});
    add("Actor3D", "Actor", false, {"name", "position", "rotation", "scale", "get_component"});

    for (const auto& [name, ci] : types_) {
        std::vector<std::string> members;
        for (const auto& fd : ci->decl->fields)
            members.push_back(fd.name);
        for (const auto& fn : ci->decl->functions)
            members.push_back(fn.name);
        add(ci->name, ci->base, true, std::move(members));
    }
}

std::vector<std::string> ScriptSystem::completions(const std::string& prefix) const {
    static const char* keywords[] = {"class",  "func",   "var",    "return", "if",     "else",
                                     "switch", "case",   "default", "do",     "do_async", "true",
                                     "false",  "null",   "static", "abstract", "this",  "base",
                                     "break",  "continue"};
    static const char* globals[] = {"print", "type_of", "Vector2", "Vector3"};

    std::vector<std::string> out;
    auto consider = [&](const std::string& s) {
        if (prefix.empty() || s.compare(0, prefix.size(), prefix) == 0)
            out.push_back(s);
    };
    for (const char* k : keywords)
        consider(k);
    for (const char* g : globals)
        consider(g);
    for (const auto& d : typeDocs_)
        consider(d.name);
    return out;
}

} // namespace crate::script
