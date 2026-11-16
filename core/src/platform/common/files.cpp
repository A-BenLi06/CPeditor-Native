// SPDX-License-Identifier: GPL-3.0-or-later
#include "cpn/files.hpp"
#include "cpn/text.hpp"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <fstream>
#include <random>
#include <stdexcept>
#include <system_error>
#include <thread>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <unistd.h>
#endif
namespace cpn {
namespace {
class NativeFileLock final : public FileLock {
public:
#ifdef _WIN32
    explicit NativeFileLock(HANDLE handle):handle_(handle){}
    ~NativeFileLock() override {OVERLAPPED position{};UnlockFileEx(handle_,0,1,0,&position);CloseHandle(handle_);}
private:
    HANDLE handle_;
#else
    explicit NativeFileLock(int handle):handle_(handle){}
    ~NativeFileLock() override {::flock(handle_,LOCK_UN);::close(handle_);}
private:
    int handle_;
#endif
};
std::string unique_suffix() {
    static std::atomic<std::uint64_t> counter{0};
    std::random_device random;
    return std::to_string(random())+"-"+std::to_string(random())+"-"+std::to_string(++counter);
}
class NativeFileStore final : public FileStore {
public:
    std::optional<std::string> read_optional(const std::filesystem::path& path,std::size_t max_bytes) const override {
        (void)path_to_utf8(path);
        std::error_code failure;
        const auto state=std::filesystem::symlink_status(path,failure);
        if(!std::filesystem::exists(state)){
            if(failure && failure!=std::errc::no_such_file_or_directory)throw std::system_error(failure);
            return std::nullopt;
        }
        if(failure)throw std::system_error(failure);
        return read(path,max_bytes);
    }
    std::unique_ptr<FileLock> acquire_lock(const std::filesystem::path& path,
        std::chrono::milliseconds timeout,std::stop_token stop) const override {
        (void)path_to_utf8(path);
        const auto maximum=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::duration::max()).count()/4;
        if(timeout.count()<0 || timeout.count()>maximum)throw std::invalid_argument("Invalid lock timeout");
        if(stop.stop_requested())throw std::system_error(std::make_error_code(std::errc::operation_canceled));
        const auto deadline=std::chrono::steady_clock::now()+timeout;
#ifdef _WIN32
        const auto handle=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,
            nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if(handle==INVALID_HANDLE_VALUE)throw std::system_error(GetLastError(),std::system_category(),"Open lock file");
        bool locked=false;
        try{
            BY_HANDLE_FILE_INFORMATION info{};
            if(!GetFileInformationByHandle(handle,&info))throw std::system_error(GetLastError(),std::system_category(),"Inspect lock file");
            if(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))throw std::invalid_argument("Lock path must be a regular file");
            for(;;){
                if(stop.stop_requested())throw std::system_error(std::make_error_code(std::errc::operation_canceled));
                OVERLAPPED position{};
                if(LockFileEx(handle,LOCKFILE_EXCLUSIVE_LOCK|LOCKFILE_FAIL_IMMEDIATELY,0,1,0,&position)){locked=true;return std::make_unique<NativeFileLock>(handle);}
                const auto failure=GetLastError();
                if(failure!=ERROR_LOCK_VIOLATION)throw std::system_error(failure,std::system_category(),"LockFileEx");
                if(std::chrono::steady_clock::now()>=deadline)throw std::system_error(std::make_error_code(std::errc::timed_out));
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }catch(...){if(locked){OVERLAPPED position{};UnlockFileEx(handle,0,1,0,&position);}CloseHandle(handle);throw;}
#else
        const auto handle=::open(path.c_str(),O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);
        if(handle<0)throw std::system_error(errno,std::generic_category(),"Open lock file");
        bool locked=false;
        try{
            struct stat info{};
            if(::fstat(handle,&info)<0)throw std::system_error(errno,std::generic_category(),"Inspect lock file");
            if(!S_ISREG(info.st_mode))throw std::invalid_argument("Lock path must be a regular file");
            for(;;){
                if(stop.stop_requested())throw std::system_error(std::make_error_code(std::errc::operation_canceled));
                if(::flock(handle,LOCK_EX|LOCK_NB)==0){locked=true;return std::make_unique<NativeFileLock>(handle);}
                const auto failure=errno;
                if(failure!=EWOULDBLOCK && failure!=EAGAIN && failure!=EINTR)throw std::system_error(failure,std::generic_category(),"flock");
                if(std::chrono::steady_clock::now()>=deadline)throw std::system_error(std::make_error_code(std::errc::timed_out));
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }catch(...){if(locked)::flock(handle,LOCK_UN);::close(handle);throw;}
#endif
    }
    std::string read(const std::filesystem::path& path, std::size_t max_bytes) const override {
        (void)path_to_utf8(path);
        std::ifstream stream(path,std::ios::binary);
        if(!stream) throw std::runtime_error("Cannot open file: "+path_to_utf8(path));
        std::string bytes;
        char buffer[8192];
        while(stream.read(buffer,sizeof(buffer)) || stream.gcount()>0) {
            auto count=static_cast<std::size_t>(stream.gcount());
            if(count>max_bytes-bytes.size()) throw std::runtime_error("File exceeds byte limit");
            bytes.append(buffer,count);
        }
        if(!stream.eof()) throw std::runtime_error("File read failed");
        return bytes;
    }
    std::filesystem::path create_workspace() const override {
        for(int attempt=0;attempt<16;++attempt) {
            auto path=std::filesystem::temp_directory_path()/path_from_utf8("cpn-"+unique_suffix());
#ifdef _WIN32
            std::error_code error;
            if(std::filesystem::create_directory(path,error)) return path;
            if(error && error!=std::errc::file_exists) throw std::system_error(error);
#else
            if(::mkdir(path.c_str(),0700)==0) return path;
            if(errno!=EEXIST) throw std::system_error(errno,std::generic_category(),"mkdir");
#endif
        }
        throw std::runtime_error("Cannot allocate unique workspace");
    }
    void remove_workspace(const std::filesystem::path& path) const noexcept override {
        std::error_code ignored;std::filesystem::remove_all(path,ignored);
    }
    void write_atomic(const std::filesystem::path& destination,std::string_view bytes) const override {
        (void)path_to_utf8(destination);
        auto parent=destination.parent_path();
        auto temporary=parent/path_from_utf8(".cpn-"+unique_suffix()+".tmp");
#ifdef _WIN32
        HANDLE file=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file==INVALID_HANDLE_VALUE) throw std::system_error(GetLastError(),std::system_category(),"CreateFileW");
        bool closed=false;
        try {
            std::size_t offset=0;
            while(offset<bytes.size()) {
                const auto count=static_cast<DWORD>(std::min<std::size_t>(bytes.size()-offset,1u<<20));
                DWORD written=0;
                if(!WriteFile(file,bytes.data()+offset,count,&written,nullptr) || written==0)
                    throw std::system_error(GetLastError(),std::system_category(),"WriteFile");
                offset+=written;
            }
            if(!FlushFileBuffers(file)) throw std::system_error(GetLastError(),std::system_category(),"FlushFileBuffers");
            CloseHandle(file);closed=true;
            if(!MoveFileExW(temporary.c_str(),destination.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
                throw std::system_error(GetLastError(),std::system_category(),"MoveFileExW");
        } catch(...) {
            if(!closed) CloseHandle(file);
            DeleteFileW(temporary.c_str());throw;
        }
#else
        if(parent.empty()) parent=".";
        // Even exception-message storage is ready before rename commits. A late
        // allocation failure must never hide that the destination was replaced.
        const AtomicWriteError directory_open_failure("File replaced; cannot open directory for durability",true);
        const AtomicWriteError directory_sync_failure("File replaced; directory durability could not be confirmed",true);
        int file=::open(temporary.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
        if(file<0) throw std::system_error(errno,std::generic_category(),"open");
        bool closed=false;
        try {
            struct stat previous{};
            if(::lstat(destination.c_str(),&previous)==0) {
                if(S_ISREG(previous.st_mode) && ::fchmod(file,previous.st_mode & 0777)<0)
                    throw std::system_error(errno,std::generic_category(),"fchmod");
            } else if(errno!=ENOENT) throw std::system_error(errno,std::generic_category(),"lstat");
            std::size_t offset=0;
            while(offset<bytes.size()) {
                auto written=::write(file,bytes.data()+offset,bytes.size()-offset);
                if(written<0 && errno==EINTR) continue;
                if(written<=0) throw std::system_error(errno,std::generic_category(),"write");
                offset+=static_cast<std::size_t>(written);
            }
            if(::fsync(file)<0) throw std::system_error(errno,std::generic_category(),"fsync");
            if(::close(file)<0) {closed=true;throw std::system_error(errno,std::generic_category(),"close");}
            closed=true;
            if(::rename(temporary.c_str(),destination.c_str())<0)
                throw std::system_error(errno,std::generic_category(),"rename");
            const int directory=::open(parent.c_str(),O_RDONLY|O_CLOEXEC);
            if(directory<0) throw directory_open_failure;
            const auto synced=::fsync(directory);::close(directory);
            if(synced<0) throw directory_sync_failure;
        } catch(...) {
            if(!closed) ::close(file);
            ::unlink(temporary.c_str());throw;
        }
#endif
    }
};
}
std::shared_ptr<FileStore> native_file_store(){return std::make_shared<NativeFileStore>();}
std::filesystem::path path_from_utf8(std::string_view value){
    if(value.find('\0')!=std::string_view::npos || decode_utf8(value).error_offset)
        throw std::invalid_argument("Path must be valid UTF-8 without NUL");
    std::u8string bytes;bytes.reserve(value.size());
    for(const auto byte:value) bytes.push_back(static_cast<char8_t>(static_cast<unsigned char>(byte)));
    return std::filesystem::path(bytes);
}
std::string path_to_utf8(const std::filesystem::path& path){
    auto bytes=path.u8string();std::string value{bytes.begin(),bytes.end()};
    if(value.find('\0')!=std::string::npos || decode_utf8(value).error_offset)
        throw std::invalid_argument("Path must be valid UTF-8 without NUL");
    return value;
}
}
