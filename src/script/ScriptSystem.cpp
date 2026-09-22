#include "script/ScriptSystem.h"

#include "assets/AssetDatabase.h"
#include "core/Log.h"
#include "input/Input.h"
#include "scene/Actor.h"
#include "scene/BuiltinComponents.h"
#include "scene/ComponentRegistry.h"
#include "scene/Scene.h"
#include "script/Format.h"
#include "script/Lexer.h"
#include "script/NativeClassRegistry.h"
#include "script/NativeScriptComponent.h"
#include "script/ObjectDispatch.h"
#include "script/Parser.h"
#include "script/ScriptComponent.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace crate::script {
namespace fs = std::filesystem;

namespace {
const std::string kDefaultNamespace = "Global";
}

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
            if (auto* sc = dynamic_cast<ScriptComponent*>(c.get())) {
                if (sc->classInfo() &&
                    (sc->classInfo()->name == typeName || sc->classInfo()->isA(typeName)))
                    return sc->object();
                continue;
            }
            // Natively-compiled script components (Phase 9a): matched by
            // the SAME interpreted ClassInfo every script still has
            // (NativeScriptComponent keeps cls_ around purely for this and
            // Inspector purposes) -- once matched, return the instance's
            // OWN canonical selfView_ (Phase 9d) rather than constructing a
            // fresh wrapper ScriptObject here: signals/connections live on
            // a specific ScriptObject, so get_component() must always hand
            // back the SAME one a `this`-originated reference inside that
            // instance would use, or a connection made through one
            // reference would be invisible to an emit through another.
            // This also generalizes the exact pattern the Fog/Camera cases
            // below already use for a raw native pointer. FIXES a real
            // pre-existing bug: before Phase 9a, get_component() silently
            // never found a native script component at all (only
            // ScriptComponent was ever checked).
            if (auto* nsc = dynamic_cast<NativeScriptComponent*>(c.get())) {
                if (nsc->classInfo() &&
                    (nsc->classInfo()->name == typeName || nsc->classInfo()->isA(typeName))) {
                    auto view = nsc->ensureNativeView();
                    if (view.instance && view.classInfo && view.classInfo->selfView)
                        if (auto self = view.classInfo->selfView(view.instance))
                            return self;
                }
            }
        }
        // Native (non-script) components: get_component(type_of(Fog)) returns
        // a live view onto the real component (see Interpreter's
        // getNativeField/setNativeField), not a copy -- add a case here for
        // each native component type that should be reachable this way.
        if (typeName == "Fog") {
            if (auto* fc = a->getComponent<FogComponent>()) {
                auto o = std::make_shared<ScriptObject>();
                o->builtin = "Fog";
                o->nativePtr = fc;
                o->owner = a;
                return o;
            }
        }
        if (typeName == "Camera") {
            if (auto* cc = a->getComponent<CameraComponent>()) {
                auto o = std::make_shared<ScriptObject>();
                o->builtin = "Camera";
                o->nativePtr = cc;
                o->owner = a;
                return o;
            }
        }
        // The actor's own Transform isn't a Component at all, but is
        // reachable the same way for consistency (get_component(type_of(
        // Transform)), and the Inspector's generic component picker).
        if (typeName == "Transform") {
            auto o = std::make_shared<ScriptObject>();
            o->builtin = "Transform";
            o->nativePtr = &a->transform();
            o->owner = a;
            return o;
        }
        if (typeName == "Light") {
            if (auto* lc = a->getComponent<LightComponent>()) {
                auto o = std::make_shared<ScriptObject>();
                o->builtin = "Light";
                o->nativePtr = lc;
                o->owner = a;
                return o;
            }
        }
        if (typeName == "VolumetricFog") {
            if (auto* vf = a->getComponent<VolumetricFogComponent>()) {
                auto o = std::make_shared<ScriptObject>();
                o->builtin = "VolumetricFog";
                o->nativePtr = vf;
                o->owner = a;
                return o;
            }
        }
        if (typeName == "MeshRenderer") {
            if (auto* mr = a->getComponent<MeshRenderer>()) {
                auto o = std::make_shared<ScriptObject>();
                o->builtin = "MeshRenderer";
                o->nativePtr = mr;
                o->owner = a;
                return o;
            }
        }
        if (typeName == "LightProbe") {
            if (auto* lp = a->getComponent<LightProbeComponent>()) {
                auto o = std::make_shared<ScriptObject>();
                o->builtin = "LightProbe";
                o->nativePtr = lp;
                o->owner = a;
                return o;
            }
        }
        if (typeName == "Spinner") {
            if (auto* sp = a->getComponent<SpinnerComponent>()) {
                auto o = std::make_shared<ScriptObject>();
                o->builtin = "Spinner";
                o->nativePtr = sp;
                o->owner = a;
                return o;
            }
        }
        return nullptr;
    };
    ctx_.getStatic = [this](const std::string& name) -> std::shared_ptr<ScriptObject> {
        auto it = statics_.find(name);
        return it == statics_.end() ? nullptr : it->second;
    };
    ctx_.inputButton = [this](const std::string& name) { return inputButton(name); };
    ctx_.inputQuery = [](const std::string& name, int what) -> double {
        auto& in = crate::Input::get();
        switch (what) {
            case 0: return in.pressed(name) ? 1.0 : 0.0;
            case 1: return in.justPressed(name) ? 1.0 : 0.0;
            case 2: return in.justReleased(name) ? 1.0 : 0.0;
            case 3: return in.axis(name).x;
            case 4: return in.axis(name).y;
            case 5: return in.mouseDelta().x;
            case 6: return in.mouseDelta().y;
            case 7: return in.scrollDelta().x;
            case 8: return in.scrollDelta().y;
        }
        return 0.0;
    };
    ctx_.instantiateScene = [](const std::string& path, crate::Actor* parent) -> crate::Actor* {
        std::string err;
        return Scene::instantiateUnder(parent, path, std::string(), &err);
    };
    ctx_.destroyActor = [this](crate::Actor* a) {
        if (a) pendingDestroy_.push_back(a);
    };
    ctx_.removeComponent = [this](crate::Actor* a, const std::shared_ptr<ScriptObject>& obj) {
        if (!a || !obj)
            return;
        // Native builtin component view: nativePtr IS directly the
        // Component* (every builtin type except "Transform", which isn't a
        // real removable Component -- nativePtr there is a raw Transform*).
        if (obj->nativePtr && !obj->cls && !obj->compiledInfo && obj->builtin != "Transform") {
            pendingComponentRemove_.emplace_back(a, static_cast<Component*>(obj->nativePtr));
            return;
        }
        // Script instance: find the wrapper Component on `a` whose OWN
        // ScriptObject identity matches (works for both interpreted and
        // natively-compiled script components).
        for (const auto& c : a->components()) {
            if (auto* sc = dynamic_cast<ScriptComponent*>(c.get())) {
                if (sc->object() == obj) {
                    pendingComponentRemove_.emplace_back(a, sc);
                    return;
                }
            } else if (auto* nc = dynamic_cast<NativeScriptComponent*>(c.get())) {
                if (nc->object() == obj) {
                    pendingComponentRemove_.emplace_back(a, nc);
                    return;
                }
            }
        }
    };
    ctx_.addComponent = [this](crate::Actor* a, const std::string& typeName) -> std::shared_ptr<ScriptObject> {
        if (!a || typeName.empty())
            return nullptr;
        auto comp = ComponentRegistry::get().create(typeName);
        if (!comp)
            return nullptr;
        a->addComponent(std::move(comp));
        return ctx_.getComponent ? ctx_.getComponent(a, typeName) : nullptr;
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

        const std::string cname = decl->name;

        // Recompile IN PLACE when the class already exists: the ClassInfo object
        // (and thus every pointer the registry lambdas and attached
        // ScriptComponents hold) stays valid; only its AST is swapped.
        ClassInfo* slot = nullptr;
        auto existing = types_.find(cname);
        if (existing != types_.end()) {
            slot = existing->second.get();
        } else {
            auto up = std::make_unique<ClassInfo>();
            slot = up.get();
            types_[cname] = std::move(up);
        }
        slot->name = decl->name;
        slot->base = decl->base;
        slot->isStatic = decl->isStatic;
        slot->isAbstract = decl->isAbstract;
        slot->decl = std::move(decl);
        slot->indexFunctions();
        ++slot->generation;

        if (nameOut)
            *nameOut = cname;
        bool wasStatic = slot->isStatic;
        resolveBases();
        // static / abstract classes cannot be added to actors as components
        if (!wasStatic && !slot->isAbstract)
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
    ComponentRegistry::get().add(
        className, "Scripts",
        [ctx, cls] { return std::make_unique<ScriptComponent>(ctx, cls); }, /*replace=*/true);
}

