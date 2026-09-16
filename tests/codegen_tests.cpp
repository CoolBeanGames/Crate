// Coverage for src/script/CodeGen.cpp (transpiration.txt, "Transplation"
// Phase 2): for an escalating battery of small cScript sources, run
// Lexer -> Parser -> generateClass(), and for everything Phase 2 claims to
// support, actually invoke cl.exe /c on the emitted .gen.cpp and assert it
// compiles with zero errors -- not just that CodeGen produced *some* text.
// For constructs Phase 2 explicitly refuses (script-to-script inheritance,
// signals, get_component, this.base), assert generateClass() fails with a
// clear error instead of silently emitting something wrong.
//
// Toolchain discovery here is deliberately minimal and test-local (Phase 3
// builds the real, cached version as ScriptBuild::ToolchainEnv) -- skips
// gracefully, rather than failing, on a machine with no MSVC toolchain
// discoverable via vswhere.exe.
//
// No framework: asserts + a pass counter, run via CTest (matches every
// other test file in this repo).

#include "script/CodeGen.h"
#include "script/Lexer.h"
#include "script/Parser.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace crate::script;
namespace fs = std::filesystem;

#ifndef CRATE_REPO_DIR
#define CRATE_REPO_DIR "."
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

// ---------------------------------------------------------------------------
// Minimal MSVC toolchain discovery, test-local only.
// ---------------------------------------------------------------------------

static std::string runCapture(const std::string& cmd) {
    std::string out;
    FILE* p = _popen((cmd + " 2>&1").c_str(), "r");
    if (!p)
        return out;
    char buf[4096];
    while (std::fgets(buf, sizeof(buf), p))
        out += buf;
    _pclose(p);
    return out;
}

static void trimTrailingNewlines(std::string& s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
        s.pop_back();
}

static std::string findVcvars() {
    std::string vswhere =
        "\"C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer\\vswhere.exe\"";
    std::string out = runCapture(vswhere + " -latest -property installationPath");
    trimTrailingNewlines(out);
    if (out.empty())
        return {};
    std::string vcvars = out + "\\VC\\Auxiliary\\Build\\vcvars64.bat";
    std::error_code ec;
    if (!fs::exists(vcvars, ec))
        return {};
    return vcvars;
}

// Compiles `cppPath` with `cl.exe /c`; returns true iff it succeeds. On
// failure, `diagnostics` gets the compiler's output for the test to print.
static bool compileWithCl(const std::string& vcvars, const std::string& cppPath,
                          const std::string& objPath, std::string& diagnostics) {
    std::string cmd = "cmd.exe /c \"\"" + vcvars + "\" >nul && cl.exe /nologo /c /std:c++17 /EHsc "
                      "/W3 -I\"" + std::string(CRATE_REPO_DIR) + "\\src\" -I\"" +
                      std::string(CRATE_REPO_DIR) + "\\third_party\" \"" + cppPath +
                      "\" /Fo\"" + objPath + "\"\"";
    diagnostics = runCapture(cmd);
    std::error_code ec;
    return fs::exists(objPath, ec);
}

// ---------------------------------------------------------------------------
// Harness: parse `src`, run generateClass(), write the result to
// `<scratchDir>/<ClassName>.gen.h/.cpp`, and (if `vcvars` is non-empty)
// verify it compiles.
// ---------------------------------------------------------------------------

static bool parseOne(const std::string& src, ClassDecl& outDecl, std::string& err) {
    try {
        Lexer lex(src);
        Parser parser(lex.tokenize());
        auto decl = parser.parseClass();
        outDecl = std::move(*decl);
        return true;
    } catch (const ParseError& e) {
        err = "line " + std::to_string(e.line) + ": " + e.what();
        return false;
    }
}

