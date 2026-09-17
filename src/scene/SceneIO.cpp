// Scene (de)serialization -- Phase 1 of the "Scenes" task (see zen.tasks.json,
// Project Management branch): a hand-rolled, line-oriented text format
// matching the project's existing convention of small custom formats over a
// JSON dependency (c.f. AssetDatabase's tab-separated .assetdb).
//
// FILE FORMAT (one directive per line, order matters, no nesting braces):
//   CRATE_SCENE <version>                          -- always first
//   SCENE <name>                                    -- rest of line
//   ACTOR <id> <parentId> <kind> <visible> <enabled> <name>
//       kind: ACTOR | ACTOR2D | ACTOR3D | SPRITE | UI (Actor::typeName()).
//       parentId -1 means "directly under the scene root". Actors are
//       always written in pre-order (a parent's ACTOR line always precedes
//       its children's), so the tree can be rebuilt in one top-to-bottom
//       pass; `id` is a save-time-only local id, unrelated to Actor::id()/
//       auid() in memory.
//   XFORM <px> <py> <pz> <rx> <ry> <rz> <sx> <sy> <sz>
//       Always immediately follows its owning ACTOR line.
//   FIELD <key> <kind> <value...rest of line>
//       Belongs to the most recently opened scope: the current actor's own
//       extra fields (Actor::writeFields/readField) if no COMP has been
//       opened since the last ACTOR line, else the current component
//       (Component::writeFields/readField). `kind` is one of i/f/b/s/n/
//       v2/v3/aref/cref (see scene/FieldCodec.h and script/ScriptFieldIO.h).
//       Reference kinds (aref/cref) are resolved in a second pass, after
//       every actor in the file exists, so forward references work.
//   COMP <enabled> <typeName...rest of line>
//       Starts a new component (constructed via ComponentRegistry::create,
//       so this covers builtin AND script component types identically) on
//       the current actor. typeName may contain spaces ("Mesh Renderer").
//   INSTANCE <id> <parentId> <sourcePath>\t<name>
//       A nested scene instance (see Scene::instantiate() / Actor::
//       isInstanceRoot()), in place of an ACTOR line: `sourcePath` is
//       loaded recursively (so instances can nest arbitrarily deep -- a
//       source that would directly or transitively instance itself is
//       detected and skipped rather than infinite-looping) and its root's
//       CHILDREN become this node's children, rebuilt fresh every load. The
//       node itself still gets its own XFORM line right after, exactly
//       like an ACTOR, since an instance can be repositioned independent
//       of its source. sourcePath/name are tab-separated on one line
//       (rather than two FIELD lines) since both need "rest of line"
//       freedom and neither may contain a tab.
//
// Known limitations (disclosed for the next session, not silently swallowed):
//   - FIELD string values may not contain an embedded newline.
//   - Value::T::Array fields are not yet persisted (written/read as null).
//   - An instance's children are ALWAYS exactly what its source scene
//     currently contains -- no per-instance overrides yet (editing an
//     instanced actor's fields in the Inspector won't persist across a
//     reload) and no adding extra "local" children under an instance root.
//     Both are planned follow-ups (see the Scenes task's Zen notes).
//   - The Asset Browser drag-to-instantiate workflow and the prefab-
//     extraction workflow (drag a node OUT to create a new saved scene)
//     are separate, later increments.

#include "scene/Actor2D.h"
#include "scene/Actor3D.h"
#include "scene/ComponentRegistry.h"
#include "scene/FieldCodec.h"
#include "scene/Scene.h"

