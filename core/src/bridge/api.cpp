// SPDX-License-Identifier: GPL-3.0-or-later
#include "cpn/api.h"
#include "cpn/jobs.hpp"
#include "cpn/json.hpp"
#include "cpn/session.hpp"
#include "cpn/companion_inbox.hpp"
#include "cpn/companion_listener.hpp"
#include "cpn/text.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
namespace {
using Json=nlohmann::json;
void session_fields(const Json& object,std::initializer_list<std::string_view> allowed) {
    if (!object.is_object()) throw std::invalid_argument("Session request field must be an object");
    for (const auto& [key,value] : object.items()) {
        (void)value;
        if (std::find(allowed.begin(),allowed.end(),key)==allowed.end())
            throw std::invalid_argument("Unknown session request field: "+key);
    }
}
constexpr std::size_t total_buffer_limit=256*1024*1024;
constexpr std::size_t individual_buffer_limit=128*1024*1024;
struct ListenerConfiguration {
    std::string workspace_id;
    cpn::CompanionInboxPaths paths;
    cpn::CompanionInboxBounds bounds;
    cpn::CompanionListenerOptions options;
};
struct Engine {
    Engine(cpn_engine_options value):options(value),jobs({value.max_workers,value.max_jobs}){}
    cpn_engine_options options;
    cpn::JobRegistry jobs;
    std::stop_source operations;
    std::mutex drain_mutex;
    // Do not retain handles_mutex while joining listener threads.
    std::mutex listener_mutex;
    std::optional<ListenerConfiguration> listener_configuration;
    std::optional<cpn::CompanionListenerStartResult> listener_start_result;
    std::unique_ptr<cpn::CompanionListener> listener;
    void close_listener() {
        std::lock_guard lock(listener_mutex);
        if(listener)listener->stop();
    }
};
struct Buffer {std::uint64_t owner;std::string bytes;};
std::mutex handles_mutex;
std::map<std::uint64_t,std::shared_ptr<Engine>> engines;
std::map<std::uint64_t,std::shared_ptr<Buffer>> buffers;
std::uint64_t next_handle=1;
std::size_t buffer_bytes=0;
cpn_status error(cpn_error* destination,cpn_status status,std::string_view message) noexcept {
    if(destination && destination->struct_size>=sizeof(cpn_error)){
        destination->status=status;
        const auto size=std::min(message.size(),sizeof(destination->message)-1);
        if(size)std::memcpy(destination->message,message.data(),size);destination->message[size]='\0';
    }
    return status;
}
template<class F> cpn_status boundary(cpn_error* destination,F action) noexcept {
    try{return action();}
    catch(const Json::exception& ex){return error(destination,CPN_INVALID_ARGUMENT,ex.what());}
    catch(const std::invalid_argument& ex){return error(destination,CPN_INVALID_ARGUMENT,ex.what());}
    catch(const std::exception& ex){return error(destination,CPN_INTERNAL_ERROR,ex.what());}
    catch(...){return error(destination,CPN_INTERNAL_ERROR,"Unknown core exception");}
}
std::shared_ptr<Engine> lookup(std::uint64_t handle){
    std::lock_guard guard(handles_mutex);const auto found=engines.find(handle);
    return found==engines.end()?nullptr:found->second;
}
cpn_status job_status(const cpn::JobStatus& value,cpn_error* destination){
    cpn_status status=CPN_INTERNAL_ERROR;
    switch(value.code){
        case cpn::JobError::none:status=CPN_OK;break;
        case cpn::JobError::closed:status=CPN_CLOSED;break;
        case cpn::JobError::capacity_exceeded:case cpn::JobError::id_exhausted:status=CPN_CAPACITY;break;
        case cpn::JobError::unknown_job:status=CPN_INVALID_HANDLE;break;
        case cpn::JobError::not_completed:case cpn::JobError::terminal_unacknowledged:status=CPN_BUSY;break;
        case cpn::JobError::invalid_acknowledgement:status=CPN_INVALID_ARGUMENT;break;
    }
    return error(destination,status,value.message);
}
cpn_status allocate_buffer(std::uint64_t owner,const Engine& engine,std::string bytes,cpn_buffer* output,cpn_error* destination){
    if(bytes.size()>individual_buffer_limit)return error(destination,CPN_CAPACITY,"Event batch exceeds buffer byte limit");
    std::lock_guard guard(handles_mutex);
    std::size_t count=0;for(const auto& [id,buffer]:buffers){(void)id;if(buffer->owner==owner)++count;}
    if(count>=engine.options.max_buffers || bytes.size()>total_buffer_limit-buffer_bytes || next_handle==std::numeric_limits<std::uint64_t>::max())
        return error(destination,CPN_CAPACITY,"Release event buffers before draining more events");
    const auto handle=next_handle++;
    auto storage=std::make_shared<Buffer>(Buffer{owner,std::move(bytes)});
    buffers.emplace(handle,storage);buffer_bytes+=storage->bytes.size();
    output->data=reinterpret_cast<const std::uint8_t*>(storage->bytes.data());output->size=storage->bytes.size();output->handle=handle;
    return error(destination,CPN_OK,{});
}
struct ReservedBuffer {
    cpn_buffer value{};
    bool published{false};
    ~ReservedBuffer(){if(value.handle && !published)cpn_buffer_release(value.handle,nullptr);}
    void publish(std::string_view bytes,cpn_buffer* output){
        std::lock_guard guard(handles_mutex);
        auto storage=buffers.at(value.handle);
        if(bytes.size()>storage->bytes.size())throw std::length_error("Response exceeds reserved buffer");
        buffer_bytes-=storage->bytes.size()-bytes.size();
        storage->bytes.resize(bytes.size());std::copy(bytes.begin(),bytes.end(),storage->bytes.begin());
        value.size=bytes.size();value.data=reinterpret_cast<const std::uint8_t*>(storage->bytes.data());
        *output=value;published=true;
    }
};
Json event_json(const cpn::JobEvent& event){
    auto json=Json{{"job_id",event.job_id},{"sequence",event.sequence},{"state",cpn::job_state_name(event.state)},
        {"testcase_id",event.testcase_id}};
    if(event.result)json["result"]=Json::parse(cpn::serialize_batch_result(*event.result));
    return json;
}
std::size_t output_size(const cpn::JobEvent& event){
    if(!event.result)return 1024;
    std::size_t size=4096;
    if(event.result->build)size+=event.result->build->standard_output.size()+event.result->build->standard_error.size();
    for(const auto& test:event.result->tests)size+=test.process.standard_output.size()+test.process.standard_error.size()+1024+test.testcase_id.size()+test.detail.size();
    return size;
}
namespace companion_field {
constexpr auto schema_version="schema_version";
constexpr auto workspace_id="workspace_id";
constexpr auto paths="paths";
constexpr auto inbox_file="inbox_file";
constexpr auto backup_file="backup_file";
constexpr auto lock_file="lock_file";
constexpr auto bounds="bounds";
constexpr auto payload="payload";
constexpr auto original_json="original_json";
constexpr auto lock_timeout_ms="lock_timeout_ms";
constexpr auto after_sequence="after_sequence";
constexpr auto max_items="max_items";
constexpr auto max_page_bytes="max_page_bytes";
constexpr auto import_id="import_id";
constexpr auto session="session";
constexpr auto session_paths="session_paths";
constexpr auto recovery_file="recovery_file";
constexpr auto defaults="defaults";
constexpr auto source_text="source_text";
constexpr auto language_id="language_id";
constexpr auto toolchain_profile_id="toolchain_profile_id";
constexpr auto document_preferences="document_preferences";
constexpr auto namespace_id="namespace_id";
constexpr auto object_json="object_json";
constexpr auto max_receipts="max_receipts";
constexpr auto status="status";
constexpr auto detail="detail";
constexpr auto disk_digest="disk_digest";
constexpr auto inbox_id="inbox_id";
constexpr auto revision="revision";
constexpr auto next_after_sequence="next_after_sequence";
constexpr auto has_more="has_more";
constexpr auto items="items";
constexpr auto receipt="receipt";
constexpr auto inbox_status="inbox_status";
constexpr auto inbox_replaced="inbox_replaced";
constexpr auto recovery_replaced="recovery_replaced";
constexpr auto backup_replaced="backup_replaced";
constexpr auto item="item";
constexpr auto sequence="sequence";
constexpr auto state="state";
constexpr auto problem_id="problem_id";
constexpr auto document_id="document_id";
constexpr auto test_count="test_count";
constexpr auto test_ids="test_ids";
constexpr auto previous_import_id="previous_import_id";
constexpr auto payload_digest="payload_digest";
constexpr auto ordinary_stdio_compatible="ordinary_stdio_compatible";
constexpr auto applied_session_digest="applied_session_digest";
constexpr auto applied_session_revision="applied_session_revision";
}
namespace listener_field {
constexpr auto options="options", port="port", enable_ipv6="enable_ipv6";
constexpr auto has_listener="has_listener", engine_closed="engine_closed", ipv6="ipv6";
constexpr auto start_status="start_status", native_error="native_error", failure_detail="failure_detail";
constexpr auto statistics="statistics", running="running", closed="closed", failed="failed";
constexpr auto active_connections="active_connections", outstanding_admissions="outstanding_admissions", inflight_body_bytes="inflight_body_bytes";
constexpr auto accepted="accepted", duplicates="duplicates", admission_failures="admission_failures";
constexpr auto connection_rejections="connection_rejections", admission_rejections="admission_rejections";
constexpr auto read_timeouts="read_timeouts", write_timeouts="write_timeouts", request_timeouts="request_timeouts";
}
constexpr std::uint64_t companion_request_schema_version=1;
constexpr std::size_t companion_response_bytes=65536;
constexpr std::size_t companion_request_depth=64;
constexpr std::size_t companion_request_nodes=262144;
Json companion_json(std::string_view bytes,std::size_t maximum,std::size_t depth=companion_request_depth) {
    if(bytes.empty() || bytes.size()>maximum)throw std::invalid_argument("Companion request JSON exceeds byte limit");
    std::vector<std::optional<std::set<std::string>>> containers;
    std::size_t nodes=0;
    return Json::parse(bytes,[&](int,Json::parse_event_t event,Json& parsed){
        const bool starts=event==Json::parse_event_t::object_start || event==Json::parse_event_t::array_start;
        if(starts || event==Json::parse_event_t::key || event==Json::parse_event_t::value){
            if(nodes==companion_request_nodes)throw std::invalid_argument("Companion request JSON exceeds node limit");
            ++nodes;
        }
        if(starts){
            if(containers.size()>=depth)throw std::invalid_argument("Companion request JSON exceeds nesting limit");
            if(event==Json::parse_event_t::object_start)containers.emplace_back(std::in_place);
            else containers.emplace_back(std::nullopt);
        }else if(event==Json::parse_event_t::object_end || event==Json::parse_event_t::array_end)containers.pop_back();
        else if(event==Json::parse_event_t::key && !containers.back()->insert(parsed.get<std::string>()).second)
            throw std::invalid_argument("Duplicate Companion request JSON field");
        return true;
    });
}
std::uint64_t companion_integer(const Json& value,std::uint64_t minimum,std::uint64_t maximum) {
    if(!value.is_number_integer() || (!value.is_number_unsigned() && value.get<std::int64_t>()<0))
        throw std::invalid_argument("Companion request integer has invalid type or sign");
    const auto result=value.get<std::uint64_t>();
    if(result<minimum || result>maximum)throw std::invalid_argument("Companion request integer exceeds supported bounds");
    return result;
}
std::string companion_text(const Json& value,std::size_t maximum,bool identity=false) {
    if(!value.is_string() || value.get_ref<const Json::string_t&>().size()>maximum)
        throw std::invalid_argument("Companion request string has invalid type or byte limit");
    auto result=value.get<std::string>();
    if(identity && (result.empty() || result.find('\0')!=std::string::npos || cpn::decode_utf8(result).error_offset))
        throw std::invalid_argument("Invalid Companion request identity");
    return result;
}
std::filesystem::path companion_path(const Json& value,std::size_t maximum) {
    auto path=cpn::path_from_utf8(companion_text(value,maximum,true));
    if(!path.is_absolute())throw std::invalid_argument("Companion storage path must be absolute");
    return path;
}
template<class Bounds> struct CompanionBoundField {const char* name;std::size_t Bounds::* member;};
constexpr CompanionBoundField<cpn::CompanionBounds> companion_payload_fields[]{
    {"max_json_bytes",&cpn::CompanionBounds::max_json_bytes},{"max_json_depth",&cpn::CompanionBounds::max_json_depth},
    {"max_json_nodes",&cpn::CompanionBounds::max_json_nodes},{"max_string_bytes",&cpn::CompanionBounds::max_string_bytes},
    {"max_total_string_bytes",&cpn::CompanionBounds::max_total_string_bytes},{"max_name_bytes",&cpn::CompanionBounds::max_name_bytes},
    {"max_url_bytes",&cpn::CompanionBounds::max_url_bytes},{"max_tests",&cpn::CompanionBounds::max_tests},
    {"max_test_bytes",&cpn::CompanionBounds::max_test_bytes},{"max_total_test_bytes",&cpn::CompanionBounds::max_total_test_bytes},
    {"max_batch_id_bytes",&cpn::CompanionBounds::max_batch_id_bytes}};
constexpr CompanionBoundField<cpn::CompanionInboxBounds> companion_inbox_fields[]{
    {"max_json_bytes",&cpn::CompanionInboxBounds::max_json_bytes},{"max_total_payload_bytes",&cpn::CompanionInboxBounds::max_total_payload_bytes},
    {"max_records",&cpn::CompanionInboxBounds::max_records},{"max_pending",&cpn::CompanionInboxBounds::max_pending},
    {"max_batches",&cpn::CompanionInboxBounds::max_batches},{"max_total_tests",&cpn::CompanionInboxBounds::max_total_tests},
    {"max_json_depth",&cpn::CompanionInboxBounds::max_json_depth},{"max_json_nodes",&cpn::CompanionInboxBounds::max_json_nodes},
    {"max_id_bytes",&cpn::CompanionInboxBounds::max_id_bytes},{"max_path_bytes",&cpn::CompanionInboxBounds::max_path_bytes}};
template<class Bounds,std::size_t N> void companion_bound_values(const Json& value,Bounds& bounds,
    const CompanionBoundField<Bounds>(&fields)[N],std::initializer_list<std::string_view> extra={}) {
    if(!value.is_object())throw std::invalid_argument("Companion bounds must be an object");
    const Bounds hard;
    for(const auto& [key,item]:value.items()){
        const auto found=std::find_if(std::begin(fields),std::end(fields),[&](const auto& field){return field.name==key;});
        if(found!=std::end(fields))bounds.*(found->member)=static_cast<std::size_t>(companion_integer(item,1,hard.*(found->member)));
        else if(std::find(extra.begin(),extra.end(),key)==extra.end())throw std::invalid_argument("Unknown Companion bound: "+key);
    }
}
struct CompanionRequest {
    std::string workspace_id;
    cpn::CompanionInboxPaths paths;
    cpn::CompanionInboxBounds bounds;
};
CompanionRequest companion_common(const Json& doc) {
    using namespace companion_field;
    (void)companion_integer(doc.at(schema_version),companion_request_schema_version,companion_request_schema_version);
    CompanionRequest request;
    if(doc.contains(bounds)){
        const auto& values=doc.at(bounds);
        companion_bound_values(values,request.bounds,companion_inbox_fields,{payload});
        if(values.contains(payload)){
            const auto& limits=values.at(payload);
            companion_bound_values(limits,request.bounds.payload,companion_payload_fields,{"max_batch_size"});
            if(limits.contains("max_batch_size"))request.bounds.payload.max_batch_size=
                companion_integer(limits.at("max_batch_size"),1,cpn::CompanionBounds{}.max_batch_size);
        }
        if(request.bounds.max_id_bytes<64)throw std::invalid_argument("Companion identity capacity must hold a SHA-256 identity");
    }
    request.workspace_id=companion_text(doc.at(workspace_id),request.bounds.max_id_bytes,true);
    const auto& location=doc.at(paths);
    session_fields(location,{inbox_file,backup_file,lock_file});
    request.paths={companion_path(location.at(inbox_file),request.bounds.max_path_bytes),
        companion_path(location.at(backup_file),request.bounds.max_path_bytes),companion_path(location.at(lock_file),request.bounds.max_path_bytes)};
    return request;
}
Json companion_request(cpn_bytes bytes,const Engine& engine,std::initializer_list<std::string_view> fields) {
    if(!bytes.data || !bytes.size || bytes.size>engine.options.max_request_bytes)
        throw std::invalid_argument("Invalid Companion request bytes");
    auto result=companion_json(std::string_view(reinterpret_cast<const char*>(bytes.data),bytes.size),
        static_cast<std::size_t>(engine.options.max_request_bytes));
    session_fields(result,fields);return result;
}
std::chrono::milliseconds companion_timeout(const Json& doc) {
    return std::chrono::milliseconds{doc.contains(companion_field::lock_timeout_ms)?
        static_cast<std::int64_t>(companion_integer(doc.at(companion_field::lock_timeout_ms),0,60000)):5000};
}
cpn::ImportApplicationBounds companion_application_bounds(const Json& doc,const CompanionRequest& request) {
    cpn::ImportApplicationBounds bounds;bounds.payload=request.bounds.payload;
    if(doc.contains(companion_field::max_receipts))bounds.max_receipts=static_cast<std::size_t>(
        companion_integer(doc.at(companion_field::max_receipts),1,cpn::ImportApplicationBounds{}.max_receipts));
    return bounds;
}
cpn::ImportDocumentDefaults companion_defaults(const Json& doc,const cpn::SessionBounds& bounds) {
    using namespace companion_field;
    cpn::ImportDocumentDefaults result;
    if(!doc.contains(defaults))return result;
    const auto& value=doc.at(defaults);
    session_fields(value,{source_text,language_id,toolchain_profile_id,document_preferences});
    if(value.contains(source_text))result.source_text=companion_text(value.at(source_text),bounds.max_source_bytes);
    if(cpn::decode_utf8(result.source_text).error_offset)throw std::invalid_argument("Invalid UTF-8 default source text");
    for(const auto& [key,destination]:std::array{
        std::pair{language_id,&result.language_id},std::pair{toolchain_profile_id,&result.toolchain_profile_id}}){
        if(value.contains(key) && !value.at(key).is_null())*destination=companion_text(value.at(key),bounds.max_id_bytes,true);
    }
    if(value.contains(document_preferences)){
        const auto& preferences=value.at(document_preferences);
        if(!preferences.is_array() || preferences.size()>bounds.max_preferences_per_owner)
            throw std::invalid_argument("Invalid default document preference array count/type");
        std::set<std::pair<std::string,std::uint64_t>> identities;
        std::size_t total=0;
        for(const auto& preference:preferences){
            session_fields(preference,{namespace_id,schema_version,object_json});
            auto name=companion_text(preference.at(namespace_id),bounds.max_id_bytes,true);
            const auto version=companion_integer(preference.at(schema_version),1,std::numeric_limits<std::uint64_t>::max());
            auto object=companion_text(preference.at(object_json),bounds.max_preferences_bytes);
            if(object.size()>bounds.max_total_preferences_bytes-total)throw std::invalid_argument("Default preferences exceed total byte capacity");
            total+=object.size();
            if(!companion_json(object,bounds.max_preferences_bytes,bounds.max_preferences_depth).is_object())
                throw std::invalid_argument("Default preference must contain an opaque JSON object");
            if(!identities.emplace(name,version).second)throw std::invalid_argument("Duplicate default preference namespace/version");
            if(name==cpn::companion_document_namespace)throw std::invalid_argument("Defaults cannot supply an assigned Companion provenance");
            result.document_preferences.push_back({std::move(name),version,std::move(object)});
        }
    }
    return result;
}
struct ListenerSizeField { const char* name;std::size_t cpn::CompanionListenerOptions::* member; };
constexpr ListenerSizeField listener_size_fields[]{
    {"max_connections",&cpn::CompanionListenerOptions::max_connections},
    {"max_admissions",&cpn::CompanionListenerOptions::max_admissions},
    {"max_body_bytes",&cpn::CompanionListenerOptions::max_body_bytes},
    {"max_inflight_body_bytes",&cpn::CompanionListenerOptions::max_inflight_body_bytes},
    {"max_header_bytes",&cpn::CompanionListenerOptions::max_header_bytes},
    {"max_header_count",&cpn::CompanionListenerOptions::max_header_count},
    {"max_response_bytes",&cpn::CompanionListenerOptions::max_response_bytes},
    {"socket_send_buffer_bytes",&cpn::CompanionListenerOptions::socket_send_buffer_bytes}};
struct ListenerTimeField { const char* name;std::chrono::milliseconds cpn::CompanionListenerOptions::* member; };
constexpr ListenerTimeField listener_time_fields[]{
    {"read_timeout_ms",&cpn::CompanionListenerOptions::read_timeout},
    {"write_timeout_ms",&cpn::CompanionListenerOptions::write_timeout},
    {"request_timeout_ms",&cpn::CompanionListenerOptions::request_timeout},
    {"lock_timeout_ms",&cpn::CompanionListenerOptions::lock_timeout}};
ListenerConfiguration listener_configuration(const Json& doc) {
    auto context=companion_common(doc);
    ListenerConfiguration result{std::move(context.workspace_id),std::move(context.paths),context.bounds,{}};
    // A reduced payload bound also reduces the omitted transport body default.
    result.options.max_body_bytes=std::min(result.options.max_body_bytes,result.bounds.payload.max_json_bytes);
    if(!doc.contains(listener_field::options))return result;
    const auto& values=doc.at(listener_field::options);
    if(!values.is_object())throw std::invalid_argument("Companion listener options must be an object");
    for(const auto& [name,value]:values.items()){
        if(name==listener_field::port)result.options.port=static_cast<std::uint16_t>(companion_integer(value,0,65535));
        else if(name==listener_field::enable_ipv6){
            if(!value.is_boolean())throw std::invalid_argument("Companion listener IPv6 option must be boolean");
            result.options.enable_ipv6=value.get<bool>();
        }else{
            const auto size=std::find_if(std::begin(listener_size_fields),std::end(listener_size_fields),[&](const auto& field){return name==field.name;});
            if(size!=std::end(listener_size_fields)){
                result.options.*(size->member)=static_cast<std::size_t>(companion_integer(value,1,std::numeric_limits<std::size_t>::max()));
                continue;
            }
            const auto time=std::find_if(std::begin(listener_time_fields),std::end(listener_time_fields),[&](const auto& field){return name==field.name;});
            if(time!=std::end(listener_time_fields)){
                result.options.*(time->member)=std::chrono::milliseconds{static_cast<std::int64_t>(companion_integer(value,0,300000))};
                continue;
            }
            throw std::invalid_argument("Unknown Companion listener option: "+name);
        }
    }
    return result;
}
bool same_listener_configuration(const ListenerConfiguration& left,const ListenerConfiguration& right) noexcept {
    if(left.workspace_id!=right.workspace_id || left.paths.inbox_file!=right.paths.inbox_file ||
       left.paths.backup_file!=right.paths.backup_file || left.paths.lock_file!=right.paths.lock_file ||
       left.options.port!=right.options.port || left.options.enable_ipv6!=right.options.enable_ipv6 ||
       left.bounds.payload.max_batch_size!=right.bounds.payload.max_batch_size)return false;
    for(const auto& field:listener_size_fields)if(left.options.*(field.member)!=right.options.*(field.member))return false;
    for(const auto& field:listener_time_fields)if(left.options.*(field.member)!=right.options.*(field.member))return false;
    for(const auto& field:companion_payload_fields)if(left.bounds.payload.*(field.member)!=right.bounds.payload.*(field.member))return false;
    for(const auto& field:companion_inbox_fields)if(left.bounds.*(field.member)!=right.bounds.*(field.member))return false;
    return true;
}
void listener_request_schema(const Json& request) {
    (void)companion_integer(request.at(companion_field::schema_version),companion_request_schema_version,companion_request_schema_version);
}
Json companion_receipt(const cpn::ImportApplicationReceipt& receipt) {
    return Json{{companion_field::inbox_id,receipt.inbox_id},{companion_field::import_id,receipt.import_id},
        {companion_field::problem_id,receipt.problem_id},{companion_field::document_id,receipt.document_id},
        {companion_field::payload_digest,receipt.payload_digest},{companion_field::test_ids,receipt.test_ids}};
}
// Build the finite write response without allocations after a file replacement.
// Buffer quota AND string capacity are reserved before invoking the mutating service.
class CompanionWriteResponse {
public:
    CompanionWriteResponse(){bytes_.reserve(companion_response_bytes);}
    std::string_view render(const cpn::SessionSaveResult& result) {
        using namespace companion_field;
        append("{");key(schema_version);number(companion_request_schema_version);
        comma_key(status);string(cpn::session_save_status_name(result.status));
        comma_key(detail);string(std::string_view(result.detail).substr(0,4096));
        comma_key(disk_digest);optional(result.disk_digest);
        comma_key(recovery_replaced);append(result.recovery_replaced?"true":"false");
        comma_key(backup_replaced);append(result.backup_replaced?"true":"false");
        append("}");return bytes_;
    }
    std::string_view render(const cpn::CompanionInboxWriteResult& result) {
        using namespace companion_field;
        append("{");key(schema_version);number(companion_request_schema_version);comma_key(status);string(cpn::companion_inbox_write_status_name(result.status));
        comma_key(detail);string(std::string_view(result.detail).substr(0,4096));
        comma_key(disk_digest);optional(result.disk_digest);
        comma_key(inbox_replaced);append(result.inbox_replaced?"true":"false");
        comma_key(backup_replaced);append(result.backup_replaced?"true":"false");
        comma_key(item);
        if(result.item){
            const auto& value=*result.item;
            append("{");key(sequence);number(value.sequence);comma_key(state);string(value.state==cpn::CompanionInboxState::pending?"Pending":"Applied");
            comma_key(import_id);string(value.import_id);comma_key(problem_id);string(value.problem_id);
            comma_key(document_id);string(value.document_id);comma_key(test_count);number(value.test_ids.size());
            comma_key(previous_import_id);optional(value.previous_import_id);comma_key(payload_digest);string(value.payload_digest);
            comma_key(ordinary_stdio_compatible);append(value.candidate.compatible_with_ordinary_stdio()?"true":"false");
            comma_key(applied_session_digest);optional(value.applied_session_digest);
            comma_key(applied_session_revision);if(value.applied_session_revision)number(*value.applied_session_revision);else append("null");
            append("}");
        }else append("null");
        append("}");return bytes_;
    }
    std::string_view render_listener(std::string_view operation,const Engine& engine,
        const cpn::CompanionListenerStatistics& stats,std::string_view failure) {
        using namespace companion_field;
        append("{");key(schema_version);number(companion_request_schema_version);comma_key(status);string(operation);
        comma_key(listener_field::has_listener);append(engine.listener?"true":"false");
        comma_key(listener_field::engine_closed);append(engine.operations.stop_requested()?"true":"false");
        const auto* started=engine.listener_start_result?&*engine.listener_start_result:nullptr;
        comma_key(listener_field::port);number(started?started->port:0);
        comma_key(listener_field::ipv6);append(started && started->ipv6?"true":"false");
        comma_key(listener_field::start_status);if(started)string(cpn::companion_listener_start_status_name(started->status));else append("null");
        comma_key(listener_field::native_error);signed_number(started?started->native_error:0);
        comma_key(detail);string(started?std::string_view(started->detail).substr(0,4096):std::string_view{});
        comma_key(listener_field::failure_detail);string(failure.substr(0,4096));
        comma_key(listener_field::statistics);append("{");key(listener_field::running);append(stats.running?"true":"false");
        comma_key(listener_field::closed);append(stats.closed?"true":"false");
        comma_key(listener_field::failed);append(stats.failed?"true":"false");
        for(const auto& [name,value]:std::array{
            std::pair{listener_field::active_connections,static_cast<std::uint64_t>(stats.active_connections)},
            std::pair{listener_field::outstanding_admissions,static_cast<std::uint64_t>(stats.outstanding_admissions)},
            std::pair{listener_field::inflight_body_bytes,static_cast<std::uint64_t>(stats.inflight_body_bytes)},
            std::pair{listener_field::accepted,stats.accepted},std::pair{listener_field::duplicates,stats.duplicates},
            std::pair{listener_field::admission_failures,stats.admission_failures},
            std::pair{listener_field::connection_rejections,stats.connection_rejections},std::pair{listener_field::admission_rejections,stats.admission_rejections},
            std::pair{listener_field::read_timeouts,stats.read_timeouts},std::pair{listener_field::write_timeouts,stats.write_timeouts},
            std::pair{listener_field::request_timeouts,stats.request_timeouts}}){comma_key(name);number(value);}
        append("}}");return bytes_;
    }
private:
    void signed_number(std::int64_t value){char text[21];const auto end=std::to_chars(std::begin(text),std::end(text),value).ptr;append({text,static_cast<std::size_t>(end-text)});}
    void append(std::string_view value){
        if(value.size()>companion_response_bytes-bytes_.size())throw std::length_error("Companion response exceeded its reserved capacity");
        bytes_.append(value);
    }
    void number(std::uint64_t value){char text[20];const auto end=std::to_chars(std::begin(text),std::end(text),value).ptr;append({text,static_cast<std::size_t>(end-text)});}
    void key(std::string_view name){string(name);append(":");}
    void comma_key(std::string_view name){append(",");key(name);}
    void optional(const std::optional<std::string>& value){if(value)string(*value);else append("null");}
    void string(std::string_view value){
        append("\"");
        for(std::size_t i=0;i<value.size();){
            const auto first=static_cast<unsigned char>(value[i]);
            if(first=='"' || first=='\\'){char escaped[]{'\\',static_cast<char>(first)};append({escaped,2});++i;}
            else if(first<0x20){constexpr char digits[]="0123456789abcdef";char escaped[]{'\\','u','0','0',digits[first>>4],digits[first&15]};append({escaped,6});++i;}
            else if(first<0x80){append(value.substr(i++,1));}
            else {
                // Native error text may contain invalid locale bytes. Preserve valid UTF-8;
                // represent invalid bytes explicitly as replacement scalars in diagnostics.
                const std::size_t length=first>=0xc2 && first<=0xdf?2:first>=0xe0 && first<=0xef?3:first>=0xf0 && first<=0xf4?4:0;
                bool valid=length && length<=value.size()-i;
                if(valid)for(std::size_t j=1;j<length;++j)if((static_cast<unsigned char>(value[i+j])&0xc0)!=0x80)valid=false;
                if(valid && length==3){const auto second=static_cast<unsigned char>(value[i+1]);if((first==0xe0 && second<0xa0) || (first==0xed && second>=0xa0))valid=false;}
                if(valid && length==4){const auto second=static_cast<unsigned char>(value[i+1]);if((first==0xf0 && second<0x90) || (first==0xf4 && second>0x8f))valid=false;}
                if(valid){append(value.substr(i,length));i+=length;}else{append("\\ufffd");++i;}
            }
        }
        append("\"");
    }
    std::string bytes_;
};
}
extern "C" {
uint32_t cpn_abi_version(void){return CPN_ABI_VERSION;}
cpn_status cpn_engine_create(const cpn_engine_options* given,cpn_engine_handle* output,cpn_error* destination){
    if(output)*output=0;
    return boundary(destination,[&]{
        if(!output)return error(destination,CPN_INVALID_ARGUMENT,"Engine output is required");
        cpn_engine_options options{sizeof(cpn_engine_options),CPN_ABI_VERSION,2,16,4,0,16*1024*1024,8*1024*1024};
        if(given){
            if(given->struct_size<sizeof(cpn_engine_options) || given->abi_version!=CPN_ABI_VERSION)
                return error(destination,CPN_ABI_MISMATCH,"Unsupported ABI version or options size");
            if(given->reserved)return error(destination,CPN_INVALID_ARGUMENT,"Reserved options must be zero");
            if(given->max_workers)options.max_workers=given->max_workers;
            if(given->max_jobs)options.max_jobs=given->max_jobs;
            if(given->max_buffers)options.max_buffers=given->max_buffers;
            if(given->max_request_bytes)options.max_request_bytes=given->max_request_bytes;
            if(given->max_retained_output_bytes)options.max_retained_output_bytes=given->max_retained_output_bytes;
        }
        if(options.max_workers>8 || options.max_jobs>64 || options.max_workers>options.max_jobs || options.max_buffers>16 ||
           options.max_request_bytes>64*1024*1024 || options.max_retained_output_bytes>64*1024*1024)
            return error(destination,CPN_INVALID_ARGUMENT,"Engine options exceed supported limits");
        std::lock_guard guard(handles_mutex);
        if(engines.size()>=16 || next_handle==std::numeric_limits<std::uint64_t>::max())return error(destination,CPN_CAPACITY,"Engine capacity exhausted");
        auto engine=std::make_shared<Engine>(options);const auto handle=next_handle++;
        engines.emplace(handle,std::move(engine));*output=handle;return error(destination,CPN_OK,{});
    });
}
cpn_status cpn_engine_close(cpn_engine_handle handle,cpn_error* destination){
    return boundary(destination,[&]{auto engine=lookup(handle);if(!engine)return error(destination,CPN_INVALID_HANDLE,"Unknown engine");
        engine->operations.request_stop();engine->close_listener();engine->jobs.close();return error(destination,CPN_OK,{});});
}
cpn_status cpn_engine_destroy(cpn_engine_handle handle,cpn_error* destination){
    return boundary(destination,[&]{std::shared_ptr<Engine> engine;
        {std::lock_guard guard(handles_mutex);auto found=engines.find(handle);if(found==engines.end())return error(destination,CPN_INVALID_HANDLE,"Unknown engine");
         engine=std::move(found->second);engines.erase(found);}
        engine->operations.request_stop();engine->close_listener();engine->jobs.close();return error(destination,CPN_OK,{});});
}
cpn_status cpn_engine_submit_json(cpn_engine_handle handle,cpn_bytes json,cpn_job_handle* output,cpn_error* destination){
    if(output)*output=0;
    return boundary(destination,[&]{auto engine=lookup(handle);if(!engine)return error(destination,CPN_INVALID_HANDLE,"Unknown engine");
        if(!output || (!json.data && json.size))return error(destination,CPN_INVALID_ARGUMENT,"Request bytes and job output are required");
        if(json.size>engine->options.max_request_bytes)return error(destination,CPN_INVALID_ARGUMENT,"Request exceeds byte limit");
        cpn::BatchRequest request;
        try{request=cpn::parse_batch_request(std::string_view(json.data?reinterpret_cast<const char*>(json.data):"",json.size),
            {static_cast<std::size_t>(engine->options.max_request_bytes),static_cast<std::size_t>(engine->options.max_retained_output_bytes),4096});}
        catch(const std::bad_alloc&){throw;}
        catch(const std::exception& ex){return error(destination,CPN_INVALID_ARGUMENT,ex.what());}
        const auto result=engine->jobs.submit(request);if(result)*output=result.job_id;return job_status(result.status,destination);
    });
}
cpn_status cpn_engine_cancel(cpn_engine_handle handle,cpn_job_handle job,cpn_error* destination){
    return boundary(destination,[&]{auto engine=lookup(handle);return engine?job_status(engine->jobs.cancel(job),destination):error(destination,CPN_INVALID_HANDLE,"Unknown engine");});
}
cpn_status cpn_engine_release_job(cpn_engine_handle handle,cpn_job_handle job,cpn_error* destination){
    return boundary(destination,[&]{auto engine=lookup(handle);return engine?job_status(engine->jobs.release(job),destination):error(destination,CPN_INVALID_HANDLE,"Unknown engine");});
}
cpn_status cpn_engine_drain_json(cpn_engine_handle handle,uint32_t maximum,cpn_buffer* output,cpn_error* destination){
    if(output)*output={};
    return boundary(destination,[&]{auto engine=lookup(handle);if(!engine)return error(destination,CPN_INVALID_HANDLE,"Unknown engine");
        if(!output || maximum==0 || maximum>128)return error(destination,CPN_INVALID_ARGUMENT,"Drain count must be between 1 and 128");
        std::lock_guard guard(engine->drain_mutex);
        const auto pending=engine->jobs.poll(maximum);
        auto events=Json::array();std::vector<std::pair<std::uint64_t,std::uint64_t>> acknowledgements;
        std::size_t estimated=0;
        for(const auto& event:pending){
            const auto size=output_size(event);
            if(!events.empty() && size>(individual_buffer_limit/2)-std::min(estimated,individual_buffer_limit/2))break;
            events.push_back(event_json(event));estimated+=size;acknowledgements.emplace_back(event.job_id,event.sequence);
        }
        auto serialized=Json{{"schema_version",1},{"events",std::move(events)}}.dump(-1,' ',false,Json::error_handler_t::replace);
        const auto allocated=allocate_buffer(handle,*engine,std::move(serialized),output,destination);
        if(allocated!=CPN_OK)return allocated;
        const auto status=engine->jobs.acknowledge(acknowledgements);
        if(!status){cpn_buffer_release(output->handle,nullptr);*output={};return job_status(status,destination);}
        return error(destination,CPN_OK,{});
    });
}
cpn_status cpn_engine_info_json(cpn_engine_handle handle,cpn_buffer* output,cpn_error* destination){
    if(output)*output={};
    return boundary(destination,[&]{auto engine=lookup(handle);if(!engine)return error(destination,CPN_INVALID_HANDLE,"Unknown engine");
        if(!output)return error(destination,CPN_INVALID_ARGUMENT,"Buffer output is required");
        const auto capabilities=cpn::process_capabilities();
        auto doc=Json{{"schema_version",1},{"abi_version",CPN_ABI_VERSION},
            {"max_workers",engine->options.max_workers},{"max_jobs",engine->options.max_jobs},
            {"max_buffers",engine->options.max_buffers},{"max_request_bytes",engine->options.max_request_bytes},
            {"max_retained_output_bytes",engine->options.max_retained_output_bytes},
            {"capabilities",{{"managed_process_group",capabilities.managed_process_group},
                             {"memory_limit",capabilities.memory_limit},{"spawn_deadline",capabilities.spawn_deadline}}}};
        return allocate_buffer(handle,*engine,doc.dump(),output,destination);
    });
}
cpn_status cpn_engine_session_load(cpn_engine_handle handle,cpn_bytes path,cpn_buffer* output,cpn_error* destination){
    if(output)*output={};
    return boundary(destination,[&]{auto engine=lookup(handle);if(!engine)return error(destination,CPN_INVALID_HANDLE,"Unknown engine");
        if(!output || !path.data || !path.size || path.size>32768)return error(destination,CPN_INVALID_ARGUMENT,"Invalid recovery path bytes");
        auto loaded=cpn::SessionService().load(cpn::path_from_utf8(std::string_view(reinterpret_cast<const char*>(path.data),path.size)));
        auto doc=Json{{"schema_version",1},{"status",cpn::session_load_status_name(loaded.status)},{"detail",loaded.detail}};
        doc["disk_digest"]=loaded.disk_digest?Json(*loaded.disk_digest):Json(nullptr);
        doc["session"]=loaded.session?Json::parse(cpn::serialize_workspace_session(*loaded.session)):Json(nullptr);
        return allocate_buffer(handle,*engine,doc.dump(-1,' ',false,Json::error_handler_t::replace),output,destination);
    });
}
cpn_status cpn_engine_session_save_json(cpn_engine_handle handle,cpn_bytes request,cpn_buffer* output,cpn_error* destination){
    if(output)*output={};
    return boundary(destination,[&]{auto engine=lookup(handle);if(!engine)return error(destination,CPN_INVALID_HANDLE,"Unknown engine");
        if(engine->operations.stop_requested())return error(destination,CPN_CLOSED,"Engine is closed");
        if(!output || !request.data || !request.size || request.size>engine->options.max_request_bytes)
            return error(destination,CPN_INVALID_ARGUMENT,"Invalid session request bytes");
        cpn::SessionPaths locations;cpn::WorkspaceSession session;std::optional<std::string> digest;
        std::int64_t timeout=5000;
        {
            std::vector<std::set<std::string>> keys;
            const auto doc=Json::parse(request.data,request.data+request.size,[&](int depth,Json::parse_event_t event,Json& parsed){
                if(depth>64)throw std::invalid_argument("Session request nesting exceeds limit");
                if(event==Json::parse_event_t::object_start)keys.emplace_back();
                else if(event==Json::parse_event_t::object_end)keys.pop_back();
                else if(event==Json::parse_event_t::key && !keys.back().insert(parsed.get<std::string>()).second)
                    throw std::invalid_argument("Duplicate session request field");
                return true;
            });
            if(!doc.is_object() || !doc.at("schema_version").is_number_integer() || doc.at("schema_version")!=1)
                return error(destination,CPN_INVALID_ARGUMENT,"Unsupported session request version");
            session_fields(doc,{"schema_version","paths","session","expected_disk_digest","lock_timeout_ms"});
            const auto& paths=doc.at("paths");
            session_fields(paths,{"recovery_file","backup_file","lock_file"});
            locations={cpn::path_from_utf8(paths.at("recovery_file").get<std::string>()),
                cpn::path_from_utf8(paths.at("backup_file").get<std::string>()),cpn::path_from_utf8(paths.at("lock_file").get<std::string>())};
            session=cpn::parse_workspace_session(doc.at("session").dump());
            if(doc.contains("expected_disk_digest") && !doc.at("expected_disk_digest").is_null())digest=doc.at("expected_disk_digest").get<std::string>();
            if(doc.contains("lock_timeout_ms")){
                const auto& value=doc.at("lock_timeout_ms");
                if(!value.is_number_integer() || value<0 || value>60000)return error(destination,CPN_INVALID_ARGUMENT,"Invalid session lock timeout");
                timeout=value.get<std::int64_t>();
            }
        }
        ReservedBuffer reserved;CompanionWriteResponse response;
        const auto allocated=allocate_buffer(handle,*engine,std::string(companion_response_bytes,'\0'),&reserved.value,destination);
        if(allocated!=CPN_OK)return allocated;
        const auto saved=cpn::SessionService().save(locations,session,digest,std::chrono::milliseconds(timeout),engine->operations.get_token());
        reserved.publish(response.render(saved),output);
        return error(destination,CPN_OK,{});
    });
}
cpn_status cpn_engine_companion_listener_start_json(cpn_engine_handle handle,cpn_bytes request,cpn_buffer* output,cpn_error* destination){
    if(output)*output={};
    return boundary(destination,[&]{
        auto engine=lookup(handle);if(!engine)return error(destination,CPN_INVALID_HANDLE,"Unknown engine");
        if(!output)return error(destination,CPN_INVALID_ARGUMENT,"Buffer output is required");
        if(engine->operations.stop_requested())return error(destination,CPN_CLOSED,"Engine is closed");
        ListenerConfiguration configuration;
        {
            const auto doc=companion_request(request,*engine,{companion_field::schema_version,companion_field::workspace_id,
                companion_field::paths,companion_field::bounds,listener_field::options});
            configuration=listener_configuration(doc);
        }
        // JSON destruction can allocate an iterative cleanup stack. Finish it before
        // reserving the response or changing the active listener.
        // The service constructor validates all supported option combinations before binding.
        auto proposed=std::make_unique<cpn::CompanionListener>(configuration.options,configuration.workspace_id,
            configuration.paths,cpn::native_file_store(),configuration.bounds);
        std::lock_guard lock(engine->listener_mutex);
        if(engine->operations.stop_requested())return error(destination,CPN_CLOSED,"Engine is closed");
        const bool same=engine->listener_configuration && same_listener_configuration(*engine->listener_configuration,configuration);
        if(engine->listener && engine->listener->statistics().running && !same)
            return error(destination,CPN_BUSY,"A running Companion listener owns a different configuration");
        ReservedBuffer reserved;CompanionWriteResponse response;
        const auto allocated=allocate_buffer(handle,*engine,std::string(companion_response_bytes,'\0'),&reserved.value,destination);
        if(allocated!=CPN_OK)return allocated;
        if(!engine->listener || engine->listener->statistics().closed || !same){
            // All potentially allocating configuration/instance work precedes any stop/bind.
            if(engine->listener)engine->listener->stop();
            engine->listener_configuration=std::move(configuration);
            engine->listener=std::move(proposed);
        }
        engine->listener_start_result=engine->listener->start();
        if(engine->operations.stop_requested()){
            engine->listener->stop();return error(destination,CPN_CLOSED,"Engine closed during listener startup");
        }
        reserved.publish(response.render_listener(cpn::companion_listener_start_status_name(engine->listener_start_result->status),
            *engine,engine->listener->statistics(),{}),output);
        return error(destination,CPN_OK,{});
    });
}
cpn_status cpn_engine_companion_listener_stop_json(cpn_engine_handle handle,cpn_bytes request,cpn_buffer* output,cpn_error* destination){
    if(output)*output={};
    return boundary(destination,[&]{
        auto engine=lookup(handle);if(!engine)return error(destination,CPN_INVALID_HANDLE,"Unknown engine");
        if(!output)return error(destination,CPN_INVALID_ARGUMENT,"Buffer output is required");
        listener_request_schema(companion_request(request,*engine,{companion_field::schema_version}));
        std::lock_guard lock(engine->listener_mutex);
        // Copy diagnostics and reserve both output allocations before stop mutates/join workers.
        const auto failure=engine->listener?engine->listener->failure_detail():std::string{};
        ReservedBuffer reserved;CompanionWriteResponse response;
        const auto allocated=allocate_buffer(handle,*engine,std::string(companion_response_bytes,'\0'),&reserved.value,destination);
        if(allocated!=CPN_OK)return allocated;
        if(engine->listener)engine->listener->stop();
        reserved.publish(response.render_listener(engine->listener?"Stopped":"NotStarted",*engine,
            engine->listener?engine->listener->statistics():cpn::CompanionListenerStatistics{},failure),output);
        return error(destination,CPN_OK,{});
    });
}
cpn_status cpn_engine_companion_listener_info_json(cpn_engine_handle handle,cpn_bytes request,cpn_buffer* output,cpn_error* destination){
    if(output)*output={};
    return boundary(destination,[&]{
        auto engine=lookup(handle);if(!engine)return error(destination,CPN_INVALID_HANDLE,"Unknown engine");
        if(!output)return error(destination,CPN_INVALID_ARGUMENT,"Buffer output is required");
        listener_request_schema(companion_request(request,*engine,{companion_field::schema_version}));
        std::lock_guard lock(engine->listener_mutex);
        const auto failure=engine->listener?engine->listener->failure_detail():std::string{};
        ReservedBuffer reserved;CompanionWriteResponse response;
        const auto allocated=allocate_buffer(handle,*engine,std::string(companion_response_bytes,'\0'),&reserved.value,destination);
        if(allocated!=CPN_OK)return allocated;
        reserved.publish(response.render_listener("ListenerInfo",*engine,
            engine->listener?engine->listener->statistics():cpn::CompanionListenerStatistics{},failure),output);
        return error(destination,CPN_OK,{});
    });
}
cpn_status cpn_engine_companion_page_json(cpn_engine_handle handle,cpn_bytes request,cpn_buffer* output,cpn_error* destination){
    if(output)*output={};
    return boundary(destination,[&]{
        using namespace companion_field;
        auto engine=lookup(handle);if(!engine)return error(destination,CPN_INVALID_HANDLE,"Unknown engine");
        if(engine->operations.stop_requested())return error(destination,CPN_CLOSED,"Engine is closed");
        if(!output)return error(destination,CPN_INVALID_ARGUMENT,"Companion buffer output is required");
        const auto doc=companion_request(request,*engine,{schema_version,workspace_id,paths,bounds,after_sequence,max_items,max_page_bytes});
        const auto context=companion_common(doc);
        const auto after=companion_integer(doc.at(after_sequence),0,std::numeric_limits<std::uint64_t>::max());
        const auto count=companion_integer(doc.at(max_items),1,cpn::CompanionInboxBounds{}.max_records);
        const auto byte_limit=static_cast<std::size_t>(companion_integer(doc.at(max_page_bytes),1,individual_buffer_limit));
        const auto loaded=cpn::CompanionInboxService(context.workspace_id,cpn::native_file_store(),context.bounds).load(context.paths);
        if(engine->operations.stop_requested())return error(destination,CPN_CLOSED,"Engine is closed");
        Json result{{schema_version,companion_request_schema_version},{status,cpn::companion_inbox_load_status_name(loaded.status)},
            {detail,loaded.detail},{workspace_id,context.workspace_id},{inbox_id,nullptr},{revision,nullptr},
            {disk_digest,loaded.disk_digest?Json(*loaded.disk_digest):Json(nullptr)},{next_after_sequence,after},{has_more,false},{items,Json::array()}};
        std::vector<std::string> selected;
        std::size_t selected_bytes=0;
        if(loaded.snapshot){
            const auto& snapshot=*loaded.snapshot;
            result[inbox_id]=snapshot.inbox_id;result[revision]=snapshot.revision;
            for(std::size_t index=0;index<snapshot.items.size();++index){
                const auto& item=snapshot.items[index];
                if(item.sequence<=after)continue;
                if(selected.size()==count){result[has_more]=true;break;}
                auto serialized=cpn::serialize_companion_inbox_item(item,snapshot.inbox_id,context.bounds);
                const auto previous_cursor=result[next_after_sequence];
                result[next_after_sequence]=item.sequence;result[has_more]=index+1<snapshot.items.size();
                const auto envelope_bytes=result.dump(-1,' ',false,Json::error_handler_t::replace).size();
                const auto separator=selected.empty()?0u:1u;
                if(envelope_bytes>byte_limit || selected_bytes>byte_limit-envelope_bytes ||
                    separator>byte_limit-envelope_bytes-selected_bytes || serialized.size()>byte_limit-envelope_bytes-selected_bytes-separator){
                    result[next_after_sequence]=previous_cursor;result[has_more]=true;
                    if(selected.empty())return error(destination,CPN_CAPACITY,"Companion item exceeds page byte limit; no item was consumed");
                    break;
                }
                selected_bytes+=serialized.size()+separator;selected.push_back(std::move(serialized));
            }
        }
        auto serialized=result.dump(-1,' ',false,Json::error_handler_t::replace);
        if(serialized.size()>byte_limit || selected_bytes>byte_limit-serialized.size())
            return error(destination,CPN_CAPACITY,"Companion page envelope exceeds byte limit; no item was consumed");
        if(!selected.empty()){
            // Canonical items already encode original_json as a string; do not parse their payloads again.
            constexpr std::string_view marker="\"items\":[]";
            const auto location=serialized.find(marker);
            if(location==std::string::npos)throw std::logic_error("Companion page envelope has no item array");
            std::string body;body.reserve(selected_bytes);
            for(const auto& item:selected){if(!body.empty())body.push_back(',');body+=item;}
            serialized.insert(location+marker.size()-1,body);
        }
        return allocate_buffer(handle,*engine,std::move(serialized),output,destination);
    });
}
cpn_status cpn_engine_companion_accept_json(cpn_engine_handle handle,cpn_bytes request,cpn_buffer* output,cpn_error* destination){
    if(output)*output={};
    return boundary(destination,[&]{
        using namespace companion_field;
        auto engine=lookup(handle);if(!engine)return error(destination,CPN_INVALID_HANDLE,"Unknown engine");
        if(engine->operations.stop_requested())return error(destination,CPN_CLOSED,"Engine is closed");
        if(!output)return error(destination,CPN_INVALID_ARGUMENT,"Companion buffer output is required");
        CompanionRequest context;std::string original;std::chrono::milliseconds timeout;
        {
            const auto doc=companion_request(request,*engine,{schema_version,workspace_id,paths,bounds,original_json,lock_timeout_ms});
            context=companion_common(doc);
            original=companion_text(doc.at(original_json),context.bounds.payload.max_json_bytes);
            timeout=companion_timeout(doc);
        }
        ReservedBuffer reserved;CompanionWriteResponse response;
        const auto allocated=allocate_buffer(handle,*engine,std::string(companion_response_bytes,'\0'),&reserved.value,destination);
        if(allocated!=CPN_OK)return allocated;
        const auto accepted=cpn::CompanionInboxService(context.workspace_id,cpn::native_file_store(),context.bounds)
            .accept(context.paths,original,timeout,engine->operations.get_token());
        reserved.publish(response.render(accepted),output);
        return error(destination,CPN_OK,{});
    });
}
cpn_status cpn_engine_companion_prepare_json(cpn_engine_handle handle,cpn_bytes request,cpn_buffer* output,cpn_error* destination){
    if(output)*output={};
    return boundary(destination,[&]{
        using namespace companion_field;
        auto engine=lookup(handle);if(!engine)return error(destination,CPN_INVALID_HANDLE,"Unknown engine");
        if(engine->operations.stop_requested())return error(destination,CPN_CLOSED,"Engine is closed");
        if(!output)return error(destination,CPN_INVALID_ARGUMENT,"Companion buffer output is required");
        const auto doc=companion_request(request,*engine,{schema_version,workspace_id,paths,bounds,import_id,session,defaults,max_receipts});
        const auto context=companion_common(doc);
        const auto identity=companion_text(doc.at(import_id),context.bounds.max_id_bytes,true);
        const auto limits=companion_application_bounds(doc,context);
        const auto initial=cpn::parse_workspace_session(doc.at(session).dump(-1,' ',false,Json::error_handler_t::strict),limits.session);
        const auto initial_defaults=companion_defaults(doc,limits.session);
        const auto loaded=cpn::CompanionInboxService(context.workspace_id,cpn::native_file_store(),context.bounds).load(context.paths);
        if(engine->operations.stop_requested())return error(destination,CPN_CLOSED,"Engine is closed");
        Json result{{schema_version,companion_request_schema_version},{inbox_status,cpn::companion_inbox_load_status_name(loaded.status)},
            {status,cpn::companion_inbox_load_status_name(loaded.status)},{detail,loaded.detail},
            {disk_digest,loaded.disk_digest?Json(*loaded.disk_digest):Json(nullptr)},{session,nullptr},{receipt,nullptr}};
        if(loaded.snapshot){
            const auto prepared=cpn::prepare_import_application(initial,*loaded.snapshot,identity,initial_defaults,limits);
            result[status]=cpn::import_preparation_status_name(prepared.status);result[detail]=prepared.detail;
            if(prepared.session)result[session]=Json::parse(cpn::serialize_workspace_session(*prepared.session,limits.session));
            if(prepared.receipt)result[receipt]=companion_receipt(*prepared.receipt);
        }
        if(engine->operations.stop_requested())return error(destination,CPN_CLOSED,"Engine is closed");
        return allocate_buffer(handle,*engine,result.dump(-1,' ',false,Json::error_handler_t::replace),output,destination);
    });
}
cpn_status cpn_engine_companion_mark_applied_json(cpn_engine_handle handle,cpn_bytes request,cpn_buffer* output,cpn_error* destination){
    if(output)*output={};
    return boundary(destination,[&]{
        using namespace companion_field;
        auto engine=lookup(handle);if(!engine)return error(destination,CPN_INVALID_HANDLE,"Unknown engine");
        if(engine->operations.stop_requested())return error(destination,CPN_CLOSED,"Engine is closed");
        if(!output)return error(destination,CPN_INVALID_ARGUMENT,"Companion buffer output is required");
        CompanionRequest context;std::string identity;std::chrono::milliseconds timeout;
        cpn::ImportApplicationBounds limits;cpn::SessionPaths locations;
        {
            const auto doc=companion_request(request,*engine,{schema_version,workspace_id,paths,bounds,import_id,session_paths,lock_timeout_ms,max_receipts});
            context=companion_common(doc);
            identity=companion_text(doc.at(import_id),context.bounds.max_id_bytes,true);
            timeout=companion_timeout(doc);limits=companion_application_bounds(doc,context);
            const auto& location=doc.at(session_paths);
            session_fields(location,{recovery_file,backup_file,lock_file});
            const auto path_limit=std::min(context.bounds.max_path_bytes,limits.session.max_path_bytes);
            locations={companion_path(location.at(recovery_file),path_limit),
                companion_path(location.at(backup_file),path_limit),companion_path(location.at(lock_file),path_limit)};
        }
        ReservedBuffer reserved;CompanionWriteResponse response;
        const auto allocated=allocate_buffer(handle,*engine,std::string(companion_response_bytes,'\0'),&reserved.value,destination);
        if(allocated!=CPN_OK)return allocated;
        const auto applied=cpn::CompanionInboxService(context.workspace_id,cpn::native_file_store(),context.bounds)
            .mark_applied(context.paths,locations,identity,timeout,engine->operations.get_token(),limits);
        reserved.publish(response.render(applied),output);
        return error(destination,CPN_OK,{});
    });
}
cpn_status cpn_buffer_release(cpn_buffer_handle handle,cpn_error* destination){
    return boundary(destination,[&]{std::lock_guard guard(handles_mutex);auto found=buffers.find(handle);
        if(found==buffers.end())return error(destination,CPN_INVALID_HANDLE,"Unknown or released buffer");
        buffer_bytes-=found->second->bytes.size();buffers.erase(found);return error(destination,CPN_OK,{});});
}
}
