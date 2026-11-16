// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "cpn/batch.hpp"
#include <cstddef>
#include <string_view>
namespace cpn {
struct RequestBounds {
    std::size_t max_json_bytes{16 * 1024 * 1024};
    std::size_t max_retained_output_bytes{64 * 1024 * 1024};
    std::size_t max_tests{4096};
};
[[nodiscard]] BatchRequest parse_batch_request(std::string_view, RequestBounds = {});
[[nodiscard]] std::vector<TestCase> parse_test_cases(std::string_view, RequestBounds = {});
[[nodiscard]] ExternalCheckerSpec parse_external_checker_spec(std::string_view);
[[nodiscard]] std::string serialize_batch_result(const BatchResult&);
[[nodiscard]] std::string encode_base64(std::string_view);
[[nodiscard]] std::string decode_base64(std::string_view);
}
