// Coverage for CodeGen.cpp's do_async frame-stepped codegen (transpiration.txt,
// "Transplation" Phase 6): the switch/goto-based resumable state machine for
// TOP-LEVEL do_async statements in start()/update()/physics_update(), and the
// break/continue-targeting fix (a real, pre-existing bug found while building
// this) for a switch nested inside any loop.
//
// Every scenario is run through BOTH the interpreter and the compiled path
// where practical, asserting identical field values -- direct parity, same
// spirit as native_reflection_tests.cpp. Skips gracefully (not a failure)
// without a discoverable MSVC toolchain.
//
// No framework: asserts + a pass counter, run via CTest.

#include "editor/ScriptBuild.h"
#include "scene/Actor3D.h"
#include "scene/ComponentRegistry.h"
#include "scene/Scene.h"
#include "script/NativeClassRegistry.h"
#include "script/NativeScriptComponent.h"
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

namespace {

// Shared scaffolding: write a .cscript, assign it to a throwaway namespace,
// build+load, and hand back BOTH an interpreted and a compiled Component to
// drive frame-by-frame, plus a cleanup guard. Every test in this file uses
// a distinct class name (its own file) so they can run independently and
// leave nothing behind.
struct Harness {
    ScriptSystem& sys;
    std::string dir;
    std::string ns;
    std::string className;
    std::string path;
    Scene interpScene{"interp"};
    Scene nativeScene{"native"};
    Actor* interpActor = nullptr;
    Actor* nativeActor = nullptr;
    ScriptComponent* sc = nullptr;
    NativeScriptComponent* nc = nullptr;
    BuildResult build;
    bool ok = false;

    Harness(const std::string& className_, const std::string& source)
        : sys(ScriptSystem::get()), dir(sys.scriptsDir()), ns("DaTest_" + className_),
          className(className_), path(dir + "/" + className_ + ".cscript") {
        {
            std::ofstream o(path, std::ios::binary);
            o << source;
        }
        sys.reload();
        if (!sys.file(className))
            return;

        // Interpreted instance (before the namespace is ever built/loaded,
        // ComponentRegistry still resolves to ScriptComponent).
        interpActor = interpScene.add(std::make_unique<Actor3D>("Subject"));
        auto comp1 = ComponentRegistry::get().create(className);
        if (!comp1)
            return;
        sc = dynamic_cast<ScriptComponent*>(comp1.get());
        if (!sc)
            return;
        interpActor->addComponent(std::move(comp1));
        interpScene.startPlay();

        // Compiled instance.
        sys.setNamespace(className, ns);
        build = buildNamespace(ns, dir);
        if (!build.ok)
            return;
        if (!NativeClassRegistry::get().loadNamespace(ns, build.dllPath, {className}))
            return;
        nativeActor = nativeScene.add(std::make_unique<Actor3D>("Subject"));
        auto comp2 = ComponentRegistry::get().create(className);
        if (!comp2)
            return;
        nc = dynamic_cast<NativeScriptComponent*>(comp2.get());
        if (!nc)
            return;
        nativeActor->addComponent(std::move(comp2));
        nativeScene.startPlay();

        ok = true;
    }

    // Ticks BOTH scenes one frame and returns true if they're still in sync
    // afterward (caller does the actual field assertions).
    void tick(float dt) {
        interpScene.tick(dt);
        nativeScene.tick(dt);
    }

    Value interpField(const std::string& name) { return sc->object()->fields.at(name); }
    Value nativeField(const std::string& name) { return nc->object()->fields.at(name); }