void ScriptSystem::reload() {
    if (dir_.empty())
        return;
    loadFolder(dir_); // new + changed files

    std::error_code ec;
    for (auto it = files_.begin(); it != files_.end();) {
        if (!it->path.empty() && !fs::exists(it->path, ec)) {
            const std::string name = it->name;
            CR_LOG("script", "Script deleted: " + name);
            if (auto node = types_.find(name); node != types_.end()) {
                retired_.push_back(std::move(node->second)); // keep alive for attached components
                types_.erase(node);
            }
            statics_.erase(name);
            ComponentRegistry::get().remove(name);
            if (namespaces_.erase(name) > 0)
                saveNamespaces();
            it = files_.erase(it);
        } else {
            ++it;
        }
    }
    resolveBases();
    rebuildTypeDocs();
}

void ScriptSystem::destroyNativeStaticIfAny(const std::string& className) {
    auto it = nativeStatics_.find(className);
    if (it == nativeStatics_.end())
        return;
    if (it->second.instance && it->second.destroy)
        it->second.destroy(it->second.instance);
    nativeStatics_.erase(it);
}

void ScriptSystem::rebuildStatic(const std::string& className) {
    auto it = types_.find(className);
    if (it == types_.end() || !it->second->isStatic)
        return;
    // Always drop any existing native instance first -- whether we're
    // about to construct a fresh native one (a rebuild) or fall back to
    // interpreted (its namespace just unloaded), the OLD native instance
    // must never survive this call, or a namespace unload would leave
    // nativeStatics_ pointing at soon-to-be-freed DLL memory.
    destroyNativeStaticIfAny(className);
    if (const NativeClassExport* exp = NativeClassRegistry::get().find(className); exp && exp->isStatic) {
        void* inst = exp->createStatic(&ctx_);
        const CompiledClassInfo* ci = exp->classInfo();
        nativeStatics_[className] = {inst, exp->destroyStatic};
        statics_[className] = (ci && ci->selfView) ? ci->selfView(inst) : nullptr;
        return;
    }
    statics_[className] = Interpreter::instantiate(&ctx_, it->second.get(), nullptr);
}

