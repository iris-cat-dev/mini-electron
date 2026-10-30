// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <thread>

#include "base/time/time.h"
#include "base/trace_event/process_memory_dump.h"
#include "src/runtime/engine/renderer/render_thread_impl.h"
#if BUILDFLAG(IS_MAC)
#include "src/runtime/electron/common/renderer_memory_mac.h"
#endif

namespace {

void Require(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::abort();
    }
}

void DiscardableMemoryReclaimsOnlyUnlockedBuffers()
{
    content::RendererDiscardableMemoryAllocator allocator(128);
    auto locked = allocator.AllocateLockedDiscardableMemory(64);
    auto unlocked = allocator.AllocateLockedDiscardableMemory(48);
    Require(locked && unlocked, "initial allocations fit the budget");
    static_cast<uint8_t*>(locked->data())[0] = 0x5a;
    Require(allocator.GetBytesAllocated() == 112,
        "accounting includes both committed buffers");

    unlocked->Unlock();
    allocator.ReleaseFreeMemory();
    Require(allocator.GetBytesAllocated() == 64,
        "release removes only the unlocked buffer from accounting");
    Require(locked->data() != nullptr,
        "a locked buffer survives explicit reclamation");
    Require(static_cast<uint8_t*>(locked->data())[0] == 0x5a,
        "reclamation preserves the contents of a locked buffer");
    Require(!unlocked->Lock(), "a reclaimed buffer cannot be relocked");

    locked->Unlock();
    allocator.ReleaseFreeMemory();
    Require(allocator.GetBytesAllocated() == 0,
        "all unlocked buffers are reclaimed");
    Require(!locked->Lock(), "a subsequently reclaimed buffer stays discarded");
}

void DiscardableMemoryBudgetAdmissionIsExact()
{
    content::RendererDiscardableMemoryAllocator allocator(100);
    auto locked = allocator.AllocateLockedDiscardableMemory(60);
    auto reclaimable = allocator.AllocateLockedDiscardableMemory(30);
    Require(locked && reclaimable, "allocations below the budget succeed");
    reclaimable->Unlock();

    auto too_large = allocator.AllocateLockedDiscardableMemory(50);
    Require(!too_large,
        "admission still fails when reclaiming unlocked data is insufficient");
    Require(allocator.GetBytesAllocated() == 60,
        "failed admission leaves exact live-buffer accounting");

    auto exact = allocator.AllocateLockedDiscardableMemory(40);
    Require(exact && allocator.GetBytesAllocated() == 100,
        "an allocation may fill but never exceed the budget");
    Require(!allocator.AllocateLockedDiscardableMemory(
                std::numeric_limits<size_t>::max()),
        "overflow-sized admission fails without wrapping");

    exact->Unlock();
    locked->Unlock();
    allocator.ReleaseFreeMemory();
    Require(allocator.GetBytesAllocated() == 0,
        "budget test releases every unlocked buffer");
}

void DiscardableMemoryDestructionAndRepeatedPurgeAreExact()
{
    content::RendererDiscardableMemoryAllocator allocator(128);
    auto first = allocator.AllocateLockedDiscardableMemory(32);
    auto middle = allocator.AllocateLockedDiscardableMemory(32);
    auto last = allocator.AllocateLockedDiscardableMemory(32);
    Require(first && middle && last, "destruction test allocations succeed");
    middle.reset();
    Require(allocator.GetBytesAllocated() == 64,
        "destroying a locked middle allocation unlinks and subtracts it");
    first->Unlock();
    first->DiscardForTesting();
    first->DiscardForTesting();
    allocator.ReleaseFreeMemory();
    Require(allocator.GetBytesAllocated() == 32,
        "repeated discard never subtracts a buffer twice");
    first.reset();
    Require(allocator.GetBytesAllocated() == 32,
        "destroying an already-discarded tail does not subtract again");
    last->Unlock();
    last.reset();
    Require(allocator.GetBytesAllocated() == 0,
        "destroying an unlocked live head removes its bytes");
    auto full = allocator.AllocateLockedDiscardableMemory(128);
    Require(full && allocator.GetBytesAllocated() == 128,
        "the whole budget is reusable after all list removals");
}

