// Phase 9d end-to-end parity test: signals (declare/connect/emit/
// disconnect/is_connected, the Godot-3 shortcut form, cross-object signals
// via get_component, InputButton signals) and first-class function
// references, exercised from BOTH an interpreted and a natively-compiled
// caller/target, asserting identical results. See transpiration.txt,
// "Transplation" Phase 9d.
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

// Same rationale as get_component_parity_tests.cpp's fieldOf: for a native
// component, read straight through the CompiledClassInfo accessor (what
// script code itself sees), not the Inspector-display mirror.
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

// Pre-Play field seeding (e.g. pointing a holder's Actor-reference field at
// its sibling before scene.startPlay() runs) -- for a NATIVE component this
// MUST be the interpreted MIRROR (object(), Phase 5's Inspector-sync
// object), NOT selfView_: a Kind-3 ScriptObject's `fields` map is never
// read/written at all (field access goes through the CompiledClassInfo
// accessor table instead), so writing to selfView_->fields would silently
// do nothing -- exactly matching the technique
// get_component_parity_tests.cpp/camera_main_parity_tests.cpp already use
// (obj->fields["x"] = ...; the NEXT hook call's pushFieldsToNative() pushes
// it into the real native instance before that hook body runs).
static std::shared_ptr<ScriptObject> mirrorOf(Component* c) {
    if (auto* sc = dynamic_cast<ScriptComponent*>(c))
        return sc->object();
    if (auto* nc = dynamic_cast<NativeScriptComponent*>(c))
        return nc->object();
    return nullptr;
}

// The canonical ScriptObject for a component regardless of kind -- for a
// native component this MUST be selfView_ (Phase 9d), never a fresh
// wrapper, or the InputButton test below couldn't reach its connections.
static std::shared_ptr<ScriptObject> objectOf(Component* c) {
    if (auto* sc = dynamic_cast<ScriptComponent*>(c))
        return sc->object();
    if (auto* nc = dynamic_cast<NativeScriptComponent*>(c)) {
        auto view = nc->ensureNativeView();
        return view.classInfo->selfView ? view.classInfo->selfView(view.instance) : nullptr;
    }
    return nullptr;
}

