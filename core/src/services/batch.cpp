// SPDX-License-Identifier: GPL-3.0-or-later
#include "cpn/batch.hpp"
#include "cpn/digest.hpp"
#include "cpn/text.hpp"
#include <algorithm>
#include <atomic>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <stdexcept>
namespace cpn {
namespace {
std::string bounded_detail(std::string_view detail) {
    constexpr std::size_t maximum=4096;
    auto end=std::min(detail.size(),maximum);
    if(end<detail.size()) while(end>0 && (static_cast<unsigned char>(detail[end])&0xc0)==0x80) --end;
    return std::string(detail.substr(0,end));
}
void retain_outputs(ProcessResult& result,const ProcessLimits& limits) {
    result.stdout_total_bytes=std::max<std::uint64_t>(result.stdout_total_bytes,result.standard_output.size());
    result.stderr_total_bytes=std::max<std::uint64_t>(result.stderr_total_bytes,result.standard_error.size());
    const auto out=std::min<std::uint64_t>(result.standard_output.size(),std::min(limits.stdout_bytes,limits.combined_bytes));
    const auto err=std::min<std::uint64_t>(result.standard_error.size(),std::min(limits.stderr_bytes,limits.combined_bytes-out));
    result.stdout_truncated=result.stdout_truncated || out<result.standard_output.size();
    result.stderr_truncated=result.stderr_truncated || err<result.standard_error.size();
    result.standard_output.resize(static_cast<std::size_t>(out));
    result.standard_error.resize(static_cast<std::size_t>(err));
    const auto maximum=std::numeric_limits<std::uint64_t>::max();
    const auto total=result.stderr_total_bytes>maximum-result.stdout_total_bytes?maximum:
        result.stdout_total_bytes+result.stderr_total_bytes;
    const auto exceeded=result.stdout_truncated || result.stderr_truncated ||
        result.stdout_total_bytes>limits.stdout_bytes || result.stderr_total_bytes>limits.stderr_bytes || total>limits.combined_bytes;
    if (exceeded && result.termination==ProcessTermination::exited) {
        result.termination=ProcessTermination::output_limit;
        if (result.detail.empty()) result.detail="Process output exceeded configured byte limit";
    }
}
class Workspace {
public:
    explicit Workspace(std::shared_ptr<FileStore> store):store_(std::move(store)),path(store_->create_workspace()){}
    ~Workspace(){store_->remove_workspace(path);}
private:
    std::shared_ptr<FileStore> store_;
public:
    std::filesystem::path path;
};
std::string expand(std::string_view text,const std::map<std::string,std::string>& values){
    std::string result;
    for(std::size_t i=0;i<text.size();) {
        if(text[i]=='$' && i+1<text.size() && text[i+1]=='{') {
            auto end=text.find('}',i+2);
            if(end==std::string_view::npos) throw std::runtime_error("Unclosed command placeholder");
            const auto key=std::string(text.substr(i+2,end-i-2));
            auto value=values.find(key);
            if(value==values.end()) throw std::runtime_error("Unknown command placeholder: "+key);
            result+=value->second;i=end+1;
        } else result+=text[i++];
    }
    if(result.find('\0')!=std::string::npos) throw std::runtime_error("NUL in command argument");
    return result;
}
ProcessSpec command(const CommandTemplate& tmpl,const ToolchainProfile& profile,const ProcessLimits& limits,
    const std::map<std::string,std::string>& values,const std::filesystem::path& workspace,std::string input={}){
    ProcessSpec spec;spec.executable=path_from_utf8(expand(tmpl.executable,values));
    for(const auto& arg:tmpl.arguments) spec.arguments.push_back(expand(arg,values));
    spec.working_directory=workspace;spec.input=std::move(input);spec.limits=limits;
    spec.inherit_environment=profile.inherit_environment;spec.environment=profile.environment;
    return spec;
}
Verdict process_verdict(const ProcessResult& process){
    switch(process.termination){
        case ProcessTermination::cancelled:return Verdict::cancelled;
        case ProcessTermination::timed_out:return Verdict::time_limit;
        case ProcessTermination::output_limit:return Verdict::output_limit;
        case ProcessTermination::exited:
            return process.signal || !process.exit_code || *process.exit_code!=0 ? Verdict::runtime_error : Verdict::unchecked;
        default:return Verdict::infrastructure_error;
    }
}
void remaining(BatchResult& result,const BatchRequest& request,std::size_t start,Verdict verdict){
    for(std::size_t i=start;i<request.tests.size();++i)
        if(request.tests[i].enabled) result.tests.push_back(TestResult{request.tests[i].id,verdict,{},result.error});
}
void validate_artifact_path(const std::optional<std::filesystem::path>& artifact_path){
    if(!artifact_path) return;
    const auto utf8=path_to_utf8(*artifact_path);
    if(utf8.empty() || utf8.size()>32768 || utf8.find('\0')!=std::string::npos || decode_utf8(utf8).error_offset)
        throw std::runtime_error("Invalid artifact path");
    if(!artifact_path->is_absolute()) throw std::runtime_error("Artifact path must be absolute");
}
std::string unique_artifact_suffix(){
    static std::atomic<std::uint64_t> counter{0};
    std::random_device random;
    return std::to_string(random())+"-"+std::to_string(random())+"-"+std::to_string(++counter);
}
void export_artifact(const std::filesystem::path& source_artifact,const std::filesystem::path& destination){
    if(!std::filesystem::is_regular_file(source_artifact))
        throw std::runtime_error("Compiled artifact was not produced");
    const auto parent=destination.parent_path();
    if(parent.empty() || !std::filesystem::is_directory(parent))
        throw std::runtime_error("Artifact destination directory does not exist");
    const auto temporary=parent/path_from_utf8(".cpn-artifact-"+unique_artifact_suffix()+".tmp");
    try{
        std::filesystem::copy_file(source_artifact,temporary,std::filesystem::copy_options::none);
        std::filesystem::rename(temporary,destination);
    }catch(...){
        std::error_code ignored;
        std::filesystem::remove(temporary,ignored);
        throw;
    }
}
}
BatchService::BatchService(std::shared_ptr<FileStore> files,ProcessExecutor process):files_(std::move(files)),process_(std::move(process)){
    if(!files_ || !process_) throw std::invalid_argument("Missing service port");
}
BatchResult BatchService::run(const BatchRequest& request,std::stop_token stop,BatchObserver observer) const {
    BatchResult result;result.document_id=request.source.document_id;result.revision=request.source.revision;
    result.source_digest=sha256(request.source.text);result.profile_id=request.toolchain.id;
    bool running_phase=false;
    std::optional<std::size_t> active_test;
    try {
        validate_toolchain_profile(request.toolchain);
        if (request.external_checker) validate_external_checker(*request.external_checker);
        validate_artifact_path(request.artifact_path);
        if(!request.max_retained_output_bytes) throw std::runtime_error("Invalid retained output budget");
        if(request.source.document_id.empty() || decode_utf8(request.source.text).error_offset)
            throw std::runtime_error("Invalid source snapshot");
        std::set<std::string> ids;
        for(const auto& test:request.tests)
            if(test.id.empty() || !ids.insert(test.id).second) throw std::runtime_error("Missing or duplicate testcase ID");
        if(stop.stop_requested()){
            result.build_outcome=request.mode==BatchMode::run_only && !request.tests.empty()
                ?BuildOutcome::not_required:BuildOutcome::cancelled;
            if(request.mode!=BatchMode::compile_only) remaining(result,request,0,Verdict::cancelled);
            return result;
        }
        Workspace workspace(files_);
        workspace.path=std::filesystem::absolute(workspace.path);
        const auto source=workspace.path/path_from_utf8(request.toolchain.source_filename);
        const auto artifact=workspace.path/path_from_utf8(request.toolchain.artifact_name);
        files_->write_atomic(source,request.source.text);
        if(request.toolchain.build && std::filesystem::exists(artifact) && std::filesystem::equivalent(source,artifact))
            throw std::runtime_error("Artifact aliases the source snapshot on this filesystem");
        auto original_directory=request.source.original_path.empty()?workspace.path:std::filesystem::absolute(request.source.original_path).parent_path();
        std::map<std::string,std::string> values{
            {"source",path_to_utf8(source)},{"artifact",path_to_utf8(artifact)},
            {"source_dir",path_to_utf8(original_directory)},{"job_dir",path_to_utf8(workspace.path)}};
        for(const auto& [key,value]:request.toolchain.variables)
            if(!values.emplace(key,value).second) throw std::runtime_error("Duplicate or reserved profile variable");
        std::size_t retained=0;
        if(request.mode==BatchMode::run_only){
            running_phase=true;
            if(request.toolchain.build){
                if(!request.artifact_path || !std::filesystem::is_regular_file(*request.artifact_path))
                    throw std::runtime_error("Compiled artifact is missing; compile before running");
                values["artifact"]=path_to_utf8(*request.artifact_path);
            }
        }else if(request.toolchain.build){
            if(observer) observer(BatchPhase::building,{});
            auto build_limits=request.toolchain.build_limits;
            build_limits.combined_bytes=std::min<std::uint64_t>(build_limits.combined_bytes,request.max_retained_output_bytes);
            result.build=process_(command(*request.toolchain.build,request.toolchain,build_limits,values,workspace.path),stop);
            retain_outputs(*result.build,build_limits);
            result.build->detail=bounded_detail(result.build->detail);
            retained=result.build->standard_output.size()+result.build->standard_error.size();
            if(result.build->termination==ProcessTermination::cancelled) result.build_outcome=BuildOutcome::cancelled;
            else if(result.build->termination!=ProcessTermination::exited) result.build_outcome=BuildOutcome::failed;
            else if(result.build->signal || !result.build->exit_code || *result.build->exit_code!=0) result.build_outcome=BuildOutcome::source_error;
            else result.build_outcome=BuildOutcome::succeeded;
            if(result.build_outcome!=BuildOutcome::succeeded) {
                result.error=result.build->detail;
                if(request.mode!=BatchMode::compile_only)
                    remaining(result,request,0,result.build_outcome==BuildOutcome::cancelled?Verdict::cancelled:Verdict::not_run);
                return result;
            }
            if(request.artifact_path){
                if(!std::filesystem::is_regular_file(artifact)){
                    result.build_outcome=BuildOutcome::failed;
                    result.error="Compiled artifact was not produced";
                    if(request.mode!=BatchMode::compile_only) remaining(result,request,0,Verdict::not_run);
                    return result;
                }
                export_artifact(artifact,*request.artifact_path);
            }
            if(request.mode==BatchMode::compile_only) return result;
        }else if(request.mode==BatchMode::compile_only){
            return result;
        }
        running_phase=true;
        for(std::size_t i=0;i<request.tests.size();++i) {
            const auto& test=request.tests[i];if(!test.enabled) continue;
            if(stop.stop_requested()){remaining(result,request,i,Verdict::cancelled);break;}
            if(retained>=request.max_retained_output_bytes){result.error="Batch retained output budget exhausted";remaining(result,request,i,Verdict::not_run);break;}
            active_test=i;
            if(observer) observer(BatchPhase::running,test.id);
            auto limits=request.toolchain.run_limits;
            limits.combined_bytes=std::min<std::uint64_t>(limits.combined_bytes,request.max_retained_output_bytes-retained);
            auto process=process_(command(request.toolchain.run,request.toolchain,limits,values,workspace.path,test.input),stop);
            retain_outputs(process,limits);
            process.detail=bounded_detail(process.detail);
            retained+=process.standard_output.size()+process.standard_error.size();
            TestResult current;current.testcase_id=test.id;current.verdict=process_verdict(process);current.detail=process.detail;
            current.process=std::move(process);result.tests.push_back(std::move(current));
            auto& checked=result.tests.back();
            if(checked.verdict==Verdict::unchecked && test.expected) {
                if(observer) observer(BatchPhase::checking,test.id);
                if (request.external_checker) {
                    auto spec=*request.external_checker;
                    spec.limits.combined_bytes=std::min<std::uint64_t>(spec.limits.combined_bytes,
                        retained<request.max_retained_output_bytes?request.max_retained_output_bytes-retained:0);
                    checked.checker=ExternalCheckerService(files_,process_).run(spec,
                        CheckerInput{test.input,checked.process.standard_output,*test.expected,original_directory},stop);
                    retained+=checked.checker->process.standard_output.size()+checked.checker->process.standard_error.size();
                    switch (checked.checker->verdict) {
                        case ExternalCheckerVerdict::accepted: checked.verdict=Verdict::accepted; break;
                        case ExternalCheckerVerdict::wrong_answer: checked.verdict=Verdict::wrong_answer; break;
                        case ExternalCheckerVerdict::cancelled: checked.verdict=Verdict::cancelled; break;
                        case ExternalCheckerVerdict::checker_error: checked.verdict=Verdict::checker_error; break;
                    }
                    checked.detail=checked.checker->detail;
                } else {
                    const auto comparison=compare_outputs(checked.process.standard_output,*test.expected,request.comparison,stop);
                    checked.verdict=comparison.verdict==ComparisonVerdict::accepted?Verdict::accepted:
                        comparison.verdict==ComparisonVerdict::wrong_answer?Verdict::wrong_answer:
                        comparison.verdict==ComparisonVerdict::cancelled?Verdict::cancelled:Verdict::checker_error;
                    if(comparison.verdict==ComparisonVerdict::encoding_error) checked.detail="Invalid UTF-8 in text comparison";
                    if(comparison.verdict==ComparisonVerdict::cancelled) checked.detail="Cancelled during output comparison";
                }
            }
            active_test.reset();
            if(result.tests.back().verdict==Verdict::cancelled){remaining(result,request,i+1,Verdict::cancelled);break;}
        }
    } catch(const std::exception& error) {
        result.error=bounded_detail(error.what());
        if(!running_phase) result.build_outcome=BuildOutcome::failed;
        if(active_test) for(auto& test:result.tests) if(test.testcase_id==request.tests[*active_test].id) {
            test.verdict=Verdict::infrastructure_error;test.detail=result.error;
        }
        // Failure after partial execution keeps all produced stable-ID results.
        std::set<std::string> finished;for(const auto& test:result.tests) finished.insert(test.testcase_id);
        if(request.mode!=BatchMode::compile_only){
            for(const auto& test:request.tests) if(test.enabled && !finished.contains(test.id))
                result.tests.push_back(TestResult{test.id,active_test && request.tests[*active_test].id==test.id?
                    Verdict::infrastructure_error:Verdict::not_run,{},result.error});
        }
    }
    return result;
}
const char* verdict_name(Verdict verdict) noexcept {
    constexpr const char* names[]{"AC","WA","TLE","RE","OLE","Unchecked","Cancelled","NotRun","CheckerError","InfrastructureError"};
    const auto index=static_cast<unsigned>(verdict);return index<std::size(names)?names[index]:"Unknown";
}
const char* build_outcome_name(BuildOutcome outcome) noexcept {
    constexpr const char* names[]{"NotRequired","Succeeded","CompilationError","Failed","Cancelled"};
    const auto index=static_cast<unsigned>(outcome);return index<std::size(names)?names[index]:"Unknown";
}
}
