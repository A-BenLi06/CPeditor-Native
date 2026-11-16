// SPDX-License-Identifier: GPL-3.0-or-later
#include "cpn/api.h"
#include "cpn/files.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
namespace {
using Json=nlohmann::json;
void expect(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
cpn_bytes view(const std::string& text){return {reinterpret_cast<const uint8_t*>(text.data()),text.size()};}
Json read(const cpn_buffer& buffer){return Json::parse(buffer.data,buffer.data+buffer.size);}
std::string request(const std::filesystem::path& helper,std::string mode="echo"){
    auto args=Json::array({mode});if(mode=="sleep")args.push_back("5000");
    return Json{{"schema_version",1},{"source",{{"document_id","source-中文"},{"revision",42},{"text","committed snapshot"}}},
        {"toolchain",{{"schema_version",1},{"id","helper"},{"language_id","test"},{"source_filename","source.txt"},
            {"run",{{"executable",cpn::path_to_utf8(helper)},{"arguments",args}}}}},
        {"comparison","byte"},{"tests",Json::array({{{"id","binary"},{"input",{{"encoding","base64"},{"data","YQBi"}}},
            {"expected",{{"encoding","base64"},{"data","YQBi"}}}}})}}.dump();
}
void run(const std::filesystem::path& helper){
    cpn_error error{};error.struct_size=sizeof(error);
    cpn_engine_handle engine=0;
    cpn_engine_options options{sizeof(options),CPN_ABI_VERSION,1,1,1,0,1024*1024,1024*1024};
    auto wrong=options;wrong.abi_version=99;
    expect(cpn_engine_create(&wrong,&engine,&error)==CPN_ABI_MISMATCH && !engine,"ABI mismatch");
    expect(cpn_engine_create(&options,&engine,&error)==CPN_OK,"engine create");
    cpn_buffer held{};expect(cpn_engine_info_json(engine,&held,&error)==CPN_OK,"info allocate");
    expect(read(held)["abi_version"]==1,"info schema");
    cpn_buffer blocked{};expect(cpn_engine_info_json(engine,&blocked,&error)==CPN_CAPACITY && !blocked.handle,"buffer capacity");
    cpn_job_handle job=0,other=0;
    expect(cpn_engine_submit_json(engine,{nullptr,5},&job,&error)==CPN_INVALID_ARGUMENT,"invalid request pointer");
    auto json=request(helper);expect(cpn_engine_submit_json(engine,view(json),&job,&error)==CPN_OK,"submit");
    expect(cpn_engine_submit_json(engine,view(json),&other,&error)==CPN_CAPACITY && !other,"job capacity");
    json.assign("caller modified bytes");
    expect(cpn_engine_cancel(engine,999999,&error)==CPN_INVALID_HANDLE,"unknown job");
    expect(cpn_engine_drain_json(engine,8,&blocked,&error)==CPN_CAPACITY && !blocked.handle,"failed allocation cannot ack");
    expect(cpn_engine_release_job(engine,job,&error)==CPN_BUSY,"terminal must drain");
    expect(cpn_buffer_release(held.handle,&error)==CPN_OK,"buffer release");
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);bool terminal=false;
    while(std::chrono::steady_clock::now()<deadline && !terminal){
        cpn_buffer events{};expect(cpn_engine_drain_json(engine,8,&events,&error)==CPN_OK,"drain");
        const auto document=read(events);
        for(const auto& event:document["events"]){
            expect(event["job_id"]==job,"event job ID");
            if(event.contains("result")){
                const auto& result=event["result"];terminal=true;
                expect(result["document_id"]=="source-中文" && result["revision"]==42,"snapshot copied");
                expect(result["tests"][0]["verdict"]=="AC","C ABI binary judge");
                expect(result["tests"][0]["process"]["stdout"]["data"]=="YQBi","binary result");
            }
        }
        expect(cpn_buffer_release(events.handle,&error)==CPN_OK,"events release");if(!terminal)std::this_thread::yield();
    }
    expect(terminal,"terminal deadline");
    expect(cpn_engine_cancel(engine,job,&error)==CPN_OK,"terminal cancel idempotent");
    expect(cpn_engine_release_job(engine,job,&error)==CPN_OK,"release completed");
    expect(cpn_engine_release_job(engine,job,&error)==CPN_INVALID_HANDLE,"released job stale");
    cpn_buffer events{};expect(cpn_engine_drain_json(engine,8,&events,&error)==CPN_OK,"drain empty");
    expect(read(events)["events"].empty(),"terminal exactly once");cpn_buffer_release(events.handle,nullptr);
    auto sleeping=request(helper,"sleep");expect(cpn_engine_submit_json(engine,view(sleeping),&job,&error)==CPN_OK,"capacity returns");
    expect(cpn_engine_close(engine,&error)==CPN_OK,"close cancels and joins");
    expect(cpn_engine_submit_json(engine,view(sleeping),&other,&error)==CPN_CLOSED,"closed rejects");
    expect(cpn_engine_info_json(engine,&held,&error)==CPN_OK,"held buffer after close");
    auto retained=read(held);expect(cpn_engine_destroy(engine,&error)==CPN_OK,"destroy");
    expect(read(held)==retained,"buffer survives engine destruction");
    expect(cpn_buffer_release(held.handle,&error)==CPN_OK,"release after destroy");
    expect(cpn_buffer_release(held.handle,&error)==CPN_INVALID_HANDLE,"double release");
    expect(cpn_engine_close(engine,&error)==CPN_INVALID_HANDLE,"stale engine");
}
}
int verify(const std::filesystem::path& helper){try{run(std::filesystem::absolute(helper));
    std::cout<<"C ABI tests passed\n";return 0;}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
#ifdef _WIN32
int wmain(int argc,wchar_t** argv){return argc==2?verify(std::filesystem::path(argv[1])):2;}
#else
int main(int argc,char** argv){return argc==2?verify(cpn::path_from_utf8(argv[1])):2;}
#endif
