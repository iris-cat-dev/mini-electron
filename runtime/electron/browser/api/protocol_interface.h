
#ifndef browser_api_ProtocolInterface_h
#define browser_api_ProtocolInterface_h

#include <cstdint>
#include <functional>
#include <string>

#include "base/values.h"
#include "runtime/electron/common/renderer_client.h"

namespace atom {

class ProtocolInterface {
public:
    static ProtocolInterface* inst()
    {
        return m_inst;
    }

    virtual bool isProtocolHandled(const std::string& scheme) = 0;
    using BrokerReply = std::function<void(base::Value::Dict, std::string)>;
    virtual void handleBrokerRequest(int contentsId, uint64_t frameId,
        const base::Value::Dict& payload, BrokerReply reply) = 0;
    virtual std::vector<RendererPrivilegedScheme> getPrivilegedSchemes() const = 0;
    virtual ~ProtocolInterface()
    {
    }

    virtual v8::Local<v8::Object> getWrapper(v8::Isolate* isolate) = 0;

protected:
    static ProtocolInterface* m_inst;
};

}

#endif // browser_api_ProtocolInterface_h