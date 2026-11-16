// SPDX-License-Identifier: GPL-3.0-or-later
#include "cpn/api.h"
#include "cpn/companion_inbox.hpp"
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>
#include <array>
#include <barrier>
#include <cstdlib>
#include <future>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>

namespace allocation_probe {
thread_local const cpn_buffer* published_output{};
}
// Detect late request-DOM cleanup on the actual ABI calling thread. Worker-thread
// allocations are unaffected, and parsing remains unrestricted before publication.
void* operator new(std::size_t size) {
    if(allocation_probe::published_output && allocation_probe::published_output->handle)
        throw std::bad_alloc();
    if(void* memory=std::malloc(size?size:1))return memory;
    throw std::bad_alloc();
}
void operator delete(void* memory) noexcept {std::free(memory);}
void operator delete(void* memory,std::size_t) noexcept {std::free(memory);}

namespace {
namespace net=boost::asio;
namespace beast=boost::beast;
namespace http=beast::http;
using tcp=net::ip::tcp;
using Error=boost::system::error_code;
using Json=nlohmann::json;
using Api=cpn_status(*)(cpn_engine_handle,cpn_bytes,cpn_buffer*,cpn_error*);
using namespace std::chrono_literals;
void expect(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
cpn_bytes view(const std::string& value){return {reinterpret_cast<const std::uint8_t*>(value.data()),value.size()};}
template<class F> void eventually(F condition,const char* message){
    const auto deadline=std::chrono::steady_clock::now()+3s;
    while(!condition()){
        if(std::chrono::steady_clock::now()>=deadline)throw std::runtime_error(message);
        std::this_thread::yield();
    }
}
struct Workspace {
    std::shared_ptr<cpn::FileStore> files=cpn::native_file_store();
    std::filesystem::path path=files->create_workspace();
    ~Workspace(){files->remove_workspace(path);}
    cpn::CompanionInboxPaths paths() const{
        return {path/cpn::path_from_utf8("ABI 监听收件箱.json"),path/"inbox-backup.json",path/"inbox.lock"};
    }
};
struct Engine {
    cpn_engine_handle handle{};
    explicit Engine(std::uint64_t request_bytes=64*1024*1024){
        cpn_engine_options options{sizeof(options),CPN_ABI_VERSION,1,1,1,0,request_bytes,1024*1024};
        expect(cpn_engine_create(&options,&handle,nullptr)==CPN_OK,"create single-response ABI engine");
    }
    ~Engine(){if(handle)cpn_engine_destroy(handle,nullptr);}
};
struct Buffer {
    cpn_buffer value{};
    ~Buffer(){release();}
    void release(){if(value.handle)expect(cpn_buffer_release(value.handle,nullptr)==CPN_OK,"release ABI response");value={};}
    Json json() const{return Json::parse(value.data,value.data+value.size);}
    std::string bytes() const{return {reinterpret_cast<const char*>(value.data),value.size};}
};
Json call(Api api,cpn_engine_handle engine,const std::string& request,cpn_status expected=CPN_OK){
    Buffer output;cpn_error failure{};failure.struct_size=sizeof(failure);
    cpn_status status;
    {
        struct PublicationGuard {
            ~PublicationGuard(){allocation_probe::published_output=nullptr;}
        } guard;
        if(api==cpn_engine_companion_listener_start_json || api==cpn_engine_companion_listener_stop_json)
            allocation_probe::published_output=&output.value;
        status=api(engine,view(request),&output.value,&failure);
    }
    if(status!=expected)throw std::runtime_error("Unexpected listener ABI status "+std::to_string(status)+": "+failure.message);
    expect(failure.status==static_cast<std::uint32_t>(status),"caller-owned error status matches return value");
    if(status!=CPN_OK){expect(!output.value.handle && !output.value.data && !output.value.size,"failed listener ABI clears output");return nullptr;}
    expect(output.value.handle && output.value.data && output.value.size<65536,"successful listener ABI has finite owned response");
    return output.json();
}
Json call(Api api,cpn_engine_handle engine,const Json& request,cpn_status expected=CPN_OK){return call(api,engine,request.dump(),expected);}
const Json lifecycle_request{{"schema_version",1}};
Json start_request(const Workspace& workspace,std::uint16_t port=0){
    const auto paths=workspace.paths();
    return Json{{"schema_version",1},{"workspace_id","listener-abi-workspace"},{"paths",{
        {"inbox_file",cpn::path_to_utf8(paths.inbox_file)},{"backup_file",cpn::path_to_utf8(paths.backup_file)},
        {"lock_file",cpn::path_to_utf8(paths.lock_file)}}},{"options",{{"port",port},{"enable_ipv6",false}}}};
}
Json page_request(const Workspace& workspace){
    auto result=start_request(workspace);result.erase("options");
    result["after_sequence"]=0;result["max_items"]=4;result["max_page_bytes"]=1024*1024;return result;
}
std::string payload(){
    auto bytes=Json{{"name","题目 😀 e\u0301"},{"group","Group"},{"url","https://listener-abi.test/task"},
        {"timeLimit",1000},{"memoryLimit",256},{"tests",{{{"input",std::string("a\0b\r\n",5)},{"output",""}}}}}.dump();
    bytes.pop_back();bytes+=R"(, "unknown": {"large":18446744073709551616,"float":1.2300e+02} })";return bytes;
}
class Client {
public:
    explicit Client(std::uint16_t port):stream(io){
        stream.expires_after(3s);Error result;
        stream.async_connect(tcp::endpoint{net::ip::address_v4::loopback(),port},[&](Error error){result=error;});
        io.run();if(result)throw boost::system::system_error(result);
    }
    void send(std::string_view bytes){
        io.restart();stream.expires_after(3s);Error result;
        net::async_write(stream,net::buffer(bytes.data(),bytes.size()),[&](Error error,std::size_t){result=error;});
        io.run();if(result)throw boost::system::system_error(result);
    }
    http::response<http::string_body> response(){
        io.restart();stream.expires_after(3s);Error result;http::response_parser<http::string_body> parser;parser.body_limit(65536);
        http::async_read(stream,buffer,parser,[&](Error error,std::size_t){result=error;});
        io.run();if(result)throw boost::system::system_error(result);return parser.release();
    }
    bool disconnected(){
        io.restart();stream.expires_after(3s);Error result;std::array<char,1024> bytes{};
        stream.async_read_some(net::buffer(bytes),[&](Error error,std::size_t){result=error;});io.run();
        return result==net::error::eof || result==net::error::connection_reset;
    }
    net::io_context io;beast::tcp_stream stream;beast::flat_buffer buffer;
};
std::string headers(std::uint16_t port,std::size_t length){
    return "POST / HTTP/1.1\r\nHost: localhost:"+std::to_string(port)+
        "\r\nContent-Type: application/json\r\nContent-Length: "+std::to_string(length)+"\r\n\r\n";
}
http::response<http::string_body> post(std::uint16_t port,std::string_view body){
    Client client(port);client.send(headers(port,body.size())+std::string(body));return client.response();
}
class BoundPort {
public:
    explicit BoundPort(std::uint16_t port=0):acceptor(io){
        acceptor.open(tcp::v4());acceptor.bind({net::ip::address_v4::loopback(),port});acceptor.listen();
    }
    std::uint16_t port() const{return acceptor.local_endpoint().port();}
    void close(){Error ignored;acceptor.close(ignored);}
    net::io_context io;tcp::acceptor acceptor;
};
void assert_port_released(std::uint16_t port){BoundPort bound(port);expect(bound.port()==port,"closed ABI listener releases actual loopback port");}
void assert_not_listening(std::uint16_t port){
    // A completed HTTP connection may leave TIME_WAIT despite a fully closed listener.
    // A refused TCP connection directly verifies absence of the listening endpoint.
    try{Client connected(port);}catch(const boost::system::system_error& error){
        expect(error.code()==net::error::connection_refused,"closed endpoint refuses connection rather than timing out");return;
    }
    throw std::runtime_error("closed ABI listener still accepts loopback connections");
}
void assert_stopped(const Json& result){
    const auto& stats=result.at("statistics");
    expect(!stats.at("running").get<bool>() && stats.at("closed").get<bool>() && stats.at("active_connections")==0 &&
        stats.at("outstanding_admissions")==0 && stats.at("inflight_body_bytes")==0,"stop joins threads and releases all listener reservations");
}
void strict_and_quota(){
    Workspace workspace;Engine engine;BoundPort selected;const auto port=selected.port();selected.close();
    const auto valid=start_request(workspace,port);Buffer held;
    expect(cpn_engine_info_json(engine.handle,&held.value,nullptr)==CPN_OK,"hold all ABI response quota");
    for(const auto api:{cpn_engine_companion_listener_start_json,cpn_engine_companion_listener_stop_json,cpn_engine_companion_listener_info_json}){
        const auto request=api==cpn_engine_companion_listener_start_json?valid:lifecycle_request;
        auto malformed=request;malformed["schema_version"]=1.0;call(api,engine.handle,malformed,CPN_INVALID_ARGUMENT);
        malformed=request;malformed["schema_version"]=2;call(api,engine.handle,malformed,CPN_INVALID_ARGUMENT);
        malformed=request;malformed["schema_version"]=-1;call(api,engine.handle,malformed,CPN_INVALID_ARGUMENT);
        malformed=request;malformed.erase("schema_version");call(api,engine.handle,malformed,CPN_INVALID_ARGUMENT);
        malformed=request;malformed["unknown"]=1;call(api,engine.handle,malformed,CPN_INVALID_ARGUMENT);
        auto duplicate=request.dump();duplicate.insert(1,"\"schema_version\":1,");call(api,engine.handle,duplicate,CPN_INVALID_ARGUMENT);
        Buffer out;expect(api(engine.handle,{nullptr,0},&out.value,nullptr)==CPN_INVALID_ARGUMENT,"empty request rejected");
        expect(api(engine.handle,view(request.dump()),nullptr,nullptr)==CPN_INVALID_ARGUMENT,"missing output rejected");
        call(api,0,request,CPN_INVALID_HANDLE);
    }
    for(const auto& [name,value]:std::array<std::pair<const char*,Json>,20>{{
        {"port",65536},{"port",-1},{"port",1.0},{"enable_ipv6",1},{"max_connections",129},
        {"max_admissions",65},{"max_body_bytes",0},{"max_inflight_body_bytes",134217729},
        {"max_header_bytes",65537},{"max_header_count",257},{"max_response_bytes",32767},
        {"socket_send_buffer_bytes",1023},{"read_timeout_ms",0},{"read_timeout_ms",60001},
        {"write_timeout_ms",60001},{"request_timeout_ms",300001},{"lock_timeout_ms",60001},
        {"max_connections",std::numeric_limits<std::uint64_t>::max()},{"unknown",1},{"lock_timeout_ms",1.0}}}){
        auto malformed=valid;malformed["options"][name]=value;call(cpn_engine_companion_listener_start_json,engine.handle,malformed,CPN_INVALID_ARGUMENT);
    }
    auto malformed=valid;malformed["options"]=Json::array();call(cpn_engine_companion_listener_start_json,engine.handle,malformed,CPN_INVALID_ARGUMENT);
    malformed=valid;malformed["paths"]["inbox_file"]="relative.json";call(cpn_engine_companion_listener_start_json,engine.handle,malformed,CPN_INVALID_ARGUMENT);
    malformed=valid;malformed["paths"]["unknown"]=1;call(cpn_engine_companion_listener_start_json,engine.handle,malformed,CPN_INVALID_ARGUMENT);
    malformed=valid;malformed["workspace_id"]=std::string("a\0b",3);call(cpn_engine_companion_listener_start_json,engine.handle,malformed,CPN_INVALID_ARGUMENT);
    malformed=valid;malformed["bounds"]={{"unknown",1}};call(cpn_engine_companion_listener_start_json,engine.handle,malformed,CPN_INVALID_ARGUMENT);
    malformed=valid;malformed["bounds"]={{"payload",{{"max_json_bytes",128}}}};malformed["options"]["max_body_bytes"]=129;
    call(cpn_engine_companion_listener_start_json,engine.handle,malformed,CPN_INVALID_ARGUMENT);
    auto overflow=valid.dump();const auto pos=overflow.find("\"port\":");const auto end=overflow.find_first_of(",}",pos+7);
    overflow.replace(pos+7,end-(pos+7),"18446744073709551616");call(cpn_engine_companion_listener_start_json,engine.handle,overflow,CPN_INVALID_ARGUMENT);
    call(cpn_engine_companion_listener_start_json,engine.handle,valid,CPN_CAPACITY);assert_port_released(port);
    expect(!std::filesystem::exists(workspace.paths().lock_file) && !std::filesystem::exists(workspace.paths().inbox_file),"validation and quota failures have no storage effects");
    held.release();const auto info=call(cpn_engine_companion_listener_info_json,engine.handle,lifecycle_request);
    expect(!info["has_listener"].get<bool>() && info["start_status"].is_null(),"failed preflight never installs listener");
    Engine small(32);call(cpn_engine_companion_listener_start_json,small.handle,valid,CPN_INVALID_ARGUMENT);
    std::cout<<"strict requests and preflight quota tests passed\n";
}
void lifecycle_and_network(){
    Workspace workspace;Engine engine;auto configuration=start_request(workspace);
    const auto idle=call(cpn_engine_companion_listener_stop_json,engine.handle,lifecycle_request);
    expect(idle["status"]=="NotStarted" && !idle["engine_closed"].get<bool>(),"stop before start is explicit");
    auto started=call(cpn_engine_companion_listener_start_json,engine.handle,configuration);
    expect(started["status"]=="Started" && started["statistics"]["running"].get<bool>() && started["port"]!=0,"ABI start binds actual loopback endpoint");
    auto port=started["port"].get<std::uint16_t>();const auto body=payload();auto response=post(port,body);
    const auto receipt=Json::parse(response.body());
    expect(response.result()==http::status::accepted && receipt["status"]=="Accepted","real HTTP accepts through engine-owned listener");
    const auto page=call(cpn_engine_companion_page_json,engine.handle,page_request(workspace));
    expect(page["items"].size()==1 && page["items"][0]["original_json"]==body,"202 corresponds to exact durable raw payload via C ABI page");
    const auto repeated=call(cpn_engine_companion_listener_start_json,engine.handle,configuration);
    expect(repeated["status"]=="AlreadyStarted" && repeated["port"]==port,"same active configuration is idempotent");
    auto explicit_defaults=configuration;
    explicit_defaults["options"].update({{"max_connections",16},{"max_admissions",4},{"max_body_bytes",16*1024*1024},
        {"max_inflight_body_bytes",32*1024*1024},{"max_header_bytes",16384},{"max_header_count",64},
        {"max_response_bytes",65536},{"socket_send_buffer_bytes",8192},{"read_timeout_ms",5000},
        {"write_timeout_ms",5000},{"request_timeout_ms",15000},{"lock_timeout_ms",5000}});
    expect(call(cpn_engine_companion_listener_start_json,engine.handle,explicit_defaults)["status"]=="AlreadyStarted",
        "explicit default option values compare equal to omitted defaults");
    for(const auto& field:{"workspace","paths","options","bounds","payload"}){
        auto changed=configuration;
        if(std::string_view(field)=="workspace")changed["workspace_id"]="other-workspace";
        else if(std::string_view(field)=="paths")changed["paths"]["backup_file"]=cpn::path_to_utf8(workspace.path/"other-backup.json");
        else if(std::string_view(field)=="options")changed["options"]["max_connections"]=8;
        else if(std::string_view(field)=="bounds")changed["bounds"]={{"max_records",2}};
        else changed["bounds"]={{"payload",{{"max_tests",2}}}};
        call(cpn_engine_companion_listener_start_json,engine.handle,changed,CPN_BUSY);
    }
    Buffer held;expect(cpn_engine_info_json(engine.handle,&held.value,nullptr)==CPN_OK,"hold quota with running listener");
    call(cpn_engine_companion_listener_stop_json,engine.handle,lifecycle_request,CPN_CAPACITY);
    expect(post(port,body).result()==http::status::accepted,"quota refusal does not stop listener");held.release();
    const auto info=call(cpn_engine_companion_listener_info_json,engine.handle,lifecycle_request);
    expect(info["statistics"]["accepted"]==1 && info["statistics"]["duplicates"]==1,"engine info reports actual HTTP admission counters");
    assert_stopped(call(cpn_engine_companion_listener_stop_json,engine.handle,lifecycle_request));assert_not_listening(port);
    assert_stopped(call(cpn_engine_companion_listener_stop_json,engine.handle,lifecycle_request));
    configuration["options"]["max_connections"]=8;
    configuration["bounds"]={{"payload",{{"max_json_bytes",4096}}}};
    started=call(cpn_engine_companion_listener_start_json,engine.handle,configuration);port=started["port"].get<std::uint16_t>();
    expect(started["status"]=="Started" && post(port,body).result()==http::status::accepted,"stopped engine creates new configuration and reduced omitted body default");
    expect(cpn_engine_close(engine.handle,nullptr)==CPN_OK,"close joins listener");assert_not_listening(port);
    const auto closed=call(cpn_engine_companion_listener_info_json,engine.handle,lifecycle_request);assert_stopped(closed);
    expect(closed["engine_closed"].get<bool>(),"listener info remains readable after engine close");
    call(cpn_engine_companion_listener_start_json,engine.handle,configuration,CPN_CLOSED);
    assert_stopped(call(cpn_engine_companion_listener_stop_json,engine.handle,lifecycle_request));
    std::cout<<"engine lifecycle and real durable HTTP tests passed\n";
}
void bind_failure_and_buffer_lifetime(){
    Workspace workspace;Engine engine;BoundPort blocker;const auto port=blocker.port();const auto configuration=start_request(workspace,port);
    const auto failed=call(cpn_engine_companion_listener_start_json,engine.handle,configuration);
    expect(failed["status"]=="BindFailed" && failed["native_error"]!=0 && !failed["statistics"]["running"].get<bool>(),"port collision is explicit finite start result");
    blocker.close();Buffer surviving;const auto request=configuration.dump();
    expect(cpn_engine_companion_listener_start_json(engine.handle,view(request),&surviving.value,nullptr)==CPN_OK,"retry bind after collision");
    expect(surviving.json()["status"]=="Started" && surviving.json()["port"]==port,"bind failure can retry same listener configuration");
    const auto bytes=surviving.bytes();expect(cpn_engine_destroy(engine.handle,nullptr)==CPN_OK,"destroy active listener with held response");
    engine.handle=0;assert_port_released(port);
    expect(surviving.bytes()==bytes && surviving.json()["status"]=="Started","listener response remains byte-for-byte valid after engine destruction");
    std::cout<<"port conflict retry and detached response lifetime tests passed\n";
}
void close_cancels_inflight(){
    Workspace workspace;Engine engine;const auto started=call(cpn_engine_companion_listener_start_json,engine.handle,start_request(workspace));
    const auto port=started["port"].get<std::uint16_t>();
    auto lock=workspace.files->acquire_lock(workspace.paths().lock_file,1s,{});
    Client admitting(port);const auto body=payload();admitting.send(headers(port,body.size())+body);
    eventually([&]{return call(cpn_engine_companion_listener_info_json,engine.handle,lifecycle_request)["statistics"]["outstanding_admissions"]==1;},"HTTP admission reaches actual blocked storage worker");
    Client reading(port);reading.send(headers(port,body.size())+body.substr(0,1));
    eventually([&]{return call(cpn_engine_companion_listener_info_json,engine.handle,lifecycle_request)["statistics"]["active_connections"]==2;},"both blocked body and storage connections become active");
    Buffer held;expect(cpn_engine_info_json(engine.handle,&held.value,nullptr)==CPN_OK,"exhaust quota before close");
    const auto begin=std::chrono::steady_clock::now();expect(cpn_engine_close(engine.handle,nullptr)==CPN_OK,"close bypasses response quota and cancels network plus storage");
    expect(std::chrono::steady_clock::now()-begin<3s,"close joins cancelled file-lock wait without timeout workaround");
    expect(admitting.disconnected() && reading.disconnected(),"close disconnects incomplete and queued clients");assert_not_listening(port);
    lock.reset();expect(!std::filesystem::exists(workspace.paths().inbox_file),"cancelled admission never persists after close");
    held.release();assert_stopped(call(cpn_engine_companion_listener_info_json,engine.handle,lifecycle_request));
    std::cout<<"close cancels actual in-flight network and storage tests passed\n";
}
struct ConcurrentResult {cpn_status status;Json response;};
ConcurrentResult concurrent_call(Api api,cpn_engine_handle handle,const std::string& request){
    Buffer output;const auto status=api(handle,view(request),&output.value,nullptr);
    if(status==CPN_OK)return {status,output.json()};
    expect(!output.value.handle && !output.value.data && !output.value.size,"concurrent failure clears output");return {status,nullptr};
}
void concurrent_shutdown(){
    for(unsigned iteration=0;iteration<24;++iteration){
        Workspace workspace;Engine engine;BoundPort selected;const auto port=selected.port();selected.close();
        const auto request=start_request(workspace,port).dump(),lifecycle=lifecycle_request.dump();const auto handle=engine.handle;
        // Start with an existing closed instance to exercise replacement as well as initial lookup.
        call(cpn_engine_companion_listener_start_json,handle,request);call(cpn_engine_companion_listener_stop_json,handle,lifecycle);
        std::barrier ready(4);
        auto start=std::async(std::launch::async,[&]{ready.arrive_and_wait();return concurrent_call(cpn_engine_companion_listener_start_json,handle,request);});
        auto stop=std::async(std::launch::async,[&]{ready.arrive_and_wait();return concurrent_call(cpn_engine_companion_listener_stop_json,handle,lifecycle);});
        const bool destroy=(iteration%2)==1;
        auto closing=std::async(std::launch::async,[&]{ready.arrive_and_wait();return destroy?cpn_engine_destroy(handle,nullptr):cpn_engine_close(handle,nullptr);});
        ready.arrive_and_wait();const auto started=start.get(),stopped=stop.get();expect(closing.get()==CPN_OK,"concurrent close/destroy completes");
        expect(started.status==CPN_OK || started.status==CPN_CLOSED || (destroy && started.status==CPN_INVALID_HANDLE) || started.status==CPN_CAPACITY,"start race has documented finite outcome");
        expect(stopped.status==CPN_OK || stopped.status==CPN_CAPACITY || (destroy && stopped.status==CPN_INVALID_HANDLE),"stop race has documented finite outcome");
        assert_port_released(port);
        if(destroy){engine.handle=0;call(cpn_engine_companion_listener_info_json,handle,lifecycle,CPN_INVALID_HANDLE);}
        else {
            const auto info=call(cpn_engine_companion_listener_info_json,handle,lifecycle);assert_stopped(info);
            expect(info["engine_closed"].get<bool>(),"closed concurrent engine reports joined listener");
            call(cpn_engine_companion_listener_start_json,handle,request,CPN_CLOSED);
        }
    }
    std::cout<<"24 concurrent start stop close and destroy races passed\n";
}
}
int main(){
    try{strict_and_quota();lifecycle_and_network();bind_failure_and_buffer_lifetime();close_cancels_inflight();concurrent_shutdown();return 0;}
    catch(const std::exception& error){std::cerr<<"Companion listener ABI test failure: "<<error.what()<<'\n';return 1;}
}
