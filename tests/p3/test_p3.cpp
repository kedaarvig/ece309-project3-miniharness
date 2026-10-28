// tests/p3/test_p3.cpp: tests for ToolRegistry, Workspace::resolve and calc_evaluate.
//
// Build & run:   cmake --build build && ./build/test_p3
// One area:      ./build/test_p3 reg_        (names starting with "reg_")
// One test:      ./build/test_p3 =reg_add_and_find

#include "../mini_test.h"
#include "../test_helpers.h"     // TempDir, write_text, args1, ...

#include "tools/builtin_tools.h"
#include "tools/calculator.h"
#include "tools/calculator_tool.h"
#include "tools/file_tools.h"
#include "tools/tool_registry.h"
#include "tools/workspace.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

// A tool whose name you choose, for filling the registry in tests.
class NamedTool : public Tool {
public:
    explicit NamedTool(std::string n) : n_(std::move(n)), schema_(JsonValue::object()) {}
    std::string name() const override { return n_; }
    std::string description() const override { return "test tool"; }
    const JsonValue& schema() const override { return schema_; }
    ToolResult invoke(const JsonValue&, ToolContext&) override { return ToolResult::success(n_); }
private:
    std::string n_;
    JsonValue   schema_;   // an empty JSON object: {}
};

// Keeps a live-instance count so the ownership tests can see when tools die.
class CountedTool : public NamedTool {
public:
    CountedTool(std::string n, int* alive) : NamedTool(std::move(n)), alive_(alive) { ++*alive_; }
    ~CountedTool() override { --*alive_; }
private:
    int* alive_;
};

std::vector<std::string> numbered_names(int n) {
    std::vector<std::string> names;
    names.reserve(n);
    for (int i = 0; i < n; ++i) names.push_back("tool_" + std::to_string(i));
    return names;
}

// Longest chain a table with `buckets` buckets would get if it used `h`.
std::size_t longest_chain(const std::vector<std::string>& names, std::size_t buckets,
                          const std::function<std::size_t(std::string_view)>& h) {
    std::vector<int> counts(buckets, 0);
    for (const std::string& n : names) ++counts[h(n) % buckets];
    return static_cast<std::size_t>(*std::max_element(counts.begin(), counts.end()));
}

// Tokenize with the provided tokenizer, then run YOUR calc_evaluate.
bool eval(const std::string& expr, double* value, std::string* error) {
    std::vector<CalcToken> tokens;
    if (!calc_tokenize(expr, &tokens, error)) return false;
    return calc_evaluate(tokens, value, error);
}

bool evals_to(const std::string& expr, double want) {
    double got = 0;
    std::string err;
    return eval(expr, &got, &err) && std::fabs(got - want) < 1e-9;
}

struct Outcome {
    bool        ok = false;
    fs::path    path;
    std::string reason;
};

Outcome try_resolve(const Workspace& ws, const std::string& user_path) {
    Outcome o;
    o.ok = ws.resolve(user_path, &o.path, &o.reason);
    return o;
}

}  // namespace

// ---------------------------------------------------------------- registry

TEST(reg_starts_empty) {
    ToolRegistry r;
    CHECK(r.size() == 0);
    CHECK(r.bucket_count() >= 1 && r.bucket_count() <= 64);
    CHECK(r.load_factor() == 0.0);
    CHECK(r.max_probe_length() == 0);
    CHECK(r.find("anything") == nullptr);
    int visits = 0;
    r.for_each([&](const Tool&) { ++visits; });
    CHECK(visits == 0);
}

TEST(reg_add_and_find) {
    ToolRegistry r;
    auto t = std::make_unique<NamedTool>("alpha");
    Tool* raw = t.get();                        // remember the address before moving
    REQUIRE(r.add(std::move(t)));
    CHECK(r.size() == 1);
    CHECK(r.find("alpha") == raw);              // same object, not a copy
    CHECK(r.find("beta") == nullptr);
}

TEST(reg_duplicate_keeps_original) {
    ToolRegistry r;
    auto first = std::make_unique<NamedTool>("alpha");
    Tool* original = first.get();
    REQUIRE(r.add(std::move(first)));
    CHECK(!r.add(std::make_unique<NamedTool>("alpha")));
    CHECK(r.find("alpha") == original);
    CHECK(r.size() == 1);
}

