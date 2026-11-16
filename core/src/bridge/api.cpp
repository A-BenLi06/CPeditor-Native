// SPDX-License-Identifier: GPL-3.0-or-later
#include "cpn/api.h"
#include "cpn/jobs.hpp"
#include "cpn/json.hpp"
#include "cpn/session.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
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
struct Engine {
    Engine(cpn_engine_options value):options(value),jobs({value.max_workers,value.max_jobs}){}
    cpn_engine_options options;
    cpn::JobRegistry jobs;
    std::stop_source operations;
    std::mutex drain_mutex;
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
        if(bytes.size()>storage->bytes.size())throw std::length_error("Session response exceeds reserved buffer");
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
        engine->operations.request_stop();engine->jobs.close();return error(destination,CPN_OK,{});});
}
cpn_status cpn_engine_destroy(cpn_engine_handle handle,cpn_error* destination){
    return boundary(destination,[&]{std::shared_ptr<Engine> engine;
        {std::lock_guard guard(handles_mutex);auto found=engines.find(handle);if(found==engines.end())return error(destination,CPN_INVALID_HANDLE,"Unknown engine");
         engine=std::move(found->second);engines.erase(found);}
        engine->operations.request_stop();engine->jobs.close();return error(destination,CPN_OK,{});});
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
        cpn::SessionPaths locations{cpn::path_from_utf8(paths.at("recovery_file").get<std::string>()),
            cpn::path_from_utf8(paths.at("backup_file").get<std::string>()),cpn::path_from_utf8(paths.at("lock_file").get<std::string>())};
        auto session=cpn::parse_workspace_session(doc.at("session").dump());
        std::optional<std::string> digest;
        if(doc.contains("expected_disk_digest") && !doc.at("expected_disk_digest").is_null())digest=doc.at("expected_disk_digest").get<std::string>();
        std::int64_t timeout=5000;
        if(doc.contains("lock_timeout_ms")){
            const auto& value=doc.at("lock_timeout_ms");
            if(!value.is_number_integer() || value<0 || value>60000)return error(destination,CPN_INVALID_ARGUMENT,"Invalid session lock timeout");
            timeout=value.get<std::int64_t>();
        }
        ReservedBuffer reserved;
        const auto allocated=allocate_buffer(handle,*engine,std::string(65536,'\0'),&reserved.value,destination);
        if(allocated!=CPN_OK)return allocated;
        const auto saved=cpn::SessionService().save(locations,session,digest,std::chrono::milliseconds(timeout),engine->operations.get_token());
        auto result=Json{{"schema_version",1},{"status",cpn::session_save_status_name(saved.status)},{"detail",saved.detail.substr(0,4096)},
            {"recovery_replaced",saved.recovery_replaced},{"backup_replaced",saved.backup_replaced}};
        result["disk_digest"]=saved.disk_digest?Json(*saved.disk_digest):Json(nullptr);
        reserved.publish(result.dump(-1,' ',false,Json::error_handler_t::replace),output);
        return error(destination,CPN_OK,{});
    });
}
cpn_status cpn_buffer_release(cpn_buffer_handle handle,cpn_error* destination){
    return boundary(destination,[&]{std::lock_guard guard(handles_mutex);auto found=buffers.find(handle);
        if(found==buffers.end())return error(destination,CPN_INVALID_HANDLE,"Unknown or released buffer");
        buffer_bytes-=found->second->bytes.size();buffers.erase(found);return error(destination,CPN_OK,{});});
}
}