    ~Harness() {
        // Explicitly replace both scenes (destroying their actors/
        // components, including the live native instance) BEFORE calling
        // unloadAll() below -- never unload a namespace's DLL while one of
        // its instances is still alive (see transpiration.txt's Phase 4
        // ordering-hazard note: the instance's code lives inside that DLL).
        interpScene = Scene("gone");
        nativeScene = Scene("gone");
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

// Ports tests/script_tests.cpp's own do_async case (~line 477-498) verbatim,
// through both paths.
static int testTopLevelFrameStepping() {
    Harness h("DaBasic", "class DaBasic : Actor3D {\n"
                        "    var n = 0;\n"
                        "    var phase = \"run\";\n"
                        "    func update(float delta) {\n"
                        "        do_async (n < 3) { n = n + 1; }\n"
                        "        phase = \"done\";\n"
                        "    }\n"
                        "}\n");
    CHECK(h.ok);

    for (int frame = 0; frame < 2; ++frame)
        h.tick(0.016f);
    CHECK(h.interpField("n").i == 2);
    CHECK(h.interpField("phase").s == "run");
    CHECK(h.nativeField("n").i == 2);
    CHECK(h.nativeField("phase").s == "run");

    for (int frame = 0; frame < 3; ++frame)
        h.tick(0.016f);
    CHECK(h.interpField("n").i == 3);
    CHECK(h.interpField("phase").s == "done");
    CHECK(h.nativeField("n").i == 3);
    CHECK(h.nativeField("phase").s == "done");
    std::printf("ok  top-level do_async frame-stepping matches the interpreter exactly (ported "
               "script_tests.cpp case)\n");
    return 0;
}

// A do_async NESTED inside an if is NOT frame-stepped -- it runs
// synchronously to completion in a single call, exactly like a plain do-
// while (Interpreter::execStmt's generic DoAsync case, not
// runFunction's top-level scan). Not covered by any test before this file.
static int testNestedDoAsyncRunsSynchronously() {
    Harness h("DaNested", "class DaNested : Actor3D {\n"
                         "    var total = 0;\n"
                         "    func update(float delta) {\n"
                         "        if (true) {\n"
                         "            var i = 0;\n"
                         "            do_async (i < 5) { total = total + 1; i = i + 1; }\n"
                         "        }\n"
                         "    }\n"
                         "}\n");
    CHECK(h.ok);

    h.tick(0.016f); // ONE call should run the whole nested do_async to completion
    CHECK(h.interpField("total").i == 5);
    CHECK(h.nativeField("total").i == 5);
    std::printf("ok  a do_async nested inside an if runs synchronously to completion in one call "
               "(both paths)\n");
    return 0;
}

// break; inside a top-level do_async aborts it WITHOUT yielding -- execution
// continues into the rest of the function in the SAME call.
static int testBreakInsideTopLevelDoAsync() {
    Harness h("DaBreak", "class DaBreak : Actor3D {\n"
                        "    var n = 0;\n"
                        "    var phase = \"run\";\n"
                        "    func update(float delta) {\n"
                        "        do_async (n < 100) {\n"
                        "            n = n + 1;\n"
                        "            if (n == 2) { break; }\n"
                        "        }\n"
                        "        phase = \"done\";\n"
                        "    }\n"
                        "}\n");
    CHECK(h.ok);

    h.tick(0.016f); // n=1, cond still true, no break yet -> yields, phase stays "run"
    CHECK(h.interpField("n").i == 1);
    CHECK(h.interpField("phase").s == "run");
    CHECK(h.nativeField("n").i == 1);
    CHECK(h.nativeField("phase").s == "run");

    h.tick(0.016f); // resumes at the do_async: n=2, break fires -> no yield, phase="done" SAME frame
    CHECK(h.interpField("n").i == 2);
    CHECK(h.interpField("phase").s == "done");
    CHECK(h.nativeField("n").i == 2);
    CHECK(h.nativeField("phase").s == "done");
    std::printf("ok  break; inside a top-level do_async aborts it without yielding (both paths)\n");
    return 0;
}

// continue; inside a top-level do_async behaves exactly like falling off the
// end of the body -- it still yields (one iteration per call either way).
static int testContinueInsideTopLevelDoAsync() {
    Harness h("DaContinue", "class DaContinue : Actor3D {\n"
                           "    var n = 0;\n"
                           "    var skipped = 0;\n"
                           "    func update(float delta) {\n"
                           "        do_async (n < 3) {\n"
                           "            n = n + 1;\n"
                           "            if (n == 2) { skipped = skipped + 1; continue; }\n"
                           "            skipped = skipped + 100;\n"
                           "        }\n"
                           "    }\n"
                           "}\n");
    CHECK(h.ok);

    h.tick(0.016f); // n=1, no continue -> skipped+=100
    CHECK(h.interpField("n").i == 1);
    CHECK(h.interpField("skipped").i == 100);
    CHECK(h.nativeField("n").i == 1);
    CHECK(h.nativeField("skipped").i == 100);

    h.tick(0.016f); // n=2, continue fires -> skipped+=1 only, still yields (one iter per call)
    CHECK(h.interpField("n").i == 2);
    CHECK(h.interpField("skipped").i == 101);
    CHECK(h.nativeField("n").i == 2);
    CHECK(h.nativeField("skipped").i == 101);

    h.tick(0.016f); // n=3, no continue -> skipped+=100, cond now false on NEXT call
    CHECK(h.interpField("n").i == 3);
    CHECK(h.interpField("skipped").i == 201);
    CHECK(h.nativeField("n").i == 3);
    CHECK(h.nativeField("skipped").i == 201);
    std::printf("ok  continue; inside a top-level do_async still yields (matches falling off the "
               "end of the body, both paths)\n");
    return 0;
}

// Two top-level do_asyncs in sequence: resuming the first must not
// re-execute anything before it, and reaching the second only starts once
// the first's condition goes false.
static int testMultipleTopLevelDoAsyncs() {
    Harness h("DaMulti", "class DaMulti : Actor3D {\n"
                        "    var setupRuns = 0;\n"
                        "    var a = 0;\n"
                        "    var b = 0;\n"
                        "    var done = \"no\";\n"
                        "    func update(float delta) {\n"
                        "        setupRuns = setupRuns + 1;\n"
                        "        do_async (a < 2) { a = a + 1; }\n"
                        "        do_async (b < 2) { b = b + 1; }\n"
                        "        done = \"yes\";\n"
                        "    }\n"
                        "}\n");
    CHECK(h.ok);

    h.tick(0.016f); // fresh: setupRuns=1, a=1 (yields at do_async #0)
    CHECK(h.interpField("setupRuns").i == 1);
    CHECK(h.interpField("a").i == 1);
    CHECK(h.interpField("b").i == 0);
    CHECK(h.nativeField("setupRuns").i == 1);
    CHECK(h.nativeField("a").i == 1);
    CHECK(h.nativeField("b").i == 0);

    h.tick(0.016f); // resume at do_async #0 DIRECTLY -- setupRuns must NOT increment again
    CHECK(h.interpField("setupRuns").i == 1);
    CHECK(h.interpField("a").i == 2); // a's cond now false on the call AFTER this
    CHECK(h.nativeField("setupRuns").i == 1);
    CHECK(h.nativeField("a").i == 2);

    h.tick(0.016f); // do_async #0's cond now false -> falls through to do_async #1, b=1, yields there
    CHECK(h.interpField("setupRuns").i == 1); // still never re-run
    CHECK(h.interpField("a").i == 2);
    CHECK(h.interpField("b").i == 1);
    CHECK(h.interpField("done").s == "no");
    CHECK(h.nativeField("setupRuns").i == 1);
    CHECK(h.nativeField("a").i == 2);
    CHECK(h.nativeField("b").i == 1);
    CHECK(h.nativeField("done").s == "no");

    h.tick(0.016f); // resume at do_async #1 directly, b=2, cond false next time
    h.tick(0.016f); // do_async #1's cond now false -> falls through to done="yes"
    CHECK(h.interpField("b").i == 2);
    CHECK(h.interpField("done").s == "yes");
    CHECK(h.interpField("setupRuns").i == 1); // NEVER re-executed across the whole sequence
    CHECK(h.nativeField("b").i == 2);
    CHECK(h.nativeField("done").s == "yes");
    CHECK(h.nativeField("setupRuns").i == 1);
    std::printf("ok  multiple top-level do_asyncs resume independently without re-running earlier "
               "segments (both paths)\n");
    return 0;
}

// Regression test for the pre-existing bug found and fixed while building
// Phase 6: a switch nested inside a loop must let `continue;` skip PAST the
// switch to the enclosing loop, not stop at the switch's own dispatch
// mechanism. Exercised both inside an ordinary synchronous loop AND inside
// a top-level resumable do_async (the switch itself is never "top-level"
// either way, so this is really testing the SAME codegen fix from two
// different enclosing-loop-kind angles).
static int testContinueThroughSwitchInsideLoop() {
    Harness h("DaSwitchLoop", "class DaSwitchLoop : Actor3D {\n"
                             "    var sum = 0;\n"
                             "    var afterSwitchRuns = 0;\n"
                             "    func update(float delta) {\n"
                             "        var i = 0;\n"
                             "        do (i < 5) {\n"
                             "            i = i + 1;\n"
                             "            switch (i) {\n"
                             "                case 3:\n"
                             "                    continue;\n"
                             "            }\n"
                             "            afterSwitchRuns = afterSwitchRuns + 1;\n"
                             "            sum = sum + i;\n"
                             "        }\n"
                             "    }\n"
                             "}\n");
    CHECK(h.ok);
    // i=1,2,4,5 reach "afterSwitchRuns/sum" (4 times); i=3 hits `continue;`
    // inside the switch and must skip them for that iteration only.
    // sum = 1+2+4+5 = 12; afterSwitchRuns = 4.
    h.tick(0.016f);
    CHECK(h.interpField("sum").i == 12);
    CHECK(h.interpField("afterSwitchRuns").i == 4);
    CHECK(h.nativeField("sum").i == 12);
    CHECK(h.nativeField("afterSwitchRuns").i == 4);
    std::printf("ok  continue; inside a switch nested in an ordinary loop correctly skips past the "
               "switch to the loop (regression test, both paths, sum=12 not 15)\n");
    return 0;
}

static int testContinueThroughSwitchInsideTopLevelDoAsync() {
    Harness h("DaSwitchAsync", "class DaSwitchAsync : Actor3D {\n"
                              "    var n = 0;\n"
                              "    var label = \"\";\n"
                              "    func update(float delta) {\n"
                              "        do_async (n < 3) {\n"
                              "            n = n + 1;\n"
                              "            switch (n) {\n"
                              "                case 2:\n"
                              "                    continue;\n"
                              "            }\n"
                              "            label = label + \"x\";\n"
                              "        }\n"
                              "    }\n"
                              "}\n");
    CHECK(h.ok);
    h.tick(0.016f); // n=1, no continue -> label="x"
    CHECK(h.interpField("label").s == "x");
    CHECK(h.nativeField("label").s == "x");
    h.tick(0.016f); // n=2, continue inside switch -> label untouched this frame, still yields
    CHECK(h.interpField("n").i == 2);
    CHECK(h.interpField("label").s == "x");
    CHECK(h.nativeField("n").i == 2);
    CHECK(h.nativeField("label").s == "x");
    h.tick(0.016f); // n=3, no continue -> label="xx"
    CHECK(h.interpField("label").s == "xx");
    CHECK(h.nativeField("label").s == "xx");
    std::printf("ok  continue; inside a switch nested in a top-level do_async correctly skips past "
               "the switch (regression test, both paths)\n");
    return 0;
}

// Calling a DIFFERENT hook while one is suspended must NOT clear the
// suspended hook's resume state (Interpreter::call()'s
// `else if (asyncResumeFn == method)` only ever clears ITS OWN name).
static int testDifferentHookDoesNotClearSuspension() {
    Harness h("DaCross", "class DaCross : Actor3D {\n"
                        "    var n = 0;\n"
                        "    var starts = 0;\n"
                        "    func start() { starts = starts + 1; }\n"
                        "    func update(float delta) {\n"
                        "        do_async (n < 3) { n = n + 1; }\n"
                        "    }\n"
                        "}\n");
    CHECK(h.ok);

    // start() already ran once via Scene::startPlay() in the harness setup.
    CHECK(h.interpField("starts").i == 1);
    CHECK(h.nativeField("starts").i == 1);

    h.tick(0.016f); // update(): n=1, yields (asyncResumeFn now "update")
    CHECK(h.interpField("n").i == 1);
    CHECK(h.nativeField("n").i == 1);

    // Directly invoke start() again (bypassing update()) on BOTH paths --
    // this must NOT disturb update()'s suspension.
    h.sc->start();
    h.nc->start();
    CHECK(h.interpField("starts").i == 2);
    CHECK(h.nativeField("starts").i == 2);

    h.tick(0.016f); // update() resumes correctly at n=1 -> n=2, NOT restarted from n=0
    CHECK(h.interpField("n").i == 2);
    CHECK(h.nativeField("n").i == 2);
    std::printf("ok  calling a different hook while one is suspended does not clear its resume "
               "state (both paths)\n");
    return 0;
}

int main() {
    auto& sys = ScriptSystem::get();
    sys.loadFolder(CRATE_SCRIPTS_DIR);

    if (!ToolchainEnv::get().available()) {
        std::printf("(no MSVC toolchain found -- do_async_native_tests skipped: %s)\n",
                    ToolchainEnv::get().error().c_str());
        return 0;
    }

    if (testTopLevelFrameStepping()) return 1;
    if (testNestedDoAsyncRunsSynchronously()) return 1;
    if (testBreakInsideTopLevelDoAsync()) return 1;
    if (testContinueInsideTopLevelDoAsync()) return 1;
    if (testMultipleTopLevelDoAsyncs()) return 1;
    if (testContinueThroughSwitchInsideLoop()) return 1;
    if (testContinueThroughSwitchInsideTopLevelDoAsync()) return 1;
    if (testDifferentHookDoesNotClearSuspension()) return 1;

    std::printf("ok  %d checks passed\n", g_checks);
    return 0;
}