#include "core/Log.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace crate {
namespace fs = std::filesystem;
namespace {

constexpr int kSceneFormatVersion = 1;

std::unique_ptr<Actor> makeActorOfKind(const std::string& kind, const std::string& name) {
    if (kind == "ACTOR2D") return std::make_unique<Actor2D>(name);
    if (kind == "ACTOR3D") return std::make_unique<Actor3D>(name);
    if (kind == "SPRITE") return std::make_unique<SpriteActor>(name);
    if (kind == "UI") return std::make_unique<UIControlActor>(name);
    return std::make_unique<Actor>(name);
}

// Splits off the first `n` whitespace-delimited tokens, leaving the
// remainder (if any) verbatim as the final element -- e.g. splitLine(3,
// "FIELD tint v3 1,0.5,0") -> {"FIELD", "tint", "v3", "1,0.5,0"}. A line
// with fewer than `n` tokens gets empty strings for the missing ones.
std::vector<std::string> splitLine(const std::string& line, int n) {
    std::vector<std::string> out;
    size_t pos = 0;
    for (int i = 0; i < n - 1; ++i) {
        while (pos < line.size() && line[pos] == ' ') ++pos;
        size_t start = pos;
        while (pos < line.size() && line[pos] != ' ') ++pos;
        out.push_back(line.substr(start, pos - start));
    }
    while (pos < line.size() && line[pos] == ' ') ++pos;
    out.push_back(pos < line.size() ? line.substr(pos) : std::string());
    return out;
}

// One deferred FIELD line, resolved after the whole tree exists.
struct PendingField {
    Actor* actorTarget = nullptr;   // set when this is an actor-level field
    Component* compTarget = nullptr; // set when this is a component field
    std::string key, kind, value;
};

void writeXform(std::ostream& out, const Transform& t) {
    out << "XFORM " << t.position.x << ' ' << t.position.y << ' ' << t.position.z << ' '
        << t.rotationEuler.x << ' ' << t.rotationEuler.y << ' ' << t.rotationEuler.z << ' '
        << t.scale.x << ' ' << t.scale.y << ' ' << t.scale.z << '\n';
}

void writeActorRecursive(std::ostream& out, const Actor& a, int id, int parentId,
                         std::unordered_map<const Actor*, int>& ids) {
    if (a.isInstanceRoot()) {
        // Short-circuit: no COMP/FIELD/children lines -- the source scene
        // supplies all of that fresh on every load. Only this node's own
        // placement (transform) belongs to the INSTANCING scene.
        out << "INSTANCE " << id << ' ' << parentId << ' ' << a.instanceSource() << '\t' << a.name()
            << '\n';
        writeXform(out, a.transform());
        return;
    }
    out << "ACTOR " << id << ' ' << parentId << ' ' << a.typeName() << ' ' << (a.visible() ? 1 : 0)
        << ' ' << (a.enabled() ? 1 : 0) << ' ' << a.name() << '\n';
    writeXform(out, a.transform());
    a.writeFields(out);
    auto idOf = [&ids](const Actor* p) -> int {
        auto it = ids.find(p);
        return it == ids.end() ? -1 : it->second;
    };
    for (const auto& c : a.components()) {
        out << "COMP " << (c->enabled ? 1 : 0) << ' ' << c->typeName() << '\n';
        c->writeFields(out, idOf);
    }
    for (const auto& child : a.children())
        writeActorRecursive(out, *child, ids.at(child.get()), id, ids);
}

// Resolves two scenes' worth of relative paths to the SAME absolute form
// for cycle comparison -- "Scenes/x.cscene" loaded from two different
// working directories should still be recognized as the same file.
std::string canonicalOrRaw(const std::string& path) {
    std::error_code ec;
    fs::path c = fs::weakly_canonical(path, ec);
    return ec ? path : c.generic_string();
}

} // namespace

bool Scene::save(const std::string& path, std::string* error) const {
    // Pass 1: number every actor (pre-order) before writing anything, so a
    // reference field can point at an actor that will only be WRITTEN later.
    std::unordered_map<const Actor*, int> ids;
    int next = 0;
    // Assign ids in the same pre-order writeActorRecursive will walk.
    std::function<void(const Actor&)> number = [&](const Actor& a) {
        ids[&a] = next++;
        for (const auto& c : a.children())
            number(*c);
    };
    for (const auto& c : root_->children())
        number(*c);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error) *error = "could not open '" + path + "' for writing";
        return false;
    }
    out << "CRATE_SCENE " << kSceneFormatVersion << '\n';
    out << "SCENE " << name_ << '\n';
    for (const auto& c : root_->children())
        writeActorRecursive(out, *c, ids.at(c.get()), -1, ids);
    return true;
}

