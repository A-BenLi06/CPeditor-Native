// SPDX-License-Identifier: GPL-3.0-or-later
#include "cpn/api.h"
#include "cpn/batch.hpp"
#include "cpn/json.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using Json=nlohmann::json;
void expect(bool value,const char* detail) { if (!value) throw std::runtime_error(detail); }
template<class F> void rejected(F operation,const char* detail) {
    try { operation(); } catch (const std::exception&) { return; }
    throw std::runtime_error(detail);
}
Json checker_json(const std::filesystem::path& helper,std::string mode="compare") {
    return {{"schema_version",1},{"command",{{"executable",cpn::path_to_utf8(helper)},
        {"arguments",Json::array({mode,"${input}","${actual}","${expected}"})}}}};
}
cpn::BatchRequest request(const std::filesystem::path& solver,const std::filesystem::path& checker) {
    cpn::BatchRequest result;
    result.source={"document-中文",42,"unsaved source",{}};
    result.toolchain.id="echo";result.toolchain.language_id="test";result.toolchain.source_filename="source.txt";
    result.toolchain.run={cpn::path_to_utf8(solver),{"echo"}};
    const auto raw=std::string("x\0",2)+static_cast<char>(0xff);
    result.tests={{"binary",raw,raw,true},{"wrong","actual",std::string("expected"),true},
        {"missing","",std::nullopt,true},{"empty","",std::string(""),true}};
    result.external_checker=cpn::parse_external_checker_spec(checker_json(checker).dump());
    return result;
}
void parsing(const std::filesystem::path& checker) {
    const auto original=checker_json(checker);auto json=original;
    auto spec=cpn::parse_external_checker_spec(json.dump());
    expect(spec.accepted_exit_codes==std::vector<int>{0} && spec.wrong_answer_exit_codes==std::vector<int>({1,2}),"default binary protocol");
    for (const auto& invalid : std::vector<Json>{1.5,-1,true,"1",nullptr,18446744073709551615ULL}) {
        json=original;json["schema_version"]=invalid;
        rejected([&]{(void)cpn::parse_external_checker_spec(json.dump());},"checker version must be exact integer");
        json=original;json["limits"]={{"wall_timeout_ms",invalid}};
        rejected([&]{(void)cpn::parse_external_checker_spec(json.dump());},"checker deadline strict integer");
    }
    for (const auto& invalid : std::vector<Json>{Json::array(),Json::array({256}),Json::array({-1}),Json::array({0,0}),Json::array({1}),Json::array({1.5})}) {
        json=original;json["accepted_exit_codes"]=invalid;
        rejected([&]{(void)cpn::parse_external_checker_spec(json.dump());},"checker mappings bounded and disjoint");
    }
    json=original;json["limits"]={{"unknown",1}};
    rejected([&]{(void)cpn::parse_external_checker_spec(json.dump());},"unknown limit rejected");
    json=original;json["command"]["unknown"]=1;
    rejected([&]{(void)cpn::parse_external_checker_spec(json.dump());},"unknown command field rejected");
    const auto duplicate=original.dump().substr(0,original.dump().size()-1)+",\"schema_version\":1}";
    rejected([&]{(void)cpn::parse_external_checker_spec(duplicate);},"duplicate checker field rejected");
    json=original;json["accepted_exit_codes"]=Json::array({7});
    expect(cpn::parse_external_checker_spec(json.dump()).accepted_exit_codes==std::vector<int>{7},"explicit score-code binary override allowed");
}
void batch(const std::filesystem::path& solver,const std::filesystem::path& checker) {
    auto value=request(solver,checker);auto result=cpn::BatchService().run(value);
    expect(result.error.empty() && result.document_id==value.source.document_id && result.revision==42,"batch identity remains intact");
    expect(result.tests.size()==4 && result.tests[0].verdict==cpn::Verdict::accepted && result.tests[1].verdict==cpn::Verdict::wrong_answer,"external checker AC and WA");
    expect(result.tests[2].verdict==cpn::Verdict::unchecked && !result.tests[2].checker,"missing expected does not launch checker");
    expect(result.tests[3].verdict==cpn::Verdict::accepted && result.tests[3].checker->process_attempted,"explicit empty expected runs checker");
    expect(result.tests[0].process.standard_output==value.tests[0].input && result.tests[0].checker->process.exit_code==0,"raw bytes and both process results preserved");
    value.tests.resize(1);
    for (const auto mode : {"sleep","signal","flood","exit"}) {
        auto json=checker_json(checker,mode);
        if (std::string(mode)=="exit") json["command"]["arguments"].push_back("7");
        value.external_checker=cpn::parse_external_checker_spec(json.dump());
        value.external_checker->limits.wall_timeout=std::chrono::milliseconds(30);
        value.external_checker->limits.combined_bytes=16;
        result=cpn::BatchService().run(value);
        expect(result.tests[0].verdict==cpn::Verdict::checker_error && result.tests[0].process.exit_code==0,"checker failure never becomes solution TLE or RE");
        expect(result.tests[0].checker && result.tests[0].checker->process_attempted,"checker failure evidence retained");
    }
    value.external_checker=cpn::parse_external_checker_spec(checker_json(checker).dump());
    value.toolchain.run.arguments={"exit","7"};
    result=cpn::BatchService().run(value);
    expect(result.tests[0].verdict==cpn::Verdict::runtime_error && !result.tests[0].checker,"failed solution never starts checker");
    value.toolchain.run.arguments={"echo"};
    std::stop_source cancellation;
    result=cpn::BatchService().run(value,cancellation.get_token(),[&](auto phase,const auto&) {
        if (phase==cpn::BatchPhase::checking) cancellation.request_stop();
    });
    expect(result.tests[0].verdict==cpn::Verdict::cancelled && result.tests[0].checker && !result.tests[0].checker->process_attempted,"cancel before checker preserves solution bytes without starting process");
    value.external_checker=cpn::parse_external_checker_spec(checker_json(checker,"args").dump());
    value.external_checker->command.arguments.push_back("0123456789");value.max_retained_output_bytes=6;
    value.tests.push_back({"next","",std::string(""),true});
    result=cpn::BatchService().run(value);
    std::size_t retained=0;
    for (const auto& test : result.tests) {
        retained+=test.process.standard_output.size()+test.process.standard_error.size();
        if (test.checker) retained+=test.checker->process.standard_output.size()+test.checker->process.standard_error.size();
    }
    expect(retained<=6 && result.tests[0].verdict==cpn::Verdict::checker_error && result.tests[1].verdict==cpn::Verdict::not_run,"checker output shares batch retention budget");
    const auto wire=Json::parse(cpn::serialize_batch_result(result));
    expect(wire["tests"][0]["checker"]["process"]["termination"]=="OutputLimit" && wire["tests"][0]["process"]["termination"]=="Exited","wire separates solver and checker execution");
    value=request(solver,checker);value.tests.resize(1);value.max_retained_output_bytes=3;
    int attempted=0;
    const auto oversized=[&](const cpn::ProcessSpec&,std::stop_token) {
        ++attempted;cpn::ProcessResult response;response.termination=cpn::ProcessTermination::exited;
        response.exit_code=0;response.standard_output=std::string(100,'x');return response;
    };
    result=cpn::BatchService(cpn::native_file_store(),oversized).run(value);
    expect(attempted==1 && result.tests[0].verdict==cpn::Verdict::output_limit && !result.tests[0].checker,"oversized execution port cannot underflow checker budget or report AC");
    expect(result.tests[0].process.standard_output.size()==3 && result.tests[0].process.stdout_total_bytes==100 && result.tests[0].process.stdout_truncated,"port output budget preserves totals and truncation");
    value.toolchain.build=value.toolchain.run;value.toolchain.artifact_name="artifact";
    result=cpn::BatchService(cpn::native_file_store(),oversized).run(value);
    expect(result.build_outcome==cpn::BuildOutcome::failed && result.build->standard_output.size()==3 && result.tests[0].verdict==cpn::Verdict::not_run,"oversized build output bounded before solver execution");
}
void abi(const std::filesystem::path& solver,const std::filesystem::path& checker) {
    auto json=Json{{"schema_version",1},{"source",{{"document_id","abi-checker"},{"revision",9},{"text",""}}},
        {"toolchain",{{"schema_version",1},{"id","echo"},{"language_id","test"},{"source_filename","source.txt"},
            {"run",{{"executable",cpn::path_to_utf8(solver)},{"arguments",Json::array({"echo"})}}}}},
        {"tests",Json::array({{{"id","one"},{"input","text"},{"expected","text"}}})},{"external_checker",checker_json(checker)}};
    const auto bytes=json.dump();cpn_engine_handle engine=0;cpn_error error{};error.struct_size=sizeof(error);
    expect(cpn_engine_create(nullptr,&engine,&error)==CPN_OK,"checker ABI engine");
    cpn_job_handle job=0;
    expect(cpn_engine_submit_json(engine,{reinterpret_cast<const uint8_t*>(bytes.data()),bytes.size()},&job,&error)==CPN_OK,"checker ABI submit");
    bool completed=false;const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    while (!completed && std::chrono::steady_clock::now()<deadline) {
        cpn_buffer buffer{};expect(cpn_engine_drain_json(engine,8,&buffer,&error)==CPN_OK,"checker ABI drain");
        const auto document=Json::parse(buffer.data,buffer.data+buffer.size);
        expect(cpn_buffer_release(buffer.handle,&error)==CPN_OK,"checker ABI buffer release");
        for (const auto& event : document["events"]) if (event.contains("result")) {
            const auto& result=event["result"];completed=true;
            expect(result["document_id"]=="abi-checker" && result["revision"]==9 && result["tests"][0]["verdict"]=="AC","checker ABI final result");
            expect(result["tests"][0]["checker"]["process"]["exit_code"]==0,"checker ABI evidence");
        }
        if (!completed) std::this_thread::yield();
    }
    expect(completed,"checker ABI completion deadline");
    expect(cpn_engine_release_job(engine,job,&error)==CPN_OK,"checker ABI job release");
    expect(cpn_engine_destroy(engine,&error)==CPN_OK,"checker ABI destroy");
}
int verify(const std::filesystem::path& solver,const std::filesystem::path& checker) {
    try { parsing(checker);batch(solver,checker);abi(solver,checker);std::cout<<"batch checker tests passed\n";return 0; }
    catch (const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
}
#if defined(_WIN32)
int wmain(int argc,wchar_t** argv) { return argc==3?verify(std::filesystem::absolute(argv[1]),std::filesystem::absolute(argv[2])):2; }
#else
int main(int argc,char** argv) { return argc==3?verify(std::filesystem::absolute(cpn::path_from_utf8(argv[1])),std::filesystem::absolute(cpn::path_from_utf8(argv[2]))):2; }
#endif
