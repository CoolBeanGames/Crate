// cScript: lexer / parser / interpreter and the reindent formatter.

#include "scene/Actor3D.h"
#include "script/Format.h"
#include "script/Interpreter.h"
#include "script/Lexer.h"
#include "script/Parser.h"

#include <cstdio>
#include <exception>
#include <string>
#include <unordered_map>
#include <vector>

using namespace crate::script;

static int g_checks = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        ++g_checks;                                                           \
        if (!(c)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);          \
            std::fflush(stdout);                                              \
            return 1;                                                         \
        }                                                                     \
    } while (0)

// Build a one-off type table + interpreter for a class source.
struct Harness {
    std::unordered_map<std::string, std::unique_ptr<ClassInfo>> types;
    ScriptContext ctx;
    std::vector<std::string> printed;

    std::shared_ptr<ScriptObject> load(const std::string& src) {
        Lexer lex(src);
        Parser p(lex.tokenize());
        auto decl = p.parseClass();
        auto ci = std::make_unique<ClassInfo>();
        ci->name = decl->name;
        ci->base = decl->base;
        ci->decl = std::move(decl);
        ci->indexFunctions();
        std::string name = ci->name;
        types[name] = std::move(ci);
        for (auto& [n, c] : types) {
            auto it = types.find(c->base);
            c->baseClass = (it != types.end() && it->second.get() != c.get()) ? it->second.get()
                                                                              : nullptr;
        }
        ctx.types = &types;
        ctx.print = [this](const std::string& s) { printed.push_back(s); };
        return Interpreter::instantiate(&ctx, types[name].get(), nullptr);
    }
};

static int run();

int main() {
    try {
        return run();
    } catch (const std::exception& e) {
        std::printf("FAIL uncaught exception: %s\n", e.what());
        return 1;
    }
}

static int run() {
    // --- lexer ---------------------------------------------------------
    {
        Lexer lex("func f(int a) : int { return a + 1; }");
        auto toks = lex.tokenize();
        CHECK(toks.front().kind == Tok::KwFunc);
        CHECK(toks.back().kind == Tok::End);
    }

    // --- the sample script parses -------------------------------------
    const char* sample = R"(class my_new_actor : Actor3D
{
    var unknown_var = 0;
    int known_var = 1;
    func start() { return; }
    func update(float delta) { print(delta); }
    func physics_update(float delta) { }
    func custom_func(int a, int b) : int { return a + b; }
})";
    {
        Lexer lex(sample);
        Parser p(lex.tokenize());
        auto decl = p.parseClass();
        CHECK(decl->name == "my_new_actor");
        CHECK(decl->base == "Actor3D");
        CHECK(decl->fields.size() == 2);
        CHECK(decl->functions.size() == 4);
    }

    // --- interpreter: fields, implicit typing, custom func -----------
    {
        Harness h;
        auto obj = h.load(sample);
        CHECK(obj->fields["unknown_var"].t == Value::T::Int); // var -> inferred int
        CHECK(obj->fields["known_var"].i == 1);

        Interpreter interp(&h.ctx, obj);
        Value r = interp.call("custom_func", {Value::Int(2), Value::Int(3)});
        CHECK(r.t == Value::T::Int && r.i == 5);

        interp.call("update", {Value::Float(0.25)});
        CHECK(h.printed.size() == 1);
        CHECK(h.printed[0] == "0.25");
    }

    // --- expressions -------------------------------------------------
    {
        Harness h;
        auto obj = h.load(R"(class expr : Actor
{
    func run() : string
    {
        var a = 1 + 2 * 3;
        var s = "n=" + a.str();
        if (a == 7) { s = s + " ok"; }
        return s;
    }
})");
        Value r = Interpreter(&h.ctx, obj).call("run");
        CHECK(r.str() == "n=7 ok");
    }

    // --- inheritance + this.base -----------------------------------
    {
        Harness h;
        h.load(R"(class base_c : Actor
{
    func greet() : string { return "base"; }
})");
        auto obj = h.load(R"(class child_c : base_c
{
    func greet() : string { return this.base.greet() + "+child"; }
})");
        Value r = Interpreter(&h.ctx, obj).call("greet");
        CHECK(r.str() == "base+child");
    }

    // --- variables: all base types (task 18) ------------------------
    {
        Harness h;
        crate::Actor3D host("host");
        host.transform().position = {1.0f, 2.0f, 3.0f};
        auto obj = h.load(R"(class types_c : Actor3D
{
    func run() : string
    {
        int i = 7;
        float f = 2.5;
        bool b = true;
        char c = 'z';
        string s = "hi";
        var arr = [1, "two", 3.0, true];
        Vector3 v = Vector3(1, 2, 3);
        Vector2 v2 = Vector2(4, 5);
        Actor a = this.actor;
        var mixed = arr[0].str() + arr[1] + "/" + arr.length.str();
        return i.str() + "|" + f.str() + "|" + b.str() + "|" + c + "|" + s
             + "|" + v.str() + "|" + v2.str() + "|" + a.name + "|" + mixed;
    }
})");
        obj->owner = &host;
        Value r = Interpreter(&h.ctx, obj).call("run");
        CHECK(r.str() == "7|2.5|true|z|hi|(1, 2, 3)|(4, 5)|host|1two/4");
    }

    // --- typed coercion + Actor2D/Actor3D type refs ----------------
    {
        Harness h;
        auto obj = h.load(R"(class coerce_c : Actor2D
{
    func run() : string
    {
        int fromFloat = 9.9;
        float fromInt = 4;
        return fromFloat.str() + "," + fromInt.str();
    }
})");
        CHECK(h.types.count("coerce_c") == 1);
        CHECK(h.types["coerce_c"]->base == "Actor2D");
        Value r = Interpreter(&h.ctx, obj).call("run");
        CHECK(r.str() == "9,4");
    }

    // --- functions: params, optional return, abstract/override (task 19)
    {
        Harness h;
        // abstract base; concrete override; base helper called via this.base
        h.load(R"(class shape : Actor
{
    abstract func area() : float;
    func describe() : string { return "area=" + this.area().str(); }
})");
        auto sq = h.load(R"(class square : shape
{
    float side = 3.0;
    func area() : float { return side * side; }
    func perimeter(int sides) { return side * sides; }
})");
        Interpreter i1(&h.ctx, sq);
        CHECK(i1.call("area").num() == 9.0);
        CHECK(i1.call("describe").str() == "area=9");
        // no declared return type still returns a value
        CHECK(Interpreter(&h.ctx, sq).call("perimeter", {Value::Int(4)}).num() == 12.0);

        // calling the abstract directly on a class that never overrides -> error
        Harness h2;
        auto bad = h2.load(R"(class only_abstract : Actor
{
    abstract func must_impl() : int;
    func go() : int { return this.must_impl(); }
})");
        bool threw = false;
        try {
            Interpreter(&h2.ctx, bad).call("go");
        } catch (const std::exception&) {
            threw = true;
        }
        CHECK(threw);
    }

    // --- reindent --------------------------------------------------
    {
        std::string messy = "class x : Actor\n{\nfunc start()\n{\nreturn;\n}\n}\n";
        std::string tidy = reindent(messy);
        CHECK(tidy.find("\n\tfunc start()") != std::string::npos);
        CHECK(tidy.find("\n\t\treturn;") != std::string::npos);
        CHECK(tidy.find("\n\t}") != std::string::npos);
    }

    std::printf("ok  %d checks passed\n", g_checks);
    return 0;
}
