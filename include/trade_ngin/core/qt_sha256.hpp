#pragma once

#include "trade_ngin/core/error.hpp"
#include <string>
#include <string_view>

namespace trade_ngin {
Result<std::string> qt_sha256_hex(std::string_view bytes);
}
