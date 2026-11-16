// SPDX-License-Identifier: GPL-3.0-or-later
#include "cpn/json.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <stdexcept>
namespace {
using Json=nlohmann::json;
void expect(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
template<class F> void rejected(F f,const char* text){try{f();}catch(const std::exception&){return;}throw std::runtime_error(text);}
}
int main(){
    try{
        for(const std::string& value:std::vector<std::string>{"", "f", "fo", "foo", std::string("a\0\xff",3)})
            expect(cpn::decode_base64(cpn::encode_base64(value))==value,"base64 roundtrip");
        for(const auto* invalid:{"A", "Zg=", "Zh==", "Zm9=", "====", "Zg==AAAA", "AA=A", "Zg==\n"})
            rejected([&]{(void)cpn::decode_base64(invalid);},"invalid base64 accepted");
        const auto toolchain=Json{{"schema_version",1},{"id","interpreter"},{"language_id","test"},
            {"source_filename","main.txt"},{"run",{{"executable","tool"},{"arguments",Json::array()}}}};
        auto doc=Json{{"schema_version",1},{"source",{{"document_id","source"},{"revision",17},{"text","中文 😀"}}},
            {"toolchain",toolchain},{"tests",Json::array({{{"id","empty"},{"input",""},{"expected",""}},
                 {{"id","absent"},{"input",{{"encoding","base64"},{"data","YQBi"}}}}})}};
        auto request=cpn::parse_batch_request(doc.dump());
        expect(request.source.revision==17 && request.tests[0].expected=="" && !request.tests[1].expected,"empty/absent preserved");
        expect(request.tests[1].input==std::string("a\0b",3),"binary input");
        doc["source"]["revision"]=1.5;rejected([&]{(void)cpn::parse_batch_request(doc.dump());},"fractional revision");
        doc["source"]["revision"]=-1;rejected([&]{(void)cpn::parse_batch_request(doc.dump());},"negative revision");
        doc["source"]["revision"]=17;doc["tests"][1]["id"]="empty";
        rejected([&]{(void)cpn::parse_batch_request(doc.dump());},"duplicate ID");
        doc["tests"][1]["id"]="absent";
        rejected([&]{(void)cpn::parse_batch_request(doc.dump(),{16,64,10});},"JSON byte limit");
        rejected([&]{(void)cpn::parse_batch_request(doc.dump(),{65536,64,1});},"testcase limit");
        const auto samples=cpn::parse_test_cases(doc["tests"].dump());
        expect(samples.size()==2 && samples[0].expected=="" && !samples[1].expected && samples[1].input==std::string("a\0b",3),"standalone testcase parser preserves empty and binary");
        for (const auto* invalid : {R"([{"id":"one"}])",R"([{"id":"one","input":"","extra":1}])",R"([{"id":"one","input":"","id":"two"}])",
                                  R"([{"id":"one","input":{"encoding":"base64","data":"YQBi","unknown":1}}])"})
            rejected([&]{(void)cpn::parse_test_cases(invalid);},"standalone testcase schema strict");
        rejected([&]{(void)cpn::parse_test_cases(doc["tests"].dump(),{65536,64,1});},"standalone testcase count bound");
        cpn::BatchResult result;result.document_id="中文";result.revision=17;
        cpn::TestResult test;test.testcase_id="binary";test.process.standard_output=std::string("a\0\xff",3);result.tests.push_back(test);
        auto json=Json::parse(cpn::serialize_batch_result(result));
        expect(json["document_id"]=="中文" && json["revision"]==17,"serialized UTF8 identity");
        expect(cpn::decode_base64(json["tests"][0]["process"]["stdout"]["data"].get<std::string>())==test.process.standard_output,"raw output serialization");
        std::cout<<"JSON tests passed\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
