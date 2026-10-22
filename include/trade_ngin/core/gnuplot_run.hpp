// include/trade_ngin/core/gnuplot_run.hpp
#pragma once

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <stdlib.h>
#include <system_error>
#include <vector>

#include "trade_ngin/core/timed_process.hpp"

namespace trade_ngin {
namespace core {

/**
 * One chart drawn by the chart program, with an answer to "why is there no image".
 *
 * Drawn: the image exists. NothingToPlot: the chart had no data and no process was started.
 * Failed: the process hung twice, exited with an error, or left no image.
 */
enum class ChartStatus { Drawn, NothingToPlot, Failed };

struct GnuplotRun {
    ChartStatus status{ChartStatus::Failed};
    std::vector<unsigned char> png;  ///< the image bytes when status is Drawn
    std::string reason;              ///< plain words, set when status is Failed
    int attempts{0};                 ///< how many times the process was started
};

using ProcessRunner =
    std::function<TimedProcessResult(const std::string&, std::chrono::milliseconds)>;

namespace gnuplot_detail {

inline void replace_all(std::string& text, const std::string& from, const std::string& to) {
    for (std::size_t at = text.find(from); at != std::string::npos;
         at = text.find(from, at + to.size())) {
        text.replace(at, from.size(), to);
    }
}

/// A private folder for one chart: two charts never share a file name, so a leftover process of
/// one chart can never write into the next.
inline std::filesystem::path make_chart_dir() {
    std::error_code ec;
    std::filesystem::path base = std::filesystem::temp_directory_path(ec);
    if (ec) base = ".";
    std::string pattern = (base / "trade_ngin_chart_XXXXXX").string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    if (mkdtemp(buffer.data()) == nullptr) return {};
    return std::filesystem::path(buffer.data());
}

}  // namespace gnuplot_detail

/**
 * Runs a chart script: its own folder, a time limit on each start, one more start after a hang,
 * and the folder removed at the end. The scripts name the files temp_chart_data.txt and
 * temp_chart_output.png; here those names become paths inside the folder.
 */
inline GnuplotRun run_gnuplot(const std::string& script_content, const std::string& data_content,
                              std::chrono::milliseconds timeout, const ProcessRunner& runner,
                              int max_starts = 2) {
    GnuplotRun run;
    const std::filesystem::path dir = gnuplot_detail::make_chart_dir();
    if (dir.empty()) {
        run.reason = "no temporary folder could be made";
        return run;
    }
    const std::string data_path = (dir / "data.txt").string();
    const std::string script_path = (dir / "script.gnu").string();
    const std::string chart_path = (dir / "chart.png").string();

    std::string script = script_content;
    gnuplot_detail::replace_all(script, "temp_chart_data.txt", data_path);
    gnuplot_detail::replace_all(script, "temp_chart_output.png", chart_path);

    auto write = [](const std::string& path, const std::string& text) {
        std::ofstream out(path);
        if (!out.is_open()) return false;
        out << text;
        return static_cast<bool>(out);
    };
    if (!write(data_path, data_content) || !write(script_path, script)) {
        run.reason = "the chart files could not be written";
    } else {
        const std::string command = "gnuplot '" + script_path + "' 2>&1";
        for (int start = 1; start <= max_starts; ++start) {
            run.attempts = start;
            const TimedProcessResult done = runner(command, timeout);
            if (!done.started) {
                run.reason = "the chart program could not be started";
                break;
            }
            if (done.timed_out) {
                run.reason = "the chart program did not finish";
                continue;  // a hang: start it once more
            }
            if (done.status != 0) {
                run.reason = "the chart program failed";
                break;
            }
            std::ifstream in(chart_path, std::ios::binary);
            if (!in.is_open()) {
                run.reason = "the chart program left no image";
                break;
            }
            run.png.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            if (run.png.empty()) {
                run.reason = "the chart program left an empty image";
                break;
            }
            run.status = ChartStatus::Drawn;
            run.reason.clear();
            break;
        }
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return run;
}

}  // namespace core
}  // namespace trade_ngin
