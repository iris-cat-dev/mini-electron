
#ifndef content_renderer_DataHostImpl_h
#define content_renderer_DataHostImpl_h

#include "gen/components/attribution_reporting/data_host.mojom-blink.h"
#include "gen/components/attribution_reporting/registration_header_error.mojom-blink.h"

namespace attribution_reporting {

SourceRegistration::~SourceRegistration() = default;
TriggerRegistration::~TriggerRegistration() = default;
AggregatableNamedBudgetDefs::~AggregatableNamedBudgetDefs() = default;
SourceAggregatableDebugReportingConfig::~SourceAggregatableDebugReportingConfig() = default;
AggregationKeys::~AggregationKeys() = default;
FilterData::~FilterData() = default;
TriggerSpecs::~TriggerSpecs() = default;
DestinationSet::~DestinationSet() = default;
AttributionScopesSet::~AttributionScopesSet() = default;
AggregatableDebugReportingConfig::~AggregatableDebugReportingConfig() = default;
TriggerSpec::~TriggerSpec() = default;
FilterConfig::~FilterConfig() = default;
EventReportWindows::~EventReportWindows() = default;
AggregatableTriggerConfig::~AggregatableTriggerConfig() = default;
AttributionScopesData::~AttributionScopesData() = default;
AggregatableNamedBudgetCandidate::~AggregatableNamedBudgetCandidate() = default;
AggregatableValues::~AggregatableValues() = default;
FilterPair::~FilterPair() = default;
AggregatableTriggerData::~AggregatableTriggerData() = default;

}

namespace content {

class DataHostImpl : public ::attribution_reporting::mojom::blink::DataHost {
    void SourceDataAvailable(
        ::attribution_reporting::SuitableOrigin reporting_origin, 
        ::attribution_reporting::SourceRegistration data, 
        bool was_fetched_via_service_worker) override
    {
    }

    void TriggerDataAvailable(
        ::attribution_reporting::SuitableOrigin reporting_origin, 
        ::attribution_reporting::TriggerRegistration data, 
        bool was_fetched_via_service_worker)
        override
    {

    }

    void OsSourceDataAvailable(::std::vector<::attribution_reporting::OsRegistrationItem> registration, bool was_fetched_via_service_worker) override
    {

    }

    void OsTriggerDataAvailable(::std::vector<::attribution_reporting::OsRegistrationItem> registration, bool was_fetched_via_service_worker) override
    {

    }

    void ReportRegistrationHeaderError(::attribution_reporting::SuitableOrigin reporting_origin, ::attribution_reporting::RegistrationHeaderError error) override
    {

    }
};

}

#endif // content_renderer_DataHostImpl_h