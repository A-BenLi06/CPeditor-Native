#include "pch.h"
#include "FileStore.h"
#include <objbase.h>

namespace cpn::windows {
namespace {
class Handle {
public:
    explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
    ~Handle() { close(); }
    Handle(Handle const&) = delete;
    Handle& operator=(Handle const&) = delete;
    [[nodiscard]] HANDLE get() const noexcept { return value_; }
    void close() noexcept {
        if (value_ != INVALID_HANDLE_VALUE) {
            CloseHandle(value_);
            value_ = INVALID_HANDLE_VALUE;
        }
    }
private:
    HANDLE value_;
};
void check(bool condition) {
    if (!condition) winrt::throw_last_error();
}
class TemporaryFile {
public:
    explicit TemporaryFile(std::filesystem::path path) : path_(std::move(path)) {}
    ~TemporaryFile() { if (created_ && !committed_) DeleteFileW(path_.c_str()); }
    void created() noexcept { created_ = true; }
    void committed() noexcept { committed_ = true; }
private:
    std::filesystem::path path_;
    bool created_{false};
    bool committed_{false};
};
}

void FileStore::validate_utf8(std::string_view bytes) {
    if (bytes.empty()) return;
    if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        winrt::throw_hresult(E_BOUNDS);
    }
    check(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                             static_cast<int>(bytes.size()), nullptr, 0) != 0);
}

LoadedDocument FileStore::read(std::filesystem::path const& path, std::uint32_t limit) {
    // Deny in-place writers while retaining an immutable opened-file generation.
    Handle file{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    check(file.get() != INVALID_HANDLE_VALUE);
    LARGE_INTEGER size{};
    check(GetFileSizeEx(file.get(), &size) != FALSE);
    if (size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) > limit) {
        winrt::throw_hresult(E_BOUNDS);
    }
    LoadedDocument result;
    result.utf8.resize(static_cast<std::size_t>(size.QuadPart));
    std::size_t offset = 0;
    while (offset < result.utf8.size()) {
        DWORD count{};
        const auto remaining = static_cast<DWORD>(result.utf8.size() - offset);
        check(ReadFile(file.get(), result.utf8.data() + offset, remaining, &count, nullptr) != FALSE);
        if (count == 0) winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_HANDLE_EOF));
        offset += count;
    }
    if (result.utf8.starts_with("\xEF\xBB\xBF")) {
        result.utf8.erase(0, 3);
        result.utf8_bom = true;
    }
    validate_utf8(result.utf8);
    return result;
}

void FileStore::write_atomic(std::filesystem::path const& path, std::string_view bytes) {
    GUID id{};
    winrt::check_hresult(CoCreateGuid(&id));
    wchar_t identity[40]{};
    if (StringFromGUID2(id, identity, static_cast<int>(std::size(identity))) == 0) {
        winrt::throw_hresult(E_UNEXPECTED);
    }
    const auto temporary = path.parent_path() /
        (path.filename().wstring() + L".cpn-" + identity + L".tmp");
    TemporaryFile cleanup{temporary};
    Handle file{CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL, nullptr)};
    check(file.get() != INVALID_HANDLE_VALUE);
    cleanup.created();
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD written{};
        const auto count = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset,
                                                                  1024 * 1024));
        check(WriteFile(file.get(), bytes.data() + offset, count, &written, nullptr) != FALSE);
        if (written == 0) winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_WRITE_FAULT));
        offset += written;
    }
    check(FlushFileBuffers(file.get()) != FALSE);
    file.close();
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        check(ReplaceFileW(path.c_str(), temporary.c_str(), nullptr, 0, nullptr, nullptr) != FALSE);
    } else {
        const auto error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
            winrt::throw_hresult(HRESULT_FROM_WIN32(error));
        }
        // Do not replace a destination created by another process after the existence check.
        check(MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE);
    }
    cleanup.committed();
}
}
