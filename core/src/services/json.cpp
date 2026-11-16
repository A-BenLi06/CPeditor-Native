// SPDX-License-Identifier: GPL-3.0-or-later
#include "cpn/json.hpp"
#include "cpn/text.hpp"
#include <nlohmann/json.hpp>
#include <limits>
#include <set>
#include <stdexcept>
namespace cpn {
namespace {
using Json=nlohmann::json;
std::uint64_t integer(const Json& value,std::uint64_t maximum,const char* name){
    if(!value.is_number_integer() || (value.is_number_integer() && !value.is_number_unsigned() && value.get<std::int64_t>()<0))
        throw std::invalid_argument(std::string(name)+" must be a nonnegative integer");
    const auto result=value.get<std::uint64_t>();
    if(result>maximum)throw std::invalid_argument(std::string(name)+" exceeds its limit");
    return result;
}
std::string raw(const Json& value){
    if(value.is_string())return value.get<std::string>();
    if(value.is_object() && value.at("encoding")=="base64")return decode_base64(value.at("data").get<std::string>());
    throw std::invalid_argument("Bytes must be a string or base64 object");
}
Json bytes(std::string_view value){return {{"encoding","base64"},{"data",encode_base64(value)}};}
Json process(const ProcessResult& value){
    const char* termination="Unknown";
    switch(value.termination){
        case ProcessTermination::exited:termination="Exited";break;
        case ProcessTermination::timed_out:termination="TimedOut";break;
        case ProcessTermination::output_limit:termination="OutputLimit";break;
        case ProcessTermination::cancelled:termination="Cancelled";break;
        case ProcessTermination::spawn_failed:termination="SpawnFailed";break;
        case ProcessTermination::io_failed:termination="IOFailed";break;
        case ProcessTermination::drain_timeout:termination="DrainTimeout";break;
        case ProcessTermination::unsupported_limit:termination="UnsupportedLimit";break;
    }
    auto json=Json{{"termination",termination},{"elapsed_ms",value.elapsed.count()},
        {"stdout",bytes(value.standard_output)},{"stderr",bytes(value.standard_error)},
        {"stdout_total_bytes",value.stdout_total_bytes},{"stderr_total_bytes",value.stderr_total_bytes},
        {"stdout_truncated",value.stdout_truncated},{"stderr_truncated",value.stderr_truncated},
        {"detail",value.detail},{"native_error",value.native_error}};
    json["exit_code"]=value.exit_code?Json(*value.exit_code):Json(nullptr);
    json["signal"]=value.signal?Json(*value.signal):Json(nullptr);
    return json;
}
}
std::string encode_base64(std::string_view bytes){
    constexpr char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;output.reserve((bytes.size()+2)/3*4);
    for(std::size_t i=0;i<bytes.size();i+=3){
        std::uint32_t value=static_cast<unsigned char>(bytes[i])<<16;
        if(i+1<bytes.size())value|=static_cast<unsigned char>(bytes[i+1])<<8;
        if(i+2<bytes.size())value|=static_cast<unsigned char>(bytes[i+2]);
        output+=alphabet[(value>>18)&63];output+=alphabet[(value>>12)&63];
        output+=i+1<bytes.size()?alphabet[(value>>6)&63]:'=';output+=i+2<bytes.size()?alphabet[value&63]:'=';
    }
    return output;
}
std::string decode_base64(std::string_view value){
    if(value.size()%4)throw std::invalid_argument("Invalid base64 length");
    const auto digit=[](char ch)->int{
        if(ch>='A'&&ch<='Z')return ch-'A';if(ch>='a'&&ch<='z')return ch-'a'+26;
        if(ch>='0'&&ch<='9')return ch-'0'+52;if(ch=='+')return 62;if(ch=='/')return 63;return -1;
    };
    std::string bytes;bytes.reserve(value.size()/4*3);
    for(std::size_t i=0;i<value.size();i+=4){
        const int a=digit(value[i]),b=digit(value[i+1]);
        const bool pad2=value[i+2]=='=',pad3=value[i+3]=='=';
        const int c=pad2?0:digit(value[i+2]),d=pad3?0:digit(value[i+3]);
        if(a<0||b<0||c<0||d<0||(pad2&&!pad3)||((pad2||pad3)&&i+4!=value.size())||
           (pad2&&(b&15))||(pad3&&!pad2&&(c&3)))throw std::invalid_argument("Invalid base64 data");
        const std::uint32_t bits=(a<<18)|(b<<12)|(c<<6)|d;
        bytes+=static_cast<char>(bits>>16);if(!pad2)bytes+=static_cast<char>(bits>>8);if(!pad3)bytes+=static_cast<char>(bits);
    }
    return bytes;
}
BatchRequest parse_batch_request(std::string_view text,RequestBounds bounds){
    if(text.size()>bounds.max_json_bytes)throw std::invalid_argument("Request exceeds byte limit");
    std::vector<std::set<std::string>> object_keys;
    const auto json=Json::parse(text,[&](int depth,Json::parse_event_t event,Json& parsed){
        if(depth>64)throw std::invalid_argument("Request JSON nesting exceeds limit");
        if(event==Json::parse_event_t::object_start)object_keys.emplace_back();
        else if(event==Json::parse_event_t::object_end)object_keys.pop_back();
        else if(event==Json::parse_event_t::key && !object_keys.back().insert(parsed.get<std::string>()).second)
            throw std::invalid_argument("Duplicate batch field");
        return true;
    });
    if(!json.is_object() || integer(json.at("schema_version"),1,"schema_version")!=1)
        throw std::invalid_argument("Unsupported batch schema version");
    BatchRequest request;
    const auto& source=json.at("source");
    request.source.document_id=source.at("document_id").get<std::string>();
    request.source.revision=integer(source.at("revision"),std::numeric_limits<std::uint64_t>::max(),"revision");
    request.source.text=source.at("text").get<std::string>();
    if(request.source.document_id.empty() || request.source.document_id.size()>4096 || request.source.document_id.find('\0')!=std::string::npos || decode_utf8(request.source.text).error_offset)
        throw std::invalid_argument("Invalid source snapshot");
    if(source.contains("original_path") && !source.at("original_path").is_null())
        request.source.original_path=path_from_utf8(source.at("original_path").get<std::string>());
    request.toolchain=parse_toolchain_profile(json.at("toolchain").dump());
    const auto& tests=json.at("tests");
    if(!tests.is_array() || tests.size()>bounds.max_tests)throw std::invalid_argument("Invalid testcase count");
    std::set<std::string> ids;
    for(const auto& test:tests){
        TestCase sample;sample.id=test.at("id").get<std::string>();sample.input=raw(test.at("input"));
        if(sample.id.empty() || sample.id.size()>1024 || sample.id.find('\0')!=std::string::npos || !ids.insert(sample.id).second)
            throw std::invalid_argument("Missing or duplicate testcase ID");
        if(test.contains("expected") && !test.at("expected").is_null())sample.expected=raw(test.at("expected"));
        if(test.contains("enabled"))sample.enabled=test.at("enabled").get<bool>();
        request.tests.push_back(std::move(sample));
    }
    const auto policy=json.value("comparison",std::string("trailing"));
    if(policy=="strict")request.comparison=ComparisonMode::cp_editor_strict;
    else if(policy=="trailing")request.comparison=ComparisonMode::cp_editor_ignore_trailing_spaces;
    else if(policy=="byte")request.comparison=ComparisonMode::byte_exact;
    else throw std::invalid_argument("Unknown comparison policy");
    request.max_retained_output_bytes=bounds.max_retained_output_bytes;
    if(json.contains("max_retained_output_bytes"))
        request.max_retained_output_bytes=integer(json.at("max_retained_output_bytes"),bounds.max_retained_output_bytes,"max_retained_output_bytes");
    if(!request.max_retained_output_bytes)throw std::invalid_argument("Retained output limit must be positive");
    return request;
}
std::string serialize_batch_result(const BatchResult& value){
    auto json=Json{{"schema_version",1},{"document_id",value.document_id},{"revision",value.revision},
        {"profile_id",value.profile_id},{"source_digest",value.source_digest},
        {"build_outcome",build_outcome_name(value.build_outcome)},{"error",value.error},{"tests",Json::array()}};
    if(value.build)json["build"]=process(*value.build);
    for(const auto& test:value.tests)json["tests"].push_back({{"testcase_id",test.testcase_id},
        {"verdict",verdict_name(test.verdict)},{"process",process(test.process)},{"detail",test.detail}});
    const auto capabilities=process_capabilities();
    json["capabilities"]={{"managed_process_group",capabilities.managed_process_group},
        {"memory_limit",capabilities.memory_limit},{"spawn_deadline",capabilities.spawn_deadline}};
    return json.dump(2,' ',false,Json::error_handler_t::replace);
}
}