TEST(reg_null_rejected) {
    ToolRegistry r;
    CHECK(!r.add(nullptr));
    CHECK(r.size() == 0);
}

TEST(reg_size_only_counts_successful_adds) {
    ToolRegistry r;
    CHECK(r.add(std::make_unique<NamedTool>("a")));
    CHECK(r.size() == 1);
    CHECK(!r.add(std::make_unique<NamedTool>("a")));
    CHECK(r.size() == 1);
    CHECK(!r.add(nullptr));
    CHECK(r.size() == 1);
    CHECK(r.add(std::make_unique<NamedTool>("b")));
    CHECK(r.size() == 2);
}

TEST(reg_find_needs_the_exact_name) {
    ToolRegistry r;
    REQUIRE(r.add(std::make_unique<NamedTool>("calculator")));
    CHECK(r.find("calc") == nullptr);
    CHECK(r.find("calculators") == nullptr);
    CHECK(r.find("Calculator") == nullptr);
    CHECK(r.find("") == nullptr);
    CHECK(r.find("calculator") != nullptr);
}

TEST(reg_for_each_visits_each_once) {
    ToolRegistry r;
    for (const char* n : {"read_file", "write_file", "list_dir", "calculator", "echo"})
        REQUIRE(r.add(std::make_unique<NamedTool>(n)));
    std::vector<std::string> seen;
    r.for_each([&](const Tool& t) { seen.push_back(t.name()); });
    std::sort(seen.begin(), seen.end());
    CHECK((seen == std::vector<std::string>{"calculator", "echo", "list_dir", "read_file", "write_file"}));
}

TEST(reg_for_each_survives_resizes) {
    ToolRegistry r;
    const auto names = numbered_names(200);
    for (const std::string& n : names) REQUIRE(r.add(std::make_unique<NamedTool>(n)));
    std::vector<std::string> seen;
    r.for_each([&](const Tool& t) { seen.push_back(t.name()); });
    CHECK(seen.size() == names.size());
    std::sort(seen.begin(), seen.end());
    CHECK(std::adjacent_find(seen.begin(), seen.end()) == seen.end());   // no repeats
}

TEST(reg_destroys_every_tool_exactly_once) {
    int alive = 0;
    {
        ToolRegistry r;
        for (int i = 0; i < 50; ++i)
            REQUIRE(r.add(std::make_unique<CountedTool>("t" + std::to_string(i), &alive)));
        CHECK(alive == 50);
        CHECK(!r.add(std::make_unique<CountedTool>("t7", &alive)));   // rejected tool dies right away
        CHECK(alive == 50);
    }
    CHECK(alive == 0);
}

TEST(reg_pointers_survive_resize) {
    ToolRegistry r;
    const std::size_t buckets_at_start = r.bucket_count();
    std::vector<std::pair<std::string, Tool*>> handed_out;
    for (const std::string& n : numbered_names(300)) {
        auto t = std::make_unique<NamedTool>(n);
        Tool* raw = t.get();
        REQUIRE(r.add(std::move(t)));
        handed_out.emplace_back(n, raw);
    }
    CHECK(r.bucket_count() > buckets_at_start);   // otherwise this test proves nothing
    for (const auto& [name, ptr] : handed_out) CHECK(r.find(name) == ptr);
}

TEST(reg_load_factor_and_growth) {
    ToolRegistry r;
    std::size_t prev = r.bucket_count();
    for (const std::string& n : numbered_names(100)) {
        REQUIRE(r.add(std::make_unique<NamedTool>(n)));
        const std::size_t b = r.bucket_count();
        CHECK(r.load_factor() <= 1.0);
        CHECK(r.load_factor() == static_cast<double>(r.size()) / static_cast<double>(b));
        CHECK(b == prev || b == prev * 2);        // it only ever doubles
        CHECK((b & (b - 1)) == 0);                // and stays a power of two
        prev = b;
    }
}

TEST(reg_hash_depends_on_order) {
    CHECK(ToolRegistry::hash("ab") != ToolRegistry::hash("ba"));
    CHECK(ToolRegistry::hash("read_file") != ToolRegistry::hash("file_read"));
    CHECK(ToolRegistry::hash("read_file") == ToolRegistry::hash(std::string("read_file")));
}

