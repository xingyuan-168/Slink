// No real displays or windows: force an async mutation reply, then replay the
// identical request after completion. It must return its first result exactly.
#include "virtual/broker.h"
#include "virtual/protocol.h"
#include <atomic>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <thread>

namespace {
using namespace gwm::vd;
int checks{};
void require(bool pass,const char* name){++checks;std::printf("%s %s\n",pass?"PASS":"FAIL",name);if(!pass)throw std::runtime_error(name);}
bool until(const std::function<bool()>& predicate){const auto deadline=GetTickCount64()+4000;do{if(predicate())return true;Sleep(20);}while(GetTickCount64()<deadline);return predicate();}
ipc::Reply exchange(const std::wstring& pipeName,ipc::Request request){
    request.processId=GetCurrentProcessId();HANDLE pipe=INVALID_HANDLE_VALUE;
    if(!until([&]{pipe=CreateFileW(pipeName.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);return pipe!=INVALID_HANDLE_VALUE;}))throw std::runtime_error("open test pipe");
    DWORD mode=PIPE_READMODE_MESSAGE;SetNamedPipeHandleState(pipe,&mode,nullptr,nullptr);DWORD bytes{};ipc::Reply reply;
    const bool transferred=WriteFile(pipe,&request,sizeof(request),&bytes,nullptr)&&bytes==sizeof(request)&&ReadFile(pipe,&reply,sizeof(reply),&bytes,nullptr)&&bytes==sizeof(reply);
    uint32_t ack=ipc::ReplyAcknowledged;if(transferred)WriteFile(pipe,&ack,sizeof(ack),&bytes,nullptr);CloseHandle(pipe);
    if(!transferred)throw std::runtime_error("test pipe transfer");return reply;
}
ipc::Reply complete(const std::wstring& pipe,ipc::Request request){ipc::Reply reply;require(until([&]{reply=exchange(pipe,request);return reply.error!=ERROR_IO_PENDING;}),"mutation_completes_with_same_request_id");return reply;}
}
int wmain(){
    std::atomic<unsigned> initializations{},detachments{};std::atomic<bool> active{},holdInitialization{};
    HANDLE ready=CreateEventW(nullptr,TRUE,FALSE,nullptr),stop=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    gwm::vd::BrokerOptions options;options.instanceSuffix=L"request-cache-"+std::to_wstring(GetCurrentProcessId());
    options.readyEvent=ready;options.stopEvent=stop;options.finalReplyGraceMs=500;options.testOperationDelayMs=350;
    options.inspect=[&]{gwm::vd::DisplayResult r;r.installed=true;r.active=active;r.bounds={1920,0,3840,1080};return r;};
    options.ensure=[&](int,int,const gwm::vd::PhaseCallback&){++initializations;while(holdInitialization&&WaitForSingleObject(stop,10)==WAIT_TIMEOUT){}active=true;return options.inspect();};
    options.detach=[&]{++detachments;active=false;return options.inspect();};
    DWORD serverResult{};std::thread server([&]{try{serverResult=gwm::vd::RunRecoveryBroker(options);}catch(...){serverResult=ERROR_UNHANDLED_EXCEPTION;SetEvent(ready);}});
    const auto pipe=gwm::vd::ipc::BrokerPipeName()+L"."+options.instanceSuffix;int result{};
    try{
        require(WaitForSingleObject(ready,4000)==WAIT_OBJECT_0,"isolated_fake_broker_ready");
        ipc::Request registration;registration.operation=ipc::Operation::Register;registration.requestId=1;
        registration.count=1;registration.windows[0]=UINTPTR_MAX;
        require(!IsWindow(reinterpret_cast<HWND>(UINTPTR_MAX)),"whitelist_fixture_is_not_a_real_window");
        require(exchange(pipe,registration).error==ERROR_IO_PENDING,"forced_register_returns_pending");
        require(!complete(pipe,registration).error,"register_retry_returns_first_completion");
        ipc::Request initialize;initialize.operation=ipc::Operation::Initialize;initialize.requestId=2;
        require(exchange(pipe,initialize).error==ERROR_IO_PENDING,"forced_initialize_returns_pending");
        const auto first=complete(pipe,initialize);require(!first.error&&until([&]{return initializations==1;}),"one_initialization_executed");
        const auto replay=exchange(pipe,initialize);
        require(!replay.error&&replay.state==first.state&&initializations==1,"completed_mutation_replay_does_not_execute_again");
        auto collision=initialize;collision.operation=ipc::Operation::Show;
        require(exchange(pipe,collision).error==ERROR_INVALID_DATA,"request_id_cannot_be_reused_for_different_operation");
        initialize.requestId=3;require(!complete(pipe,initialize).error&&until([&]{return initializations==2;}),"new_request_id_allows_deliberate_reinitialization");
        holdInitialization=true;initialize.requestId=4;
        require(!complete(pipe,initialize).error&&until([&]{return initializations==3;}),"initializer_remains_in_flight");
        ipc::Request show;show.operation=ipc::Operation::Show;show.requestId=5;show.window=registration.windows[0];
        require(complete(pipe,show).error==ERROR_NOT_READY,"show_cannot_clear_a_checkpoint_during_initialization");
        ipc::Request release;release.operation=ipc::Operation::Release;release.requestId=6;
        const auto waiting=complete(pipe,release);
        require(waiting.error==ERROR_BUSY&&waiting.state==ipc::State::Restoring,"release_during_initialization_is_retryable_even_with_zero_records");
        holdInitialization=false;
        ipc::Request query;query.operation=ipc::Operation::State;
        require(until([&]{const auto state=exchange(pipe,query).state;return state==ipc::State::Stopped||state==ipc::State::RestoreFailed;}),"initializer_finishes_before_release_retry");
        release.requestId=7;
        require(!complete(pipe,release).error,"release_retries_keep_same_identity");
    }catch(const std::exception& error){std::printf("FAIL %s\n",error.what());result=1;}
    holdInitialization=false;SetEvent(stop);server.join();CloseHandle(ready);CloseHandle(stop);
    if(serverResult||!detachments)result=1;
    // Exact, task-owned test journal only; never traverse or recursively delete.
    DWORD session{};ProcessIdToSessionId(GetCurrentProcessId(),&session);
    const auto directory=ipc::RuntimeDirectory()+L"\\Recovery-"+ipc::CurrentLogonSid()+L"."+std::to_wstring(session)+L"."+options.instanceSuffix;
    DeleteFileW((directory+L"\\recovery.bin").c_str());DeleteFileW((directory+L"\\recovery.pending").c_str());DeleteFileW((directory+L"\\recovery.log").c_str());RemoveDirectoryW(directory.c_str());
    std::printf("request_cache: %d checks, result=%d; no real display operations\n",checks,result);return result;
}