void ScriptSystem::resetStatics() {
    for (const auto& [name, ci] : types_)
        if (ci->isStatic)
            rebuildStatic(name);
}

void ScriptSystem::flushPending(crate::Scene& scene) {
    // Component removals first. Skip any whose OWNING ACTOR is also queued
    // for destruction this flush -- destroying the actor already takes its
    // components with it, so removing one individually first would just be
    // touching memory that's about to go away anyway (still safe, since
    // component removal alone doesn't invalidate the actor, but redundant).
    std::unordered_set<crate::Actor*> toDestroy(pendingDestroy_.begin(), pendingDestroy_.end());
    for (auto& [actor, comp] : pendingComponentRemove_)
        if (!toDestroy.count(actor))
            actor->removeComponent(comp);
    pendingComponentRemove_.clear();

    // Actor destruction: drop any entry that is a descendant of ANOTHER
    // still-pending entry (destroying the ancestor already takes it with
    // it) -- computed BEFORE any destruction happens, while every parent-
    // chain pointer in the batch is still valid to walk.
    std::vector<crate::Actor*> roots;
    for (crate::Actor* a : pendingDestroy_) {
        bool nested = false;
        for (crate::Actor* other : pendingDestroy_)
            if (other != a && a->isDescendantOf(other)) {
                nested = true;
                break;
            }
        if (!nested)
            roots.push_back(a);
    }
    pendingDestroy_.clear();
    std::unordered_set<crate::Actor*> destroyed; // destroy() called twice on the same actor
    for (crate::Actor* a : roots) {
        if (!destroyed.insert(a).second)
            continue;
        scene.remove(a); // clears a dangling Inspector selection, logs it, then frees the subtree
    }
}

