// SPDX-License-Identifier: GPL-3.0-or-later
#include "cpn/api.h"
#include "cpn/companion_inbox.hpp"
#include "cpn/process.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using Json=nlohmann::json;
using Api=cpn_status(*)(cpn_engine_handle,cpn_bytes,cpn_buffer*,cpn_error*);
using namespace std::chrono_literals;
void expect(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
cpn_bytes view(const std::string& value){return {reinterpret_cast<const std::uint8_t*>(value.data()),value.size()};}
struct Workspace {
    std::shared_ptr<cpn::FileStore> files=cpn::native_file_store();
    std::filesystem::path path=files->create_workspace();
    ~Workspace(){files->remove_workspace(path);}
};
struct Engine {
    cpn_engine_handle handle{};
    explicit Engine(std::uint64_t request_bytes=64*1024*1024){
        cpn_engine_options options{sizeof(options),CPN_ABI_VERSION,1,1,1,0,request_bytes,1024*1024};
        expect(cpn_engine_create(&options,&handle,nullptr)==CPN_OK,"create ABI engine");
    }
    ~Engine(){if(handle)cpn_engine_destroy(handle,nullptr);}
};
struct Buffer {
    cpn_buffer value{};
    ~Buffer(){if(value.handle)cpn_buffer_release(value.handle,nullptr);}
    Json json() const{return Json::parse(value.data,value.data+value.size);}
};
Json call(Api api,cpn_engine_handle engine,const std::string& request,cpn_status expected=CPN_OK){
    Buffer output;cpn_error failure{};failure.struct_size=sizeof(failure);
    const auto status=api(engine,view(request),&output.value,&failure);
    if(status!=expected)throw std::runtime_error("Unexpected ABI status "+std::to_string(status)+": "+failure.message);
    if(status!=CPN_OK){expect(!output.value.handle && !output.value.data && !output.value.size,"failed ABI clears output");return nullptr;}
    expect(output.value.handle && output.value.data && output.value.size,"successful ABI owns response bytes");
    return output.json();
}
Json call(Api api,cpn_engine_handle engine,const Json& request,cpn_status expected=CPN_OK){return call(api,engine,request.dump(),expected);}
std::string overflowing_integer(Json request,const char* field){
    request[field]="ABI-integer-placeholder";auto bytes=request.dump();
    constexpr std::string_view placeholder="\"ABI-integer-placeholder\"";
    bytes.replace(bytes.find(placeholder),placeholder.size(),"18446744073709551616");return bytes;
}
cpn::CompanionInboxPaths inbox_paths(const std::filesystem::path& path){return {path/cpn::path_from_utf8("收件箱.json"),path/"inbox-backup.json",path/"inbox.lock"};}
cpn::SessionPaths session_paths(const std::filesystem::path& path){return {path/cpn::path_from_utf8("恢复.json"),path/"session-backup.json",path/"session.lock"};}
Json inbox_request(const std::filesystem::path& path){
    const auto locations=inbox_paths(path);
    return Json{{"schema_version",1},{"workspace_id","workspace"},{"paths",{
        {"inbox_file",cpn::path_to_utf8(locations.inbox_file)},{"backup_file",cpn::path_to_utf8(locations.backup_file)},
        {"lock_file",cpn::path_to_utf8(locations.lock_file)}}}};
}
Json page_request(const std::filesystem::path& path){
    auto request=inbox_request(path);request["after_sequence"]=0;request["max_items"]=1;request["max_page_bytes"]=1024*1024;return request;
}
std::string payload(std::string url="https://sample.test/problem",std::string whitespace="\n"){
    return "{"+whitespace+"  \"name\":\"题目 😀\",\"group\":\"竞赛\",\"url\":\""+url+
        "\",\"timeLimit\":18446744073709551615,\"memoryLimit\":256,\"tests\":[{\"input\":\"a\\u0000b\\r\\n\",\"output\":\"\"}],"
        "\"future\":{\"number\":184467440737095516160,\"lexeme\":1.2300e+02}}"+whitespace;
}
Json accept_request(const std::filesystem::path& path,std::string original=payload()){
    auto request=inbox_request(path);request["original_json"]=std::move(original);return request;
}
cpn::WorkspaceSession initial_session(){
    cpn::WorkspaceSession result;result.workspace_id="workspace";result.revision=9007199254740993ULL;result.selected_document_id="user-source";
    cpn::SessionDocument document;document.document_id="user-source";document.revision=std::numeric_limits<std::uint64_t>::max();
    document.source_text="user modified source\r\n";document.tests={{"user-test","edited",std::nullopt,false}};
    document.editor_preferences={{"future.editor",std::numeric_limits<std::uint64_t>::max(),"{ \"unknown\" : 184467440737095516160 }"}};
    result.documents.push_back(std::move(document));return result;
}
Json prepare_request(const std::filesystem::path& path,std::string import_id,const cpn::WorkspaceSession& session){
    auto request=inbox_request(path);request["import_id"]=std::move(import_id);
    request["session"]=Json::parse(cpn::serialize_workspace_session(session));
    request["defaults"]={{"source_text",std::string("code\0\r\n",7)},{"language_id","language-from-config"},
        {"toolchain_profile_id","profile-from-config"},{"document_preferences",Json::array({{
            {"namespace_id","future.defaults"},{"schema_version",99},{"object_json","{ \"opaque\" : 1.2300e+02 }"}}})}};
    return request;
}
Json mark_request(const std::filesystem::path& path,std::string import_id){
    auto request=inbox_request(path);request["import_id"]=std::move(import_id);
    const auto locations=session_paths(path);
    request["session_paths"]={{"recovery_file",cpn::path_to_utf8(locations.recovery_file)},
        {"backup_file",cpn::path_to_utf8(locations.backup_file)},{"lock_file",cpn::path_to_utf8(locations.lock_file)}};return request;
}
Json save(cpn_engine_handle engine,const std::filesystem::path& path,const cpn::WorkspaceSession& session,const Json& digest=nullptr){
    const auto locations=session_paths(path);
    const Json request{{"schema_version",1},{"paths",{{"recovery_file",cpn::path_to_utf8(locations.recovery_file)},
        {"backup_file",cpn::path_to_utf8(locations.backup_file)},{"lock_file",cpn::path_to_utf8(locations.lock_file)}}},
        {"session",Json::parse(cpn::serialize_workspace_session(session))},{"expected_disk_digest",digest}};
    auto result=call(cpn_engine_session_save_json,engine,request);expect(result["status"]=="Saved","save actual session through C ABI");return result["disk_digest"];
}
void strict_requests(const Workspace& workspace){
    Engine engine;const auto path=workspace.path/cpn::path_from_utf8("严格请求");std::filesystem::create_directories(path);
    Buffer held;expect(cpn_engine_info_json(engine.handle,&held.value,nullptr)==CPN_OK,"hold buffer quota during invalid request checks");
    const auto valid_page=page_request(path),valid_accept=accept_request(path);
    for(const auto api:{cpn_engine_companion_page_json,cpn_engine_companion_accept_json,cpn_engine_companion_prepare_json,cpn_engine_companion_mark_applied_json}){
        const Json valid=api==cpn_engine_companion_page_json?valid_page:api==cpn_engine_companion_accept_json?valid_accept:
            api==cpn_engine_companion_prepare_json?prepare_request(path,std::string(64,'a'),initial_session()):mark_request(path,std::string(64,'a'));
        auto malformed=valid;malformed["unknown"]=1;call(api,engine.handle,malformed,CPN_INVALID_ARGUMENT);
        malformed=valid;malformed["paths"]["unknown"]=1;call(api,engine.handle,malformed,CPN_INVALID_ARGUMENT);
        malformed=valid;malformed["schema_version"]=1.0;call(api,engine.handle,malformed,CPN_INVALID_ARGUMENT);
        malformed=valid;malformed["schema_version"]=2;call(api,engine.handle,malformed,CPN_INVALID_ARGUMENT);
        malformed=valid;malformed["workspace_id"]=std::string("a\0b",3);call(api,engine.handle,malformed,CPN_INVALID_ARGUMENT);
        malformed=valid;malformed["paths"]["inbox_file"]="relative.json";call(api,engine.handle,malformed,CPN_INVALID_ARGUMENT);
        malformed=valid;malformed["paths"]["inbox_file"]=42;call(api,engine.handle,malformed,CPN_INVALID_ARGUMENT);
        malformed=valid;malformed["paths"]["lock_file"]=std::string("/a\0b",5);call(api,engine.handle,malformed,CPN_INVALID_ARGUMENT);
        malformed=valid;malformed["paths"]["lock_file"]=std::string(32769,'a');call(api,engine.handle,malformed,CPN_INVALID_ARGUMENT);
        malformed=valid;malformed["bounds"]={{"max_records",0}};call(api,engine.handle,malformed,CPN_INVALID_ARGUMENT);
        malformed=valid;malformed["bounds"]={{"max_id_bytes",63}};call(api,engine.handle,malformed,CPN_INVALID_ARGUMENT);
        malformed=valid;malformed["bounds"]={{"max_records",257}};call(api,engine.handle,malformed,CPN_INVALID_ARGUMENT);
        malformed=valid;malformed["bounds"]={{"payload",{{"max_batch_size",0}}}};call(api,engine.handle,malformed,CPN_INVALID_ARGUMENT);
        malformed=valid;malformed["bounds"]={{"max_records",1.0}};call(api,engine.handle,malformed,CPN_INVALID_ARGUMENT);
        malformed=valid;malformed["bounds"]={{"payload",{{"typo",1}}}};call(api,engine.handle,malformed,CPN_INVALID_ARGUMENT);
        auto duplicate=valid.dump();duplicate.insert(1,"\"\\u0073chema_version\":1,");call(api,engine.handle,duplicate,CPN_INVALID_ARGUMENT);
        call(api,engine.handle,overflowing_integer(valid,"schema_version"),CPN_INVALID_ARGUMENT);
        cpn_buffer output{reinterpret_cast<const std::uint8_t*>(1),42,42};
        expect(api(engine.handle,{nullptr,1},&output,nullptr)==CPN_INVALID_ARGUMENT && !output.handle,"null nonempty input rejects and clears output");
        expect(api(engine.handle,view(valid.dump()),nullptr,nullptr)==CPN_INVALID_ARGUMENT,"null output rejects before I/O");
        call(api,std::numeric_limits<std::uint64_t>::max(),valid,CPN_INVALID_HANDLE);
    }
    for(const auto field:{"after_sequence","max_items","max_page_bytes"}){
        auto malformed=valid_page;malformed[field]=-1;call(cpn_engine_companion_page_json,engine.handle,malformed,CPN_INVALID_ARGUMENT);
        malformed=valid_page;malformed[field]=1.0;call(cpn_engine_companion_page_json,engine.handle,malformed,CPN_INVALID_ARGUMENT);
        call(cpn_engine_companion_page_json,engine.handle,overflowing_integer(valid_page,field),CPN_INVALID_ARGUMENT);
    }
    for(const auto field:{"max_items","max_page_bytes"}){auto malformed=valid_page;malformed[field]=0;call(cpn_engine_companion_page_json,engine.handle,malformed,CPN_INVALID_ARGUMENT);}
    auto malformed=valid_accept;malformed["original_json"]=Json::object();call(cpn_engine_companion_accept_json,engine.handle,malformed,CPN_INVALID_ARGUMENT);
    malformed=valid_accept;malformed["lock_timeout_ms"]=60001;call(cpn_engine_companion_accept_json,engine.handle,malformed,CPN_INVALID_ARGUMENT);
    auto prepare=prepare_request(path,std::string(64,'a'),initial_session());
    for(const auto& bad:std::vector<Json>{Json{{"unknown",1}},Json{{"source_text",42}},Json{{"language_id",""}},
        Json{{"source_text",std::string(16*1024*1024+1,'a')}},Json{{"document_preferences",42}},
        Json{{"document_preferences",Json::array({{{"namespace_id","a"},{"schema_version",0},{"object_json","{}"}}})}},
        Json{{"document_preferences",Json::array({{{"namespace_id","a"},{"schema_version",1},{"object_json","[]"}}})}},
        Json{{"document_preferences",Json::array({{{"namespace_id","a"},{"schema_version",1},{"object_json","{\"a\":1,\"\\u0061\":2}"}}})}}}){
        malformed=prepare;malformed["defaults"]=bad;call(cpn_engine_companion_prepare_json,engine.handle,malformed,CPN_INVALID_ARGUMENT);
    }
    malformed=prepare;malformed["session"]["typo"]=1;call(cpn_engine_companion_prepare_json,engine.handle,malformed,CPN_INVALID_ARGUMENT);
    malformed=prepare;malformed["max_receipts"]=1.0;call(cpn_engine_companion_prepare_json,engine.handle,malformed,CPN_INVALID_ARGUMENT);
    auto mark=mark_request(path,std::string(64,'a'));mark["session_paths"]["unknown"]=1;call(cpn_engine_companion_mark_applied_json,engine.handle,mark,CPN_INVALID_ARGUMENT);
    mark=mark_request(path,std::string(64,'a'));mark["session_paths"]["recovery_file"]="relative.json";call(cpn_engine_companion_mark_applied_json,engine.handle,mark,CPN_INVALID_ARGUMENT);
    call(cpn_engine_companion_accept_json,engine.handle,valid_accept,CPN_CAPACITY);
    expect(!workspace.files->read_optional(inbox_paths(path).inbox_file,1024*1024),"invalid requests and exhausted quota never create inbox");
    expect(!std::filesystem::exists(inbox_paths(path).lock_file),"request validation and quota precede lock file side effects");
    Engine small(32);call(cpn_engine_companion_accept_json,small.handle,valid_accept,CPN_INVALID_ARGUMENT);
}
void finite_write_response(const Workspace& workspace){
    Engine engine;const auto path=workspace.path/cpn::path_from_utf8("有限写响应");std::filesystem::create_directories(path);
    auto body=Json{{"name","large sample set"},{"group","bounded"},{"url","https://many.test/problem"},
        {"timeLimit",0},{"memoryLimit",0},{"tests",Json::array()}};
    for(std::size_t i=0;i<cpn::CompanionBounds{}.max_tests;++i)body["tests"].push_back({{"input",""},{"output",""}});
    const auto request=accept_request(path,body.dump()).dump();Buffer accepted;
    expect(cpn_engine_companion_accept_json(engine.handle,view(request),&accepted.value,nullptr)==CPN_OK,"largest sample count accepts with reserved response");
    expect(accepted.value.size<65536 && accepted.json()["item"]["test_count"]==4096,"write response stays finite for largest sample count");
    expect(!accepted.json()["item"].contains("test_ids"),"largest sample set does not expand write response");
    cpn_buffer_release(accepted.value.handle,nullptr);accepted.value={};
    const auto corrupt_path=workspace.path/cpn::path_from_utf8("有界诊断");std::filesystem::create_directories(corrupt_path);
    std::string key="bad \"key\"\n\t 中文 😀 ";key+=std::string(5000,'"');
    const auto invalid=Json{{"schema_version",1},{key,nullptr}}.dump();
    workspace.files->write_atomic(inbox_paths(corrupt_path).inbox_file,invalid);
    const auto rejected=call(cpn_engine_companion_accept_json,engine.handle,accept_request(corrupt_path));
    const auto diagnostic=rejected["detail"].get<std::string>();
    expect(rejected["status"]=="InvalidExisting" && diagnostic.size()<=4096 && diagnostic.find('\n')!=std::string::npos && diagnostic.find("中文")!=std::string::npos,"bounded response JSON escapes diagnostic controls and preserves UTF-8");
    expect(workspace.files->read(inbox_paths(corrupt_path).inbox_file,1024*1024)==invalid,"invalid existing inbox remains byte-for-byte unchanged");
    const auto future_path=workspace.path/cpn::path_from_utf8("未来收件箱");std::filesystem::create_directories(future_path);
    const std::string future="{ \"schema_version\" : 99 }";workspace.files->write_atomic(inbox_paths(future_path).inbox_file,future);
    expect(call(cpn_engine_companion_page_json,engine.handle,page_request(future_path))["status"]=="UnsupportedSchema","future inbox version is an explicit read status");
    expect(call(cpn_engine_companion_accept_json,engine.handle,accept_request(future_path))["status"]=="UnsupportedSchema","future inbox cannot be downgraded on accept");
    expect(call(cpn_engine_companion_prepare_json,engine.handle,prepare_request(future_path,std::string(64,'a'),initial_session()))["status"]=="UnsupportedSchema","future inbox does not supply a preparation candidate");
    expect(workspace.files->read(inbox_paths(future_path).inbox_file,1024)==future,"future inbox original bytes remain unchanged");
}
void pages_and_application(const Workspace& workspace){
    Engine engine;const auto path=workspace.path/cpn::path_from_utf8("接纳与导入");std::filesystem::create_directories(path);
    auto page=page_request(path);
    auto result=call(cpn_engine_companion_page_json,engine.handle,page);
    expect(result["status"]=="Missing" && result["items"].empty() && result["revision"].is_null() && result["next_after_sequence"]==0,"missing inbox is an empty bounded page");
    const auto empty_page_size=result.dump().size();page["max_page_bytes"]=empty_page_size;
    call(cpn_engine_companion_page_json,engine.handle,page);page["max_page_bytes"]=empty_page_size-1;
    call(cpn_engine_companion_page_json,engine.handle,page,CPN_CAPACITY);page["max_page_bytes"]=1024*1024;
    const auto original=payload();const auto accepted=call(cpn_engine_companion_accept_json,engine.handle,accept_request(path,original));
    expect(accepted["status"]=="Accepted" && accepted["inbox_replaced"]==true && accepted["item"]["test_count"]==1,"accept has finite scalar summary");
    expect(!accepted["item"].contains("test_ids") && !accepted["item"].contains("original_json"),"write response never expands item arrays or payload");
    const std::string first_id=accepted["item"]["import_id"];
    const auto paths=inbox_paths(path);const auto disk_before=workspace.files->read(paths.inbox_file,128*1024*1024);
    const auto duplicate=call(cpn_engine_companion_accept_json,engine.handle,accept_request(path,original));
    expect(duplicate["status"]=="Duplicate" && duplicate["item"]["import_id"]==first_id && duplicate["inbox_replaced"]==false,"exact duplicate retains identity and does not write");
    expect(workspace.files->read(paths.inbox_file,128*1024*1024)==disk_before,"duplicate preserves exact disk bytes");
    result=call(cpn_engine_companion_page_json,engine.handle,page);
    expect(result["items"][0]["original_json"]==original,"paged original payload retains whitespace and large numeric lexemes");
    expect(result["revision"]==1 && result["next_after_sequence"]==1 && result["has_more"]==false,"page revision and cursor describe only returned items");
    const auto first_page_size=result.dump().size();page["max_page_bytes"]=first_page_size;call(cpn_engine_companion_page_json,engine.handle,page);
    page["max_page_bytes"]=first_page_size-1;call(cpn_engine_companion_page_json,engine.handle,page,CPN_CAPACITY);
    page["max_page_bytes"]=1024*1024;
    expect(call(cpn_engine_companion_page_json,engine.handle,page)==result,"capacity failure and reads do not consume pending imports");
    auto mark=mark_request(path,first_id);
    expect(call(cpn_engine_companion_mark_applied_json,engine.handle,mark)["status"]=="SessionUnavailable","memory receipt cannot acknowledge missing recovery");
    auto initial=initial_session();auto digest=save(engine.handle,path,initial);
    expect(call(cpn_engine_companion_mark_applied_json,engine.handle,mark)["status"]=="ReceiptMissing","actual recovery without receipt remains pending");
    auto preparation=prepare_request(path,first_id,initial);
    auto prepared=call(cpn_engine_companion_prepare_json,engine.handle,preparation);
    expect(prepared["status"]=="Prepared" && prepared["receipt"]["import_id"]==first_id,"prepare returns single session and matching receipt");
    auto candidate=cpn::parse_workspace_session(prepared["session"].dump());
    expect(candidate.revision==initial.revision+1 && candidate.documents.size()==2,"workspace uint64 revision and original user document preserved");
    expect(candidate.documents[0].source_text==initial.documents[0].source_text && candidate.documents[0].tests[0].input=="edited","new import never replaces user source or tests");
    const auto& document=candidate.documents.back();
    expect(document.source_text==std::string("code\0\r\n",7) && document.tests[0].input==std::string("a\0b\r\n",5) && document.tests[0].expected=="","default source and samples preserve NUL/CRLF and empty expected");
    expect(document.language_id=="language-from-config" && document.toolchain_profile_id=="profile-from-config","native configuration supplies language and toolchain");
    expect(document.editor_preferences[0].object_json=="{ \"opaque\" : 1.2300e+02 }" && candidate.documents[0].editor_preferences[0].object_json==initial.documents[0].editor_preferences[0].object_json,"opaque preference bytes are preserved");
    expect(workspace.files->read(paths.inbox_file,128*1024*1024)==disk_before,"pure preparation does not mark applied or write inbox");
    Buffer held;expect(cpn_engine_info_json(engine.handle,&held.value,nullptr)==CPN_OK,"hold acknowledgment response quota");
    call(cpn_engine_companion_mark_applied_json,engine.handle,mark,CPN_CAPACITY);
    expect(workspace.files->read(paths.inbox_file,128*1024*1024)==disk_before,"mark quota exhaustion does not replace inbox or backup");
    cpn_buffer_release(held.value.handle,nullptr);held.value={};
    digest=save(engine.handle,path,candidate,digest);
    const auto applied=call(cpn_engine_companion_mark_applied_json,engine.handle,mark);
    expect(applied["status"]=="Applied" && applied["item"]["applied_session_digest"]==digest && applied["item"]["applied_session_revision"]==candidate.revision,"acknowledgment verifies actual persisted candidate");
    expect(call(cpn_engine_companion_mark_applied_json,engine.handle,mark)["status"]=="AlreadyApplied","repeat acknowledgment is idempotent");
    auto deleted=candidate;deleted.documents.pop_back();deleted.selected_document_id="user-source";++deleted.revision;
    preparation=prepare_request(path,first_id,deleted);prepared=call(cpn_engine_companion_prepare_json,engine.handle,preparation);
    expect(prepared["status"]=="AlreadyApplied" && prepared["session"]["documents"].size()==1,"saved receipt prevents recreation of deleted import");
    auto malformed=preparation;malformed["defaults"]["language_id"]="";call(cpn_engine_companion_prepare_json,engine.handle,malformed,CPN_INVALID_ARGUMENT);
    malformed=preparation;malformed["defaults"]["document_preferences"]=Json::array({{{"namespace_id",std::string(cpn::companion_document_namespace)},
        {"schema_version",2},{"object_json","{ \"future\":true }"}}});call(cpn_engine_companion_prepare_json,engine.handle,malformed,CPN_INVALID_ARGUMENT);
    expect(call(cpn_engine_companion_prepare_json,engine.handle,prepare_request(path,first_id,initial))["status"]=="ReceiptMissing","Applied item and old session do not silently recreate document");
    auto future=candidate;future.editor_preferences.push_back({std::string(cpn::companion_receipts_namespace),2,"{ \"future\":true }"});
    expect(call(cpn_engine_companion_prepare_json,engine.handle,prepare_request(path,first_id,future))["status"]=="IncompatiblePreferences","mixed current and future owned preferences reject import");
    auto conflict=candidate;
    for(auto& preference:conflict.editor_preferences)if(preference.namespace_id==cpn::companion_receipts_namespace){
        auto value=Json::parse(preference.object_json);value["receipts"][0]["payload_digest"]=std::string(64,'a');preference.object_json=value.dump();
    }
    expect(call(cpn_engine_companion_prepare_json,engine.handle,prepare_request(path,first_id,conflict))["status"]=="IdentityConflict","caller receipt must match accepted stable identity");
    const auto update=call(cpn_engine_companion_accept_json,engine.handle,accept_request(path,payload("https://sample.test/problem"," ")));const std::string update_id=update["item"]["import_id"];
    expect(update["status"]=="Accepted" && update["item"]["previous_import_id"]==first_id && update["item"]["problem_id"]==accepted["item"]["problem_id"] && update["item"]["document_id"]!=accepted["item"]["document_id"],"changed raw delivery keeps update chain without reusing document identity");
    result=call(cpn_engine_companion_page_json,engine.handle,page);expect(result["items"].size()==1 && result["has_more"]==true && result["next_after_sequence"]==1,"first page stops at actual returned sequence");
    auto bounded=page;bounded["max_items"]=2;bounded["max_page_bytes"]=result.dump().size();
    expect(call(cpn_engine_companion_page_json,engine.handle,bounded)==result,"byte limit returns a fitting prefix including full envelope instead of consuming later item");
    page["after_sequence"]=result["next_after_sequence"];result=call(cpn_engine_companion_page_json,engine.handle,page);
    expect(result["items"][0]["import_id"]==update_id && result["next_after_sequence"]==2 && !result["has_more"].get<bool>(),"second page continues without skipped update");
    page["after_sequence"]=std::numeric_limits<std::uint64_t>::max();result=call(cpn_engine_companion_page_json,engine.handle,page);
    expect(result["items"].empty() && result["next_after_sequence"]==std::numeric_limits<std::uint64_t>::max(),"uint64 maximum cursor does not wrap");
    auto unknown=inbox_request(path);unknown["original_json"]="{}";
    expect(call(cpn_engine_companion_accept_json,engine.handle,unknown)["status"]=="InvalidPayload","invalid external payload has structured status");
    auto exhausted=accept_request(path,payload("https://other.test/problem"));exhausted["bounds"]={{"max_records",2}};
    expect(call(cpn_engine_companion_accept_json,engine.handle,exhausted)["status"]=="CapacityExceeded","reduced finite record capacity backpressures without eviction");
    auto invalid=prepare_request(path,update_id,candidate);invalid["max_receipts"]=1;
    expect(call(cpn_engine_companion_prepare_json,engine.handle,invalid)["status"]=="CapacityExceeded","full receipt history is not silently dropped");
    auto second_prepared=call(cpn_engine_companion_prepare_json,engine.handle,prepare_request(path,update_id,candidate));
    expect(second_prepared["status"]=="Prepared","new update produces a distinct candidate alongside original document");
    auto second_candidate=cpn::parse_workspace_session(second_prepared["session"].dump());
    auto wrong=second_candidate;
    for(auto& preference:wrong.editor_preferences)if(preference.namespace_id==cpn::companion_receipts_namespace){
        auto value=Json::parse(preference.object_json);value["receipts"].back()["payload_digest"]=std::string(64,'b');preference.object_json=value.dump();
    }
    digest=save(engine.handle,path,wrong,digest);
    const auto before_conflict=workspace.files->read(paths.inbox_file,128*1024*1024);
    const auto rejected_mark=call(cpn_engine_companion_mark_applied_json,engine.handle,mark_request(path,update_id));
    expect(rejected_mark["status"]=="ReceiptConflict" && rejected_mark["inbox_replaced"]==false,"mark checks actual saved receipt instead of caller-owned candidate");
    expect(workspace.files->read(paths.inbox_file,128*1024*1024)==before_conflict,"mismatched disk receipt cannot replace pending import");
    digest=save(engine.handle,path,second_candidate,digest);
    const auto second_applied=call(cpn_engine_companion_mark_applied_json,engine.handle,mark_request(path,update_id));
    expect(second_applied["status"]=="Applied" && second_applied["item"]["applied_session_digest"]==digest,"corrected persisted receipt can confirm update");
    Buffer durable;const auto request=page.dump();expect(cpn_engine_companion_page_json(engine.handle,view(request),&durable.value,nullptr)==CPN_OK,"own final page buffer");
    const auto owned=durable.json();const auto released=durable.value.handle;
    expect(cpn_engine_close(engine.handle,nullptr)==CPN_OK,"close engine");
    for(const auto api:{cpn_engine_companion_page_json,cpn_engine_companion_accept_json,cpn_engine_companion_prepare_json,cpn_engine_companion_mark_applied_json})call(api,engine.handle,inbox_request(path),CPN_CLOSED);
    expect(cpn_engine_destroy(engine.handle,nullptr)==CPN_OK,"destroy engine");engine.handle=0;
    expect(durable.json()==owned,"returned buffer remains readable after engine destruction");
    expect(cpn_buffer_release(released,nullptr)==CPN_OK,"release buffer after destroy");durable.value={};
    expect(cpn_buffer_release(released,nullptr)==CPN_INVALID_HANDLE,"double buffer release rejects");
}
int lock_child(const std::filesystem::path& path){
    auto files=cpn::native_file_store();auto held=files->acquire_lock(inbox_paths(path).lock_file,0ms);
    if(!held)return 3;files->write_atomic(path/"held","ready");
    const auto deadline=std::chrono::steady_clock::now()+10s;
    while(!std::filesystem::exists(path/"release")){
        if(std::chrono::steady_clock::now()>deadline)return 4;
        std::this_thread::sleep_for(2ms);
    }
    return 0;
}
void close_during_intake(const Workspace& workspace,const std::filesystem::path& executable){
    Engine engine;const auto path=workspace.path/cpn::path_from_utf8("关闭与锁");std::filesystem::create_directories(path);
    cpn::ProcessSpec spec;spec.executable=executable;spec.arguments={"lock-child",cpn::path_to_utf8(path)};spec.working_directory=path;spec.limits.wall_timeout=10s;
    std::stop_source child_stop;
    auto child=std::async(std::launch::async,[&]{return cpn::run_process(spec,child_stop.get_token());});
    try{
        const auto deadline=std::chrono::steady_clock::now()+3s;
        while(!std::filesystem::exists(path/"held")){
            expect(std::chrono::steady_clock::now()<deadline,"real child acquires lock and publishes marker");std::this_thread::sleep_for(2ms);
        }
        const auto request=accept_request(path).dump();
        auto accepting=std::async(std::launch::async,[&]{
            Buffer output;const auto status=cpn_engine_companion_accept_json(engine.handle,view(request),&output.value,nullptr);
            return std::pair{status,status==CPN_OK?output.json():Json(nullptr)};
        });
        expect(accepting.wait_for(50ms)==std::future_status::timeout,"locked intake cannot complete before close");
        const auto begin=std::chrono::steady_clock::now();expect(cpn_engine_close(engine.handle,nullptr)==CPN_OK,"cross-thread close requests cancellation");
        expect(accepting.wait_for(2s)==std::future_status::ready,"cancelled intake completes promptly while lock remains held");
        const auto [status,result]=accepting.get();
        expect(status==CPN_CLOSED || (status==CPN_OK && result["status"]=="Cancelled" && result["inbox_replaced"]==false),"close/admission race returns Closed or structured Cancelled");
        expect(std::chrono::steady_clock::now()-begin<2s,"close and operation drain are bounded");
        expect(!workspace.files->read_optional(inbox_paths(path).inbox_file,1024*1024),"cancel while waiting never writes inbox");
        workspace.files->write_atomic(path/"release","release");
        const auto exited=child.get();expect(exited.termination==cpn::ProcessTermination::exited && exited.exit_code==0,"lock child exits after explicit release");
    }catch(...){child_stop.request_stop();if(child.valid())(void)child.get();throw;}
}
int verify(const std::filesystem::path& executable){
    try{Workspace workspace;strict_requests(workspace);finite_write_response(workspace);pages_and_application(workspace);close_during_intake(workspace,std::filesystem::absolute(executable));
        std::cout<<"Companion C ABI tests passed\n";return 0;
    }catch(const std::exception& failure){std::cerr<<failure.what()<<'\n';return 1;}
}
}
#ifdef _WIN32
int wmain(int argc,wchar_t** argv){
    if(argc==3 && std::wstring_view(argv[1])==L"lock-child")return lock_child(std::filesystem::path(argv[2]));
    return argc==1?verify(std::filesystem::path(argv[0])):2;
}
#else
int main(int argc,char** argv){
    if(argc==3 && std::string_view(argv[1])=="lock-child")return lock_child(cpn::path_from_utf8(argv[2]));
    return argc==1?verify(cpn::path_from_utf8(argv[0])):2;
}
#endif
