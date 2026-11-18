// SPDX-License-Identifier: GPL-3.0-or-later
#include "cpn/api.h"
#include "cpn/session.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <stdexcept>
namespace {
using Json=nlohmann::json;
void expect(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
cpn_bytes view(const std::string& text){return {reinterpret_cast<const uint8_t*>(text.data()),text.size()};}
Json read(const cpn_buffer& buffer){return Json::parse(buffer.data,buffer.data+buffer.size);}
}
int main(){
    auto files=cpn::native_file_store();std::filesystem::path workspace;cpn_engine_handle engine=0;
    try{
        workspace=files->create_workspace();
        const auto recovery=workspace/cpn::path_from_utf8("会话.json"),backup=workspace/"previous.json",lock=workspace/"workspace.lock";
        cpn_engine_options options{sizeof(options),CPN_ABI_VERSION,1,1,1,0,1024*1024,1024*1024};
        expect(cpn_engine_create(&options,&engine,nullptr)==CPN_OK,"engine create");
        const auto limits = cpn_session_default_limits();
        expect(limits.max_documents == cpn::SessionBounds{}.max_documents && limits.max_request_bytes > options.max_request_bytes,
            "recovery limits match Core and allow a larger configured request");
        cpn_buffer oversized_result{};
        const std::string oversized_bytes(options.max_request_bytes + 1, ' ');
        expect(cpn_engine_session_save_json(engine, view(oversized_bytes), &oversized_result, nullptr) == CPN_SESSION_CAPACITY &&
            !oversized_result.handle, "oversized recovery request reports session capacity");
        cpn::WorkspaceSession session;session.workspace_id="workspace";session.revision=1;session.selected_document_id="source";
        cpn::SessionDocument document;document.document_id="source";document.revision=1;document.source_text="中文 😀";
        document.tests={{"binary",std::string("a\0b",3),std::string(""),true},{"absent","",std::nullopt,true}};
        document.editor_preferences={{"org.cpeditor.native.macos",1,"{ \"editorKind\" : \"codeeditorview\" }"}};
        session.documents.push_back(document);
        auto request=Json{{"schema_version",1},{"paths",{{"recovery_file",cpn::path_to_utf8(recovery)},
            {"backup_file",cpn::path_to_utf8(backup)},{"lock_file",cpn::path_to_utf8(lock)}}},
            {"session",Json::parse(cpn::serialize_workspace_session(session))},{"expected_disk_digest",nullptr}};
        cpn_buffer held{};expect(cpn_engine_info_json(engine,&held,nullptr)==CPN_OK,"hold buffer quota");
        auto json=request.dump();cpn_buffer result{};
        for (const auto path_field : {false,true}) {
            auto malformed=request;
            if (path_field) malformed["paths"]["recovery_flie"]="ignored typo";
            else malformed["expected_disk_digset"]="ignored typo";
            const auto bytes=malformed.dump();
            expect(cpn_engine_session_save_json(engine,view(bytes),&result,nullptr)==CPN_INVALID_ARGUMENT && !result.handle,"unknown session wrapper/path fields rejected before I/O or buffer quota");
            expect(!files->read_optional(recovery,1024*1024),"invalid wrapper cannot write recovery");
        }
        expect(cpn_engine_session_save_json(engine,view(json),&result,nullptr)==CPN_CAPACITY,"save reserves response before I/O");
        expect(!files->read_optional(recovery,1024*1024),"capacity failure cannot write recovery");
        cpn_buffer_release(held.handle,nullptr);
        expect(cpn_engine_session_save_json(engine,view(json),&result,nullptr)==CPN_OK,"save ABI");
        auto saved=read(result);expect(saved["status"]=="Saved" && saved["recovery_replaced"]==true,"saved state");
        const auto digest=saved["disk_digest"];cpn_buffer_release(result.handle,nullptr);
        const auto path=cpn::path_to_utf8(recovery);
        expect(cpn_engine_session_load(engine,view(path),&result,nullptr)==CPN_OK,"load ABI");
        auto loaded=read(result);expect(loaded["status"]=="Loaded" && loaded["disk_digest"]==digest,"load token");
        const auto restored=cpn::parse_workspace_session(loaded["session"].dump());
        expect(restored.documents[0].source_text==document.source_text,"unsaved source retained");
        expect(restored.documents[0].tests[0].input==std::string("a\0b",3) && restored.documents[0].tests[0].expected=="" && !restored.documents[0].tests[1].expected,"binary/empty/absent retained");
        expect(restored.documents[0].editor_preferences[0].object_json==document.editor_preferences[0].object_json,"native preferences retained verbatim");
        cpn_buffer_release(result.handle,nullptr);
        session.revision=2;session.documents[0].revision=2;session.documents[0].source_text="new revision";
        request["session"]=Json::parse(cpn::serialize_workspace_session(session));request["expected_disk_digest"]=digest;json=request.dump();
        expect(cpn_engine_session_save_json(engine,view(json),&result,nullptr)==CPN_OK,"update ABI");
        expect(read(result)["status"]=="Saved" && read(result)["backup_replaced"]==true,"backup update");cpn_buffer_release(result.handle,nullptr);
        expect(cpn_engine_session_save_json(engine,view(json),&result,nullptr)==CPN_OK,"conflict is structured");
        expect(read(result)["status"]=="Conflict","optimistic conflict");cpn_buffer_release(result.handle,nullptr);
        files->write_atomic(recovery,"{\"schema_version\":99}");
        expect(cpn_engine_session_load(engine,view(path),&result,nullptr)==CPN_OK,"unknown version load");
        expect(read(result)["status"]=="UnsupportedSchema" && read(result)["session"].is_null(),"unknown candidate unavailable");cpn_buffer_release(result.handle,nullptr);
        expect(files->read(recovery,1024)=="{\"schema_version\":99}","unknown data preserved");
        cpn_engine_destroy(engine,nullptr);engine=0;files->remove_workspace(workspace);
        std::cout<<"session C ABI tests passed\n";return 0;
    }catch(const std::exception& failure){if(engine)cpn_engine_destroy(engine,nullptr);if(!workspace.empty())files->remove_workspace(workspace);std::cerr<<failure.what()<<'\n';return 1;}
}
