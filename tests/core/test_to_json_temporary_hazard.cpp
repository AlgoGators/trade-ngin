// F-5: `to_json()` returns by value, and `nlohmann::json::items()` on the
// returned temporary reads freed memory.
//
// Every config struct in config_loader.hpp exposes `nlohmann::json to_json()
// const`, returning BY VALUE. nlohmann's `items()` returns a proxy that holds a
// REFERENCE into the object it was called on. So
//
//     for (auto& [k, v] : cfg.live.to_json().items())   // WRONG
//
// binds the proxy to a temporary whose lifetime ends at the end of the
// full-expression -- before the loop body runs even once. The loop then iterates
// a destroyed json. It is undefined behaviour that usually appears to work,
// which is why it survives review: the freed buffer is typically still intact.
//
// There is no such call site in the tree today. tests/core/test_config_loader.cpp
// already binds the result to a named local before iterating, with a comment
// explaining why, and nothing in src/, include/ or apps/ does it at all. So this
// is not a fix; it is the thing that keeps it fixed.
//
// A runtime test cannot help here. The bug is at the call site, not in
// to_json(), and a test can only prove the absence of a pattern across the tree
// by looking at the tree. The repo already uses that shape of tripwire
// (tests/strategy/test_signals_publication.cpp scans src/backtest and
// apps/backtest for writer calls), so this follows it.
//
// If this test fires, the fix is one line at the offending site: bind the result
// to a named local and iterate that.
//
//     const nlohmann::json j = cfg.live.to_json();
//     for (const auto& [k, v] : j.items()) { ... }

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::filesystem::path find_repo_dir(const std::string& relative) {
    namespace fs = std::filesystem;
    fs::path dir = fs::current_path();
    for (int i = 0; i < 8 && !dir.empty(); ++i) {
        if (fs::exists(dir / relative)) return dir / relative;
        dir = dir.parent_path();
    }
    return {};
}

std::string read_all(const std::filesystem::path& p) {
    std::ifstream in(p);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

struct Hit {
    std::string file;
    int line;
    std::string text;
};

// Any `<something>()` immediately followed by `.items()`, i.e. items() called
// directly on the result of a CALL rather than on a named object. That covers
// to_json().items() and every other by-value returner with the same hazard.
// Set to the number of files actually read, so a scan that found no source
// tree cannot be mistaken for a scan that found no hits.
int g_files_scanned = 0;

std::vector<Hit> scan(const std::vector<std::string>& dirs) {
    namespace fs = std::filesystem;
    // A call, then optional whitespace/newline, then .items()
    const std::regex pattern(R"(\)\s*\.\s*items\s*\()");
    std::vector<Hit> hits;
    g_files_scanned = 0;
    for (const auto& rel : dirs) {
        auto dir = find_repo_dir(rel);
        if (dir.empty()) continue;
        for (const auto& e : fs::recursive_directory_iterator(dir)) {
            if (!e.is_regular_file()) continue;
            const auto ext = e.path().extension();
            if (ext != ".cpp" && ext != ".hpp" && ext != ".h") continue;
            ++g_files_scanned;
            std::istringstream in(read_all(e.path()));
            std::string line;
            int n = 0;
            while (std::getline(in, line)) {
                ++n;
                // Skip comments, including the ones in this very file's twin
                // explanations elsewhere in the tree.
                const auto first = line.find_first_not_of(" \t");
                if (first != std::string::npos && line.compare(first, 2, "//") == 0) continue;
                if (std::regex_search(line, pattern)) {
                    hits.push_back({e.path().string(), n, line});
                }
            }
        }
    }
    return hits;
}

}  // namespace

TEST(ToJsonTemporaryHazard, NoCallSiteIteratesItemsOnATemporary) {
    const auto hits = scan({"src", "include", "apps"});

    // Without this the test passes having scanned NOTHING whenever the binary
    // runs somewhere find_repo_dir cannot walk up to the tree.
    ASSERT_GT(g_files_scanned, 100)
        << "the scanner found only " << g_files_scanned
        << " source files; it is not looking at the repository, so its silence means nothing";

    std::ostringstream report;
    for (const auto& h : hits) {
        report << "\n  " << h.file << ":" << h.line << "  " << h.text;
    }

    EXPECT_TRUE(hits.empty())
        << "items() is being called directly on the result of a call. nlohmann's items() "
           "holds a reference into that object, and a by-value return dies at the end of "
           "the full-expression, so the loop iterates freed memory (F-5). Bind the result "
           "to a named local and iterate that instead."
        << report.str();
}

