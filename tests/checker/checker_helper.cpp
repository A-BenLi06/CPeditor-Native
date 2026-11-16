// SPDX-License-Identifier: GPL-3.0-or-later
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
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
#endif
namespace {
std::filesystem::path path(std::string_view value) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size()));
}
std::string read(const std::filesystem::path& value) {
    std::ifstream stream(value, std::ios::binary); if (!stream) throw std::runtime_error("Missing checker file");
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
std::string fixture(std::string value) { value.push_back('\0'); value.push_back(static_cast<char>(0xff)); return value; }
int run(const std::vector<std::string>& args) {
    if (args.size() < 5) return 4;
    const auto input_path = path(args[2]), actual_path = path(args[3]), expected_path = path(args[4]);
    for (const auto& file : {input_path, actual_path, expected_path})
        if (!std::filesystem::equivalent(file.parent_path(), std::filesystem::current_path())) return 4;
    const auto input = read(input_path), actual = read(actual_path), expected = read(expected_path);
    const auto& mode = args[1];
    if (mode == "verify") {
        if (input != fixture("input") || actual != fixture("actual") || expected != fixture("expected")) return 4;
        std::cout.write(input.data(), input.size()); std::cout.write(actual.data(), actual.size()); std::cout.write(expected.data(), expected.size());
        const auto diagnostic = fixture("diagnostic"); std::cerr.write(diagnostic.data(), diagnostic.size()); return 0;
    }
    if (mode == "compare") return actual == expected ? 0 : 1;
    if (mode == "exit") {
        const auto code = std::stoi(args.at(5)); if (code == 7) std::cout << "0.5\n"; return code;
    }
    if (mode == "args" || mode == "source-dir") { std::cout << args.at(5); return 0; }
    if (mode == "sleep") { std::this_thread::sleep_for(std::chrono::seconds(5)); return 0; }
    if (mode == "signal") {
#if defined(_WIN32)
        ::TerminateProcess(::GetCurrentProcess(), 0xc0000005); return 4;
#else
        ::raise(SIGTERM); return 4;
#endif
    }
    if (mode == "flood") {
        const std::string value(32768, 'F');
        for (;;) { std::cout.write(value.data(), value.size()); std::cout.flush(); std::cerr.write(value.data(), value.size()); std::cerr.flush(); }
    }
    return 4;
}
#if defined(_WIN32)
std::string utf8(const wchar_t* value) {
    const int size = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, nullptr, 0, nullptr, nullptr);
    if (!size) throw std::runtime_error("Invalid checker Unicode argument");
    std::string result(static_cast<std::size_t>(size), '\0');
    if (!::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, result.data(), size, nullptr, nullptr)) throw std::runtime_error("Argument conversion failed");
    result.pop_back(); return result;
}
#endif
}
#if defined(_WIN32)
int wmain(int argc, wchar_t** argv) {
    ::_setmode(::_fileno(stdout), _O_BINARY); ::_setmode(::_fileno(stderr), _O_BINARY);
    try { std::vector<std::string> args; for (int index = 0; index < argc; ++index) args.push_back(utf8(argv[index])); return run(args); }
    catch (...) { return 4; }
}
#else
int main(int argc, char** argv) { try { return run({argv, argv + argc}); } catch (...) { return 4; } }
#endif
