#pragma once

// N-10: the regime pipelines print fit diagnostics to std::cerr with
// std::fixed and a precision. CerrFormatGuard restores the caller's flags and
// precision on scope exit, so nothing printed afterwards inherits them.

#include <iostream>

namespace trade_ngin {
namespace regime {

class CerrFormatGuard {
public:
    CerrFormatGuard() : flags_(std::cerr.flags()), precision_(std::cerr.precision()) {}
    ~CerrFormatGuard() {
        std::cerr.flags(flags_);
        std::cerr.precision(precision_);
    }
    CerrFormatGuard(const CerrFormatGuard&) = delete;
    CerrFormatGuard& operator=(const CerrFormatGuard&) = delete;

private:
    std::ios_base::fmtflags flags_;
    std::streamsize precision_;
};

}  // namespace regime
}  // namespace trade_ngin