Scene Scene::loadWithStack(const std::string& path, std::string* error,
                           std::vector<std::string>& stack) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) *error = "could not open '" + path + "'";
        return Scene("Untitled");
    }

    Scene scene("Untitled");
    std::unordered_map<int, Actor*> byId;
    Actor* currentActor = nullptr;
    int currentActorId = -1;
    Component* currentComp = nullptr;
    std::vector<PendingField> pending;

    std::string line;
    bool first = true;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        if (first) {
            first = false;
            if (line.rfind("CRATE_SCENE", 0) != 0) {
                if (error) *error = "not a Crate scene file";
                return Scene("Untitled");
            }
            continue;
        }
        if (line.rfind("SCENE ", 0) == 0) {
            scene.setName(line.substr(6));
        } else if (line.rfind("ACTOR ", 0) == 0) {
            auto tok = splitLine(line.substr(6), 6);
            int id = (int)std::strtol(tok[0].c_str(), nullptr, 10);
            int parentId = (int)std::strtol(tok[1].c_str(), nullptr, 10);
            const std::string& kind = tok[2];
            bool visible = tok[3] == "1";
            bool enabled = tok[4] == "1";
            const std::string& name = tok[5];
            auto actor = makeActorOfKind(kind, name);
            actor->setVisible(visible);
            actor->setEnabled(enabled);
            Actor* parent = parentId < 0 ? nullptr : byId.count(parentId) ? byId[parentId] : nullptr;
            currentActor = scene.add(std::move(actor), parent);
            currentActorId = id;
            byId[id] = currentActor;
            currentComp = nullptr;
        } else if (line.rfind("INSTANCE ", 0) == 0) {
            auto tok = splitLine(line.substr(9), 3);
            int id = (int)std::strtol(tok[0].c_str(), nullptr, 10);
            int parentId = (int)std::strtol(tok[1].c_str(), nullptr, 10);
            size_t tab = tok[2].find('\t');
            std::string sourcePath = tab == std::string::npos ? tok[2] : tok[2].substr(0, tab);
            std::string instName =
                tab == std::string::npos ? std::string() : tok[2].substr(tab + 1);
            std::string canon = canonicalOrRaw(sourcePath);
            currentActor = nullptr;
            currentComp = nullptr;
            if (std::find(stack.begin(), stack.end(), canon) != stack.end()) {
                CR_WARN("scene", "Scene load: '" + sourcePath +
                                     "' would instance itself (directly or transitively) -- skipped");
                continue;
            }
            stack.push_back(canon);
            std::string subError;
            Scene sub = loadWithStack(sourcePath, &subError, stack);
            stack.pop_back();
            if (!subError.empty()) {
                CR_WARN("scene", "Scene load: instance source '" + sourcePath +
                                     "' failed to load: " + subError);
                continue;
            }
            std::unique_ptr<Actor> instanceRoot = std::move(sub.root_);
            instanceRoot->setName(instName.empty() ? fs::path(sourcePath).stem().string() : instName);
            instanceRoot->setInstanceSource(sourcePath);
            Actor* parent = parentId < 0 ? nullptr : byId.count(parentId) ? byId[parentId] : nullptr;
            currentActor = scene.add(std::move(instanceRoot), parent);
            currentActorId = id;
            byId[id] = currentActor;
        } else if (line.rfind("XFORM ", 0) == 0) {
            if (!currentActor) continue;
            std::istringstream ss(line.substr(6));
            Transform t;
            ss >> t.position.x >> t.position.y >> t.position.z >> t.rotationEuler.x >>
                t.rotationEuler.y >> t.rotationEuler.z >> t.scale.x >> t.scale.y >> t.scale.z;
            currentActor->transform() = t;
        } else if (line.rfind("COMP ", 0) == 0) {
            auto tok = splitLine(line.substr(5), 2);
            bool enabled = tok[0] == "1";
            const std::string& typeName = tok[1];
            if (!currentActor) continue;
            auto comp = ComponentRegistry::get().create(typeName);
            if (!comp) {
                CR_WARN("scene", "Scene load: unknown component type '" + typeName + "', skipped");
                currentComp = nullptr;
                continue;
            }
            comp->enabled = enabled;
            currentComp = currentActor->addComponent(std::move(comp));
        } else if (line.rfind("FIELD ", 0) == 0) {
            auto tok = splitLine(line.substr(6), 3);
            PendingField pf;
            pf.key = tok[0];
            pf.kind = tok[1];
            pf.value = tok[2];
            if (currentComp) pf.compTarget = currentComp;
            else if (currentActor) pf.actorTarget = currentActor;
            else continue;
            pending.push_back(std::move(pf));
        }
        // Unrecognized lines are ignored (forward-compat with newer writers).
    }

    auto actorById = [&byId](int id) -> Actor* {
        auto it = byId.find(id);
        return it == byId.end() ? nullptr : it->second;
    };
    for (const auto& pf : pending) {
        if (pf.compTarget)
            pf.compTarget->readField(pf.key, pf.kind, pf.value, actorById);
        else if (pf.actorTarget)
            pf.actorTarget->readField(pf.key, pf.kind, pf.value);
    }

    return scene;
}

Scene Scene::load(const std::string& path, std::string* error) {
    std::vector<std::string> stack{canonicalOrRaw(path)};
    return loadWithStack(path, error, stack);
}

Actor* Scene::instantiate(const std::string& sourcePath, Actor* parent, const std::string& name,
                          std::string* error) {
    std::vector<std::string> stack{canonicalOrRaw(sourcePath)};
    std::string subError;
    Scene sub = loadWithStack(sourcePath, &subError, stack);
    if (!subError.empty()) {
        if (error) *error = subError;
        return nullptr;
    }
    std::unique_ptr<Actor> instanceRoot = std::move(sub.root_);
    instanceRoot->setName(name.empty() ? fs::path(sourcePath).stem().string() : name);
    instanceRoot->setInstanceSource(sourcePath);
    return add(std::move(instanceRoot), parent);
}

} // namespace crate
