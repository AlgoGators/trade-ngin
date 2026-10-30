// tests/core/test_gnuplot_run.cpp
//
// A chart is drawn in a folder of its own, a hang is started once more, and the
// result says Drawn / Failed with the reason. The chart program is a stand-in that reads the
// script it is given and writes (or does not write) the image the script names.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include "trade_ngin/core/gnuplot_run.hpp"

using namespace trade_ngin::core;
using std::chrono::milliseconds;

namespace {
std::string between(const std::string& text, const std::string& open, const std::string& close) {
    const auto a = text.find(open);
    if (a == std::string::npos) return "";
    const auto b = text.find(close, a + open.size());
    return text.substr(a + open.size(), b - a - open.size());
}

std::string slurp(const std::string& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

const std::string kScript =
    "set output 'temp_chart_output.png'\nplot 'temp_chart_data.txt' using 1:2\n";

struct Stand {
    std::vector<TimedProcessResult> script;  // what each start does: timed out / status
    std::vector<std::string> scripts_seen;
    std::vector<std::string> data_seen;
    bool write_image{true};
    int starts{0};
    ProcessRunner runner() {
        return [this](const std::string& command, milliseconds) {
            const std::string script_path = between(command, "gnuplot '", "'");
            const std::string text = slurp(script_path);
            scripts_seen.push_back(script_path);
            data_seen.push_back(between(text, "plot '", "'"));
            TimedProcessResult r = script.at(static_cast<std::size_t>(starts++));
            if (r.started && !r.timed_out && r.status == 0 && write_image) {
                std::ofstream(between(text, "set output '", "'"), std::ios::binary) << "PNGBYTES";
            }
            return r;
        };
    }
};

TimedProcessResult ok() {
    TimedProcessResult r;
    r.started = true;
    r.status = 0;
    return r;
}
TimedProcessResult hang() {
    TimedProcessResult r;
    r.started = true;
    r.timed_out = true;
    return r;
}
TimedProcessResult bad() {
    TimedProcessResult r;
    r.started = true;
    r.status = 256;
    return r;
}
}  // namespace

TEST(GnuplotRun, ADrawnChartReturnsTheImageAndLeavesNoFilesBehind) {
    Stand s;
    s.script = {ok()};
    const auto run = run_gnuplot(kScript, "2026-01-01 1\n", milliseconds(1000), s.runner());
    EXPECT_EQ(run.status, ChartStatus::Drawn);
    EXPECT_EQ(std::string(run.png.begin(), run.png.end()), "PNGBYTES");
    EXPECT_EQ(run.attempts, 1);
    EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(s.scripts_seen.at(0)).parent_path()));
}

TEST(GnuplotRun, TheScriptNamesFilesInsideAFolderThatIsNotTheWorkingDirectory) {
    Stand s;
    s.script = {ok()};
    run_gnuplot(kScript, "d\n", milliseconds(1000), s.runner());
    ASSERT_EQ(s.data_seen.size(), 1u);
    EXPECT_EQ(s.data_seen[0].find("temp_chart_data.txt"), std::string::npos);
    EXPECT_EQ(std::filesystem::path(s.data_seen[0]).parent_path(),
              std::filesystem::path(s.scripts_seen[0]).parent_path());
    EXPECT_TRUE(std::filesystem::path(s.data_seen[0]).is_absolute());
}

TEST(GnuplotRun, TwoChartsNeverShareAFolder) {
    Stand s;
    s.script = {ok(), ok()};
    run_gnuplot(kScript, "a\n", milliseconds(1000), s.runner());
    run_gnuplot(kScript, "b\n", milliseconds(1000), s.runner());
    EXPECT_NE(s.scripts_seen[0], s.scripts_seen[1]);
}

TEST(GnuplotRun, AHangIsStartedOnceMoreAndASecondStartThatWorksIsDrawn) {
    Stand s;
    s.script = {hang(), ok()};
    const auto run = run_gnuplot(kScript, "d\n", milliseconds(1000), s.runner());
    EXPECT_EQ(run.status, ChartStatus::Drawn);
    EXPECT_EQ(run.attempts, 2);
    EXPECT_TRUE(run.reason.empty());
}

TEST(GnuplotRun, TwoHangsFailTheChartWithAReasonAfterExactlyTwoStarts) {
    Stand s;
    s.script = {hang(), hang()};
    const auto run = run_gnuplot(kScript, "d\n", milliseconds(1000), s.runner());
    EXPECT_EQ(run.status, ChartStatus::Failed);
    EXPECT_EQ(run.attempts, 2);
    EXPECT_EQ(run.reason, "the chart program did not finish");
    EXPECT_TRUE(run.png.empty());
}

TEST(GnuplotRun, AnErrorExitIsNotStartedAgain) {
    Stand s;
    s.script = {bad()};
    const auto run = run_gnuplot(kScript, "d\n", milliseconds(1000), s.runner());
    EXPECT_EQ(run.status, ChartStatus::Failed);
    EXPECT_EQ(run.attempts, 1);
    EXPECT_EQ(run.reason, "the chart program failed");
}

TEST(GnuplotRun, ACleanExitThatLeavesNoImageIsAFailureNotAnEmptyChart) {
    Stand s;
    s.script = {ok()};
    s.write_image = false;
    const auto run = run_gnuplot(kScript, "d\n", milliseconds(1000), s.runner());
    EXPECT_EQ(run.status, ChartStatus::Failed);
    EXPECT_EQ(run.reason, "the chart program left no image");
}

TEST(GnuplotRun, AProcessThatCannotBeStartedFailsTheChart) {
    Stand s;
    s.script = {TimedProcessResult{}};
    const auto run = run_gnuplot(kScript, "d\n", milliseconds(1000), s.runner());
    EXPECT_EQ(run.status, ChartStatus::Failed);
    EXPECT_EQ(run.reason, "the chart program could not be started");
}