static int expectCompiles(const std::string& label, const std::string& src, const std::string& vcvars,
                          const std::string& scratchDir,
                          const std::unordered_set<std::string>& knownClassNames = {}) {
    ClassDecl decl;
    std::string perr;
    CHECK(parseOne(src, decl, perr));

    CodeGenResult r = generateClass(decl, knownClassNames);
    if (!r.ok)
        std::printf("  (%s) generateClass error: %s\n", label.c_str(), r.error.c_str());
    CHECK(r.ok);

    std::string hPath = scratchDir + "/" + r.className + ".gen.h";
    std::string cPath = scratchDir + "/" + r.className + ".gen.cpp";
    {
        std::ofstream hf(hPath, std::ios::binary);
        hf << r.header;
    }
    {
        std::ofstream cf(cPath, std::ios::binary);
        cf << r.source;
    }

    if (vcvars.empty()) {
        std::printf("ok  %s (generated, compiler check skipped -- no MSVC toolchain found)\n",
                    label.c_str());
        return 0;
    }

    std::string objPath = scratchDir + "/" + r.className + ".obj";
    std::error_code ec;
    fs::remove(objPath, ec);
    std::string diag;
    bool compiled = compileWithCl(vcvars, cPath, objPath, diag);
    if (!compiled)
        std::printf("  (%s) cl.exe output:\n%s\n", label.c_str(), diag.c_str());
    CHECK(compiled);
    std::printf("ok  %s (compiles via cl.exe /c)\n", label.c_str());
    return 0;
}

static int expectRefused(const std::string& label, const std::string& src,
                         const std::string& mustContain) {
    ClassDecl decl;
    std::string perr;
    CHECK(parseOne(src, decl, perr));

    CodeGenResult r = generateClass(decl);
    CHECK(!r.ok);
    CHECK(!r.error.empty());
    if (!mustContain.empty() && r.error.find(mustContain) == std::string::npos) {
        std::printf("FAIL %s: error was '%s', expected to contain '%s'\n", label.c_str(),
                    r.error.c_str(), mustContain.c_str());
        return 1;
    }
    std::printf("ok  %s (refused: %s)\n", label.c_str(), r.error.c_str());
    return 0;
}

