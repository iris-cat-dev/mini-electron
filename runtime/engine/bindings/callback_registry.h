
#ifndef MINI_ELECTRON_ENGINE_BINDINGS_CALLBACK_REGISTRY_H_
#define MINI_ELECTRON_ENGINE_BINDINGS_CALLBACK_REGISTRY_H_

#include <functional>
#include "runtime/engine/public/engine_api.h"

namespace mini_electron::engine {

class CallbackClosure {

public:
    std::function<bool(mini_electron_navigation_type, const char*)>* m_navigationClosure { nullptr };
    void setNavigationClosure(std::function<bool(mini_electron_navigation_type, const char*)>* closure)
    {
        if (m_navigationClosure)
            delete m_navigationClosure;
        m_navigationClosure = closure;
    }

    std::function<void(mini_electron_web_frame_handle)>* m_documentReadyClosure { nullptr };
    void setDocumentReadyClosure(std::function<void(mini_electron_web_frame_handle)>* closure)
    {
        if (m_documentReadyClosure)
            delete m_documentReadyClosure;
        m_documentReadyClosure = closure;
    }

    std::function<mini_electron_web_view(mini_electron_navigation_type, const utf8*, const mini_electron_window_features*)>* m_createViewClosure { nullptr };
    void setCreateViewClosure(std::function<mini_electron_web_view(mini_electron_navigation_type, const utf8*, const mini_electron_window_features*)>* closure)
    {
        if (m_createViewClosure)
            delete m_createViewClosure;
        m_createViewClosure = closure;
    }

    std::function<bool(const char* url, void* job)>* m_loadUrlBeginClosure { nullptr };
    void setLoadUrlBeginClosure(std::function<bool(const char* url, void* job)>* closure)
    {
        if (m_loadUrlBeginClosure)
            delete m_loadUrlBeginClosure;
        m_loadUrlBeginClosure = closure;
    }

    std::function<void(const char* url, void*, void*, int)>* m_loadUrlEndClosure { nullptr };
    void setLoadUrlEndClosure(std::function<void(const char* url, void*, void*, int)>* closure)
    {
        if (m_loadUrlEndClosure)
            delete m_loadUrlEndClosure;
        m_loadUrlEndClosure = closure;
    }

    std::function<void(const char*)>* m_titleChangedClosure { nullptr };
    void setTitleChangedClosure(std::function<void(const char*)>* closure)
    {
        if (m_titleChangedClosure)
            delete m_titleChangedClosure;
        m_titleChangedClosure = closure;
    }

    // #define GEN_MB_CALLBACK(name) \
//     mb##name##Callback m_##name##Callback { nullptr }; \
//     void* m_##name##Param { nullptr }; \
//     void set##name##Callback(mb##name##Callback callback, void* param) { \
//         m_##name##Callback = callback; \
//         m_##name##Param = param; \
//     }
    //
    //     GEN_MB_CALLBACK(Navigation);
    //     GEN_MB_CALLBACK(DocumentReady);
    //     GEN_MB_CALLBACK(LoadUrlBegin);
    //     GEN_MB_CALLBACK(LoadUrlEnd);
    //     GEN_MB_CALLBACK(TitleChanged);
    //     GEN_MB_CALLBACK(URLChanged);
    //     GEN_MB_CALLBACK(LoadingFinish);
    //     GEN_MB_CALLBACK(PaintUpdated);
    //     GEN_MB_CALLBACK(CreateView);
    //     GEN_MB_CALLBACK(Download);
    //     GEN_MB_CALLBACK(DownloadInBlinkThread);
    //     GEN_MB_CALLBACK(AlertBox);
    //     GEN_MB_CALLBACK(ConfirmBox);
    //     GEN_MB_CALLBACK(PromptBox);
    //     GEN_MB_CALLBACK(Console);
    //     GEN_MB_CALLBACK(NetGetFavicon);
    //     GEN_MB_CALLBACK(Close);
    //
    // #undef GEN_MB_CALLBACK


    mini_electron_navigation_callback m_NavigationCallback { nullptr };
    void* m_NavigationParam { nullptr };
    void setNavigationCallback(mini_electron_navigation_callback callback, void* param)
    {
        m_NavigationCallback = callback;
        m_NavigationParam = param;
    };

    mini_electron_navigation_callback m_NavigationSyncCallback { nullptr };
    void* m_NavigationSyncParam { nullptr };
    void setNavigationSyncCallback(mini_electron_navigation_callback callback, void* param)
    {
        m_NavigationSyncCallback = callback;
        m_NavigationSyncParam = param;
    };