void DiscardableMemoryDumpsReportOnlyLiveBytes()
{
    content::RendererDiscardableMemoryAllocator allocator(64);
    auto memory = allocator.AllocateLockedDiscardableMemory(64);
    Require(memory != nullptr, "dump test allocation succeeds");
    base::trace_event::ProcessMemoryDump pmd(
        { base::trace_event::MemoryDumpLevelOfDetail::kDetailed });
    auto* locked_dump = memory->CreateMemoryAllocatorDump("test/locked", &pmd);
    Require(locked_dump && locked_dump->GetSizeInternal() == 64,
        "a locked buffer provides a non-null dump with its live size");
    memory->Unlock();
    auto* unlocked_dump = memory->CreateMemoryAllocatorDump("test/unlocked", &pmd);
    Require(unlocked_dump && unlocked_dump->GetSizeInternal() == 64,
        "an unlocked but retained buffer is still counted in a dump");
    allocator.ReleaseFreeMemory();
    auto* discarded_dump = memory->CreateMemoryAllocatorDump("test/discarded", &pmd);
    Require(discarded_dump && discarded_dump->GetSizeInternal() == 0,
        "a discarded buffer provides a non-null zero-sized dump");
}

void DiscardableMemoryPurgeAndRelockRaceIsSafe()
{
    content::RendererDiscardableMemoryAllocator allocator(4096);
    auto memory = allocator.AllocateLockedDiscardableMemory(2048);
    Require(memory != nullptr, "race buffer allocation succeeds");
    memory->Unlock();

    std::atomic<bool> start { false };
    std::thread relocker([&] {
        while (!start.load(std::memory_order_acquire)) {
        }
        for (int i = 0; i < 10000; ++i) {
            if (!memory->Lock())
                return;
            Require(memory->data() != nullptr,
                "successful relock retains a live buffer");
            memory->Unlock();
        }
    });
    start.store(true, std::memory_order_release);
    for (int i = 0; i < 10000; ++i) {
        allocator.ReleaseFreeMemory();
        if (!allocator.GetBytesAllocated())
            break;
    }
    relocker.join();
    allocator.ReleaseFreeMemory();
    Require(allocator.GetBytesAllocated() == 0,
        "concurrent purge and relock ends with exact accounting");
}

