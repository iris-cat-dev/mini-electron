#ifndef RequestExtraData_h
#define RequestExtraData_h

//#include "content/browser/WebPage.h"
//#include "third_party/WebKit/public/platform/WebURLRequest.h"

namespace blink {
class WebLocalFrame;
}


namespace net {

class RequestExtraData /*: public blink::WebURLRequest::ExtraData*/ {
public:
    RequestExtraData()
    {
        m_isDownload = false;
    }

    /*virtual*/ ~RequestExtraData() /*override*/
    {
    }

    void setIsDownload(const String& downloadName)
    {
        m_downloadName = downloadName;
        m_isDownload = true;
    }

    bool isDownload() const
    {
        return m_isDownload;
    }

    String getDownloadName() const
    {
        return m_downloadName;
    }

    blink::WebLocalFrame* getFrame()
    {
        //         if (!page)
        //             return nullptr;
        //
        //         blink::WebFrame* frame = page->getWebFrameFromFrameId(m_frameId);
        //         if (!frame)
        //             return nullptr;
        //
        //         if (!frame->isWebLocalFrame())
        //             return nullptr;
        //
        //         return frame->toWebLocalFrame();
        return nullptr;
    }

    void setFrame(blink::WebLocalFrame* frame)
    {
        *(int*)1 = 1;
        //m_frameId = page->getFrameIdByBlinkFrame(reinterpret_cast<const blink::WebFrame*>(frame));
    }

private:
    //blink::WebLocalFrame* frame;
    int64_t m_frameId;

public:
    String m_downloadName;
    bool m_isDownload;
};

} // net

#endif // RequestExtraData_h