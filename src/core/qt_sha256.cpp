#include "trade_ngin/core/qt_sha256.hpp"
#include <openssl/evp.h>
#include <array>

namespace trade_ngin {
Result<std::string> qt_sha256_hex(std::string_view bytes) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int length = 0;
    if (EVP_Digest(bytes.data(), bytes.size(), digest.data(), &length, EVP_sha256(), nullptr) != 1 || length != 32)
        return make_error<std::string>(ErrorCode::ENCRYPTION_ERROR, "qt_sha256_failed");
    constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (unsigned int i = 0; i < length; ++i) {
        out.push_back(hex[digest[i] >> 4]);
        out.push_back(hex[digest[i] & 0x0f]);
    }
    return out;
}
}
