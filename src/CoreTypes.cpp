#include "lob/CoreTypes.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace lob {

std::string formatRawPrice(Price price) {
    const bool negative = price < 0;
    // Negate in unsigned arithmetic so INT64_MIN doesn't overflow
    const std::uint64_t magnitude =
        negative ? 0 - static_cast<std::uint64_t>(price) : static_cast<std::uint64_t>(price);
    std::string digits = std::to_string(magnitude);
    const auto tickLen = static_cast<std::size_t>(MAX_TICK_LEN);
    if (digits.length() <= tickLen) {
        digits.insert(0, tickLen + 1 - digits.length(), '0');
    }
    digits.insert(digits.length() - tickLen, 1, '.');
    return negative ? '-' + digits : digits;
}

}  // namespace lob