int main() {
    auto& sys = ScriptSystem::get();
    sys.loadFolder(CRATE_SCRIPTS_DIR);
    const std::string dir = sys.scriptsDir();

    if (!ToolchainEnv::get().available()) {
        std::printf("(no MSVC toolchain found -- signals_parity_tests skipped: %s)\n",
                    ToolchainEnv::get().error().c_str());
        return 0;
    }

    std::vector<std::string> paths;
    auto writeScript = [&](const std::string& className, const std::string& src) {
        std::string p = dir + "/" + className + ".cscript";
        std::ofstream o(p, std::ios::binary);
        o << src;
        paths.push_back(p);
    };

    // ---- (1) self-signal: declare/connect/emit_signal/disconnect/
    // is_connected, plus a first-class function reference invoked via a
    // local variable call -- one class per combo (I = never compiled, N =
    // compiled). ------------------------------------------------------
    for (const char* suf : {"I", "N"}) {
        writeScript(std::string("SgSelf") + suf,
                   "class SgSelf" + std::string(suf) + " : Actor3D {\n"
                   "    signal pinged(msg);\n"
                   "    var count = 0;\n"
                   "    var lastMsg = \"\";\n"
                   "    var wasConnected = false;\n"
                   "    var stillConnectedAfterDisconnect = true;\n"
                   "    var callableResult = 0;\n"
                   "    func onPing(msg) {\n"
                   "        count = count + 1;\n"
                   "        lastMsg = msg;\n"
                   "    }\n"
                   "    func double_it(x) {\n"
                   "        return x * 2;\n"
                   "    }\n"
                   "    func start() {\n"
                   "        this.pinged.connect(onPing);\n"
                   "        wasConnected = this.pinged.is_connected(onPing);\n"
                   "        this.emit_signal(\"pinged\", \"hello\");\n"
                   "        this.pinged.disconnect(onPing);\n"
                   "        stillConnectedAfterDisconnect = this.pinged.is_connected(onPing);\n"
                   "        this.emit_signal(\"pinged\", \"world\");\n"
                   "        var f = double_it;\n"
                   "        callableResult = f(21);\n"
                   "    }\n"
                   "}\n");
    }

    // ---- (2) cross-object signal via get_component + the Godot-3
    // connect(name, callable)/emit_signal(name, ...) shortcut forms -- 4
    // combos (holder-native x target-native). --------------------------
    struct Combo {
        const char* suffix;
        bool holderNative;
        bool targetNative;
    };
    static const Combo kCombos[] = {
        {"II", false, false}, {"NI", true, false}, {"IN", false, true}, {"NN", true, true}};
    for (const auto& c : kCombos) {
        writeScript(std::string("SgTarget") + c.suffix,
                   "class SgTarget" + std::string(c.suffix) + " : Actor3D {\n"
                   "    signal fired(payload);\n"
                   "    func doFire(payload) {\n"
                   "        emit_signal(\"fired\", payload);\n" // bare global form
                   "    }\n"
                   "}\n");
        writeScript(std::string("SgHolder") + c.suffix,
                   "class SgHolder" + std::string(c.suffix) + " : Actor3D {\n"
                   "    var otherActor = null;\n"
                   "    var received = 0;\n"
                   "    func onFired(payload) {\n"
                   "        received = payload;\n"
                   "    }\n"
                   "    func start() {\n"
                   "        var t = this.otherActor.get_component(type_of(SgTarget" +
                       std::string(c.suffix) +
                       "));\n"
                       "        t.connect(\"fired\", onFired);\n"
                       "        t.doFire(99);\n"
                       "    }\n"
                       "}\n");
    }

    // ---- (3) InputButton signal connected from a native handler -------
    for (const char* suf : {"I", "N"}) {
        writeScript(std::string("SgInputHandler") + suf,
                   "class SgInputHandler" + std::string(suf) + " : Actor3D {\n"
                   "    var fired = false;\n"
                   "    func onFire() {\n"
                   "        fired = true;\n"
                   "    }\n"
                   "    func start() {\n"
                   "        Input.get_button(\"Fire\").just_pressed.connect(onFire);\n"
                   "    }\n"
                   "}\n");
    }

    sys.reload();
    for (const char* suf : {"I", "N"}) {
        CHECK(sys.file(std::string("SgSelf") + suf) != nullptr);
        CHECK(sys.file(std::string("SgInputHandler") + suf) != nullptr);
    }
    for (const auto& c : kCombos) {
        CHECK(sys.file(std::string("SgHolder") + c.suffix) != nullptr);
        CHECK(sys.file(std::string("SgTarget") + c.suffix) != nullptr);
    }

    const std::string ns = "SgParityNs";
    struct Cleanup {
        ScriptSystem* sys;
        std::vector<std::string> paths;
        std::string scriptsDir, ns;
        ~Cleanup() {
            NativeClassRegistry::get().unloadAll();
            for (const auto& p : paths)
                std::remove(p.c_str());
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

    // Everything suffixed "N" (and the holder/target combos with a native
    // side) compiles into ONE shared namespace.
    sys.setNamespace("SgSelfN", ns);
    sys.setNamespace("SgInputHandlerN", ns);
    for (const auto& c : kCombos) {
        if (c.holderNative)
            sys.setNamespace(std::string("SgHolder") + c.suffix, ns);
        if (c.targetNative)
            sys.setNamespace(std::string("SgTarget") + c.suffix, ns);
    }
    BuildResult build = buildNamespace(ns, dir);
    if (!build.ok)
        std::printf("  buildNamespace error: %s\n", build.error.c_str());
    CHECK(build.ok);
    std::vector<std::string> nativeClasses = {"SgSelfN", "SgInputHandlerN"};
    for (const auto& c : kCombos) {
        if (c.holderNative)
            nativeClasses.push_back(std::string("SgHolder") + c.suffix);
        if (c.targetNative)
            nativeClasses.push_back(std::string("SgTarget") + c.suffix);
    }
    CHECK(NativeClassRegistry::get().loadNamespace(ns, build.dllPath, nativeClasses));

    // ---- run (1): self-signal + first-class callable -------------------
    {
        Scene scene("sg-self");
        for (const char* suf : {"I", "N"}) {
            Actor* a = scene.add(std::make_unique<Actor3D>(std::string("Self_") + suf));
            auto comp = ComponentRegistry::get().create(std::string("SgSelf") + suf);
            CHECK(comp != nullptr);
            bool isNative = dynamic_cast<NativeScriptComponent*>(comp.get()) != nullptr;
            CHECK(isNative == (std::string(suf) == "N"));
            Component* c = a->addComponent(std::move(comp));
            (void)c;
        }
        scene.startPlay();
        Actor* selfI = scene.root().children()[0].get();
        Actor* selfN = scene.root().children()[1].get();
        Component* cI = selfI->components()[0].get();
        Component* cN = selfN->components()[0].get();
        for (auto* c : {cI, cN}) {
            CHECK(fieldOf(c, "count").i == 1); // only "hello" landed, not "world" (disconnected)
            CHECK(fieldOf(c, "lastMsg").s == "hello");
            CHECK(fieldOf(c, "wasConnected").b == true);
            CHECK(fieldOf(c, "stillConnectedAfterDisconnect").b == false);
            CHECK(fieldOf(c, "callableResult").i == 42);
        }
        std::printf("ok  self-signal parity (interpreted + native): declare/connect/emit_signal/"
                    "disconnect/is_connected + a first-class function reference invoked via a "
                    "local, identical results (count=1, callableResult=42)\n");
    }

    // ---- run (2): cross-object signal, all 4 combos ---------------------
    {
        Scene scene("sg-cross");
        struct Live {
            const Combo* combo;
            Component* holder;
        };
        std::vector<Live> live;
        for (const auto& c : kCombos) {
            Actor* a = scene.add(std::make_unique<Actor3D>(std::string("Subject_") + c.suffix));
            auto targetComp = ComponentRegistry::get().create(std::string("SgTarget") + c.suffix);
            CHECK(targetComp != nullptr);
            Component* target = a->addComponent(std::move(targetComp));

            auto holderComp = ComponentRegistry::get().create(std::string("SgHolder") + c.suffix);
            CHECK(holderComp != nullptr);
            Component* holder = a->addComponent(std::move(holderComp));

            auto obj = mirrorOf(holder);
            CHECK(obj != nullptr);
            obj->fields["otherActor"] = Value::ActorRef(a);
            live.push_back({&c, holder});
            (void)target;
        }
        scene.startPlay();
        for (const auto& l : live) {
            CHECK(fieldOf(l.holder, "received").i == 99);
            std::printf("ok  cross-object signal combo %s (holder %s, target %s): "
                        "get_component + connect(name, callable) + doFire's bare emit_signal(...) "
                        "all landed on the SAME connections (received=99)\n",
                        l.combo->suffix, l.combo->holderNative ? "native" : "interpreted",
                        l.combo->targetNative ? "native" : "interpreted");
        }
    }

    // ---- run (3): InputButton signal, interpreted + native handler -----
    // Input::get()'s real event queue is only populated by poll() (reads
    // live ImGui/XInput state, unavailable headless) -- instead, this
    // drives the EXACT SAME underlying mechanism ScriptSystem::
    // dispatchInput() uses (crate::script::emitSignal on the button's real
    // connections list, populated by a REAL script's .connect(...) call),
    // which is precisely what Phase 9d's dispatchInput() fix changed (it
    // used to silently no-op for a native handler -- see ScriptSystem.cpp).
    {
        Scene scene("sg-input");
        for (const char* suf : {"I", "N"}) {
            Actor* a = scene.add(std::make_unique<Actor3D>(std::string("InputHandler_") + suf));
            auto comp = ComponentRegistry::get().create(std::string("SgInputHandler") + suf);
            CHECK(comp != nullptr);
            a->addComponent(std::move(comp));
        }
        scene.startPlay(); // runs start(): Input.get_button("Fire").just_pressed.connect(onFire)

        auto btn = sys.inputButton("Fire");
        CHECK(btn != nullptr);
        CHECK(btn->connections["just_pressed"].size() == 2); // one per handler (I and N)
        crate::script::emitSignal(&sys.context(), btn, "just_pressed", {}, 0);

        Actor* handlerI = scene.root().children()[0].get();
        Actor* handlerN = scene.root().children()[1].get();
        CHECK(fieldOf(handlerI->components()[0].get(), "fired").b == true);
        CHECK(fieldOf(handlerN->components()[0].get(), "fired").b == true);
        std::printf("ok  InputButton signal parity (interpreted + native handler): "
                    "just_pressed.connect(...) + emitSignal both fired correctly (regression test "
                    "for dispatchInput()'s pre-9d silent-no-op-on-a-native-target bug)\n");
        sys.resetInput(); // don't leak connections into a later test run
    }

    std::printf("ok  %d checks passed\n", g_checks);
    return 0;
}