    mini_electron_document_ready_callback m_DocumentReadyInBlinkCallback { nullptr };
    void* m_DocumentReadyInBlinkParam { nullptr };
    void setDocumentReadyInBlinkCallback(mini_electron_document_ready_callback callback, void* param)
    {
        m_DocumentReadyInBlinkCallback = callback;
        m_DocumentReadyInBlinkParam = param;
    };

    mini_electron_document_ready_callback m_DocumentReadyCallback { nullptr };
    void* m_DocumentReadyParam { nullptr };
    void setDocumentReadyCallback(mini_electron_document_ready_callback callback, void* param)
    {
        m_DocumentReadyCallback = callback;
        m_DocumentReadyParam = param;
    };

    mini_electron_load_url_begin_callback m_LoadUrlBeginCallback { nullptr };
    void* m_LoadUrlBeginParam { nullptr };
    void setLoadUrlBeginCallback(mini_electron_load_url_begin_callback callback, void* param)
    {
        m_LoadUrlBeginCallback = callback;
        m_LoadUrlBeginParam = param;
    };

    mini_electron_load_url_end_callback m_LoadUrlEndCallback { nullptr };
    void* m_LoadUrlEndParam { nullptr };
    void setLoadUrlEndCallback(mini_electron_load_url_end_callback callback, void* param)
    {
        m_LoadUrlEndCallback = callback;
        m_LoadUrlEndParam = param;
    };

    mini_electron_load_url_fail_callback m_LoadUrlFailCallback { nullptr };
    void* m_LoadUrlFailParam { nullptr };
    void setLoadUrlFailCallback(mini_electron_load_url_fail_callback callback, void* param)
    {
        m_LoadUrlFailCallback = callback;
        m_LoadUrlFailParam = param;
    };

    mini_electron_title_changed_callback m_TitleChangedCallback { nullptr };
    void* m_TitleChangedParam { nullptr };
    void setTitleChangedCallback(mini_electron_title_changed_callback callback, void* param)
    {
        m_TitleChangedCallback = callback;
        m_TitleChangedParam = param;
    };

    mini_electron_mouse_over_url_changed_callback m_MouseOverUrlChangedCallback { nullptr };
    void* m_MouseOverUrlChangedParam { nullptr };
    void setMouseOverUrlChangedCallback(mini_electron_mouse_over_url_changed_callback callback, void* param)
    {
        m_MouseOverUrlChangedCallback = callback;
        m_MouseOverUrlChangedParam = param;
    };

    mini_electron_url_changed_callback m_URLChangedCallback { nullptr };
    void* m_URLChangedParam { nullptr };
    void setURLChangedCallback(mini_electron_url_changed_callback callback, void* param)
    {
        m_URLChangedCallback = callback;
        m_URLChangedParam = param;
    };

    mini_electron_loading_finish_callback m_LoadingFinishCallback { nullptr };
    void* m_LoadingFinishParam { nullptr };
    void setLoadingFinishCallback(mini_electron_loading_finish_callback callback, void* param)
    {
        m_LoadingFinishCallback = callback;
        m_LoadingFinishParam = param;
    };

    mini_electron_paint_updated_callback m_PaintUpdatedCallback { nullptr };
    void* m_PaintUpdatedParam { nullptr };
    void setPaintUpdatedCallback(mini_electron_paint_updated_callback callback, void* param)
    {
        m_PaintUpdatedCallback = callback;
        m_PaintUpdatedParam = param;
    };

    mini_electron_paint_bit_updated_callback m_PaintBitUpdatedCallback { nullptr };
    void* m_PaintBitUpdatedParam { nullptr };
    void setPaintBitUpdatedCallback(mini_electron_paint_bit_updated_callback callback, void* param)
    {
        m_PaintBitUpdatedCallback = callback;
        m_PaintBitUpdatedParam = param;
    };

    mini_electron_create_view_callback m_CreateViewCallback { nullptr };
    void* m_CreateViewParam { nullptr };
    void setCreateViewCallback(mini_electron_create_view_callback callback, void* param)
    {
        m_CreateViewCallback = callback;
        m_CreateViewParam = param;
    };

    mini_electron_download_callback m_DownloadCallback { nullptr };
    void* m_DownloadParam { nullptr };
    void setDownloadCallback(mini_electron_download_callback callback, void* param)
    {
        m_DownloadCallback = callback;
        m_DownloadParam = param;
    };