int main() {
    std::string vcvars = findVcvars();
    if (vcvars.empty())
        std::printf("(no MSVC toolchain found via vswhere.exe -- compiling the generated code will "
                    "be skipped, generation itself is still checked)\n");

    std::string scratchDir = std::string(CRATE_REPO_DIR) + "/build/codegen_scratch";
    fs::create_directories(scratchDir);

    // ---- escalating battery of SUPPORTED constructs -----------------------

    if (expectCompiles("empty class", R"(
        class GenEmpty : Actor {
            func update(float delta) {}
        }
    )", vcvars, scratchDir)) return 1;

    if (expectCompiles("fields + arithmetic", R"(
        class GenArith : Actor {
            var speed = 5;
            float accel = 1.5;
            func update(float delta) {
                speed = speed + accel * delta;
                var doubled = speed * 2;
            }
        }
    )", vcvars, scratchDir)) return 1;

    if (expectCompiles("if/else + comparisons", R"(
        class GenIf : Actor {
            var health = 100;
            func update(float delta) {
                if (health <= 0) {
                    health = 0;
                } else if (health > 100) {
                    health = 100;
                } else {
                    health = health - 1;
                }
            }
        }
    )", vcvars, scratchDir)) return 1;

    if (expectCompiles("switch with default", R"(
        class GenSwitch : Actor {
            var state = 1;
            var label = "none";
            func update(float delta) {
                switch (state) {
                    case 1:
                        label = "one";
                        break;
                    case 2:
                        label = "two";
                        break;
                    default:
                        label = "other";
                }
            }
        }
    )", vcvars, scratchDir)) return 1;

    if (expectCompiles("do_async as synchronous loop", R"(
        class GenLoop : Actor {
            var n = 0;
            func update(float delta) {
                do_async (n < 5) {
                    n = n + 1;
                }
            }
        }
    )", vcvars, scratchDir)) return 1;

    if (expectCompiles("nested do inside if, with break/continue", R"(
        class GenNested : Actor {
            var total = 0;
            func update(float delta) {
                var i = 0;
                if (total >= 0) {
                    do (i < 10) {
                        i = i + 1;
                        if (i == 3) { continue; }
                        if (i == 7) { break; }
                        total = total + i;
                    }
                }
            }
        }
    )", vcvars, scratchDir)) return 1;

    if (expectCompiles("vectors: construct, arithmetic, components", R"(
        class GenVector : Actor {
            func update(float delta) {
                var a = Vector3(1, 2, 3);
                var b = Vector3(10, 20, 30);
                var sum = a + b;
                var scaled = a * 2;
                var x = sum.x;
                sum.y = 99;
            }
        }
    )", vcvars, scratchDir)) return 1;

    if (expectCompiles("transform read/write + single-component write-back", R"(
        class GenTransform : Actor {
            var speed = 1.0;
            func update(float delta) {
                transform.position = transform.position + transform.forward * speed * delta;
                transform.rotation.y += 10 * delta;
                var p = transform.position;
                var nm = actor.name;
            }
        }
    )", vcvars, scratchDir)) return 1;

    if (expectCompiles("locals, string concat, print", R"(
        class GenPrint : Actor {
            func update(float delta) {
                var msg = "delta=" + delta;
                print(msg, "ok", 5);
                var s = str(42);
            }
        }
    )", vcvars, scratchDir)) return 1;

    if (expectCompiles("arrays: literal, add, length, index", R"(
        class GenArray : Actor {
            func update(float delta) {
                var items = [1, 2, 3];
                items.add(4);
                var n = items.length;
                var first = items[0];
                items[0] = 99;
            }
        }
    )", vcvars, scratchDir)) return 1;

    if (expectCompiles("own-method calls, bare and this.-qualified", R"(
        class GenCalls : Actor {
            var total = 0;
            func helper(int x) {
                return x * 2;
            }
            func update(float delta) {
                total = helper(5);
                total = this.helper(total);
            }
        }
    )", vcvars, scratchDir)) return 1;

    if (expectCompiles("undeclared-field auto-vivification", R"(
        class GenAutoField : Actor {
            func update(float delta) {
                made_up_field = 42;
                made_up_field = made_up_field + 1;
            }
        }
    )", vcvars, scratchDir)) return 1;

    if (expectCompiles("Math.* namespace", R"(
        class GenMath : Actor {
            func update(float delta) {
                var c = Math.clamp(150, 0, 100);
                var l = Math.lerp(0, 10, 0.5);
                var r = Math.rand_i_range(1, 6);
            }
        }
    )", vcvars, scratchDir)) return 1;

    if (expectCompiles("Actor2D/Actor3D bases also accepted", R"(
        class GenActor3D : Actor3D {
            func update(float delta) {
                var p = transform.position;
            }
        }
    )", vcvars, scratchDir)) return 1;

    if (expectCompiles("unary minus and not, bool literals", R"(
        class GenUnary : Actor {
            var flag = true;
            func update(float delta) {
                var neg = -5;
                var negf = -1.5;
                var notFlag = !flag;
            }
        }
    )", vcvars, scratchDir)) return 1;

    if (expectCompiles("compound assignment on a field", R"(
        class GenCompound : Actor {
            var counter = 0;
            func update(float delta) {
                counter += 1;
                counter -= 1;
                counter *= 2;
            }
        }
    )", vcvars, scratchDir)) return 1;

    if (expectCompiles("field initializer referencing an earlier field", R"(
        class GenFieldOrder : Actor {
            var a = 10;
            var b = a + 5;
            var c = a + b;
            func update(float delta) {}
        }
    )", vcvars, scratchDir)) return 1;

    if (expectCompiles("string literal escaping round-trips through emitted C++", R"(
        class GenEscaping : Actor {
            func update(float delta) {
                var s1 = "quote \" inside";
                var s2 = "backslash \\ inside";
                var s3 = "tab\tand\nnewline";
                print(s1, s2, s3);
            }
        }
    )", vcvars, scratchDir)) return 1;

    if (expectCompiles("multi-hop own-method calls", R"(
        class GenChain : Actor {
            func a() { return this.b(); }
            func b() { return this.c(); }
            func c() { return 7; }
            func update(float delta) {
                var v = a();
            }
        }
    )", vcvars, scratchDir)) return 1;

    if (expectCompiles("compound assignment on transform.position component", R"(
        class GenTransformCompound : Actor {
            func update(float delta) {
                transform.position.x += 1;
                transform.position.y -= 2;
            }
        }
    )", vcvars, scratchDir)) return 1;

    if (expectRefused("bare own-method name used as a value is refused", R"(
        class GenBareMethod : Actor {
            func helper() { return 1; }
            func update(float delta) {
                var f = helper;
            }
        }
    )", "Phase 5")) return 1;

    // ---- constructs Phase 2 explicitly REFUSES (must fail cleanly) --------

    if (expectRefused("script-to-script inheritance is refused", R"(
        class GenBadBase : SomeOtherScript {
            func update(float delta) {}
        }
    )", "base")) return 1;

    if (expectRefused("signal declarations are refused", R"(
        class GenSignal : Actor {
            signal died();
            func update(float delta) {}
        }
    )", "signal")) return 1;

    if (expectRefused("bare get_component(...) (no such global function -- only "
                      "this.get_component/actor.get_component exist) is refused", R"(
        class GenGetComponent : Actor {
            func update(float delta) {
                var fog = get_component(type_of(Fog));
            }
        }
    )", "unknown function")) return 1;

    // ---- Phase 9b: get_component / type_of(AnyClass) / general cross-
    // object access, now real (not refused) ---------------------------------

    if (expectCompiles("this.get_component(type_of(Fog))", R"(
        class GenThisGetComponent : Actor {
            func update(float delta) {
                var fog = this.get_component(type_of(Fog));
            }
        }
    )", vcvars, scratchDir)) return 1;

    if (expectCompiles("actor.get_component(...) on the bare actor/transform alias", R"(
        class GenActorGetComponent : Actor {
            func update(float delta) {
                var fog = actor.get_component(type_of(Fog));
            }
        }
    )", vcvars, scratchDir)) return 1;

    if (expectCompiles("get_component on a general (non-this) receiver expression", R"(
        class GenGeneralGetComponent : Actor {
            var other = null;
            func update(float delta) {
                var c = this.other.get_component(type_of(Fog));
            }
        }
    )", vcvars, scratchDir)) return 1;

    if (expectCompiles("type_of() on another known script class name", R"(
        class GenTypeOfOther : Actor {
            func update(float delta) {
                var t = type_of(GenOtherKnownClass);
            }
        }
    )", vcvars, scratchDir, {"GenOtherKnownClass"})) return 1;

    if (expectCompiles("method call on a general (non-this) object-typed expression", R"(
        class GenGeneralMethodCall : Actor {
            var other = null;
            func update(float delta) {
                var r = this.other.some_method(1, 2);
            }
        }
    )", vcvars, scratchDir)) return 1;

    if (expectCompiles("member read on a general (non-this) expression -- field, .length, "
                      "and an Actor-typed receiver's position", R"(
        class GenGeneralMemberRead : Actor {
            var other = null;
            var otherActor = null;
            func update(float delta) {
                var f = this.other.some_field;
                var n = this.other.length;
                var p = this.otherActor.position;
                var nm = this.otherActor.name;
            }
        }
    )", vcvars, scratchDir)) return 1;

    if (expectCompiles("member write on a general (non-this) expression -- field and "
                      "an Actor-typed receiver's position/rotation.y", R"(
        class GenGeneralMemberWrite : Actor {
            var other = null;
            var otherActor = null;
            func update(float delta) {
                this.other.some_field = 42;
                this.otherActor.position = Vector3(1, 2, 3);
                this.otherActor.rotation.y = 10;
            }
        }
    )", vcvars, scratchDir)) return 1;

    if (expectRefused("this.base.method() is refused", R"(
        class GenBaseCall : Actor {
            func update(float delta) {
                this.base.update(delta);
            }
        }
    )", "base")) return 1;

    if (expectRefused("static classes are refused", R"(
        static class GenStatic : Actor {
            func update(float delta) {}
        }
    )", "static")) return 1;

    if (expectRefused("abstract classes are refused", R"(
        abstract class GenAbstract : Actor {
            func update(float delta) {}
        }
    )", "abstract")) return 1;

    if (expectRefused("bare `this` as a value is refused", R"(
        class GenThisValue : Actor {
            func helper(int x) { return x; }
            func update(float delta) {
                var x = helper(this);
            }
        }
    )", "")) return 1;

    std::printf("ok  %d checks passed\n", g_checks);
    return 0;
}
