#ifndef CPN_PORTABLE_ADAPTER_TEST
#include "pch.h"
#endif
#include "SourceStore.h"
#include "cpn/digest.hpp"
#include "cpn/text.hpp"
#include <climits>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace cpn::windows {
namespace {
std::string lock_identity(std::filesystem::path const& path) {
#if defined(_WIN32)
    const auto& native = path.native();
    if (native.size() > static_cast<std::size_t>(INT_MAX)) throw std::invalid_argument("Source coordination path is too long.");
    const auto length = static_cast<int>(native.size());
    const auto required = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, native.data(), length, nullptr, 0, nullptr, nullptr, 0);
    if (!required) throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Normalize source coordination identity");
    std::wstring folded(static_cast<std::size_t>(required), L'\0');
    if (!LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, native.data(), length, folded.data(), required, nullptr, nullptr, 0))
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Normalize source coordination identity");
    // Conservative on Windows directories that opt into case sensitivity: distinct files may share a lock.
    return cpn::path_to_utf8(std::filesystem::path{folded});
#else
    return cpn::path_to_utf8(path);
#endif
}
bool write_new(cpn::FileStore const& files, std::filesystem::path const& path, std::string_view bytes) {
    std::random_device entropy;
    std::filesystem::path temporary;
    for (int attempt = 0; attempt < 8; ++attempt) {
        std::ostringstream name; name << ".cpn-save-" << std::hex;
        for (int part = 0; part < 4; ++part) name << entropy();
        auto candidate = path.parent_path() / cpn::path_from_utf8(name.str());
        if (std::filesystem::create_directory(candidate)) { temporary = std::move(candidate); break; }
    }
    if (temporary.empty()) throw std::runtime_error("Cannot reserve a private source-save directory.");
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); } } cleanup{temporary};
    const auto prepared = temporary / cpn::path_from_utf8("contents");
    try { files.write_atomic(prepared, bytes); }
    catch (cpn::AtomicWriteError const& error) { throw cpn::AtomicWriteError(error.what(), false); }
#if defined(_WIN32)
    if (!MoveFileExW(prepared.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH)) {
        const auto error = GetLastError();
        if (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS) return false;
        throw std::system_error(static_cast<int>(error), std::system_category(), "Create source file");
    }
#else
    // link commits the prepared file only if the destination is still absent; it never replaces a racer.
    if (::link(prepared.c_str(), path.c_str()) != 0) {
        const auto error = errno; if (error == EEXIST) return false;
        throw std::system_error(error, std::generic_category(), "Create source file");
    }
    const int parent = ::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
    if (parent < 0) throw cpn::AtomicWriteError(std::system_error(errno, std::generic_category()).what(), true);
    const int flushed = ::fsync(parent); const auto error = errno; ::close(parent);
    if (flushed != 0) throw cpn::AtomicWriteError(std::system_error(error, std::generic_category()).what(), true);
