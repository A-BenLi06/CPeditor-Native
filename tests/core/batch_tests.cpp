// SPDX-License-Identifier: GPL-3.0-or-later
#include "cpn/batch.hpp"
#include "cpn/text.hpp"
#include <iostream>
#include <stdexcept>
#include <thread>
namespace {
void expect(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class Operation> void rejects(Operation operation,const char* message){
    try{operation();}catch(const std::exception&){return;}throw std::runtime_error(message);
}
void profiles(){
    const auto document=[](const std::string& extra={},const std::string& version="1",const std::string& command=R"({"executable":"helper","arguments":[]})"){
        return std::string("{\"schema_version\":")+version+
            R"(,"id":"test","language_id":"cpp","source_filename":"main.cpp","run":)"+command+extra+"}";
    };
    auto profile=cpn::parse_toolchain_profile(document());
    expect(profile.id=="test" && profile.run_limits.wall_timeout.count()==2000,"profile defaults");
    for(const auto& version:{"1.0","true","\"1\"","-4294967295","4294967297","18446744073709551615","null"})
        rejects([&]{(void)cpn::parse_toolchain_profile(document({},version));},"strict version integer and range");
    for(const auto& key:{"spawn_timeout_ms","wall_timeout_ms","terminate_grace_ms","drain_timeout_ms","stdout_bytes","stderr_bytes","combined_bytes"})
        for(const auto& invalid:{"2.9","\"2\"","true","null","-1","18446744073709551615"})
            rejects([&]{(void)cpn::parse_toolchain_profile(document(",\"run_limits\":{\""+std::string(key)+"\":"+invalid+"}"));},"strict process limit fields");
    for(const auto& extra:{",\"schema_version\":1",",\"unknown\":1",",\"inherit_environment\":1",
                          ",\"run_limits\":{\"memory_bytes\":1}",",\"variables\":{}",
                          ",\"variables\":[{\"name\":\"x\",\"value\":1}]",
                          ",\"variables\":[{\"name\":\"x\",\"value\":\"ok\",\"unknown\":1}]"})
        rejects([&]{(void)cpn::parse_toolchain_profile(document(extra));},"strict profile object fields");
    rejects([&]{(void)cpn::parse_toolchain_profile(document({},"1",R"({"executable":"helper","executable":"other","arguments":[]})"));},"duplicate nested keys");
    rejects([&]{(void)cpn::parse_toolchain_profile(std::string(1024*1024+1,' '));},"profile byte limit");
    rejects([&]{(void)cpn::parse_toolchain_profile(std::string(70,'[')+"0"+std::string(70,']'));},"profile nesting limit");
    const auto original=profile;
    for(const auto& name:std::vector<std::string>{"../escape","..\\escape","stream:ads","CON.cpp","NUL","CON .txt","LPT¹.cpp","main.cpp.","main.cpp ","a?b",std::string("a\0b",3),std::string("main")+static_cast<char>(0xff)}){
        profile=original;profile.source_filename=name;
        rejects([&]{cpn::validate_toolchain_profile(profile);},"portable source filename validation");
    }
    profile=original;profile.source_filename="解答.cpp";cpn::validate_toolchain_profile(profile);
    profile=original;profile.build=cpn::CommandTemplate{"helper",{}};profile.artifact_name="MAIN.CPP";
    rejects([&]{cpn::validate_toolchain_profile(profile);},"case-insensitive source/artifact alias");
    for(const auto& variables:std::vector<std::vector<std::pair<std::string,std::string>>>{
        {{"","ok"}},{{"1name","ok"}},{{"name-x","ok"}},{{"source","ok"}},{{"x","one"},{"x","two"}},
        {{"x",std::string("a\0b",3)}},{{"x",std::string(1,static_cast<char>(0xff))}}}){
        profile=original;profile.variables=variables;
        rejects([&]{cpn::validate_toolchain_profile(profile);},"strict variable name/value/uniqueness");
    }
    profile=original;profile.environment={{"PATH","one"},{"path","two"}};
    rejects([&]{cpn::validate_toolchain_profile(profile);},"portable environment uniqueness");
    for(const auto& text:{"${MISSING}","${","${}"}){
        profile=original;profile.run.arguments={text};
        rejects([&]{cpn::validate_toolchain_profile(profile);},"template validated before build");
    }
}
cpn::ToolchainProfile helper_profile(const std::filesystem::path& helper,const std::string& mode){
    cpn::ToolchainProfile profile;profile.id="helper";profile.language_id="test-process";profile.source_filename="snapshot.txt";
    profile.run={cpn::path_to_utf8(helper),{mode}};return profile;
}
void batch(const std::filesystem::path& helper){
    cpn::BatchRequest request;request.source={"document-A",42,"",{}};
    request.toolchain=helper_profile(helper,"echo");
    request.tests={{"empty-input","",std::string(""),true},{"missing-answer","",std::nullopt,true},
        {"wrong","left",std::string("right"),true},{"binary",std::string("a\0b",3),std::string("a\0b",3),true},
        {"disabled","x",std::string("x"),false}};
    auto result=cpn::BatchService().run(request);
    expect(result.error.empty(),"batch echo error");expect(result.document_id=="document-A" && result.revision==42,"source identity");
    expect(result.tests.size()==4,"selected cases only");
    expect(result.tests[0].verdict==cpn::Verdict::accepted,"explicit empty answer");
    expect(result.tests[1].verdict==cpn::Verdict::unchecked,"absent answer");
    expect(result.tests[2].verdict==cpn::Verdict::wrong_answer,"wrong answer");
    expect(result.tests[3].verdict==cpn::Verdict::accepted && result.tests[3].process.standard_output.size()==3,"NUL preserved");
    request.tests[1].id=request.tests[0].id;
    expect(!cpn::BatchService().run(request).error.empty(),"duplicate IDs rejected");
    request.tests={{"path","",std::nullopt,true}};request.toolchain.source_filename="../escape";
    expect(!cpn::BatchService().run(request).error.empty(),"public API rejects path escape");
    request.toolchain=helper_profile(helper,"sleep");request.toolchain.run.arguments.push_back("500");request.toolchain.run_limits.wall_timeout=std::chrono::milliseconds(30);
    expect(cpn::BatchService().run(request).tests[0].verdict==cpn::Verdict::time_limit,"TLE classification");
    request.toolchain=helper_profile(helper,"exit");request.toolchain.run.arguments.push_back("7");
    expect(cpn::BatchService().run(request).tests[0].verdict==cpn::Verdict::runtime_error,"RE classification");
    request.toolchain=helper_profile(helper,"flood");request.toolchain.run.arguments.push_back("500000");request.toolchain.run_limits.stdout_bytes=100;
    expect(cpn::BatchService().run(request).tests[0].verdict==cpn::Verdict::output_limit,"OLE classification");
    request.toolchain=helper_profile(helper,"sleep");request.toolchain.run.arguments.push_back("500");
    std::stop_source stop;
    std::jthread cancel([&]{std::this_thread::sleep_for(std::chrono::milliseconds(30));stop.request_stop();});
    expect(cpn::BatchService().run(request,stop.get_token()).tests[0].verdict==cpn::Verdict::cancelled,"cancel is not TLE");
    request.toolchain=helper_profile(helper,"echo");request.tests={{"first","ab",std::string("ab"),true},{"second","cd",std::string("cd"),true},{"last","",std::nullopt,true}};
    request.max_retained_output_bytes=3;
    result=cpn::BatchService().run(request);
    std::size_t retained=0;for(const auto& test:result.tests)retained+=test.process.standard_output.size()+test.process.standard_error.size();
    expect(retained<=3 && result.tests.size()==3,"batch output budget is bounded");
    expect(result.tests[0].verdict==cpn::Verdict::accepted && result.tests[1].verdict==cpn::Verdict::output_limit && result.tests[2].verdict==cpn::Verdict::not_run,"budget terminal results");
    request.max_retained_output_bytes=64*1024*1024;
    std::stop_source checking_stop;
    result=cpn::BatchService().run(request,checking_stop.get_token(),[&](auto phase,const auto&){if(phase==cpn::BatchPhase::checking)checking_stop.request_stop();});
    expect(result.tests.size()==3 && result.tests[0].verdict==cpn::Verdict::cancelled && result.tests[1].verdict==cpn::Verdict::cancelled && result.tests[2].verdict==cpn::Verdict::cancelled,"checking cancellation wins terminal result");
    expect(result.tests[0].process.standard_output=="ab","cancel preserves executed output");
}
void failures(const std::filesystem::path& helper){
    cpn::BatchRequest request;request.source={"doc",1,"",{}};request.toolchain=helper_profile(helper,"echo");
    request.toolchain.build=cpn::CommandTemplate{cpn::path_to_utf8(helper),{"echo"}};request.toolchain.artifact_name="artifact.exe";
    request.tests={{"first","",std::string("observed"),true},{"second","",std::nullopt,true}};
    auto successful=[](const cpn::ProcessSpec&,std::stop_token){cpn::ProcessResult result;result.termination=cpn::ProcessTermination::exited;result.exit_code=0;result.standard_output="observed";return result;};
    std::size_t executions=0;
    auto throws_on_run=[&](const cpn::ProcessSpec& spec,std::stop_token stop){
        if(++executions==2)throw std::runtime_error("executor unavailable");return successful(spec,stop);
    };
    auto result=cpn::BatchService(cpn::native_file_store(),throws_on_run).run(request);
    expect(result.build_outcome==cpn::BuildOutcome::succeeded && result.build->exit_code==0,"run exception preserves successful build");
    expect(result.tests[0].verdict==cpn::Verdict::infrastructure_error && result.tests[1].verdict==cpn::Verdict::not_run,"run failure preserves terminal IDs");
    const auto long_detail=std::string(4095,'D')+"😀"+std::string(9000,'D');
    result=cpn::BatchService(cpn::native_file_store(),successful).run(request,{},[&](auto phase,const auto&){if(phase==cpn::BatchPhase::checking)throw std::runtime_error(long_detail);});
    expect(result.build_outcome==cpn::BuildOutcome::succeeded && result.tests[0].process.standard_output=="observed","checking failure preserves completed process");
    expect(result.error.size()<=4096 && !cpn::decode_utf8(result.error).error_offset && result.tests[0].detail==result.error,"failure detail bounded at UTF-8 boundary");
}
void compiled(const std::filesystem::path& profiles,const std::filesystem::path& compiler){
    auto files=cpn::native_file_store();cpn::BatchRequest request;
    request.source={"unsaved-document",8,"#include <iostream>\nint main(){long a,b;if(std::cin>>a>>b)std::cout<<a+b;}\n",{}};
    request.toolchain=cpn::load_toolchain_profile(profiles/"cpp-clang.json",*files);
    request.toolchain.build->executable=cpn::path_to_utf8(compiler);
#if defined(_WIN32)
    request.toolchain.artifact_name="solution.exe";
#endif
#if defined(_MSC_VER)
    request.toolchain.build->arguments={"/std:c++20","/EHsc","/I${source_dir}","${source}","/Fe${artifact}"};
#endif
    request.tests={{"first","2 3\n",std::string("5"),true},{"second","1 9\n",std::string("10"),true}};
    auto result=cpn::BatchService().run(request);
    expect(result.build_outcome==cpn::BuildOutcome::succeeded,"real C++ compilation");
    expect(result.tests.size()==2 && result.tests[0].verdict==cpn::Verdict::accepted && result.tests[1].verdict==cpn::Verdict::accepted,"real compiled samples");
    request.source.text="this is invalid C++";
    result=cpn::BatchService().run(request);
    expect(result.build_outcome==cpn::BuildOutcome::source_error,"CE classification");
    expect(result.tests.size()==2 && result.tests[0].verdict==cpn::Verdict::not_run,"CE cases terminal");
    request.toolchain.build->executable="cpn-nonexistent-compiler";
    expect(cpn::BatchService().run(request).build_outcome==cpn::BuildOutcome::failed,"compiler infrastructure failure");
}
}
#if defined(_WIN32)
int wmain(int argc,wchar_t** argv){
    try{if(argc!=4)throw std::runtime_error("helper, profiles and compiler required");profiles();batch(std::filesystem::absolute(argv[1]));failures(std::filesystem::absolute(argv[1]));compiled(argv[2],std::filesystem::absolute(argv[3]));std::cout<<"batch tests passed\n";return 0;}
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
#else
int main(int argc,char** argv){
    try{if(argc!=4)throw std::runtime_error("helper, profiles and compiler required");profiles();batch(std::filesystem::absolute(cpn::path_from_utf8(argv[1])));failures(std::filesystem::absolute(cpn::path_from_utf8(argv[1])));compiled(cpn::path_from_utf8(argv[2]),std::filesystem::absolute(cpn::path_from_utf8(argv[3])));std::cout<<"batch tests passed\n";return 0;}
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
#endif
