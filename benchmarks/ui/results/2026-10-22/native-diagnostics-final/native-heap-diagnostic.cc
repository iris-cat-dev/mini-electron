#include <windows.h>
#include <psapi.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include "base/run_loop.h"
#include "mini_ui/ui.h"
#include "runtime/ui/quickjs_bindings.h"
#include "runtime/ui/quickjs_v8_core.h"
#include "third_party/blink/renderer/platform/heap/thread_state.h"

namespace {
double Now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void Require(bool value, const char* message) {
    if (!value) { std::fprintf(stderr,"FAIL diagnostic: %s\n",message); std::abort(); }
}
std::string Evaluate(mini_ui::View& view, const std::string& expression) {
    mini_ui::Error error;
    std::string result;
    if (!view.Evaluate(expression,"framework-memory-diagnostic.js",&result,&error)) {
        std::fprintf(stderr,"FAIL script: %s\n",error.message.c_str()); std::abort();
    }
    return result;
}
void Pump(mini_ui::View& view, const std::string& predicate) {
    const double deadline=Now()+600;
    while (Evaluate(view,predicate)!="true") {
        mini_ui::Error error;
        if (!view.Tick(Now(),&error)) {
            std::fprintf(stderr,"FAIL Tick: %s\n",error.message.c_str()); std::abort();
        }
        Require(Now()<deadline,"actual framework phase completed within diagnostic deadline");
    }
}
void Snapshot(const char* label,v8::Isolate* isolate) {
    PROCESS_MEMORY_COUNTERS_EX process{};
    process.cb=sizeof(process);
    Require(GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&process),sizeof(process)),"read real process memory");
    JSMemoryUsage js{};
    JS_ComputeMemoryUsage(isolate->GetQuickJSRuntime(),&js);
    const auto native=blink::ThreadState::Current()->cpp_heap().CollectStatistics(cppgc::HeapStatistics::kBrief);
    const auto joint=mini_ui::GetQuickJSGCStats(isolate);
    std::printf("MEMORY {\"phase\":\"%s\",\"process_private_bytes\":%zu,\"process_working_set_bytes\":%zu,\"js_malloc_bytes\":%lld,\"js_used_bytes\":%lld,\"js_objects\":%lld,\"js_properties\":%lld,\"js_functions\":%lld,\"js_shapes\":%lld,\"native_used_bytes\":%zu,\"native_committed_bytes\":%zu,\"native_pooled_bytes\":%zu,\"joint_epoch\":%llu,\"wrapper_count\":%zu,\"edge_count\":%zu}\n",
        label,process.PrivateUsage,process.WorkingSetSize,
        static_cast<long long>(js.malloc_size),static_cast<long long>(js.memory_used_size),
        static_cast<long long>(js.obj_count),static_cast<long long>(js.prop_count),
        static_cast<long long>(js.js_func_count),static_cast<long long>(js.shape_count),
        native.used_size_bytes,native.committed_size_bytes,native.pooled_memory_size_bytes,
        static_cast<unsigned long long>(joint.epoch),joint.wrapper_count,joint.edge_count);
    std::fflush(stdout);
}
void Collect(v8::Isolate* isolate) {
    v8::Isolate::Scope entered(isolate);
    for(int round=0;round<3;++round) {
        JS_RunGC(isolate->GetQuickJSRuntime());
        v8::detail::CollectWeakPersistents(isolate);
        blink::ThreadState::Current()->cpp_heap().Collect(cppgc::EmbedderStackState::kNoHeapPointers);
    }
    JS_RunGC(isolate->GetQuickJSRuntime());
    v8::detail::CollectWeakPersistents(isolate);
}
}
int main(int argc,char** argv) {
    Require(argc==5,"assets framework rows iterations arguments");
    const std::string framework=argv[2];
    Require(framework=="vue"||framework=="react","real framework name");
    const int rows=std::atoi(argv[3]);
    const int iterations=std::atoi(argv[4]);
    Require(rows>0&&rows<=100000&&iterations>0&&iterations<=1000,"valid diagnostic scale");
    mini_ui::ViewOptions options;
    options.resource_root=std::filesystem::absolute(argv[1]);
    options.width=1280;options.height=720;options.device_scale=1;
    mini_ui::Error error;
    auto view=mini_ui::View::Create(options,&error);
    Require(view!=nullptr,"create real UI view");
    Require(view->LoadFile(framework+".html",&error),"load actual pinned framework page");
    Pump(*view,"typeof uiBench==='object'&&uiBench.status==='ready'");
    v8::Isolate* isolate=v8::Isolate::TryGetCurrent();
    Require(isolate&&isolate->GetQuickJSRuntime(),"actual facade isolate and VM");
    Snapshot("ready_empty",isolate);
    std::puts("WORKLOAD_BEGIN");std::fflush(stdout);
    Require(Evaluate(*view,"uiBench.start({rows:"+std::to_string(rows)+",warmups:2,iterations:"+std::to_string(iterations)+"})")=="true","start real workload");
    Pump(*view,"uiBench.done");
    Require(Evaluate(*view,"uiBench.status==='complete'&&uiBench.result().assertion_failures.length===0")=="true","all real application assertions passed");
    std::puts("WORKLOAD_END");std::fflush(stdout);
    Snapshot("workload_unmounted_before_forced_gc",isolate);
    Collect(isolate);
    Snapshot("workload_unmounted_after_forced_gc",isolate);
    Require(Evaluate(*view,"document.getElementById('bench-app').childElementCount===0")=="true","unmounted host stays empty after actual GC");
    Require(Evaluate(*view,"uiBench.prepareCapture()")=="true","prepare actual populated verification");
    Pump(*view,"uiBench.captureReady");
    Require(Evaluate(*view,"uiBench.status==='complete'&&uiBench.result().capture_verification_assertions===1")=="true","actual populated capture assertions");
    Snapshot("populated_before_forced_gc",isolate);
    Collect(isolate);
    Snapshot("populated_after_forced_gc",isolate);
    Require(Evaluate(*view,"document.querySelectorAll('.bench-row').length==="+std::to_string(rows))=="true","mounted real DOM survives forced collection");
    view.reset();
    Snapshot("view_released_before_extra_gc",isolate);
    Collect(isolate);
    Snapshot("view_released_after_extra_gc",isolate);
    base::RunLoop().RunUntilIdle();
    Collect(isolate);
    Snapshot("view_released_after_idle_drain_and_gc",isolate);
    std::puts("PASS diagnostic: actual workload, unmounted/mounted/destroyed heap counters, forced joint/native GC and mounted DOM survival; diagnostic data are not benchmark samples");
    return 0;
}
