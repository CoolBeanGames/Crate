// Phase (Scenes-adjacent, mouse input): Input.Mouse.<member> parity test --
// exercised from BOTH an interpreted and a natively-compiled caller,
// asserting identical observable results. Input.Mouse is a member-access
// chain (not a method call), so this specifically exercises the CodeGen
// emission path added alongside Interpreter::evalMember's identical
// structural check (see Runtime.h's inputMouseMember).
//
// No real mouse ever moves in a headless test process, so this can't check
// against nonzero hardware state -- what it DOES check is that every member
// resolves without error and that the interpreted and native readings agree
// with each other exactly, which is what would break if the two emission
// paths ever drifted.
//
// No framework: asserts + a pass counter, run via CTest. Skips gracefully
// (not a failure) without a discoverable MSVC toolchain, matching every
// other Phase 3+ test's convention.

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

static Value fieldOf(Component* c, const std::string& name) {
    if (auto* sc = dynamic_cast<ScriptComponent*>(c))
        return sc->object()->fields.at(name);
    if (auto* nc = dynamic_cast<NativeScriptComponent*>(c)) {
        auto view = nc->ensureNativeView();
        for (size_t i = 0; i < view.classInfo->fieldCount; ++i)
            if (view.classInfo->fields[i].name == name)
                return view.classInfo->fields[i].get(view.instance);
        std::fprintf(stderr, "fieldOf: native field '%s' not found\n", name.c_str());
        std::abort();
    }
    std::fprintf(stderr, "fieldOf: neither ScriptComponent nor NativeScriptComponent\n");
    std::abort();
}

int main() {
    auto& sys = ScriptSystem::get();
    sys.loadFolder(CRATE_SCRIPTS_DIR);
    const std::string dir = sys.scriptsDir();

    if (!ToolchainEnv::get().available()) {
        std::printf("(no MSVC toolchain found -- input_mouse_parity_tests skipped: %s)\n",
                    ToolchainEnv::get().error().c_str());
        return 0;
    }

    struct Combo {
        const char* suffix;
        bool native;
    };
    static const Combo kCombos[] = {{"I", false}, {"N", true}};

    std::vector<std::string> paths;
    for (const auto& c : kCombos) {
        std::string src = "class MouseReader" + std::string(c.suffix) + " : Actor3D {\n"
                          "    var dx = -1.0;\n"
                          "    var dy = -1.0;\n"
                          "    var sx = -1.0;\n"
                          "    var sy = -1.0;\n"
                          "    var leftDown = true;\n"
                          "    var leftJustDown = true;\n"
                          "    var leftJustUp = true;\n"
                          "    var rightDown = true;\n"
                          "    var rightJustDown = true;\n"
                          "    var rightJustUp = true;\n"
                          "    var middleDown = true;\n"
                          "    var middleJustDown = true;\n"
                          "    var middleJustUp = true;\n"
                          "    func start() {\n"
                          "        var d = Input.Mouse.delta;\n"
                          "        dx = d.x; dy = d.y;\n"
                          "        var s = Input.Mouse.scroll;\n"
                          "        sx = s.x; sy = s.y;\n"
                          "        leftDown = Input.Mouse.left_down;\n"
                          "        leftJustDown = Input.Mouse.left_just_down;\n"
                          "        leftJustUp = Input.Mouse.left_just_up;\n"
                          "        rightDown = Input.Mouse.right_down;\n"
                          "        rightJustDown = Input.Mouse.right_just_down;\n"
                          "        rightJustUp = Input.Mouse.right_just_up;\n"
                          "        middleDown = Input.Mouse.middle_down;\n"
                          "        middleJustDown = Input.Mouse.middle_just_down;\n"
                          "        middleJustUp = Input.Mouse.middle_just_up;\n"
                          "    }\n"
                          "}\n";
        std::string p = dir + "/MouseReader" + c.suffix + ".cscript";
        {
            std::ofstream o(p, std::ios::binary);
            o << src;
        }
        paths.push_back(p);
    }
    sys.reload();
    for (const auto& c : kCombos)
        CHECK(sys.file(std::string("MouseReader") + c.suffix) != nullptr);

    const std::string ns = "MouseReaderNativeNs";
    struct Cleanup {
        ScriptSystem* sys;
        std::vector<std::string> paths;
        std::string scriptsDir, ns;
        ~Cleanup() {
            NativeClassRegistry::get().unloadAll();
            for (const auto& path : paths)
                std::remove(path.c_str());
            sys->reload();
            std::error_code ec;
            fs::remove_all(scriptsDir + "/.crate_build/gen/" + ns, ec);
            std::error_code ec2;
            for (const auto& entry : fs::directory_iterator(scriptsDir + "/.crate_build/bin", ec2)) {
                if (ec2)
                    break;
                if (entry.path().filename().string().rfind(ns + "_v", 0) == 0)
                    fs::remove(entry.path(), ec);
            }
            const std::string manifestPath = scriptsDir + "/.crate_build/manifest.tsv";
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
    } cleanup{&sys, paths, dir, ns};

    sys.setNamespace("MouseReaderN", ns);
    BuildResult build = buildNamespace(ns, dir);
    if (!build.ok)
        std::printf("  buildNamespace error: %s\n", build.error.c_str());
    CHECK(build.ok);
    CHECK(NativeClassRegistry::get().loadNamespace(ns, build.dllPath, {"MouseReaderN"}));

    for (const auto& c : kCombos) {
        Scene scene(std::string("mouse-parity-") + c.suffix);
        Actor* readerActor = scene.add(std::make_unique<Actor3D>("Reader"));
        auto comp = ComponentRegistry::get().create(std::string("MouseReader") + c.suffix);
        CHECK(comp != nullptr);
        CHECK((dynamic_cast<NativeScriptComponent*>(comp.get()) != nullptr) == c.native);
        Component* reader = readerActor->addComponent(std::move(comp));

        scene.startPlay(); // runs start(): reads every Input.Mouse member once

        // No real mouse moved in this headless process -- every field must
        // still have resolved to SOME queried value (not the -1/true
        // placeholders each field started at), proving the member chain
        // actually reached ctx_->inputQuery rather than silently no-oping.
        CHECK(fieldOf(reader, "dx").num() == 0.0);
        CHECK(fieldOf(reader, "dy").num() == 0.0);
        CHECK(fieldOf(reader, "sx").num() == 0.0);
        CHECK(fieldOf(reader, "sy").num() == 0.0);
        CHECK(fieldOf(reader, "leftDown").b == false);
        CHECK(fieldOf(reader, "leftJustDown").b == false);
        CHECK(fieldOf(reader, "leftJustUp").b == false);
        CHECK(fieldOf(reader, "rightDown").b == false);
        CHECK(fieldOf(reader, "rightJustDown").b == false);
        CHECK(fieldOf(reader, "rightJustUp").b == false);
        CHECK(fieldOf(reader, "middleDown").b == false);
        CHECK(fieldOf(reader, "middleJustDown").b == false);
        CHECK(fieldOf(reader, "middleJustUp").b == false);
        std::printf("ok  combo %s (%s caller): every Input.Mouse.<member> resolved without error\n",
                    c.suffix, c.native ? "native" : "interpreted");
    }

    std::printf("ok  %d checks passed\n", g_checks);
    return 0;
}
