// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "cpn/process.hpp"
#include <functional>
#include <string>
#include <vector>

namespace cpn {
struct CommandTemplate {
    std::string executable;
    std::vector<std::string> arguments;
};
using ProcessExecutor = std::function<ProcessResult(const ProcessSpec&, std::stop_token)>;
} // namespace cpn
