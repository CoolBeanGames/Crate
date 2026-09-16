// Coverage for src/script/ObjectDispatch.h/.cpp (transpiration.txt,
// "Transplation" Phase 9a): the generalized getObjectMember/
// trySetObjectMember/callObjectMethod dispatch that works uniformly across
// all four kinds of ScriptObject a Value::Object can wrap:
//   1. an interpreted script instance (cls != nullptr)
//   2. a BuiltinComponent live view, e.g. Fog/Camera (nativePtr set, no compiledInfo)
//   3. a compiled script instance live view (nativePtr + compiledInfo set)
//   4. a plain aggregate with no cls at all, e.g. Vector2/Vector3 (fields map only)
//
// Also the dedicated regression test for the real, pre-existing bug found
// while auditing for this phase: ScriptSystem's get_component() never
// recognized a NativeScriptComponent at all (only ScriptComponent), so it
// silently failed to find any native-backed component -- from BOTH
// interpreted and native calling code.
//
// Skips the compiler-dependent (Kind 3 / native) checks gracefully without
// a discoverable MSVC toolchain; Kinds 1/2/4 need no compiler at all and
// always run.
//
// No framework: asserts + a pass counter, run via CTest.

#include "editor/ScriptBuild.h"
#include "scene/Actor3D.h"
#include "scene/BuiltinComponents.h"
#include "scene/ComponentRegistry.h"
#include "scene/Scene.h"
#include "script/NativeClassRegistry.h"
#include "script/NativeScriptComponent.h"
#include "script/ObjectDispatch.h"
#include "script/ScriptComponent.h"
#include "script/ScriptSystem.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace crate;
using namespace crate::script;
using namespace crate::editor::scriptbuild;
namespace fs = std::filesystem;

#ifndef CRATE_SCRIPTS_DIR
#define CRATE_SCRIPTS_DIR "."
#endif

static int g_checks = 0;
#define CHECK(cond)                                                                   \
    do {                                                                             \
        ++g_checks;                                                                  \
        if (!(cond)) {                                                               \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);              \
            return 1;                                                                \
        }                                                                           \
    } while (0)