    mini_electron_download_in_blink_thread_callback m_DownloadInBlinkThreadCallback { nullptr };
    void* m_DownloadInBlinkThreadParam { nullptr };
    void setDownloadInBlinkThreadCallback(mini_electron_download_in_blink_thread_callback callback, void* param)
    {
        m_DownloadInBlinkThreadCallback = callback;
        m_DownloadInBlinkThreadParam = param;
    };

    mini_electron_alert_box_callback m_AlertBoxCallback { nullptr };
    void* m_AlertBoxParam { nullptr };
    void setAlertBoxCallback(mini_electron_alert_box_callback callback, void* param)
    {
        m_AlertBoxCallback = callback;
        m_AlertBoxParam = param;
    };

    mini_electron_confirm_box_callback m_ConfirmBoxCallback { nullptr };
    void* m_ConfirmBoxParam { nullptr };
    void setConfirmBoxCallback(mini_electron_confirm_box_callback callback, void* param)
    {
        m_ConfirmBoxCallback = callback;
        m_ConfirmBoxParam = param;
    };

    mini_electron_prompt_box_callback m_PromptBoxCallback { nullptr };
    void* m_PromptBoxParam { nullptr };
    void setPromptBoxCallback(mini_electron_prompt_box_callback callback, void* param)
    {
        m_PromptBoxCallback = callback;
        m_PromptBoxParam = param;
    };

    mini_electron_console_callback m_ConsoleCallback { nullptr };
    void* m_ConsoleParam { nullptr };
    void setConsoleCallback(mini_electron_console_callback callback, void* param)
    {
        m_ConsoleCallback = callback;
        m_ConsoleParam = param;
    };

    mini_electron_net_get_favicon_callback m_NetGetFaviconCallback { nullptr };
    void* m_NetGetFaviconParam { nullptr };
    void setNetGetFaviconCallback(mini_electron_net_get_favicon_callback callback, void* param)
    {
        m_NetGetFaviconCallback = callback;
        m_NetGetFaviconParam = param;
    };

    mini_electron_close_callback m_ClosingCallback { nullptr };
    void* m_ClosingParam { nullptr };
    void setCloseCallback(mini_electron_close_callback callback, void* param)
    {
        m_ClosingCallback = callback;
        m_ClosingParam = param;
    };

    mini_electron_did_create_script_context_callback m_DidCreateScriptContextCallback { nullptr };
    void* m_DidCreateScriptContextParam { nullptr };
    void setDidCreateScriptContextCallback(mini_electron_did_create_script_context_callback callback, void* param)
    {
        m_DidCreateScriptContextCallback = callback;
        m_DidCreateScriptContextParam = param;
    };

    mini_electron_will_release_script_context_callback m_WillReleaseScriptContextCallback { nullptr };
    void* m_WillReleaseScriptContextParam { nullptr };
    void setWillReleaseScriptContextCallback(mini_electron_will_release_script_context_callback callback, void* param)
    {
        m_WillReleaseScriptContextCallback = callback;
        m_WillReleaseScriptContextParam = param;
    };

    mini_electron_image_buffer_to_data_url_callback m_ImageBufferToDataURLCallback { nullptr };
    void* m_ImageBufferToDataURLParam { nullptr };
    void setImageBufferToDataURLCallback(mini_electron_image_buffer_to_data_url_callback callback, void* param)
    {
        m_ImageBufferToDataURLCallback = callback;
        m_ImageBufferToDataURLParam = param;
    };

    mini_electron_net_response_callback m_NetResponseCallback { nullptr };
    void* m_NetResponseParam { nullptr };
    void setNetResponseCallback(mini_electron_net_response_callback callback, void* param)
    {
        m_NetResponseCallback = callback;
        m_NetResponseParam = param;
    };

    std::function<void(mini_electron_js_exec_state es, int64_t queryId, int customMsg, const utf8* request)>* m_jsQueryClosure { nullptr };
    void setJsQueryClosure(std::function<void(mini_electron_js_exec_state es, int64_t queryId, int customMsg, const utf8* request)>* closure)
    {
        if (m_jsQueryClosure)
            delete m_jsQueryClosure;
        m_jsQueryClosure = closure;
    }

    std::function<void(mini_electron_js_exec_state es, const mini_electron_js_value* val, int count)>* m_jsQueryClosure2 { nullptr };
    void setJsQuery2Closure(std::function<void(mini_electron_js_exec_state es, const mini_electron_js_value* val, int count)>* closure)
    {
        if (m_jsQueryClosure2)
            delete m_jsQueryClosure2;
        m_jsQueryClosure2 = closure;
    }
};

}

#endif // CallbackClosure_h