TEST(reg_hash_uses_every_character) {
    const std::string base = "tool_12345";
    for (std::size_t i = 0; i < base.size(); ++i) {
        std::string changed = base;
        changed[i] = (changed[i] == 'x') ? 'y' : 'x';
        CHECK(ToolRegistry::hash(changed) != ToolRegistry::hash(base));
    }
}

TEST(reg_hash_spreads_numbered_names) {
    std::vector<std::size_t> hashes;
    for (const std::string& n : numbered_names(10000)) hashes.push_back(ToolRegistry::hash(n));
    std::sort(hashes.begin(), hashes.end());
    const auto distinct = std::unique(hashes.begin(), hashes.end()) - hashes.begin();
    CHECK(distinct >= 9990);
    // A hash already reduced to a table size would have no value past the table.
    CHECK(hashes.back() > 1000000);
}

TEST(reg_10000_tools) {
    const auto names = numbered_names(10000);
    ToolRegistry r;
    Tool* first = nullptr;
    for (const std::string& n : names) {
        auto t = std::make_unique<NamedTool>(n);
        if (first == nullptr) first = t.get();
        REQUIRE(r.add(std::move(t)));
        REQUIRE(r.load_factor() <= 1.0);
    }
    CHECK(r.size() == names.size());
    CHECK(r.find("tool_0") == first);             // same pointer after every resize on the way
    for (const std::string& n : names) {
        Tool* t = r.find(n);
        REQUIRE(t != nullptr);
        CHECK(t->name() == n);
    }
    CHECK(r.find("tool_10000") == nullptr);
    CHECK(r.max_probe_length() <= 64);
    std::printf("    10,000 tools: bucket_count=%zu load_factor=%.3f max_probe_length=%zu\n",
                r.bucket_count(), r.load_factor(), r.max_probe_length());
}

TEST(reg_alternative_hash) {
    const auto names = numbered_names(10000);
    ToolRegistry r;
    for (const std::string& n : names) REQUIRE(r.add(std::make_unique<NamedTool>(n)));
    const std::size_t buckets = r.bucket_count();

    const std::size_t fnv = longest_chain(names, buckets,
        [](std::string_view s) { return ToolRegistry::hash(s); });
    const std::size_t djb2 = longest_chain(names, buckets, [](std::string_view s) {
        std::size_t h = 5381;
        for (unsigned char c : s) h = h * 33 + c;
        return h;
    });
    const std::size_t weak = longest_chain(names, buckets, [](std::string_view s) {
        return static_cast<std::size_t>(static_cast<unsigned char>(s[0])) + s.size();
    });

    std::printf("    longest chain at %zu buckets: FNV-1a=%zu  djb2=%zu  s[0]+size=%zu\n",
                buckets, fnv, djb2, weak);
    std::printf("    registry reports max_probe_length=%zu\n", r.max_probe_length());

    CHECK_EQ(fnv, r.max_probe_length());          // chaining: a recount must agree
    CHECK(weak > 1000);
    CHECK(weak > 100 * fnv);
}

TEST(reg_builtin_tools_listing) {
    ToolRegistry r;
    CHECK(register_builtin_tools(r) == 4);
    CHECK(register_builtin_tools(r) == 0);        // every name is taken now
    CHECK(r.size() == 4);
    const std::string listing = describe_tools(r);
    std::size_t at = 0;
    for (const char* n : {"- calculator:", "- list_dir:", "- read_file:", "- write_file:"}) {
        const std::size_t found = listing.find(n);
        REQUIRE(found != std::string::npos);
        CHECK(found >= at);                       // alphabetical, whatever order the table holds them in
        at = found;
    }
}

// --------------------------------------------------------------- workspace

TEST(ws_accepts_plain_relative_path) {
    TempDir d;
    Workspace ws(d.path());
    Outcome o = try_resolve(ws, "notes/todo.txt");
    REQUIRE(o.ok);
    CHECK(o.path == ws.root() / "notes" / "todo.txt");
}

TEST(ws_cleans_dots_and_double_slashes) {
    TempDir d;
    Workspace ws(d.path());
    Outcome o = try_resolve(ws, "./notes//todo.txt");
    REQUIRE(o.ok);
    CHECK(o.path == ws.root() / "notes" / "todo.txt");
}

TEST(ws_dot_is_the_root) {
    TempDir d;
    Workspace ws(d.path());
    Outcome o = try_resolve(ws, ".");
    REQUIRE(o.ok);
    CHECK(o.path == ws.root());
}