std::shared_ptr<ScriptObject> ScriptSystem::inputButton(const std::string& name) {
    auto& obj = inputButtons_[name];
    if (!obj) {
        obj = std::make_shared<ScriptObject>();
        obj->builtin = "InputButton";
    }
    return obj;
}

void ScriptSystem::dispatchInput() {
    for (const auto& ev : crate::Input::get().events()) {
        auto it = inputButtons_.find(ev.button);
        if (it == inputButtons_.end())
            continue;
        const char* sig = ev.kind == 1 ? "just_pressed" : ev.kind == 2 ? "just_released" : "pressed";
        auto cit = it->second->connections.find(sig);
        if (cit == it->second->connections.end())
            continue;
        for (const auto& cb : std::vector<Value>(cit->second)) {
            auto self = cb.wobj.lock();
            if (!self)
                continue;
            try {
                if (self->cls) {
                    // Unchanged: resumable (do_async inside a handler can
                    // frame-step across calls), exactly as before Phase 9d.
                    Interpreter(&ctx_, self).call(cb.s);
                } else if (self->nativePtr && self->compiledInfo) {
                    // A NATIVELY-COMPILED handler (Phase 9d) -- REAL,
                    // PRE-EXISTING BUG FIX: before this, an InputButton
                    // signal connected to a compiled script's method was a
                    // silent no-op (Interpreter::call() bails out
                    // immediately when self_->cls is null, which is always
                    // true for a compiled target). Non-resumable, matching
                    // every other cross-object call onto a compiled
                    // instance -- only a generated class's OWN start/
                    // update/physics_update hooks ever get resumable
                    // treatment (see CodeGen.cpp's emitHookBody).
                    crate::script::callObjectMethod(&ctx_, self, cb.s, {}, 0);
                }
            } catch (const std::exception& ex) {
                CR_ERROR("script", std::string("input signal handler: ") + ex.what());
            }
        }
    }
}

void ScriptSystem::resetInput() {
    for (auto& [name, obj] : inputButtons_)
        obj->connections.clear();
}

namespace {
// Calls a lifecycle hook on a static's ScriptObject, whichever kind it is
// (Phase 9e) -- REAL BUG FIX, same class as dispatchInput()'s (Phase 9d):
// before this, a NATIVE static's start/update/physics_update NEVER ran at
// all, since Interpreter::call() silently bails out (self_->cls == nullptr
// for a compiled instance) instead of dispatching through the reflection
// table. A missing hook is a silent no-op for EITHER kind, matching
// Interpreter::call()'s own required=false default and CodeGen's
// hasStart/hasUpdate/hasPhysicsUpdate-gated overrides.
void callStaticHook(ScriptContext* ctx, const std::shared_ptr<ScriptObject>& obj,
                    const std::string& method, std::vector<Value> args) {
    if (!obj)
        return;
    if (obj->cls) {
        Interpreter(ctx, obj).call(method, std::move(args)); // unchanged: resumable
        return;
    }
    if (obj->nativePtr && obj->compiledInfo) {
        const CompiledClassInfo* ci = obj->compiledInfo;
        for (size_t i = 0; i < ci->methodCount; ++i)
            if (ci->methods[i].name == method) {
                ci->methods[i].invoke(obj->nativePtr, ctx, std::move(args));
                return;
            }
        // not declared -- silent no-op, matching the interpreted path
    }
}
} // namespace

void ScriptSystem::startStatics() {
    resetStatics();
    for (auto& [name, obj] : statics_) {
        try {
            callStaticHook(&ctx_, obj, "start", {});
        } catch (const std::exception& ex) {
            CR_ERROR("script", name + ".start (static): " + ex.what());
        }
    }
}

void ScriptSystem::tickStatics(float dt) {
    for (auto& [name, obj] : statics_) {
        try {
            callStaticHook(&ctx_, obj, "update", {Value::Float(dt)});
        } catch (const std::exception& ex) {
            CR_ERROR("script", name + ".update (static): " + ex.what());
        }
    }
}