#endif
    return true;
}
}
SourceStore::SourceStore(Options options, std::shared_ptr<cpn::FileStore> files)
    : options_(std::move(options)), files_(std::move(files)) {
    if (!files_ || options_.lock_directory.empty() || !options_.maximum_bytes ||
        options_.maximum_bytes > std::numeric_limits<std::size_t>::max() - 3 ||
        options_.lock_timeout.count() < 0 || options_.lock_timeout.count() > 60000)
        throw std::invalid_argument("Invalid native source-file options.");
    (void)cpn::path_to_utf8(options_.lock_directory);
}
std::filesystem::path SourceStore::resolve(std::filesystem::path const& path) {
    if (path.empty()) throw std::invalid_argument("Source path is empty.");
    (void)cpn::path_to_utf8(path);
    // Resolve existing symlinks to their file, so atomic replacement does not overwrite the link itself.
    auto status = std::filesystem::symlink_status(path);
    if (status.type() != std::filesystem::file_type::not_found) {
        auto canonical = std::filesystem::canonical(path);
        if (!std::filesystem::is_regular_file(canonical)) throw std::invalid_argument("Source path is not a regular file.");
        return canonical;
    }
    return std::filesystem::canonical(std::filesystem::absolute(path).parent_path()) / path.filename();
}
SourceContents SourceStore::read(std::filesystem::path const& input) const {
    auto path = resolve(input);
    auto bytes = files_->read(path, options_.maximum_bytes + 3);
    SourceContents result{path, bytes, cpn::sha256(bytes), false};
    if (result.utf8.starts_with("\xEF\xBB\xBF")) { result.utf8.erase(0, 3); result.utf8_bom = true; }
    if (result.utf8.size() > options_.maximum_bytes || cpn::decode_utf8(result.utf8).error_offset)
        throw std::invalid_argument("Source file exceeds the UTF-8 document limit.");
    return result;
}
std::optional<std::string> SourceStore::disk_digest(std::filesystem::path const& input) const {
    auto bytes = files_->read_optional(resolve(input), options_.maximum_bytes + 3);
    return bytes ? std::optional<std::string>{cpn::sha256(*bytes)} : std::nullopt;
}
SourceSaveResult SourceStore::save(std::filesystem::path const& input, std::string_view utf8, bool bom,
    std::optional<std::string> const& expected, std::stop_token stop) const {
    if (utf8.size() > options_.maximum_bytes || cpn::decode_utf8(utf8).error_offset)
        throw std::invalid_argument("Source snapshot exceeds the UTF-8 document limit.");
    auto path = resolve(input);
    std::filesystem::create_directories(options_.lock_directory);
    auto lock_path = options_.lock_directory / cpn::path_from_utf8(cpn::sha256(lock_identity(path)) + ".lock");
    if (std::filesystem::weakly_canonical(lock_path) == path ||
        (std::filesystem::exists(lock_path) && std::filesystem::exists(path) && std::filesystem::equivalent(lock_path, path)))
        throw std::invalid_argument("Source path aliases its coordination lock.");
    std::unique_ptr<cpn::FileLock> lock;
    try { lock = files_->acquire_lock(lock_path, options_.lock_timeout, stop); }
    catch (std::system_error const& error) {
        if (error.code() != std::errc::operation_canceled && error.code() != std::errc::timed_out) throw;
        return {SourceSaveResult::Status::lock_unavailable, {}, false, error.what()};
    }
    if (!lock) return {SourceSaveResult::Status::lock_unavailable, {}, false, "Source coordination lock is unavailable."};
    auto current = files_->read_optional(path, options_.maximum_bytes + 3);
    auto digest = current ? std::optional<std::string>{cpn::sha256(*current)} : std::nullopt;
    if (digest != expected) return {SourceSaveResult::Status::conflict, digest, false, "Source file changed outside this document."};
    std::string bytes;
    if (bom) bytes = "\xEF\xBB\xBF";
    bytes.append(utf8);
    auto written_digest = cpn::sha256(bytes);
    if (stop.stop_requested()) return {SourceSaveResult::Status::lock_unavailable, digest, false, "Source save was cancelled."};
    // Recheck the path after preparing bytes. External writers bypassing the lock can still race rename.
    current = files_->read_optional(path, options_.maximum_bytes + 3);
    digest = current ? std::optional<std::string>{cpn::sha256(*current)} : std::nullopt;
    if (digest != expected) return {SourceSaveResult::Status::conflict, digest, false, "Source file changed before replacement."};
    try {
        if (!expected) {
            if (!write_new(*files_, path, bytes)) {
                auto raced = files_->read_optional(path, options_.maximum_bytes + 3);
                return {SourceSaveResult::Status::conflict, raced ? std::optional<std::string>{cpn::sha256(*raced)} : std::nullopt,
                    false, "Another writer created the source destination."};
            }
        } else files_->write_atomic(path, bytes);
    }
    catch (cpn::AtomicWriteError const& error) {
        if (!error.destination_replaced) throw;
        return {SourceSaveResult::Status::durability_unknown, std::move(written_digest), true, error.what()};
    }
    return {SourceSaveResult::Status::saved, std::move(written_digest), true, {}};
}
}
