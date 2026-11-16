// SPDX-License-Identifier: GPL-3.0-or-later
#include "cpn/files.hpp"
#include <system_error>
int verify(const std::filesystem::path& path){
    try{auto lock=cpn::native_file_store()->acquire_lock(path,std::chrono::milliseconds(0));return 0;}
    catch(const std::system_error& failure){return failure.code()==std::errc::timed_out?3:2;}
    catch(...){return 2;}
}
#ifdef _WIN32
int wmain(int argc,wchar_t** argv){return argc==2?verify(std::filesystem::path(argv[1])):2;}
#else
int main(int argc,char** argv){return argc==2?verify(cpn::path_from_utf8(argv[1])):2;}
#endif