void ScriptSystem::physicsStatics(float dt) {
    for (auto& [name, obj] : statics_) {
        try {
            callStaticHook(&ctx_, obj, "physics_update", {Value::Float(dt)});
        } catch (const std::exception&) {
        }
    }
}

const std::string& ScriptSystem::namespaceOf(const std::string& className) const {
    auto it = namespaces_.find(className);
    return it == namespaces_.end() ? kDefaultNamespace : it->second;
}

void ScriptSystem::setNamespace(const std::string& className, std::string ns) {
    // Storing an explicit "Global" entry would just duplicate the default,
    // so treat empty/"Global" as "no override" and drop any existing entry
    // instead -- keeps the sidecar file minimal, same spirit as
    // AssetDatabase only persisting what it actually needs to.
    if (ns.empty() || ns == kDefaultNamespace)
        namespaces_.erase(className);
    else
        namespaces_[className] = std::move(ns);
    saveNamespaces();
}

void ScriptSystem::loadNamespaces() {
    namespaces_.clear();
    if (dir_.empty())
        return;
    std::ifstream in(dir_ + "/.scriptmeta", std::ios::binary);
    if (!in)
        return;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        auto tab = line.find('\t');
        if (tab == std::string::npos)
            continue;
        std::string cname = line.substr(0, tab);
        std::string ns = line.substr(tab + 1);
        if (cname.empty() || ns.empty())
            continue;
        namespaces_[cname] = ns;
    }
}

void ScriptSystem::saveNamespaces() const {
    if (dir_.empty())
        return;
    std::ofstream out(dir_ + "/.scriptmeta", std::ios::binary | std::ios::trunc);
    if (!out) {
        CR_ERROR("script", "Could not write script metadata: " + dir_ + "/.scriptmeta");
        return;
    }
    for (const auto& [cname, ns] : namespaces_)
        out << cname << '\t' << ns << '\n';
}

void ScriptSystem::unloadAll() {
    types_.clear();
    namespaces_.clear();
    retired_.clear();
    inputButtons_.clear();
    files_.clear();
    typeDocs_.clear();
    dir_.clear();
}