TEST(ws_folds_inner_dotdot) {
    TempDir d;
    Workspace ws(d.path());
    Outcome in = try_resolve(ws, "a/../b");
    REQUIRE(in.ok);
    CHECK(in.path == ws.root() / "b");
    Outcome back = try_resolve(ws, "a/..");
    REQUIRE(back.ok);
    CHECK(back.path == ws.root());
}

TEST(ws_accepts_paths_that_do_not_exist) {
    TempDir d;
    Workspace ws(d.path());
    Outcome o = try_resolve(ws, "new/dir/file.txt");
    REQUIRE(o.ok);
    CHECK(o.path == ws.root() / "new" / "dir" / "file.txt");
}

TEST(ws_allows_dots_inside_a_name) {
    TempDir d;
    Workspace ws(d.path());
    Outcome o = try_resolve(ws, "my..notes.txt");
    REQUIRE(o.ok);
    CHECK(o.path == ws.root() / "my..notes.txt");
}

TEST(ws_rejects_empty_path) {
    TempDir d;
    Workspace ws(d.path());
    Outcome o = try_resolve(ws, "");
    CHECK(!o.ok);
    CHECK(!o.reason.empty());
}

TEST(ws_rejects_nul_byte) {
    TempDir d;
    Workspace ws(d.path());
    // a literal "notes\0/x" would be cut at the NUL before resolve ever saw it
    Outcome o = try_resolve(ws, std::string("notes") + '\0' + "/x");
    CHECK(!o.ok);
    CHECK(!o.reason.empty());
}

TEST(ws_rejects_absolute_paths) {
    TempDir d;
    Workspace ws(d.path());
    Outcome etc = try_resolve(ws, "/etc/passwd");
    CHECK(!etc.ok);
    CHECK(!etc.reason.empty());
    // absolute is absolute, even when it happens to point inside the root
    Outcome inside = try_resolve(ws, (ws.root() / "notes").string());
    CHECK(!inside.ok);
    CHECK(!inside.reason.empty());
}

TEST(ws_rejects_parent_escape) {
    TempDir d;
    Workspace ws(d.path());
    fs::path out;
    std::string reason;
    REQUIRE(ws.resolve("notes/todo.txt", &out, &reason));   // sanity: accepts *something*
    CHECK(out == ws.root() / "notes" / "todo.txt");
    CHECK(!ws.resolve("../etc/passwd", &out, &reason));
    CHECK(!reason.empty());

    for (const char* bad : {"..", "../..", "a/../../b", "./../x"}) {
        Outcome o = try_resolve(ws, bad);
        CHECK(!o.ok);
        CHECK(!o.reason.empty());
    }
}

TEST(ws_rejects_escape_that_comes_back_in) {
    TempDir d;
    Workspace ws(d.path());
    const std::string again = "notes/../../" + ws.root().filename().string() + "/notes/todo.txt";
    Outcome o = try_resolve(ws, again);
    CHECK(!o.ok);
    CHECK(!o.reason.empty());
}

TEST(ws_resolve_leaves_the_disk_alone) {
    TempDir d;
    Workspace ws(d.path() / "not_created_yet");
    Outcome o = try_resolve(ws, "a/b/c.txt");
    CHECK(o.ok);
    CHECK(!fs::exists(d.path() / "not_created_yet"));
    CHECK(fs::is_empty(d.path()));
}

TEST(ws_file_tools_cannot_escape) {
    TempDir d;
    const fs::path root = d.path() / "ws";
    fs::create_directories(root);
    write_text(d.path() / "secret.txt", "top secret");
    ToolContext ctx{Workspace(root)};

    ReadFileTool reader;
    ToolResult peek = reader.invoke(args1("path", JsonValue::string("../secret.txt")), ctx);
    CHECK(!peek.ok);
    CHECK(peek.error.find("top secret") == std::string::npos);

    WriteFileTool writer;
    JsonValue evil = args1("path", JsonValue::string("../planted.txt"));
    evil.set("content", JsonValue::string("x"));
    CHECK(!writer.invoke(evil, ctx).ok);
    CHECK(!fs::exists(d.path() / "planted.txt"));

    JsonValue fine = args1("path", JsonValue::string("inside/ok.txt"));
    fine.set("content", JsonValue::string("hello"));
    CHECK(writer.invoke(fine, ctx).ok);
    CHECK(read_text(root / "inside" / "ok.txt") == "hello");
}

