/**
 * @file src/stat_trackers.cpp
 * @brief Definitions for streaming statistic tracking.
 */
// standard includes
#include <cstdio>

// local includes
#include "stat_trackers.h"

namespace stat_trackers {

  /**
   * @brief Format a value with one fractional digit.
   */
  std::string one_digit_after_decimal(double value) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", value);
    return std::string(buf);
  }

  /**
   * @brief Format a value with two fractional digits.
   */
  std::string two_digits_after_decimal(double value) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", value);
    return std::string(buf);
  }

}  // namespace stat_trackers
