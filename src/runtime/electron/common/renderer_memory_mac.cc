// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#include "src/runtime/electron/common/renderer_memory_mac.h"

#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <sys/sysctl.h>

#include "base/apple/scoped_mach_port.h"

namespace atom {

bool ReadMacRendererMemoryStatus(MacRendererMemoryStatus* output)
{
    if (!output)
        return false;
    *output = {};
    MacRendererMemoryStatus status;
    size_t size = sizeof(status.physical_bytes);
    if (sysctlbyname("hw.memsize", &status.physical_bytes, &size, nullptr, 0)
        || size != sizeof(status.physical_bytes) || !status.physical_bytes) {
        return false;
    }

    base::apple::ScopedMachSendRight host(mach_host_self());
    vm_size_t page_size = 0;
    vm_statistics64_data_t vm {};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_page_size(host.get(), &page_size) != KERN_SUCCESS || !page_size
        || host_statistics64(host.get(), HOST_VM_INFO64,
               reinterpret_cast<host_info64_t>(&vm), &count) != KERN_SUCCESS
        || count < HOST_VM_INFO64_COUNT) {
        return false;
    }
    status.available_bytes = MacAvailableMemoryBytes(
        vm.free_count, vm.speculative_count, page_size);
    if (status.available_bytes > status.physical_bytes)
        return false;

    task_vm_info_data_t task {};
    count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO,
            reinterpret_cast<task_info_t>(&task), &count) != KERN_SUCCESS
        || count < TASK_VM_INFO_REV1_COUNT) {
        return false;
    }
    status.footprint_bytes = task.phys_footprint;

    int pressure = 0;
    size = sizeof(pressure);
    if (!sysctlbyname("kern.memorystatus_vm_pressure_level", &pressure, &size,
            nullptr, 0) && size == sizeof(pressure)) {
        switch (pressure) {
        case DISPATCH_MEMORYPRESSURE_NORMAL:
            status.pressure = MacMemoryPressureLevel::kNormal;
            break;
        case DISPATCH_MEMORYPRESSURE_WARN:
            status.pressure = MacMemoryPressureLevel::kWarning;
            break;
        case DISPATCH_MEMORYPRESSURE_CRITICAL:
            status.pressure = MacMemoryPressureLevel::kCritical;
            break;
        }
    }
    *output = status;
    return true;
}

} // namespace atom