// -------------------------------------------------------------- calculator

TEST(calc_precedence) {
    double v = 0;
    std::string err;
    REQUIRE(eval("2 + 3 * 4", &v, &err));
    CHECK(v == 14);
    CHECK(evals_to("2 * 3 + 4", 10));
    CHECK(evals_to("20 - 6 / 2", 17));
}

TEST(calc_left_to_right) {
    CHECK(evals_to("10 - 4 - 3", 3));
    CHECK(evals_to("100 / 10 / 5", 2));
    CHECK(evals_to("2 - 3 + 4", 3));
    CHECK(evals_to("8 / 4 / 2", 1));
}

TEST(calc_parentheses) {
    CHECK(evals_to("(2 + 3) * 4", 20));
    CHECK(evals_to("2 * (3 + 4) - 5", 9));
    CHECK(evals_to("((1 + 2) * (3 + 4)) - ((5 - 6) * 2)", 23));
    CHECK(evals_to("(((7)))", 7));
    CHECK(evals_to("10 - (4 - 3)", 9));
}

TEST(calc_negative_numbers) {
    CHECK(evals_to("-3 * -3", 9));
    CHECK(evals_to("2 * -3 + 10 / 4", -3.5));
    CHECK(evals_to("(-1) - -1", 0));
}

TEST(calc_decimals) {
    CHECK(evals_to("7 / 2", 3.5));
    CHECK(evals_to("1.5 * 4", 6));
    CHECK(evals_to("0.1 + 0.2", 0.3));
}

TEST(calc_single_number) {
    CHECK(evals_to("42", 42));
    CHECK(evals_to("(42)", 42));
}

TEST(calc_division_by_zero) {
    double v = 0;
    std::string err;
    CHECK(!eval("1 / 0", &v, &err));
    CHECK(err == "division by zero");
}

TEST(calc_division_by_zero_inside_parens) {
    double v = 0;
    std::string err;
    CHECK(!eval("1 / (2 - 2)", &v, &err));
    CHECK(err == "division by zero");
    err.clear();
    CHECK(!eval("5 + 3 / (4 - 4) * 2", &v, &err));
    CHECK(err == "division by zero");
}

TEST(calc_twenty_thousand_terms) {
    std::string expr = "1";
    for (int i = 0; i < 20000; ++i) expr += " + 1";
    CHECK(evals_to(expr, 20001));
}

TEST(calc_five_thousand_nested_parens) {
    std::string flat(5000, '(');
    flat += "7";
    flat += std::string(5000, ')');
    CHECK(evals_to(flat, 7));

    // same depth, but with an operator waiting at every level
    std::string busy(5000, '(');
    busy += "1";
    for (int i = 0; i < 5000; ++i) busy += " + 1)";
    CHECK(evals_to(busy, 5001));
}

TEST(calc_survives_malformed_token_lists) {
    using K = CalcToken::Kind;
    const std::vector<std::vector<CalcToken>> bad = {
        {},
        {{K::Plus, 0.0}},
        {{K::Number, 1.0}, {K::Plus, 0.0}},
        {{K::Number, 1.0}, {K::Number, 2.0}},
        {{K::LParen, 0.0}, {K::Number, 1.0}},
        {{K::Number, 1.0}, {K::RParen, 0.0}},
        {{K::RParen, 0.0}},
    };
    for (const auto& tokens : bad) {
        double v = 0;
        std::string err;
        CHECK(!calc_evaluate(tokens, &v, &err));
        CHECK(!err.empty());
    }
}

TEST(calc_tool_formats_result) {
    TempDir d;
    ToolContext ctx{Workspace(d.path())};
    CalculatorTool tool;
    ToolResult ok = tool.invoke(args1("expression", JsonValue::string("(12 + 30) * 2 / 7")), ctx);
    REQUIRE(ok.ok);
    CHECK(ok.content == "12");
    ToolResult bad = tool.invoke(args1("expression", JsonValue::string("1 / (2 - 2)")), ctx);
    CHECK(!bad.ok);
    CHECK(bad.error == "division by zero");
}

int main(int argc, char** argv) { return mt::run_all(argc, argv); }