void MemoryPressurePolicyUsesHysteresisAndCooldowns()
{
    using Action = content::RendererMemoryPressurePolicy::Action;
    using Level = content::RendererMemoryPressurePolicy::Level;
    const base::TimeTicks now = base::TimeTicks::Now();

    content::RendererMemoryPressurePolicy policy;
    Require(policy.Evaluate(699, 1000, 0, 0, Level::kNone, now) == Action::kNone,
        "pressure remains clear below the moderate threshold");
    Require(policy.Evaluate(700, 1000, 0, 0, Level::kNone, now + base::Seconds(1))
            == Action::kModerate,
        "moderate pressure fires at its entry threshold");
    Require(policy.Evaluate(650, 1000, 0, 0, Level::kNone, now + base::Seconds(2))
            == Action::kNone
            && policy.level() == Level::kModerate,
        "moderate pressure remains latched above its exit threshold");
    Require(policy.Evaluate(850, 1000, 0, 0, Level::kNone, now + base::Seconds(3))
            == Action::kCritical,
        "critical pressure fires at its entry threshold");
    Require(policy.Evaluate(900, 1000, 0, 0, Level::kNone, now + base::Seconds(62))
            == Action::kNone,
        "critical pressure is not repeated before its cooldown");
    Require(policy.Evaluate(900, 1000, 0, 0, Level::kNone, now + base::Seconds(63))
            == Action::kCritical,
        "sustained critical pressure is retried only after its cooldown");
    Require(policy.Evaluate(749, 1000, 0, 0, Level::kNone, now + base::Seconds(64))
            == Action::kNone
            && policy.level() == Level::kModerate,
        "critical pressure exits through the hysteresis band");
    Require(policy.Evaluate(599, 1000, 0, 0, Level::kNone, now + base::Seconds(65))
            == Action::kNone
            && policy.level() == Level::kNone,
        "pressure clears only below the moderate exit threshold");
    Require(policy.Evaluate(0, 0, 0, 0, Level::kCritical, now + base::Seconds(124))
            == Action::kCritical,
        "the OS low-memory state triggers critical pressure without a job sample");

    content::RendererMemoryPressurePolicy moderate_cooldown;
    Require(moderate_cooldown.Evaluate(700, 1000, 0, 0, Level::kNone, now)
            == Action::kModerate,
        "first moderate transition is delivered");
    Require(moderate_cooldown.Evaluate(500, 1000, 0, 0, Level::kNone,
                now + base::Seconds(1))
            == Action::kNone,
        "moderate state can clear");
    Require(moderate_cooldown.Evaluate(700, 1000, 0, 0, Level::kNone,
                now + base::Seconds(2))
            == Action::kNone,
        "rapid moderate re-entry is rate limited");
    Require(moderate_cooldown.Evaluate(700, 1000, 0, 0, Level::kNone,
                now + base::Seconds(30))
            == Action::kModerate,
        "a rate-limited re-entry is delivered when its cooldown expires");
    Require(moderate_cooldown.Evaluate(700, 1000, 0, 0, Level::kNone,
                now + base::Seconds(31))
            == Action::kNone,
        "a delivered moderate transition is not repeated on every poll");
}

void HeapPressurePrecedesProcessPressure()
{
    using Action = content::RendererMemoryPressurePolicy::Action;
    using Level = content::RendererMemoryPressurePolicy::Level;
    const base::TimeTicks now = base::TimeTicks::Now();
    content::RendererMemoryPressurePolicy policy;
    Require(policy.Evaluate(200, 1000, 699, 1000, Level::kNone, now) == Action::kNone,
        "heap pressure remains clear just below its threshold");
    Require(policy.Evaluate(200, 1000, 700, 1000, Level::kNone, now + base::Seconds(1))
            == Action::kModerate,
        "heap pressure notifies while process commit remains low");
    Require(policy.Evaluate(200, 1000, 850, 1000, Level::kNone, now + base::Seconds(2))
            == Action::kCritical,
        "V8 reaches critical before the process Job budget");
    Require(policy.Evaluate(200, 1000, 750, 1000, Level::kNone, now + base::Seconds(3))
            == Action::kNone && policy.level() == Level::kCritical,
        "heap critical hysteresis includes its exit boundary");
    policy.Evaluate(200, 1000, 749, 1000, Level::kNone, now + base::Seconds(4));
    Require(policy.level() == Level::kModerate,
        "heap pressure steps down below the critical exit boundary");
    policy.Evaluate(200, 1000, 599, 1000, Level::kNone, now + base::Seconds(5));
    Require(policy.level() == Level::kNone,
        "heap pressure clears only after its own hysteresis exits");
}

void PressureSourcesRetainIndependentHysteresis()
{
    using Action = content::RendererMemoryPressurePolicy::Action;
    using Level = content::RendererMemoryPressurePolicy::Level;
    const base::TimeTicks now = base::TimeTicks::Now();
    content::RendererMemoryPressurePolicy policy;
    Require(policy.Evaluate(850, 1000, 650, 1000, Level::kNone, now)
            == Action::kCritical,
        "process pressure dominates a healthy heap");
    policy.Evaluate(500, 1000, 650, 1000, Level::kNone, now + base::Seconds(1));
    Require(policy.level() == Level::kNone,
        "the heap cannot inherit the process hysteresis latch");
    policy.Evaluate(700, 1000, 850, 1000, Level::kNone, now + base::Seconds(61));
    Require(policy.level() == Level::kCritical,
        "the heap can become the stronger source");
    policy.Evaluate(650, 1000, 500, 1000, Level::kNone, now + base::Seconds(62));
    Require(policy.level() == Level::kModerate,
        "reclaiming the heap must not clear latched process pressure");
    policy.Evaluate(0, 0, 0, 0, Level::kNone, now + base::Seconds(63));
    Require(policy.level() == Level::kNone,
        "missing limits clear their own stale pressure state");
    Require(policy.Evaluate(0, 0, 850, 1000, Level::kNone, now + base::Seconds(121))
            == Action::kCritical,
        "V8 pressure still works when Job sampling is unavailable");
}