void ScriptSystem::loadFolder(const std::string& dir) {
    dir_ = dir;
    loadNamespaces();
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

std::string ScriptSystem::newScript(const std::string& className) {
    if (dir_.empty())
        dir_ = "assets";
    // New scripts always land in a "scripts" subfolder of the project's
    // asset root for tidiness, even though dir_ itself is now that whole
    // root (task 132: the editor is aware of .cscript files anywhere under
    // it, but that doesn't mean newly-created ones should scatter loose in
    // the root).
    const std::string newScriptDir = dir_ + "/scripts";
    std::error_code ec;
    fs::create_directories(newScriptDir, ec);

    // Sanitize to a valid class identifier.
    std::string base;
    for (char c : className)
        if (std::isalnum((unsigned char)c) || c == '_')
            base += c;
    if (base.empty() || std::isdigit((unsigned char)base[0]))
        base = "Script" + base;
    if (std::islower((unsigned char)base[0]))
        base[0] = (char)std::toupper((unsigned char)base[0]);

    // Make it unique.
    std::string name = base;
    for (int n = 2;
         (types_.count(name) || fs::exists(fs::path(newScriptDir) / (name + ".cscript"), ec));
         ++n)
        name = base + std::to_string(n);

    const std::string tpl =
        "class " + name + " : Actor\n"
        "{\n"
        "\tfunc start()\n"
        "\t{\n"
        "\t}\n"
        "\n"
        "\tfunc update(float delta)\n"
        "\t{\n"
        "\t}\n"
        "\n"
        "\tfunc physics_update(float delta)\n"
        "\t{\n"
        "\t}\n"
        "}\n";

    const std::string path = (fs::path(newScriptDir) / (name + ".cscript")).string();
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        CR_ERROR("script", "Could not create " + path);
        return {};
    }
    out << tpl;
    out.close();

    std::string err;
    if (!compile(tpl, &err)) {
        CR_ERROR("script", "New script failed to compile: " + err);
        return {};
    }
    ScriptFile f;
    f.path = path;
    f.name = name;
    f.source = tpl;
    files_.push_back(std::move(f));
    AssetDatabase::get().idFor(fs::path(path).generic_string());
    CR_LOG("script", "Created script '" + name + "'");
    return name;
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

std::string ScriptSystem::setSource(const std::string& name, std::string source) {
    ScriptFile* f = file(name);
    if (!f)
        return {};
    const std::string oldName = f->name;
    f->source = std::move(source);
    f->dirty = true;
    std::string err, newName;
    if (compile(f->source, &err, &newName)) {
        f->error.clear();
        if (!newName.empty() && newName != oldName) {
            f->name = newName;
            // The class was renamed: retire the old type and drop its stale
            // entry from the Add Component menu.
            if (auto it = types_.find(oldName); it != types_.end()) {
                retired_.push_back(std::move(it->second));
                types_.erase(it);
            }
            statics_.erase(oldName);
            ComponentRegistry::get().remove(oldName);
            // Carry an explicit namespace override across the rename too.
            if (auto nsIt = namespaces_.find(oldName); nsIt != namespaces_.end()) {
                namespaces_[newName] = std::move(nsIt->second);
                namespaces_.erase(nsIt);
                saveNamespaces();
            }
            resolveBases();
            rebuildTypeDocs();
            CR_LOG("script", "Script renamed " + oldName + " -> " + newName);
        }
    } else {
        f->error = err;
    }
    return f->name;
}

const ScriptSystem::TypeDoc* ScriptSystem::typeDoc(const std::string& name) const {
    for (const auto& d : typeDocs_)
        if (d.name == name)
            return &d;
    return nullptr;
}

// Marks a member as a callable function, purely for autocomplete's "insert
// ()" behavior (task 126) and the completion list's own display -- ANY
// member listed with this suffix auto-inserts its parens on completion,
// generically, via the same code path in ScriptEditor::updateAutocomplete()
// that handles every other member. This is the one place that decides
// "is this callable"; nothing downstream special-cases a member by name (see
// that function's own comment). TypeDoc::members is consumed ONLY here and
// in ScriptEditor.cpp's two completion-building loops -- never for real type
// resolution (see builtinMemberType()/resolveChainType(), which use their
// own separate logic) -- so this cosmetic suffix can never affect runtime
// behavior.
static std::string asFn(const std::string& name) { return name + "()"; }

void ScriptSystem::rebuildTypeDocs() {
    typeDocs_.clear();
    auto add = [&](std::string name, std::string base, bool isScript,
                   std::vector<std::string> members) {
        typeDocs_.push_back({std::move(name), std::move(base), isScript, std::move(members)});
    };
    add("int", "", false, {asFn("str")});
    add("float", "", false, {asFn("str")});
    add("bool", "", false, {asFn("str")});
    add("char", "", false, {});
    add("string", "", false, {"length"});
    add("array", "", false, {"length", asFn("add"), asFn("str")});
    add("Vector2", "", false, {"x", "y", asFn("str"), asFn("normalize")});
    add("Vector3", "", false, {"x", "y", "z", asFn("str"), asFn("normalize")});
    add("Transform", "", false, {"position", "rotation", "scale", "forward", "right", "up"});
    add("Actor", "", false,
        {"name", "position", "rotation", "scale", "forward", "right", "up", asFn("get_component"),
         asFn("add_component"), asFn("destroy")});
    add("Actor2D", "Actor", false,
        {"name", "position", "rotation", "scale", "forward", "right", "up", asFn("get_component"),
         asFn("add_component"), asFn("destroy")});
    add("Actor3D", "Actor", false,
        {"name", "position", "rotation", "scale", "forward", "right", "up", asFn("get_component"),
         asFn("add_component"), asFn("destroy")});
    // Not spawnable/constructible from script -- only reachable via
    // get_component(type_of(Fog)). Listed so its members show up in
    // completions once you're chained off that call. Every native
    // component view also reaches its OWNING ACTOR's Transform via
    // `.transform` (getNativeField handles this generically for all of
    // them), so "transform" is listed on every one below too.
    add("Fog", "", false, {"color", "start", "end", "height_range", "transform", asFn("remove")});
    // "Camera" is also reachable as a bare global for Camera.main (task 77):
    // the scene's one active camera, readable and assignable to switch it.
    add("Camera", "", false, {"main", "fov", "near", "far", "active", "transform", asFn("remove")});
    // Reachable via get_component(type_of(Light)) / drag-drop onto a field
    // declared with this type -- see ScriptSystem's ctx_.getComponent.
    add("Light", "", false,
        {"type", "color", "intensity", "range", "spot_inner_deg", "spot_outer_deg", "is_static",
         "transform", asFn("remove")});
    add("VolumetricFog", "", false, {"color", "density", "transform", asFn("remove")});
    add("MeshRenderer", "", false,
        {"tint", "cast_shadows", "receive_shadows", "transform", asFn("remove")});
    add("LightProbe", "", false, {"baked_light", "baked_valid", "transform", asFn("remove")});
    add("Spinner", "", false, {"degrees_per_second", "axis", "transform", asFn("remove")});
    // Not a component -- an ASSET reference (a saved .cscene path), for a
    // field declared `Scene`. Drag a scene tile from the Asset Browser onto
    // it in the Inspector; .instantiate() spawns a live instance at runtime
    // (Scenes task; see Runtime.h's valueInstantiate).
    add("Scene", "", false, {asFn("instantiate")});
    // Global helper namespace, reached as Math.<fn>(...) (Interpreter::
    // evalCall's mathCall branch) -- registered here purely for
    // autocomplete visibility, which it never had before (Math wasn't in
    // any typeDoc/globals list at all, so Math.-prefixed calls -- including
    // ones that already existed, like clamp/lerp/sin/rand_i_range -- were
    // simply undiscoverable via Ctrl+Space).
    add("Math", "", false,
       {asFn("clamp"), asFn("lerp"), asFn("slerp"), asFn("sin"), asFn("cos"), asFn("tan"),
        asFn("sqrt"), asFn("exp"), asFn("pow"), asFn("abs"), asFn("floor"), asFn("ceil"),
        asFn("round"), asFn("min"), asFn("max"), asFn("deg2rad"), asFn("rad2deg"), asFn("rand_f"),
        asFn("rand_i"), asFn("rand_f_range"), asFn("rand_i_range")});
    // Global namespace reached as Input.<method>(...), handled directly in
    // Interpreter::evalCall rather than through get_component -- listed here
    // purely so its methods show up in autocomplete (task: "Input global
    // missing from script autocomplete").
    // Mouse buttons use the same get_button/is_pressed/is_just_pressed/
    // is_just_released API under reserved names "mouse_left"/"mouse_right"/
    // "mouse_middle" (e.g. Input.is_pressed("mouse_left"),
    // Input.get_button("mouse_left").just_pressed.connect(...)). Delta/
    // scroll/per-button convenience booleans live at Input.Mouse.<member>
    // (a member-access chain, not a method call -- see Interpreter::
    // evalMember / CodeGen's identical structural check).
    add("Input", "", false,
       {asFn("get_button"), asFn("get_axis"), asFn("is_pressed"), asFn("is_just_pressed"),
        asFn("is_just_released"), "Mouse"});
    add("InputMouse", "", false,
       {"delta", "scroll", "left_down", "left_just_down", "left_just_up", "right_down",
        "right_just_down", "right_just_up", "middle_down", "middle_just_down", "middle_just_up"});

    for (const auto& [name, ci] : types_) {
        std::vector<std::string> members;
        for (const auto& fd : ci->decl->fields)
            members.push_back(fd.name);
        for (const auto& fn : ci->decl->functions)
            members.push_back(asFn(fn.name));
        add(ci->name, ci->base, true, std::move(members));
    }
}

std::vector<std::string> ScriptSystem::completions(const std::string& prefix) const {
    static const char* keywords[] = {"class",  "func",   "var",    "return", "if",     "else",
                                     "switch", "case",   "default", "do",     "do_async", "true",
                                     "false",  "null",   "static", "abstract", "this",  "base",
                                     "break",  "continue"};
    static const char* globals[] = {"print()", "type_of()", "Vector2", "Vector3", "transform",
                                    "actor", "Camera",  "Input",   "get_root()", "Math"};

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
