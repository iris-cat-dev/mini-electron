#include "components/performance_manager/public/mojom/web_memory.mojom-blink.h"
#include "components/performance_manager/public/mojom/v8_contexts.mojom-blink.h"

#include <utility>

#include "base/trace_event/trace_event_stub.h"

namespace performance_manager::mojom::blink {

WebMemoryAttribution::WebMemoryAttribution() = default;
WebMemoryAttribution::WebMemoryAttribution(Scope scope_in,
                                           const WTF::String& url_in,
                                           const WTF::String& src_in,
                                           const WTF::String& id_in)
    : scope(std::move(scope_in)),
      url(std::move(url_in)),
      src(std::move(src_in)),
      id(std::move(id_in)) {}
WebMemoryAttribution::~WebMemoryAttribution() = default;
void WebMemoryAttribution::WriteIntoTrace(perfetto::TracedValue) const {}

WebMemoryUsage::WebMemoryUsage() = default;
WebMemoryUsage::WebMemoryUsage(uint64_t bytes_in) : bytes(bytes_in) {}
WebMemoryUsage::~WebMemoryUsage() = default;
size_t WebMemoryUsage::Hash(size_t seed) const {
  return seed ^ static_cast<size_t>(bytes);
}
void WebMemoryUsage::WriteIntoTrace(perfetto::TracedValue) const {}

WebMemoryBreakdownEntry::WebMemoryBreakdownEntry() = default;
WebMemoryBreakdownEntry::WebMemoryBreakdownEntry(
    WebMemoryUsagePtr memory_in,
    WebMemoryUsagePtr canvas_memory_in,
    WTF::Vector<WebMemoryAttributionPtr> attribution_in)
    : memory(std::move(memory_in)),
      canvas_memory(std::move(canvas_memory_in)),
      attribution(std::move(attribution_in)) {}
WebMemoryBreakdownEntry::~WebMemoryBreakdownEntry() = default;
void WebMemoryBreakdownEntry::WriteIntoTrace(perfetto::TracedValue) const {}

WebMemoryMeasurement::WebMemoryMeasurement() = default;
WebMemoryMeasurement::WebMemoryMeasurement(
    WTF::Vector<WebMemoryBreakdownEntryPtr> breakdown_in,
    WebMemoryUsagePtr detached_memory_in,
    WebMemoryUsagePtr shared_memory_in,
    WebMemoryUsagePtr blink_memory_in)
    : breakdown(std::move(breakdown_in)),
      detached_memory(std::move(detached_memory_in)),
      shared_memory(std::move(shared_memory_in)),
      blink_memory(std::move(blink_memory_in)) {}
WebMemoryMeasurement::~WebMemoryMeasurement() = default;
void WebMemoryMeasurement::WriteIntoTrace(perfetto::TracedValue) const {}

IframeAttributionData::IframeAttributionData() = default;
IframeAttributionData::IframeAttributionData(const WTF::String& id_in,
                                             const WTF::String& src_in)
    : id(id_in), src(src_in) {}
IframeAttributionData::~IframeAttributionData() = default;
void IframeAttributionData::WriteIntoTrace(perfetto::TracedValue) const {}

V8ContextDescription::V8ContextDescription() = default;
V8ContextDescription::V8ContextDescription(
    const ::blink::V8ContextToken& token_in,
    V8ContextWorldType world_type_in,
    const WTF::String& world_name_in,
    const std::optional<::blink::ExecutionContextToken>& execution_context_token_in)
    : token(token_in),
      world_type(world_type_in),
      world_name(world_name_in),
      execution_context_token(execution_context_token_in) {}
V8ContextDescription::~V8ContextDescription() = default;
void V8ContextDescription::WriteIntoTrace(perfetto::TracedValue) const {}

}  // namespace performance_manager::mojom::blink
