// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
namespace cpn {
class AtomicWriteError : public std::runtime_error {
public:
    AtomicWriteError(std::string message, bool replaced)
        : std::runtime_error(std::move(message)), destination_replaced(replaced) {}
    // true means rename committed; persistence to storage could not be confirmed.
    const bool destination_replaced;
};
class FileStore {
public:
    virtual ~FileStore() = default;
    virtual std::string read(const std::filesystem::path&, std::size_t max_bytes) const = 0;
    virtual void write_atomic(const std::filesystem::path&, std::string_view) const = 0;
    virtual std::filesystem::path create_workspace() const = 0;
    virtual void remove_workspace(const std::filesystem::path&) const noexcept = 0;
};
[[nodiscard]] std::shared_ptr<FileStore> native_file_store();
[[nodiscard]] std::filesystem::path path_from_utf8(std::string_view);
[[nodiscard]] std::string path_to_utf8(const std::filesystem::path&);
}
