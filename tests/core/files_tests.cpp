// SPDX-License-Identifier: GPL-3.0-or-later
#include "cpn/files.hpp"
#include <iostream>
#include <stdexcept>
#ifndef _WIN32
#include <sys/stat.h>
#endif
namespace {
void expect(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
template<class F> void rejected(F f,const char* text){try{f();}catch(const std::exception&){return;}throw std::runtime_error(text);}
}
int main(){
    auto files=cpn::native_file_store();std::filesystem::path workspace;
    try{
        workspace=files->create_workspace();
        auto path=workspace/cpn::path_from_utf8("空格 文件.txt");
        const std::string bytes{"a\0b\r\n",5};files->write_atomic(path,bytes);
        expect(files->read(path,5)==bytes,"binary roundtrip");
        rejected([&]{files->read(path,4);},"read limit must reject");
#ifndef _WIN32
        expect(::chmod(path.c_str(),0644)==0,"set original mode");
#endif
        files->write_atomic(path,"replacement");
        expect(files->read(path,20)=="replacement","replace existing");
#ifndef _WIN32
        struct stat info{};expect(::stat(path.c_str(),&info)==0 && (info.st_mode&0777)==0644,"preserve permission");
#endif
        rejected([&]{(void)cpn::path_from_utf8(std::string("bad\0path",8));},"NUL path must reject");
        rejected([&]{(void)cpn::path_from_utf8(std::string("bad\xff",4));},"invalid UTF8 path must reject");
        rejected([&]{files->write_atomic(workspace/"missing"/"file","x");},"missing parent must fail");
        expect(std::distance(std::filesystem::directory_iterator(workspace),std::filesystem::directory_iterator{})==1,"temporary cleanup");
        files->remove_workspace(workspace);std::cout<<"file tests passed\n";return 0;
    }catch(const std::exception& error){if(!workspace.empty())files->remove_workspace(workspace);std::cerr<<error.what()<<'\n';return 1;}
}