void OsPressureLevelsAndHeapRecoveryAreIndependent()
{
    using Policy = content::RendererMemoryPressurePolicy;
    using Action = Policy::Action;
    using Level = Policy::Level;
    const base::TimeTicks now = base::TimeTicks::Now();
    Policy policy;
    Require(policy.Evaluate(0, 0, 0, 0, Level::kModerate, now)
            == Action::kModerate,
        "OS warning produces moderate pressure without any heap limit");
    Require(policy.Evaluate(0, 0, 0, 0, Level::kModerate,
                now + base::Seconds(1)) == Action::kNone,
        "sustained OS warning does not repeat every tick");
    Require(policy.Evaluate(0, 0, 850, 1000, Level::kModerate,
                now + base::Seconds(2)) == Action::kCritical,
        "independent V8 pressure dominates an OS warning");
    Require(policy.Evaluate(0, 0, 599, 1000, Level::kModerate,
                now + base::Seconds(3)) == Action::kNone
            && policy.level() == Level::kModerate,
        "heap recovery retains only the current OS warning");
    policy.Evaluate(0, 0, 599, 1000, Level::kNone, now + base::Seconds(4));
    Require(policy.level() == Level::kNone,
        "normal or unknown OS pressure does not retain a stale warning");
    Require(policy.Evaluate(0, 0, 0, 0, Level::kCritical,
                now + base::Seconds(61)) == Action::kNone
            && policy.level() == Level::kCritical,
        "OS critical re-entry obeys the shared critical cooldown");
    Require(policy.Evaluate(0, 0, 0, 0, Level::kCritical,
                now + base::Seconds(62)) == Action::kCritical,
        "OS critical is delivered exactly at the cooldown boundary");
    policy.Evaluate(0, 0, 0, 0, Level::kNone, now + base::Seconds(63));
    Require(policy.level() == Level::kNone,
        "OS critical recovery clears without synthetic usage hysteresis");
}

#if BUILDFLAG(IS_MAC)
void MacAvailablePagesDoNotDoubleCountSpeculative()
{
    Require(atom::MacAvailableMemoryBytes(100, 30, 4096) == 409600,
        "speculative pages already in free are not counted twice");
    Require(atom::MacAvailableMemoryBytes(100, 100, 4096) == 409600,
        "speculative pages cannot double the available budget");
    Require(atom::MacAvailableMemoryBytes(30, 100, 4096) == 409600,
        "staggered counters preserve the larger speculative sample");
}
#endif

} // namespace

int main()
{
    DiscardableMemoryReclaimsOnlyUnlockedBuffers();
    DiscardableMemoryBudgetAdmissionIsExact();
    DiscardableMemoryDestructionAndRepeatedPurgeAreExact();
    DiscardableMemoryDumpsReportOnlyLiveBytes();
    DiscardableMemoryPurgeAndRelockRaceIsSafe();
    MemoryPressurePolicyUsesHysteresisAndCooldowns();
    HeapPressurePrecedesProcessPressure();
    PressureSourcesRetainIndependentHysteresis();
    OsPressureLevelsAndHeapRecoveryAreIndependent();
#if BUILDFLAG(IS_MAC)
    MacAvailablePagesDoNotDoubleCountSpeculative();
#endif
    std::puts("PASS: renderer discardable memory and pressure policy");
    return 0;
}
