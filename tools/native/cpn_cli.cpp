// SPDX-License-Identifier: GPL-3.0-or-later
#include "cpn/json.hpp"
#include <nlohmann/json.hpp>
#include <atomic>
#include <csignal>
#include <iostream>
#include <map>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif
namespace {
std::atomic<bool> interrupted{false};
static_assert(std::atomic<bool>::is_always_lock_free);
void on_signal(int){interrupted.store(true,std::memory_order_relaxed);}
struct TestDocument{std::string id,input;nlohmann::json expected;bool enabled{true};};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(TestDocument,id,input,expected,enabled)
int run(const std::vector<std::string>& arguments){
    try {
        std::map<std::string,std::string> options;
        for(std::size_t i=1;i<arguments.size();++i){
            if(arguments[i]=="--help"){
                std::cout<<"cpn-cli --profile PROFILE.json --source FILE --tests TESTS.json [--comparison strict|trailing|byte] [--output RESULT.json]\n";return 0;
            }
            if(i+1>=arguments.size())throw std::runtime_error("Missing option value");
            const auto key=arguments[i++];
            if(key!="--profile" && key!="--source" && key!="--tests" && key!="--comparison" && key!="--output")
                throw std::runtime_error("Unknown option: "+key);
            if(!options.emplace(key,arguments[i]).second)throw std::runtime_error("Duplicate option: "+key);
        }
        for(const auto key:{"--profile","--source","--tests"})if(!options.contains(key))throw std::runtime_error("Required option missing: "+std::string(key));
        auto files=cpn::native_file_store();cpn::BatchRequest request;
        request.source.original_path=std::filesystem::absolute(cpn::path_from_utf8(options.at("--source")));
        request.source.document_id=cpn::path_to_utf8(request.source.original_path);request.source.revision=1;
        request.source.text=files->read(request.source.original_path,16*1024*1024);
        request.toolchain=cpn::load_toolchain_profile(cpn::path_from_utf8(options.at("--profile")),*files);
        const auto tests=nlohmann::json::parse(files->read(cpn::path_from_utf8(options.at("--tests")),64*1024*1024)).get<std::vector<TestDocument>>();
        for(const auto& test:tests){
            cpn::TestCase sample{test.id,test.input,std::nullopt,test.enabled};
            if(!test.expected.is_null())sample.expected=test.expected.get<std::string>();
            request.tests.push_back(std::move(sample));
        }
        if(options.contains("--comparison")){
            const auto& mode=options.at("--comparison");
            if(mode=="strict")request.comparison=cpn::ComparisonMode::cp_editor_strict;
            else if(mode=="trailing")request.comparison=cpn::ComparisonMode::cp_editor_ignore_trailing_spaces;
            else if(mode=="byte")request.comparison=cpn::ComparisonMode::byte_exact;
            else throw std::runtime_error("Unknown comparison policy");
        }
        auto previous=std::signal(SIGINT,on_signal);
        std::stop_source cancellation;
        cpn::BatchResult result;
        {
            std::jthread watcher([&](std::stop_token stop){
                while(!stop.stop_requested()){
                    if(interrupted.load(std::memory_order_relaxed)){cancellation.request_stop();return;}
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            });
            result=cpn::BatchService(files).run(request,cancellation.get_token());
        }
        std::signal(SIGINT,previous);
        const auto output=cpn::serialize_batch_result(result)+"\n";
        if(options.contains("--output"))files->write_atomic(cpn::path_from_utf8(options.at("--output")),output);
        else {std::cout<<output;std::cout.flush();if(!std::cout)throw std::runtime_error("Result output write failed");}
        if(!result.error.empty() || result.build_outcome==cpn::BuildOutcome::failed || result.build_outcome==cpn::BuildOutcome::source_error)return 2;
        if(result.build_outcome==cpn::BuildOutcome::cancelled)return 1;
        for(const auto& test:result.tests)if(test.verdict!=cpn::Verdict::accepted && test.verdict!=cpn::Verdict::unchecked)return 1;
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 2;}
}
}
#ifdef _WIN32
int wmain(int argc,wchar_t** argv){
    std::vector<std::string> arguments;
    for(int i=0;i<argc;++i){
        const int size=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,argv[i],-1,nullptr,0,nullptr,nullptr);
        if(!size)return 2;
        std::string value(static_cast<std::size_t>(size),'\0');
        if(!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,argv[i],-1,value.data(),size,nullptr,nullptr))return 2;
        value.pop_back();arguments.push_back(std::move(value));
    }
    return run(arguments);
}
#else
int main(int argc,char** argv){return run({argv,argv+argc});}
#endif