#define CHECK_THROWS(expr)                                                           \
    do {                                                                             \
        ++g_checks;                                                                  \
        bool threw = false;                                                         \
        try {                                                                        \
            (void)(expr);                                                           \
        } catch (const RuntimeError&) {                                             \
            threw = true;                                                          \
        }                                                                            \
        if (!threw) {                                                               \
            std::printf("FAIL %s:%d  expected RuntimeError from: %s\n", __FILE__,   \
                        __LINE__, #expr);                                          \
            return 1;                                                              \
        }                                                                           \
    } while (0)

// ---------------------------------------------------------------------------
// Kind 4: plain aggregate (Vector2/Vector3) -- no cScript compilation needed.
// ---------------------------------------------------------------------------
static int testKind4Aggregate() {
    Value v3 = makeVector("Vector3", 1.0, 2.0, 3.0);
    CHECK(getObjectMember(v3.obj, "x", 1).f == 1.0);
    CHECK(getObjectMember(v3.obj, "y", 1).f == 2.0);
    CHECK(getObjectMember(v3.obj, "z", 1).f == 3.0);
    CHECK_THROWS(getObjectMember(v3.obj, "bogus", 1));

    CHECK(trySetObjectMember(v3.obj, "y", Value::Float(99.0)));
    CHECK(getObjectMember(v3.obj, "y", 1).f == 99.0);
    // Implicit creation of a new field on a Kind-4 object -- matches
    // Interpreter::lvalue()'s unconditional `fields[name]` semantics.
    CHECK(trySetObjectMember(v3.obj, "w", Value::Int(7)));
    CHECK(getObjectMember(v3.obj, "w", 1).i == 7);

    // Kind 4 objects never have methods to call (only the universal .str(),
    // which is handled by the CALLER, not callObjectMethod).
    CHECK_THROWS(callObjectMethod(nullptr, v3.obj, "nonexistent", {}, 1));

    std::printf("ok  Kind 4 (plain aggregate / Vector3) get/set via ObjectDispatch\n");
    return 0;
}

// ---------------------------------------------------------------------------
// Kind 2: BuiltinComponent live view (Fog).
// ---------------------------------------------------------------------------
static int testKind2BuiltinComponent() {
    FogComponent fog;
    fog.start = 5.0f;

    auto view = std::make_shared<ScriptObject>();
    view->builtin = "Fog";
    view->nativePtr = &fog;

    CHECK(getObjectMember(view, "start", 1).f == 5.0);
    CHECK(trySetObjectMember(view, "start", Value::Float(9.0)));
    CHECK(fog.start == 9.0f);
    CHECK_THROWS(getObjectMember(view, "bogus", 1));
    CHECK(!trySetObjectMember(view, "bogus", Value::Int(1)));

    std::printf("ok  Kind 2 (BuiltinComponent / Fog) get/set via ObjectDispatch\n");
    return 0;
}

// ---------------------------------------------------------------------------
// Kind 1: interpreted script instance.
// ---------------------------------------------------------------------------
static int testKind1Interpreted() {
    auto& sys = ScriptSystem::get();
    std::string err;
    CHECK(sys.compile("class OdTestInterp : Actor3D { var n = 5; func double_it() { return n * 2; } }",
                      &err));
    const ClassInfo* cls = sys.types().at("OdTestInterp").get();

    Actor3D actor("subject");
    auto obj = Interpreter::instantiate(&sys.context(), cls, &actor);

    CHECK(getObjectMember(obj, "n", 1).i == 5);
    CHECK(trySetObjectMember(obj, "n", Value::Int(10)));
    CHECK(getObjectMember(obj, "n", 1).i == 10);
    // Implicit field creation on an interpreted instance.
    CHECK(trySetObjectMember(obj, "extra", Value::Str("hi")));
    CHECK(getObjectMember(obj, "extra", 1).s == "hi");
    // "actor" magic member.
    CHECK(getObjectMember(obj, "actor", 1).t == Value::T::Actor);
    CHECK(getObjectMember(obj, "actor", 1).actor == &actor);
    // Bound method reference.
    Value fn = getObjectMember(obj, "double_it", 1);
    CHECK(fn.t == Value::T::Callable);

    Value result = callObjectMethod(&sys.context(), obj, "double_it", {}, 1);
    CHECK(result.i == 20); // n was set to 10 above
    CHECK_THROWS(callObjectMethod(&sys.context(), obj, "no_such_method", {}, 1));
    CHECK_THROWS(getObjectMember(obj, "no_such_member_and_not_a_field", 1));

    std::printf("ok  Kind 1 (interpreted script instance) get/set/call via ObjectDispatch\n");
    return 0;
}

// ---------------------------------------------------------------------------
// Kind 3 (compiled) + the get_component regression test. Both need a real
// build, so share one Harness.
// ---------------------------------------------------------------------------
namespace {
struct Harness {
    ScriptSystem& sys;
    std::string dir, ns, path;
    bool ok = false;

    Harness(const std::string& className, const std::string& source)
        : sys(ScriptSystem::get()), dir(sys.scriptsDir()), ns("OdTestNamespace"),
          path(sys.scriptsDir() + "/" + className + ".cscript") {
        {
            std::ofstream o(path, std::ios::binary);
            o << source;
        }
        sys.reload();
        if (!sys.file(className))
            return;
        sys.setNamespace(className, ns);
        ok = true;
    }

    ~Harness() {
        NativeClassRegistry::get().unloadAll();
        std::remove(path.c_str());
        sys.reload();
        std::error_code ec;
        fs::remove_all(dir + "/.crate_build/gen/" + ns, ec);
        std::error_code ec2;
        for (const auto& entry : fs::directory_iterator(dir + "/.crate_build/bin", ec2)) {
            if (ec2)
                break;
            if (entry.path().filename().string().rfind(ns + "_v", 0) == 0)
                fs::remove(entry.path(), ec);
        }
        const std::string manifestPath = dir + "/.crate_build/manifest.tsv";
        std::ifstream in(manifestPath, std::ios::binary);
        if (in) {
            std::string kept, line;
            while (std::getline(in, line))
                if (line.rfind(ns + "\t", 0) != 0)
                    kept += line + "\n";
            in.close();
            std::ofstream out(manifestPath, std::ios::binary | std::ios::trunc);
            out << kept;
        }
    }
};
} // namespace

static int testKind3CompiledAndGetComponentRegression() {
    if (!ToolchainEnv::get().available()) {
        std::printf("(no MSVC toolchain found -- Kind 3 / get_component regression checks "
                    "skipped: %s)\n",
                    ToolchainEnv::get().error().c_str());
        return 0;
    }

    Harness h("OdTestNative",
             "class OdTestNative : Actor3D { var n = 5; func double_it() { return n * 2; } }");
    CHECK(h.ok);

    BuildResult build = buildNamespace(h.ns, h.dir);
    if (!build.ok)
        std::printf("  buildNamespace error: %s\n", build.error.c_str());
    CHECK(build.ok);
    CHECK(NativeClassRegistry::get().loadNamespace(h.ns, build.dllPath, {"OdTestNative"}));

    {
        Scene scene("t");
        Actor* a = scene.add(std::make_unique<Actor3D>("Subject"));
        auto comp = ComponentRegistry::get().create("OdTestNative");
        CHECK(comp != nullptr);
        auto* nc = dynamic_cast<NativeScriptComponent*>(comp.get());
        CHECK(nc != nullptr);
        a->addComponent(std::move(comp));

        // ---- direct Kind 3 dispatch test -----------------------------
        auto view = nc->ensureNativeView();
        CHECK(view.instance != nullptr);
        CHECK(view.classInfo != nullptr);
        auto liveObj = std::make_shared<ScriptObject>();
        liveObj->nativePtr = view.instance;
        liveObj->compiledInfo = view.classInfo;
        liveObj->owner = a;

        CHECK(getObjectMember(liveObj, "n", 1).i == 5);
        CHECK(trySetObjectMember(liveObj, "n", Value::Int(10)));
        CHECK(getObjectMember(liveObj, "n", 1).i == 10);
        CHECK(trySetObjectMember(liveObj, "made_up", Value::Str("hi"))); // overflow map
        CHECK(getObjectMember(liveObj, "made_up", 1).s == "hi");
        CHECK(getObjectMember(liveObj, "actor", 1).actor == a);

        Value result = callObjectMethod(&h.sys.context(), liveObj, "double_it", {}, 1);
        CHECK(result.i == 20);
        CHECK_THROWS(callObjectMethod(&h.sys.context(), liveObj, "no_such_method", {}, 1));
        std::printf("ok  Kind 3 (compiled script instance) get/set/call via ObjectDispatch\n");

        // ---- get_component() regression test --------------------------
        // A DIFFERENT actor's script (here just calling ctx_.getComponent
        // directly, as any interpreted OR native caller would) must find
        // the NativeScriptComponent on `a` -- before this phase's fix,
        // ctx_.getComponent only ever checked ScriptComponent and silently
        // returned nullptr for any native-backed component.
        auto found = h.sys.context().getComponent(a, "OdTestNative");
        CHECK(found != nullptr);
        CHECK(found->nativePtr == view.instance);
        CHECK(found->compiledInfo == view.classInfo);
        CHECK(getObjectMember(found, "n", 1).i == 10); // sees the SAME live state as above
        CHECK(callObjectMethod(&h.sys.context(), found, "double_it", {}, 1).i == 20);
        std::printf("ok  get_component() now finds a NativeScriptComponent (regression test for "
                    "the bug found while auditing for Phase 9a)\n");

        // Also confirm isA()-style base-name matching works for a native
        // component, matching ScriptComponent's existing behavior.
        auto foundByBase = h.sys.context().getComponent(a, "Actor3D");
        CHECK(foundByBase != nullptr); // OdTestNative : Actor3D, isA("Actor3D") should match
        std::printf("ok  get_component() base-name (isA) matching works for a native component "
                    "too\n");
    }

    return 0;
}

int main() {
    auto& sys = ScriptSystem::get();
    sys.loadFolder(CRATE_SCRIPTS_DIR);

    if (testKind4Aggregate()) return 1;
    if (testKind2BuiltinComponent()) return 1;
    if (testKind1Interpreted()) return 1;
    if (testKind3CompiledAndGetComponentRegression()) return 1;

    std::printf("ok  %d total checks passed\n", g_checks);
    return 0;
}
