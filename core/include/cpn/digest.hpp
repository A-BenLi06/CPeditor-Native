// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <string_view>
namespace cpn {
[[nodiscard]] std::string sha256(std::string_view bytes);
}
