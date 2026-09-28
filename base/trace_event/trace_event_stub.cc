// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stddef.h>

#include <string>

#include "base/trace_event/trace_event_stub.h"
#include "base/trace_event/memory_allocator_dump.h"
#include "base/trace_event/process_memory_dump.h"
#include "base/trace_event/trace_log.h"

namespace base {
namespace trace_event {

ConvertableToTraceFormat::~ConvertableToTraceFormat() = default;

void TracedValue::AppendAsTraceFormat(std::string* out) const
{
}

MemoryDumpProvider::~MemoryDumpProvider() = default;

// static
constexpr const char* const MemoryDumpManager::kTraceCategory;

const char MemoryAllocatorDump::kNameSize[] = "size";
const char MemoryAllocatorDump::kNameObjectCount[] = "object_count";
const char MemoryAllocatorDump::kUnitsBytes[] = "bytes";
const char MemoryAllocatorDump::kUnitsObjects[] = "objects";
const char MemoryAllocatorDump::kTypeScalar[] = "scalar";
const char MemoryAllocatorDump::kTypeString[] = "string";

MemoryAllocatorDump::Entry::Entry() = default;
MemoryAllocatorDump::Entry::Entry(std::string name_in,
                                  std::string units_in,
                                  uint64_t value)
    : name(std::move(name_in)),
      units(std::move(units_in)),
      entry_type(kUint64),
      value_uint64(value) {}
MemoryAllocatorDump::Entry::Entry(std::string name_in,
                                  std::string units_in,
                                  std::string value)
    : name(std::move(name_in)),
      units(std::move(units_in)),
      entry_type(kString),
      value_uint64(0),
      value_string(std::move(value)) {}
MemoryAllocatorDump::Entry::Entry(Entry&&) noexcept = default;
MemoryAllocatorDump::Entry& MemoryAllocatorDump::Entry::operator=(Entry&&) =
    default;
bool MemoryAllocatorDump::Entry::operator==(const Entry& rhs) const {
  return name == rhs.name && units == rhs.units &&
         entry_type == rhs.entry_type && value_uint64 == rhs.value_uint64 &&
         value_string == rhs.value_string;
}

MemoryAllocatorDump::MemoryAllocatorDump(
    const std::string& absolute_name,
    MemoryDumpLevelOfDetail level,
    const MemoryAllocatorDumpGuid& guid)
    : absolute_name_(absolute_name),
      guid_(guid),
      level_of_detail_(level),
      flags_(kDefault) {}
MemoryAllocatorDump::~MemoryAllocatorDump() = default;
void MemoryAllocatorDump::AddScalar(const char* name,
                                    const char* units,
                                    uint64_t value) {
  entries_.emplace_back(name, units, value);
  cached_size_.reset();
}
void MemoryAllocatorDump::AddString(const char* name,
                                    const char* units,
                                    const std::string& value) {
  entries_.emplace_back(name, units, value);
}
uint64_t MemoryAllocatorDump::GetSizeInternal() const {
  if (cached_size_)
    return *cached_size_;
  for (const Entry& entry : entries_) {
    if (entry.entry_type == Entry::kUint64 && entry.name == kNameSize)
      return *(cached_size_ = entry.value_uint64);
  }
  return *(cached_size_ = 0);
}

ProcessMemoryDump::ProcessMemoryDump(const MemoryDumpArgs& args)
    : dump_args_(args) {}
ProcessMemoryDump::ProcessMemoryDump(ProcessMemoryDump&&) = default;
ProcessMemoryDump::~ProcessMemoryDump() = default;
ProcessMemoryDump& ProcessMemoryDump::operator=(ProcessMemoryDump&&) = default;
MemoryAllocatorDump* ProcessMemoryDump::CreateAllocatorDump(
    const std::string& name) {
  return CreateAllocatorDump(name, MemoryAllocatorDumpGuid(name));
}
MemoryAllocatorDump* ProcessMemoryDump::CreateAllocatorDump(
    const std::string& name,
    const MemoryAllocatorDumpGuid& guid) {
  auto dump = std::make_unique<MemoryAllocatorDump>(
      name, dump_args_.level_of_detail, guid);
  MemoryAllocatorDump* result = dump.get();
  allocator_dumps_[name] = std::move(dump);
  return result;
}
MemoryAllocatorDump* ProcessMemoryDump::CreateSharedGlobalAllocatorDump(
    const MemoryAllocatorDumpGuid& guid) {
  return CreateAllocatorDump("global/" + guid.ToString(), guid);
}
void ProcessMemoryDump::AddOwnershipEdge(
    const MemoryAllocatorDumpGuid& source,
    const MemoryAllocatorDumpGuid& target) {
  AddOwnershipEdge(source, target, 0);
}
void ProcessMemoryDump::AddOwnershipEdge(
    const MemoryAllocatorDumpGuid& source,
    const MemoryAllocatorDumpGuid& target,
    int importance) {
  allocator_dumps_edges_[source] = {source, target, importance, false};
}
void ProcessMemoryDump::AddSuballocation(
    const MemoryAllocatorDumpGuid& source,
    const std::string& target_name) {
  MemoryAllocatorDump* target = GetOrCreateAllocatorDump(target_name);
  AddOwnershipEdge(source, target->guid());
}
MemoryAllocatorDump* ProcessMemoryDump::GetAllocatorDump(
    const std::string& name) const {
  auto found = allocator_dumps_.find(name);
  return found == allocator_dumps_.end() ? nullptr : found->second.get();
}
MemoryAllocatorDump* ProcessMemoryDump::GetOrCreateAllocatorDump(
    const std::string& name) {
  if (MemoryAllocatorDump* dump = GetAllocatorDump(name))
    return dump;
  return CreateAllocatorDump(name);
}
void ProcessMemoryDump::CreateSharedMemoryOwnershipEdge(
    const MemoryAllocatorDumpGuid& source,
    const UnguessableToken&,
    int importance) {
  AddOwnershipEdge(source, MemoryAllocatorDumpGuid(1), importance);
}

TraceLog* TraceLog::GetInstance() {
  alignas(TraceLog) static unsigned char storage[sizeof(TraceLog)];
  return reinterpret_cast<TraceLog*>(storage);
}
void TraceLog::AddEnabledStateObserver(EnabledStateObserver*) {}
void TraceLog::RemoveEnabledStateObserver(EnabledStateObserver*) {}
void TraceLog::AddAsyncEnabledStateObserver(
    WeakPtr<AsyncEnabledStateObserver>) {}
void TraceLog::RemoveAsyncEnabledStateObserver(AsyncEnabledStateObserver*) {}

} // namespace trace_event
} // namespace base

namespace perfetto {

TracedDictionary TracedValue::WriteDictionary() &&
{
    return TracedDictionary();
}

TracedArray TracedValue::WriteArray() &&
{
    return TracedArray();
}

TracedArray TracedDictionary::AddArray(StaticString)
{
    return TracedArray();
}

TracedArray TracedDictionary::AddArray(DynamicString)
{
    return TracedArray();
}

TracedDictionary TracedDictionary::AddDictionary(StaticString)
{
    return TracedDictionary();
}

TracedDictionary TracedDictionary::AddDictionary(DynamicString)
{
    return TracedDictionary();
}

TracedArray TracedArray::AppendArray()
{
    return TracedArray();
}

TracedDictionary TracedArray::AppendDictionary()
{
    return TracedDictionary();
}

} // namespace perfetto