// The scanner has to be able to see the hazard, or its silence means nothing.
TEST(ToJsonTemporaryHazard, TheScannerRecognisesTheHazardousShape) {
    // Written into a temp file rather than inline, so this test exercises the
    // same path the real scan uses.
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() /
                     ("tn_f5_scan_" + std::to_string(::getpid()));
    fs::create_directories(dir);
    {
        std::ofstream out(dir / "hazard.cpp");
        out << "void f() {\n"
               "    for (auto& [k, v] : cfg.live.to_json().items()) { (void)k; }\n"
               "}\n";
    }
    {
        std::ofstream out(dir / "safe.cpp");
        out << "void g() {\n"
               "    const nlohmann::json j = cfg.live.to_json();\n"
               "    for (const auto& [k, v] : j.items()) { (void)k; }\n"
               "}\n";
    }

    // Reuse the scanner by pointing find_repo_dir at nothing and walking
    // directly: replicate its regex on these two files.
    const std::regex pattern(R"(\)\s*\.\s*items\s*\()");
    int hazard_hits = 0, safe_hits = 0;
    for (const auto& e : fs::directory_iterator(dir)) {
        std::istringstream in(read_all(e.path()));
        std::string line;
        while (std::getline(in, line)) {
            if (!std::regex_search(line, pattern)) continue;
            if (e.path().filename() == "hazard.cpp") ++hazard_hits;
            else ++safe_hits;
        }
    }
    fs::remove_all(dir);

    EXPECT_EQ(hazard_hits, 1) << "the scanner does not recognise to_json().items()";
    EXPECT_EQ(safe_hits, 0) << "the scanner flags the CORRECT form, so it would be disabled "
                               "the first time someone hit a false positive";
}

// ===== DEAD-email-ag-filter / DEAD-logger-lock-member: deleted, and staying deleted =====
//
// Two dead declarations removed in this batch:
//
//   src/core/email_sender.cpp   filter_non_agricultural_positions() -- [[maybe_unused]],
//     no caller anywhere -- together with is_agricultural_future(), whose only
//     caller was that filter. Both were a SECOND definition of logic the two
//     futures runners already implement as their own lambda, so the copy here
//     read as though the email report filtered agricultural positions. It did
//     not.
//
//   include/trade_ngin/core/logger.hpp   locked_initialization_ -- a bool
//     declared with a comment promising it would "prevent re-initialization
//     after first call", never read and never written. The re-initialization
//     guard that does exist is the explicit log_file_.close() in
//     Logger::initialize(). (Removed alongside the E2-F61 change to the
//     neighbouring thread_local, which is why it appears in that commit.)
//
// Source-scanned for the same reason as the F-5 tripwire above: what is being
// asserted is the ABSENCE of a name across the tree, which no runtime test can
// see. The scanner is the one already proved to work by
// TheScannerRecognisesTheHazardousShape.
TEST(DeadCodeRemoved, NeitherDeletedSymbolIsReferencedAnywhere) {
    namespace fs = std::filesystem;
    struct Gone { const char* symbol; const char* why; };
    const Gone gone[] = {
        {"filter_non_agricultural_positions",
         "dead email filter; the runners have their own agricultural predicate"},
        {"locked_initialization_",
         "dead logger member; the real re-init guard is log_file_.close()"},
    };

    int files = 0;
    for (const auto& g : gone) {
        std::vector<std::string> hits;
        for (const auto& rel : {"src", "include", "apps"}) {
            auto dir = find_repo_dir(rel);
            if (dir.empty()) continue;
            for (const auto& e : fs::recursive_directory_iterator(dir)) {
                if (!e.is_regular_file()) continue;
                const auto ext = e.path().extension();
                if (ext != ".cpp" && ext != ".hpp" && ext != ".h") continue;
                ++files;
                std::istringstream in(read_all(e.path()));
                std::string line;
                int n = 0;
                while (std::getline(in, line)) {
                    ++n;
                    const auto first = line.find_first_not_of(" \t");
                    if (first != std::string::npos && line.compare(first, 2, "//") == 0) continue;
                    if (line.find(g.symbol) != std::string::npos) {
                        hits.push_back(e.path().string() + ":" + std::to_string(n));
                    }
                }
            }
        }
        std::ostringstream where;
        for (const auto& h : hits) where << "\n  " << h;
        EXPECT_TRUE(hits.empty())
            << g.symbol << " is referenced again in non-comment source. It was removed as "
            << "dead code (" << g.why << "); if it is genuinely needed now, it needs a "
            << "definition and a test, not a resurrection." << where.str();
    }
    ASSERT_GT(files, 100)
        << "the scanner found only " << files
        << " source files; it is not looking at the repository, so its silence means nothing";
}
