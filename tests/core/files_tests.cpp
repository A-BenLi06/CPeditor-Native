// SPDX-License-Identifier: GPL-3.0-or-later
#include "cpn/files.hpp"
#include "cpn/process.hpp"
#include <iostream>
#include <stdexcept>
#include <system_error>
#ifndef _WIN32
#include <sys/stat.h>
#endif
namespace {
void expect(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
template<class F> void rejected(F f,const char* text){try{f();}catch(const std::exception&){return;}throw std::runtime_error(text);}
}
int verify(const std::filesystem::path& helper){
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
        expect(!files->read_optional(workspace/"missing",10),"missing optional read");
        expect(files->read_optional(path,20)=="replacement","existing optional read");
        const auto lock_path=workspace/"workspace.lock";
        auto first=files->acquire_lock(lock_path,std::chrono::milliseconds(0));
        cpn::ProcessSpec child;child.executable=helper;child.arguments={cpn::path_to_utf8(lock_path)};
        expect(cpn::run_process(child).exit_code==3,"other process sees held exclusive lock");
        try{(void)files->acquire_lock(lock_path,std::chrono::milliseconds(0));throw std::runtime_error("second writer lock must reject");}
        catch(const std::system_error& failure){expect(failure.code()==std::errc::timed_out,"typed lock timeout");}
        std::stop_source stop;stop.request_stop();
        try{(void)files->acquire_lock(lock_path,std::chrono::milliseconds(100),stop.get_token());throw std::runtime_error("cancelled lock must reject");}
        catch(const std::system_error& failure){expect(failure.code()==std::errc::operation_canceled,"typed lock cancellation");}
        first.reset();auto next=files->acquire_lock(lock_path,std::chrono::milliseconds(0));next.reset();
        expect(cpn::run_process(child).exit_code==0,"other process acquires released lock");
        expect(std::filesystem::exists(lock_path),"lock inode retained for later writers");
        files->remove_workspace(workspace);std::cout<<"file tests passed\n";return 0;
    }catch(const std::exception& error){if(!workspace.empty())files->remove_workspace(workspace);std::cerr<<error.what()<<'\n';return 1;}
}
#ifdef _WIN32
int wmain(int argc,wchar_t** argv){return argc==2?verify(std::filesystem::path(argv[1])):2;}
#else
int main(int argc,char** argv){return argc==2?verify(cpn::path_from_utf8(argv[1])):2;}
#endif
