// SPDX-License-Identifier: GPL-3.0-or-later
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <signal.h>
#include <spawn.h>
#include <unistd.h>
extern char** environ;
#endif

namespace {
std::string utf8_path(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}
std::filesystem::path path_from_utf8(const std::string& value) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size()));
}
#if defined(_WIN32)
std::string utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}
std::wstring quote(const std::wstring& value) {
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (wchar_t character : value) {
        if (character == L'\\') { ++slashes; continue; }
        result.append(character == L'"' ? 2 * slashes + 1 : slashes, L'\\');
        result.push_back(character); slashes = 0;
    }
    result.append(2 * slashes, L'\\'); result.push_back(L'"'); return result;
}
#endif
int run(const std::vector<std::string>& arguments) {
    if (arguments.size() < 2) return 2;
    const auto& mode = arguments[1];
    if (mode == "echo" || mode == "duplex") {
        if (mode == "duplex") { std::cerr << std::string(262144, 'E'); std::cerr.flush(); }
        char buffer[32768];
        while (std::cin.read(buffer, sizeof(buffer)) || std::cin.gcount())
            std::cout.write(buffer, std::cin.gcount());
        return 0;
    }
    if (mode == "flood") {
        const std::string chunk(32768, 'F');
        for (;;) { std::cout.write(chunk.data(), chunk.size()); std::cout.flush(); std::cerr.write(chunk.data(), chunk.size()); std::cerr.flush(); }
    }
    if (mode == "emit") {
        const auto size = static_cast<std::size_t>(std::stoull(arguments.at(2)));
        std::cout << std::string(size, 'O'); return 0;
    }
    if (mode == "sleep") { std::this_thread::sleep_for(std::chrono::milliseconds(std::stoll(arguments.at(2)))); return 0; }
    if (mode == "exit") return std::stoi(arguments.at(2));
    if (mode == "args") {
        for (std::size_t index = 2; index < arguments.size(); ++index)
            std::cout << arguments[index].size() << ':' << arguments[index] << '\n';
        return 0;
    }
    if (mode == "env") {
#if defined(_WIN32)
        std::wstring key(arguments.at(2).begin(), arguments.at(2).end());
        if (const auto* value = ::_wgetenv(key.c_str())) std::cout << utf8(value);
#else
        if (const auto* value = std::getenv(arguments.at(2).c_str())) std::cout << value;
#endif
        return 0;
    }
    if (mode == "cwd") { std::cout << utf8_path(std::filesystem::current_path()); return 0; }
    if (mode == "close-input") {
#if defined(_WIN32)
        ::_close(0);
#else
        ::close(STDIN_FILENO);
#endif
        std::cout << "closed"; return 0;
    }
    if (mode == "marker-loop") {
#if !defined(_WIN32)
        ::signal(SIGTERM, SIG_IGN);
#endif
        const auto marker = path_from_utf8(arguments.at(2));
        for (unsigned long long count = 0;; ++count) {
            { std::ofstream file(marker); file << count; }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    if (mode == "descendant" || mode == "escaped") {
        const auto marker = path_from_utf8(arguments.at(2));
        const auto executable = path_from_utf8(arguments[0]);
        unsigned long long child_id = 0;
#if defined(_WIN32)
        std::wstring command = quote(executable.native()) + L" marker-loop " + quote(marker.native());
        STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE); startup.hStdOutput = ::GetStdHandle(STD_OUTPUT_HANDLE);
        startup.hStdError = ::GetStdHandle(STD_ERROR_HANDLE);
        PROCESS_INFORMATION information{};
        if (!::CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                              nullptr, nullptr, &startup, &information)) return 3;
        child_id = information.dwProcessId;
        ::CloseHandle(information.hThread); ::CloseHandle(information.hProcess);
#else
        std::string program = executable.native(), marker_text = marker.native();
        char mode_text[] = "marker-loop";
        char* argv[]{program.data(), mode_text, marker_text.data(), nullptr};
        posix_spawnattr_t attributes;
        ::posix_spawnattr_init(&attributes);
        if (mode == "escaped") { ::posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP); ::posix_spawnattr_setpgroup(&attributes, 0); }
        pid_t child;
        const int error = ::posix_spawn(&child, program.c_str(), nullptr, &attributes, argv, environ);
        ::posix_spawnattr_destroy(&attributes);
        if (error) return 3;
        child_id = static_cast<unsigned long long>(child);
#endif
        for (int retry = 0; retry < 100 && !std::filesystem::exists(marker); ++retry)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::cout << child_id << '\n'; std::cout.flush(); return 0;
    }
#if !defined(_WIN32)
    if (mode == "signal") { ::raise(SIGTERM); return 4; }
#endif
    return 2;
}
} // namespace
#if defined(_WIN32)
int wmain(int argc, wchar_t** argv) {
    ::_setmode(::_fileno(stdin), _O_BINARY); ::_setmode(::_fileno(stdout), _O_BINARY); ::_setmode(::_fileno(stderr), _O_BINARY);
    std::vector<std::string> arguments;
    for (int index = 0; index < argc; ++index) arguments.push_back(utf8(argv[index]));
    return run(arguments);
}
#else
int main(int argc, char** argv) { return run({argv, argv + argc}); }
#endif
