/*
*
* wolar@qq.com
* http://miniblink.net
* https://github.com/weolar/miniblink49
* https://miniblink.net/views/doc/index.html api文档地址
* licence Apache-2.0
*
*/

#ifndef MINI_ELECTRON_DEFINE_H
#define MINI_ELECTRON_DEFINE_H

#if defined(_WIN32) || defined(OS_LINUX_FOR_WIN)
#include "windows.h"
#else
#include "runtime/engine/public/platform_types.h"
#endif
#include <stdint.h> // for int64_t

//////////////////////////////////////////////////////////////////////////

#if defined(__clang__) || defined(__GNUC__)
#define MINI_ELECTRON_DLLEXPORT __attribute__((visibility("default")))
#else // MSVC
#define MINI_ELECTRON_DLLEXPORT __declspec(dllexport)
#endif

#if __SIZEOF_LONG__ == 8
#define MINI_ELECTRON_CALL_TYPE
#else
#define MINI_ELECTRON_CALL_TYPE __stdcall
#endif

typedef struct mini_electron_rect_impl {
    int x;
    int y;
    int w;
    int h;

#if defined(__cplusplus)
    mini_electron_rect_impl()
    {
        x = 0;
        y = 0;
        w = 0;
        h = 0;
    }
    mini_electron_rect_impl(int xVal, int yVal, int wVal, int hVal)
    {
        x = xVal;
        y = yVal;
        w = wVal;
        h = hVal;
    }
    mini_electron_rect_impl(const mini_electron_rect_impl& other)
    {
        x = other.x;
        y = other.y;
        w = other.w;
        h = other.h;
    }
#endif
} mini_electron_rect;

typedef struct mini_electron_point_impl {
    int x;
    int y;
} mini_electron_point;

typedef struct mini_electron_size_impl {
    int w;
    int h;
} mini_electron_size;

typedef enum {
    MINI_ELECTRON_LBUTTON = 0x01,
    MINI_ELECTRON_RBUTTON = 0x02,
    MINI_ELECTRON_SHIFT = 0x04,
    MINI_ELECTRON_CONTROL = 0x08,
    MINI_ELECTRON_MBUTTON = 0x10,
} mini_electron_mouse_flags;

typedef enum {
    MINI_ELECTRON_EXTENDED = 0x0100,
    MINI_ELECTRON_REPEAT = 0x4000,
} mini_electron_key_flags;

typedef enum {
    MINI_ELECTRON_MSG_MOUSEMOVE = 0x0200,
    MINI_ELECTRON_MSG_LBUTTONDOWN = 0x0201,
    MINI_ELECTRON_MSG_LBUTTONUP = 0x0202,
    MINI_ELECTRON_MSG_LBUTTONDBLCLK = 0x0203,
    MINI_ELECTRON_MSG_RBUTTONDOWN = 0x0204,
    MINI_ELECTRON_MSG_RBUTTONUP = 0x0205,
    MINI_ELECTRON_MSG_RBUTTONDBLCLK = 0x0206,
    MINI_ELECTRON_MSG_MBUTTONDOWN = 0x0207,
    MINI_ELECTRON_MSG_MBUTTONUP = 0x0208,
    MINI_ELECTRON_MSG_MBUTTONDBLCLK = 0x0209,
    MINI_ELECTRON_MSG_MOUSEWHEEL = 0x020A,
} mini_electron_mouse_msg;

#if !defined(__cplusplus)
#ifndef HAVE_WCHAR_T
typedef unsigned short WCHAR;
#endif
#endif

#include <stdbool.h>

#if defined(__cplusplus)
#define MINI_ELECTRON_EXTERN_C extern "C"
#else
#define MINI_ELECTRON_EXTERN_C
#endif

typedef char utf8;

typedef enum { MINI_ELECTRON_PROXY_NONE, MINI_ELECTRON_PROXY_HTTP, MINI_ELECTRON_PROXY_SOCKS4, MINI_ELECTRON_PROXY_SOCKS4A, MINI_ELECTRON_PROXY_SOCKS5, MINI_ELECTRON_PROXY_SOCKS5HOSTNAME } mini_electron_proxy_type;

typedef struct mini_electron_proxy_impl {
    mini_electron_proxy_type type;
    char hostname[100];
    unsigned short port;
    char username[50];
    char password[50];
} mini_electron_proxy;

typedef enum mini_electron_setting_mask_impl {
    MINI_ELECTRON_SETTING_PROXY = 1,
    MINI_ELECTRON_ENABLE_NODEJS = 1 << 3,
    MINI_ELECTRON_ENABLE_DISABLE_H5VIDEO = 1 << 4,
    MINI_ELECTRON_ENABLE_DISABLE_PDFVIEW = 1 << 5,
    MINI_ELECTRON_ENABLE_DISABLE_CC = 1 << 6,
    MINI_ELECTRON_ENABLE_ENABLE_EGLGLES2 = 1 << 7, // 测试功能，请勿使用
    MINI_ELECTRON_ENABLE_ENABLE_SWIFTSHAER = 1 << 8, // 测试功能，请勿使用
} mini_electron_setting_mask;

typedef void(MINI_ELECTRON_CALL_TYPE* mini_electron_on_blink_thread_init_callback)(void* param);

#define kMiniElectronVersion 20210809
#define kMiniElectronMaxVersion 20600319

typedef struct mini_electron_settings_impl {
    mini_electron_proxy proxy;
    unsigned int mask; // 208 offset
    mini_electron_on_blink_thread_init_callback blinkThreadInitCallback;
    void* blinkThreadInitCallbackParam;
    intptr_t version;
    const WCHAR* mainDllPath;
    HMODULE mainDllHandle;
    const char* config;
} mini_electron_settings;

typedef struct mini_electron_view_settings_impl {
    int size;
    unsigned int bgColor;
} mini_electron_view_settings;

typedef void* mini_electron_web_frame_handle;
typedef void* mini_electron_net_job;


typedef intptr_t mini_electron_web_view;

#define NULL_WEBVIEW 0

typedef BOOL (*mini_electron_cookie_visitor)(void* params, const char* name, const char* value, const char* domain,
    const char* path, // If |path| is non-empty only URLs at or below the path will get the cookie value.
    int secure, // If |secure| is true the cookie will only be sent for HTTPS requests.
    int httpOnly, // If |httponly| is true the cookie will only be sent for HTTP requests.
    int* expires // The cookie expiration date is only valid if |has_expires| is true.
);

typedef enum mini_electron_cookie_command_impl {
    mini_electron_cookie_command_clear_all_cookies,
    mini_electron_cookie_command_clear_session_cookies,
    mini_electron_cookie_command_flush_cookies_to_file,
    mini_electron_cookie_command_reload_cookies_from_file,
} mini_electron_cookie_command;

typedef enum mini_electron_navigation_type_impl {
    MINI_ELECTRON_NAVIGATION_TYPE_LINKCLICK,
    MINI_ELECTRON_NAVIGATION_TYPE_FORMSUBMITTE,
    MINI_ELECTRON_NAVIGATION_TYPE_BACKFORWARD,
    MINI_ELECTRON_NAVIGATION_TYPE_RELOAD,
    MINI_ELECTRON_NAVIGATION_TYPE_FORMRESUBMITT,
    MINI_ELECTRON_NAVIGATION_TYPE_OTHER
} mini_electron_navigation_type;

typedef enum mini_electron_cursor_info_type_impl {
    kMiniElectronCursorInfoPointer,
    kMiniElectronCursorInfoCross,
    kMiniElectronCursorInfoHand,
    kMiniElectronCursorInfoIBeam,
    kMiniElectronCursorInfoWait,
    kMiniElectronCursorInfoHelp,
    kMiniElectronCursorInfoEastResize,
    kMiniElectronCursorInfoNorthResize,
    kMiniElectronCursorInfoNorthEastResize,
    kMiniElectronCursorInfoNorthWestResize,
    kMiniElectronCursorInfoSouthResize,
    kMiniElectronCursorInfoSouthEastResize,
    kMiniElectronCursorInfoSouthWestResize,
    kMiniElectronCursorInfoWestResize,
    kMiniElectronCursorInfoNorthSouthResize,
    kMiniElectronCursorInfoEastWestResize,
    kMiniElectronCursorInfoNorthEastSouthWestResize,
    kMiniElectronCursorInfoNorthWestSouthEastResize,
    kMiniElectronCursorInfoColumnResize,
    kMiniElectronCursorInfoRowResize,
    kMiniElectronCursorInfoMiddlePanning,
    kMiniElectronCursorInfoEastPanning,
    kMiniElectronCursorInfoNorthPanning,
    kMiniElectronCursorInfoNorthEastPanning,
    kMiniElectronCursorInfoNorthWestPanning,
    kMiniElectronCursorInfoSouthPanning,
    kMiniElectronCursorInfoSouthEastPanning,
    kMiniElectronCursorInfoSouthWestPanning,
    kMiniElectronCursorInfoWestPanning,
    kMiniElectronCursorInfoMove,
    kMiniElectronCursorInfoVerticalText,
    kMiniElectronCursorInfoCell,
    kMiniElectronCursorInfoContextMenu,
    kMiniElectronCursorInfoAlias,
    kMiniElectronCursorInfoProgress,
    kMiniElectronCursorInfoNoDrop,
    kMiniElectronCursorInfoCopy,
    kMiniElectronCursorInfoNone,
    kMiniElectronCursorInfoNotAllowed,
    kMiniElectronCursorInfoZoomIn,
    kMiniElectronCursorInfoZoomOut,
    kMiniElectronCursorInfoGrab,
    kMiniElectronCursorInfoGrabbing,
    kMiniElectronCursorInfoCustom
} mini_electron_cursor_info_type;

typedef struct {
    int x;
    int y;
    int width;
    int height;

    BOOL menuBarVisible;
    BOOL statusBarVisible;
    BOOL toolBarVisible;
    BOOL locationBarVisible;
    BOOL scrollbarsVisible;
    BOOL resizable;
    BOOL fullscreen;
} mini_electron_window_features;

typedef struct mini_electron_print_settings_impl {
    int structSize;
    int dpi;
    int width;
    int height;
    int marginTop;
    int marginBottom;
    int marginLeft;
    int marginRight;
    BOOL isPrintPageHeadAndFooter;
    BOOL isPrintBackgroud;
    BOOL isLandscape;
    BOOL isPrintToMultiPage;
} mini_electron_print_settings;

struct mini_electron_string;
typedef struct mini_electron_string* mini_electron_string_ptr;

typedef struct mini_electron_mem_buf_impl {
    int unuse; // 这字段暂时没啥用
    void* data;
    size_t length;
} mini_electron_mem_buf;

typedef struct {
    struct Item {
        enum mini_electron_storage_type {
            // String data with an associated MIME type. Depending on the MIME type, there may be
            // optional metadata attributes as well.
            StorageTypeString,
            // Stores the name of one file being dragged into the renderer.
            StorageTypeFilename,
            // An image being dragged out of the renderer. Contains a buffer holding the image data
            // as well as the suggested name for saving the image to.
            StorageTypeBinaryData,
            // Stores the filesystem URL of one file being dragged into the renderer.
            StorageTypeFileSystemFile,
        } storageType;

        // Only valid when storageType == StorageTypeString.
        mini_electron_mem_buf* stringType;
        mini_electron_mem_buf* stringData;

        // Only valid when storageType == StorageTypeFilename.
        mini_electron_mem_buf* filenameData;
        mini_electron_mem_buf* displayNameData;

        // Only valid when storageType == StorageTypeBinaryData.
        mini_electron_mem_buf* binaryData;

        // Title associated with a link when stringType == "text/uri-list".
        // Filename when storageType == StorageTypeBinaryData.
        mini_electron_mem_buf* title;

        // Only valid when storageType == StorageTypeFileSystemFile.
        mini_electron_mem_buf* fileSystemURL;
        long long fileSystemFileSize;

        // Only valid when stringType == "text/html".
        mini_electron_mem_buf* baseURL;
    };

    struct Item* m_itemList;
    int m_itemListLength;

    int m_modifierKeyState; // State of Shift/Ctrl/Alt/Meta keys.
    mini_electron_mem_buf* m_filesystemId;
} mini_electron_web_drag_data;

typedef enum {
    mini_electron_web_drag_operation_none = 0,
    mini_electron_web_drag_operation_copy = 1,
    mini_electron_web_drag_operation_link = 2,
    mini_electron_web_drag_operation_generic = 4,
    mini_electron_web_drag_operation_private = 8,
    mini_electron_web_drag_operation_move = 16,
    mini_electron_web_drag_operation_delete = 32,
    mini_electron_web_drag_operation_every = 0xffffffff
} mini_electron_web_drag_operation;

typedef mini_electron_web_drag_operation mini_electron_web_drag_operations_mask;

typedef enum {
    MINI_ELECTRON_RESOURCE_TYPE_MAIN_FRAME = 0, // top level page
    MINI_ELECTRON_RESOURCE_TYPE_SUB_FRAME = 1, // frame or iframe
    MINI_ELECTRON_RESOURCE_TYPE_STYLESHEET = 2, // a CSS stylesheet
    MINI_ELECTRON_RESOURCE_TYPE_SCRIPT = 3, // an external script
    MINI_ELECTRON_RESOURCE_TYPE_IMAGE = 4, // an image (jpg/gif/png/etc)
    MINI_ELECTRON_RESOURCE_TYPE_FONT_RESOURCE = 5, // a font
    MINI_ELECTRON_RESOURCE_TYPE_SUB_RESOURCE = 6, // an "other" subresource.
    MINI_ELECTRON_RESOURCE_TYPE_OBJECT = 7, // an object (or embed) tag for a plugin,
    // or a resource that a plugin requested.
    MINI_ELECTRON_RESOURCE_TYPE_MEDIA = 8, // a media resource.
    MINI_ELECTRON_RESOURCE_TYPE_WORKER = 9, // the main resource of a dedicated
    // worker.
    MINI_ELECTRON_RESOURCE_TYPE_SHARED_WORKER = 10, // the main resource of a shared worker.
    MINI_ELECTRON_RESOURCE_TYPE_PREFETCH = 11, // an explicitly requested prefetch
    MINI_ELECTRON_RESOURCE_TYPE_FAVICON = 12, // a favicon
    MINI_ELECTRON_RESOURCE_TYPE_XHR = 13, // a XMLHttpRequest
    MINI_ELECTRON_RESOURCE_TYPE_PING = 14, // a ping request for <a ping>
    MINI_ELECTRON_RESOURCE_TYPE_SERVICE_WORKER = 15, // the main resource of a service worker.
    MINI_ELECTRON_RESOURCE_TYPE_LAST_TYPE
} mini_electron_resource_type;

typedef enum mini_electron_request_type_impl {
    kMiniElectronRequestTypeInvalidation,
    kMiniElectronRequestTypeGet,
    kMiniElectronRequestTypePost,
    kMiniElectronRequestTypePut,
} mini_electron_request_type;

typedef struct mini_electron_slist_impl {
    char* data;
    struct mini_electron_slist_impl* next;
} mini_electron_slist;

typedef enum mini_electron_menu_item_id_impl {
    kMiniElectronMenuSelectedAllId = 1 << 1,
    kMiniElectronMenuSelectedTextId = 1 << 2,
    kMiniElectronMenuUndoId = 1 << 3,
    kMiniElectronMenuCopyImageId = 1 << 4,
    kMiniElectronMenuInspectElementAtId = 1 << 5,
    kMiniElectronMenuCutId = 1 << 6,
    kMiniElectronMenuPasteId = 1 << 7,
    kMiniElectronMenuPrintId = 1 << 8,
    kMiniElectronMenuGoForwardId = 1 << 9,
    kMiniElectronMenuGoBackId = 1 << 10,
    kMiniElectronMenuReloadId = 1 << 11,
    kMiniElectronMenuSaveImageId = 1 << 12,
} mini_electron_menu_item_id;


//////////////////////////////////////////////////////////////////////////

typedef enum mini_electron_js_type_impl {
    kMiniElectronJsTypeNumber = 0,
    kMiniElectronJsTypeString = 1,
    kMiniElectronJsTypeBool = 2,
    //kMbJsTypeObject = 3,
    //kMbJsTypeFunction = 4,
    kMiniElectronJsTypeUndefined = 5,
    //kMbJsTypeArray = 6,
    kMiniElectronJsTypeNull = 7,
    kMiniElectronJsTypeV8Value = 8,
    kMiniElectronJsTypeFrame = 9, // 单独把dom的frame列出来，方便获取mbWebFrameHandle
} mini_electron_js_type;

typedef enum mini_electron_image_format_impl {
    kMiniElectronImageFormatPng = 0,
    kMiniElectronImageFormatJpg = 1,
    kMiniElectronImageFormatBmp = 2,
} mini_electron_image_format;

typedef long long mini_electron_js_value;
typedef void* mini_electron_js_exec_state;

typedef void(MINI_ELECTRON_CALL_TYPE* mini_electron_on_get_pdf_page_data_callback)(mini_electron_web_view webView, void* param, void* data, size_t size);

typedef void(MINI_ELECTRON_CALL_TYPE* mini_electron_run_js_callback)(mini_electron_web_view webView, void* param, mini_electron_js_exec_state es, mini_electron_js_value v);
typedef void(MINI_ELECTRON_CALL_TYPE* mini_electron_js_query_callback)(mini_electron_web_view webView, void* param, mini_electron_js_exec_state es, int64_t queryId, int customMsg, const utf8* request);
typedef void(MINI_ELECTRON_CALL_TYPE* mini_electron_js_query_ex_callback)(mini_electron_web_view webView, void* param, mini_electron_js_exec_state es, const mini_electron_js_value* val, int count);

typedef void(MINI_ELECTRON_CALL_TYPE* mini_electron_title_changed_callback)(mini_electron_web_view webView, void* param, const utf8* title);
typedef void(MINI_ELECTRON_CALL_TYPE* mini_electron_mouse_over_url_changed_callback)(mini_electron_web_view webView, void* param, const utf8* url);
typedef void(MINI_ELECTRON_CALL_TYPE* mini_electron_url_changed_callback)(mini_electron_web_view webView, void* param, const utf8* url, BOOL canGoBack, BOOL canGoForward);
typedef void(MINI_ELECTRON_CALL_TYPE* mini_electron_url_changed_callback2)(mini_electron_web_view webView, void* param, mini_electron_web_frame_handle frameId, const utf8* url);
typedef void(MINI_ELECTRON_CALL_TYPE* mini_electron_paint_updated_callback)(mini_electron_web_view webView, void* param, const HDC hdc, int x, int y, int cx, int cy);
typedef void(MINI_ELECTRON_CALL_TYPE* mini_electron_paint_bit_updated_callback)(mini_electron_web_view webView, void* param, const void* buffer, const mini_electron_rect* r, int width, int height);
typedef void(MINI_ELECTRON_CALL_TYPE* mini_electron_alert_box_callback)(mini_electron_web_view webView, void* param, const utf8* msg);
typedef BOOL(MINI_ELECTRON_CALL_TYPE* mini_electron_confirm_box_callback)(mini_electron_web_view webView, void* param, const utf8* msg);
typedef mini_electron_string_ptr(MINI_ELECTRON_CALL_TYPE* mini_electron_prompt_box_callback)(mini_electron_web_view webView, void* param, const utf8* msg, const utf8* defaultResult, BOOL* result);
typedef BOOL(MINI_ELECTRON_CALL_TYPE* mini_electron_navigation_callback)(mini_electron_web_view webView, void* param, mini_electron_navigation_type navigationType, const utf8* url);
typedef mini_electron_web_view(MINI_ELECTRON_CALL_TYPE* mini_electron_create_view_callback)(
    mini_electron_web_view webView, void* param, mini_electron_navigation_type navigationType, const utf8* url, const mini_electron_window_features* windowFeatures);
typedef void(MINI_ELECTRON_CALL_TYPE* mini_electron_document_ready_callback)(mini_electron_web_view webView, void* param, mini_electron_web_frame_handle frameId);
typedef void(MINI_ELECTRON_CALL_TYPE* mini_electron_load_url_finish_callback)(mini_electron_web_view webView, void* param, const utf8* url, mini_electron_net_job job, int len);
typedef void(MINI_ELECTRON_CALL_TYPE* mini_electron_load_url_headers_received_callback)(mini_electron_web_view webView, void* param, const char* url, mini_electron_net_job job);
typedef BOOL(MINI_ELECTRON_CALL_TYPE* mini_electron_close_callback)(mini_electron_web_view webView, void* param, void* unuse);
typedef BOOL(MINI_ELECTRON_CALL_TYPE* mini_electron_destroy_callback)(mini_electron_web_view webView, void* param, void* unuse);
typedef void(MINI_ELECTRON_CALL_TYPE* mini_electron_on_show_devtools_callback)(mini_electron_web_view webView, void* param);
typedef void(MINI_ELECTRON_CALL_TYPE* mini_electron_did_create_script_context_callback)(
    mini_electron_web_view webView, void* param, mini_electron_web_frame_handle frameId, void* context, int extensionGroup, int worldId);
typedef BOOL(MINI_ELECTRON_CALL_TYPE* mini_electron_net_response_callback)(mini_electron_web_view webView, void* param, const utf8* url, mini_electron_net_job job);
typedef void(MINI_ELECTRON_CALL_TYPE* mini_electron_thread_callback)(void* param1, void* param2);
typedef void(MINI_ELECTRON_CALL_TYPE* mini_electron_node_on_create_process_callback)(
    mini_electron_web_view webView, void* param, const WCHAR* applicationPath, const WCHAR* arguments, STARTUPINFOW* startup);

typedef enum { MINI_ELECTRON_LOADING_SUCCEEDED, MINI_ELECTRON_LOADING_FAILED, MINI_ELECTRON_LOADING_CANCELED } mini_electron_loading_result;

typedef void(MINI_ELECTRON_CALL_TYPE* mini_electron_loading_finish_callback)(
    mini_electron_web_view webView, void* param, mini_electron_web_frame_handle frameId, const utf8* url, mini_electron_loading_result result, const utf8* failedReason);
typedef BOOL(MINI_ELECTRON_CALL_TYPE* mini_electron_download_callback)(mini_electron_web_view webView, void* param, mini_electron_web_frame_handle frameId, const char* url, void* downloadJob);

typedef enum {
    mini_electron_level_debug = 4,
    mini_electron_level_log = 1,
    mini_electron_level_info = 5,
    mini_electron_level_warning = 2,
    mini_electron_level_error = 3,
    mini_electron_level_revoked_error = 6,
    mini_electron_level_last = mini_electron_level_revoked_error
} mini_electron_console_level;
typedef void(MINI_ELECTRON_CALL_TYPE* mini_electron_console_callback)(
    mini_electron_web_view webView, void* param, mini_electron_console_level level, const utf8* message, const utf8* sourceName, unsigned sourceLine, const utf8* stackTrace);

typedef void(MINI_ELECTRON_CALL_TYPE* mini_electron_on_call_ui_thread)(mini_electron_web_view webView, void* paramOnInThread);
typedef void(MINI_ELECTRON_CALL_TYPE* mini_electron_call_ui_thread)(mini_electron_web_view webView, mini_electron_on_call_ui_thread func, void* param);

typedef void(MINI_ELECTRON_CALL_TYPE* mini_electron_insert_css_by_frame_result_callback)(mini_electron_web_view webView, void* param, const utf8* key);

//mbNet--------------------------------------------------------------------------------------
typedef BOOL(MINI_ELECTRON_CALL_TYPE* mini_electron_load_url_begin_callback)(mini_electron_web_view webView, void* param, const char* url, void* job);
typedef void(MINI_ELECTRON_CALL_TYPE* mini_electron_load_url_end_callback)(mini_electron_web_view webView, void* param, const char* url, void* job, void* buf, int len);
typedef void(MINI_ELECTRON_CALL_TYPE* mini_electron_load_url_fail_callback)(mini_electron_web_view webView, void* param, const char* url, void* job);

typedef void(MINI_ELECTRON_CALL_TYPE* mini_electron_did_create_script_context_callback)(
    mini_electron_web_view webView, void* param, mini_electron_web_frame_handle frameId, void* context, int extensionGroup, int worldId);
typedef void(MINI_ELECTRON_CALL_TYPE* mini_electron_will_release_script_context_callback)(mini_electron_web_view webView, void* param, mini_electron_web_frame_handle frameId, void* context, int worldId);
typedef BOOL(MINI_ELECTRON_CALL_TYPE* mini_electron_net_response_callback)(mini_electron_web_view webView, void* param, const char* url, void* job);
typedef void(MINI_ELECTRON_CALL_TYPE* mini_electron_net_get_favicon_callback)(mini_electron_web_view webView, void* param, const utf8* url, mini_electron_mem_buf* buf);

typedef enum mini_electron_async_request_state_impl {
    MINI_ELECTRON_ASYNC_REQUEST_OK = 0,
    MINI_ELECTRON_ASYNC_REQUEST_FAIL = 1,
} mini_electron_async_request_state;

typedef void(MINI_ELECTRON_CALL_TYPE* mini_electron_can_go_back_forward_callback)(mini_electron_web_view webView, void* param, mini_electron_async_request_state state, BOOL b);
typedef void(MINI_ELECTRON_CALL_TYPE* mini_electron_get_cookie_callback)(mini_electron_web_view webView, void* param, mini_electron_async_request_state state, const utf8* cookie);

typedef void* v8ContextPtr;
typedef void* v8Isolate;

typedef void(MINI_ELECTRON_CALL_TYPE* mini_electron_get_source_callback)(mini_electron_web_view webView, void* param, const utf8* mhtml);
typedef void(MINI_ELECTRON_CALL_TYPE* mini_electron_get_content_as_markup_callback)(mini_electron_web_view webView, void* param, const utf8* content, size_t size);


typedef enum mini_electron_download_opt_impl {
    kMiniElectronDownloadOptCancel,
    kMiniElectronDownloadOptCacheData,
} mini_electron_download_opt;

typedef void(MINI_ELECTRON_CALL_TYPE* mini_electron_net_job_data_recv_callback)(void* ptr, mini_electron_net_job job, const char* data, int length);
typedef void(MINI_ELECTRON_CALL_TYPE* mini_electron_net_job_data_finish_callback)(void* ptr, mini_electron_net_job job, mini_electron_loading_result result);

typedef struct mini_electron_net_job_data_bind_impl {
    void* param;
    mini_electron_net_job_data_recv_callback recvCallback;
    mini_electron_net_job_data_finish_callback finishCallback;
} mini_electron_net_job_data_bind;

typedef void(MINI_ELECTRON_CALL_TYPE* mini_electron_popup_dialog_save_name_callback)(void* ptr, const WCHAR* filePath);
typedef mini_electron_string_ptr(MINI_ELECTRON_CALL_TYPE* mini_electron_net_begin_save_callback)(void* ptr, const char* filePath, bool isPathExists);

typedef struct mini_electron_download_bind_impl {
    void* param;
    mini_electron_net_job_data_recv_callback recvCallback;
    mini_electron_net_job_data_finish_callback finishCallback;
    mini_electron_popup_dialog_save_name_callback saveNameCallback;
    mini_electron_net_begin_save_callback beginSaveCallback;
} mini_electron_download_bind;

typedef struct mini_electron_file_filter_impl {
    const utf8* name; // 例如"image"、"Movies"
    const utf8* extensions; // 例如"jpg|png|gif"
} mini_electron_file_filter;

typedef enum mini_electron_dialog_properties_impl {
    kMiniElectronDialogPropertiesOpenFile = 1 << 1, // 允许选择文件
    kMiniElectronDialogPropertiesOpenDirectory = 1 << 2, // 允许选择文件夹
    kMiniElectronDialogPropertiesMultiSelections = 1 << 3, // 允许多选。
    kMiniElectronDialogPropertiesShowHiddenFiles = 1 << 4, // 显示对话框中的隐藏文件。
    kMiniElectronDialogPropertiesCreateDirectory = 1 << 5, // macOS - 允许你通过对话框的形式创建新的目录。
    kMiniElectronDialogPropertiesPromptToCreate
    = 1 << 6, // Windows - 如果输入的文件路径在对话框中不存在, 则提示创建。 这并不是真的在路径上创建一个文件，而是允许返回一些不存在的地址交由应用程序去创建。
    kMiniElectronDialogPropertiesNoResolveAliases = 1 << 7, // macOS - 禁用自动的别名路径(符号链接) 解析。 所选别名现在将会返回别名路径而非其目标路径。
    kMiniElectronDialogPropertiesTreatPackageAsDirectory = 1 << 8, // macOS - 将包(如.app 文件夹) 视为目录而不是文件。
    kMiniElectronDialogPropertiesDontAddToRecent = 1 << 9, // Windows - 不要将正在打开的项目添加到最近的文档列表中。
} mini_electron_dialog_properties;

typedef struct mini_electron_dialog_options_impl {
    int magic; // 'mbdo'
    const utf8* title;
    const utf8* defaultPath;
    const utf8* buttonLabel;
    mini_electron_file_filter* filters;
    int filtersCount;
    mini_electron_dialog_properties prop;
    const utf8* message;
    BOOL securityScopedBookmarks;
} mini_electron_dialog_options;

typedef struct mini_electron_download_options_impl {
    int magic; // 'mbdo'
    BOOL saveAsPathAndName;
} mini_electron_download_options;

typedef mini_electron_download_opt(MINI_ELECTRON_CALL_TYPE* mini_electron_download_in_blink_thread_callback)(mini_electron_web_view webView, void* param, size_t expectedContentLength, const char* url,
    const char* mime, const char* disposition, mini_electron_net_job job, mini_electron_net_job_data_bind* dataBind);

typedef struct mini_electron_pdf_datas_impl {
    int count;
    size_t* sizes;
    const void** datas;
} mini_electron_pdf_datas;

typedef void(MINI_ELECTRON_CALL_TYPE* mini_electron_print_pdf_data_callback)(mini_electron_web_view webview, void* param, const mini_electron_pdf_datas* datas);

typedef struct mini_electron_screenshot_settings_impl {
    int structSize;
    int width;
    int height;
} mini_electron_screenshot_settings;

typedef void(MINI_ELECTRON_CALL_TYPE* mini_electron_print_bitmap_callback)(mini_electron_web_view webview, void* param, const char* data, size_t size);
typedef void(MINI_ELECTRON_CALL_TYPE* mini_electron_on_screenshot)(mini_electron_web_view webView, void* param, const char* data, size_t size);

typedef enum mini_electron_htt_body_element_type_impl {
    mini_electron_http_body_element_type_data,
    mini_electron_http_body_element_type_file,
} mini_electron_http_body_element_type;

typedef struct mini_electron_post_body_element_impl {
    int size;
    mini_electron_http_body_element_type type;
    mini_electron_mem_buf* data;
    mini_electron_string_ptr filePath;
    int64_t fileStart;
    int64_t fileLength; // -1 means to the end of the file.
} mini_electron_post_body_element;

typedef struct mini_electron_post_body_elements_impl {
    int size;
    mini_electron_post_body_element** element;
    size_t elementSize;
    bool isDirty;
} mini_electron_post_body_elements;

//mbwindow-----------------------------------------------------------------------------------
typedef enum mini_electron_window_type_impl { MINI_ELECTRON_WINDOW_TYPE_POPUP, MINI_ELECTRON_WINDOW_TYPE_TRANSPARENT, MINI_ELECTRON_WINDOW_TYPE_CONTROL } mini_electron_window_type;

typedef enum mini_electron_window_info_impl {
    MINI_ELECTRON_WINDOW_INFO_SHARTD_TEXTURE_ENABLE = 1 << 16,
} mini_electron_window_info;

typedef BOOL(MINI_ELECTRON_CALL_TYPE* mini_electron_window_closing_callback)(mini_electron_web_view webview, void* param);
typedef void(MINI_ELECTRON_CALL_TYPE* mini_electron_window_destroy_callback)(mini_electron_web_view webview, void* param);

typedef struct mini_electron_draggable_region_impl {
    RECT bounds;
    BOOL draggable;
} mini_electron_draggable_region;
typedef void(MINI_ELECTRON_CALL_TYPE* mini_electron_draggable_regions_changed_callback)(mini_electron_web_view webview, void* param, const mini_electron_draggable_region* rects, int rectCount);

typedef enum mini_electron_printint_step_impl {
    kPrintintStepStart,
    kPrintintStepPreview,
    kPrintintStepPrinting,
} mini_electron_printint_step;

typedef struct mini_electron_printint_settings_impl {
    int dpi;
    int width;
    int height;
    float scale;
} mini_electron_printint_settings;

typedef struct mini_electron_default_printer_settings_impl {
    int structSize; // 默认是4 * 10
    BOOL isLandscape; // 是否为横向打印格式
    BOOL isPrintHeadFooter; //
    BOOL isPrintBackgroud; // 是否打印背景
    int edgeDistanceLeft; // 上边距单位：毫米
    int edgeDistanceTop;
    int edgeDistanceRight;
    int edgeDistanceBottom;
    int copies; // 默认打印份数
    int paperType; // DMPAPER_A4等
#if defined(__cplusplus)
    inline mini_electron_default_printer_settings_impl();
#endif
} mini_electron_default_printer_settings;

typedef BOOL(MINI_ELECTRON_CALL_TYPE* mini_electron_printing_callback)(mini_electron_web_view webview, void* param, mini_electron_printint_step step, HDC hDC, const mini_electron_printint_settings* settings, int pageCount);

typedef mini_electron_string_ptr(MINI_ELECTRON_CALL_TYPE* mini_electron_image_buffer_to_data_url_callback)(mini_electron_web_view webView, void* param, const char* data, size_t size);

typedef struct mini_electron_will_send_request_info_impl {
    mini_electron_string_ptr url;
    mini_electron_string_ptr newUrl;
    mini_electron_resource_type resourceType;
    int httpResponseCode;
    mini_electron_string_ptr method;
    mini_electron_string_ptr referrer;
    void* headers;
} mini_electron_will_send_request_info;

typedef enum mini_electron_view_load_type_impl {
    MINI_ELECTRON_DID_START_LOADING,
    MINI_ELECTRON_DID_STOP_LOADING,
    MINI_ELECTRON_DID_NAVIGATE,
    MINI_ELECTRON_DID_NAVIGATE_IN_PAGE,
    MINI_ELECTRON_DID_GET_RESPONSE_DETAILS,
    MINI_ELECTRON_DID_GET_REDIRECT_REQUEST,
    MINI_ELECTRON_DID_POST_REQUEST,
} mini_electron_view_load_type;

typedef struct mini_electron_view_load_callback_info_impl {
    int size;
    mini_electron_web_frame_handle frame;
    mini_electron_will_send_request_info* willSendRequestInfo;
    const char* url;
    mini_electron_post_body_elements* postBody;
    mini_electron_net_job job;
} mini_electron_view_load_callback_info;

typedef void(MINI_ELECTRON_CALL_TYPE* mini_electron_net_view_load_info_callback)(mini_electron_web_view webView, void* param, mini_electron_view_load_type type, mini_electron_view_load_callback_info* info);

//JavaScript Bind-----------------------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////


#define MINI_ELECTRON_DECLARE_ITERATOR0(returnVal, name, description) MINI_ELECTRON_EXTERN_C MINI_ELECTRON_DLLEXPORT returnVal MINI_ELECTRON_CALL_TYPE name();

#define MINI_ELECTRON_DECLARE_ITERATOR1(returnVal, name, p1, description) MINI_ELECTRON_EXTERN_C MINI_ELECTRON_DLLEXPORT returnVal MINI_ELECTRON_CALL_TYPE name(p1);

#define MINI_ELECTRON_DECLARE_ITERATOR2(returnVal, name, p1, p2, description) MINI_ELECTRON_EXTERN_C MINI_ELECTRON_DLLEXPORT returnVal MINI_ELECTRON_CALL_TYPE name(p1, p2);

#define MINI_ELECTRON_DECLARE_ITERATOR3(returnVal, name, p1, p2, p3, description) MINI_ELECTRON_EXTERN_C MINI_ELECTRON_DLLEXPORT returnVal MINI_ELECTRON_CALL_TYPE name(p1, p2, p3);

#define MINI_ELECTRON_DECLARE_ITERATOR4(returnVal, name, p1, p2, p3, p4, description) MINI_ELECTRON_EXTERN_C MINI_ELECTRON_DLLEXPORT returnVal MINI_ELECTRON_CALL_TYPE name(p1, p2, p3, p4);

#define MINI_ELECTRON_DECLARE_ITERATOR5(returnVal, name, p1, p2, p3, p4, p5, description) MINI_ELECTRON_EXTERN_C MINI_ELECTRON_DLLEXPORT returnVal MINI_ELECTRON_CALL_TYPE name(p1, p2, p3, p4, p5);

#define MINI_ELECTRON_DECLARE_ITERATOR6(returnVal, name, p1, p2, p3, p4, p5, p6, description) MINI_ELECTRON_EXTERN_C MINI_ELECTRON_DLLEXPORT returnVal MINI_ELECTRON_CALL_TYPE name(p1, p2, p3, p4, p5, p6);

#define MINI_ELECTRON_DECLARE_ITERATOR7(returnVal, name, p1, p2, p3, p4, p5, p6, p7, description)                                                                         \
    MINI_ELECTRON_EXTERN_C MINI_ELECTRON_DLLEXPORT returnVal MINI_ELECTRON_CALL_TYPE name(p1, p2, p3, p4, p5, p6, p7);

#define MINI_ELECTRON_DECLARE_ITERATOR8(returnVal, name, p1, p2, p3, p4, p5, p6, p7, p8, description)                                                                     \
    MINI_ELECTRON_EXTERN_C MINI_ELECTRON_DLLEXPORT returnVal MINI_ELECTRON_CALL_TYPE name(p1, p2, p3, p4, p5, p6, p7, p8);

#define MINI_ELECTRON_DECLARE_ITERATOR9(returnVal, name, p1, p2, p3, p4, p5, p6, p7, p8, p9, description)                                                                 \
    MINI_ELECTRON_EXTERN_C MINI_ELECTRON_DLLEXPORT returnVal MINI_ELECTRON_CALL_TYPE name(p1, p2, p3, p4, p5, p6, p7, p8, p9);

#define MINI_ELECTRON_DECLARE_ITERATOR10(returnVal, name, p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, description)                                                           \
    MINI_ELECTRON_EXTERN_C MINI_ELECTRON_DLLEXPORT returnVal MINI_ELECTRON_CALL_TYPE name(p1, p2, p3, p4, p5, p6, p7, p8, p9, p10);

#define MINI_ELECTRON_DECLARE_ITERATOR11(returnVal, name, p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11, description)                                                      \
    MINI_ELECTRON_EXTERN_C MINI_ELECTRON_DLLEXPORT returnVal MINI_ELECTRON_CALL_TYPE name(p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11);

// ---


// 以下是mb的导出函数。格式按照【返回类型】【函数名】【参数】来排列
#define MINI_ELECTRON_FOR_EACH_DEFINE_FUNCTION(                                                                                                                           \
    ITERATOR0, ITERATOR1, ITERATOR2, ITERATOR3, ITERATOR4, ITERATOR5, ITERATOR6, ITERATOR7, ITERATOR8, ITERATOR9, ITERATOR10, ITERATOR11)                      \
    ITERATOR0(void, mini_electron_uninit, "")                                                                                                                              \
    ITERATOR0(mini_electron_settings*, mini_electron_create_init_settings, "方便c#等其他语言创建setting结构体")                                                                          \
    ITERATOR3(void, mini_electron_set_init_settings, mini_electron_settings* settings, const char* name, const char* value, "")                                                          \
    ITERATOR0(mini_electron_web_view, mini_electron_create_web_view, "")                                                                                                                  \
    ITERATOR7(mini_electron_web_view, mini_electron_create_web_view_bind_gtk_window, void* rootWindow, void* drawingArea, const char* type, DWORD style, DWORD styleEx, int width,           \
        int height, "用于GTK绑定窗口")                                                                                                                         \
    ITERATOR1(void, mini_electron_destroy_web_view, mini_electron_web_view, "")                                                                                                           \
    ITERATOR6(mini_electron_web_view, mini_electron_create_web_window, mini_electron_window_type type, HWND parent, int x, int y, int width, int height, "")                                           \
    ITERATOR7(mini_electron_web_view, mini_electron_create_web_window_ex, mini_electron_window_type type, HWND parent, int x, int y, int width, int height, const mini_electron_view_settings* settings, "")         \
    ITERATOR7(mini_electron_web_view, mini_electron_create_web_custom_window, HWND parent, DWORD style, DWORD styleEx, int x, int y, int width, int height, "")                            \
    ITERATOR5(void, mini_electron_move_window, mini_electron_web_view webview, int x, int y, int w, int h, "")                                                                           \
    ITERATOR1(void, mini_electron_move_to_center, mini_electron_web_view webview, "")                                                                                                     \
    ITERATOR2(void, mini_electron_set_auto_draw_to_hwnd, mini_electron_web_view webview, BOOL b, "离屏模式下控制是否自动上屏")                                                              \
    ITERATOR2(void, mini_electron_get_caret_rect, mini_electron_web_view webviewHandle, mini_electron_rect* r, "")                                                                                    \
                                                                                                                                                               \
                                                                                                                                                               \
    ITERATOR2(mini_electron_string_ptr, mini_electron_create_string, const utf8* str, size_t length, "不拷贝字符串，只引用")                                                             \
    ITERATOR2(mini_electron_string_ptr, mini_electron_create_string_with_copy, const utf8* str, size_t length, "拷贝字符串")                                                               \
    ITERATOR2(mini_electron_string_ptr, mini_electron_create_string_without_null_termination, const utf8* str, size_t length, "")                                                           \
    ITERATOR1(void, mini_electron_delete_string, mini_electron_string_ptr str, "")                                                                                                       \
    ITERATOR1(size_t, mini_electron_get_string_len, mini_electron_string_ptr str, "")                                                                                                     \
    ITERATOR1(const utf8*, mini_electron_get_string, mini_electron_string_ptr str, "")                                                                                                   \
                                                                                                                                                               \
    ITERATOR2(void, mini_electron_set_proxy, mini_electron_web_view webView, const mini_electron_proxy* proxy, "")                                                                                   \
    ITERATOR3(void, mini_electron_set_debug_config, mini_electron_web_view webView, const char* debugString, const char* param, "")                                                       \
                                                                                                                                                               \
    ITERATOR3(void, mini_electron_net_set_data, mini_electron_net_job jobPtr, void* buf, int len,                                                                                         \
        "调用此函数后,网络层收到数据会存储在一buf内,接收数据完成后响应OnLoadUrlEnd事件.#此调用严重影响性能,慎用"      \
        "此函数和mini_electron_net_set_data的区别是，mini_electron_net_hook_request会在接受到真正网络数据后再调用回调，并允许回调修改网络数据。"    \
        "而mini_electron_net_set_data是在网络数据还没发送的时候修改")                                                                                                        \
    ITERATOR1(void, mini_electron_net_hook_request, mini_electron_net_job jobPtr, "")                                                                                                     \
    ITERATOR2(void, mini_electron_net_change_request_url, mini_electron_net_job jobPtr, const char* url, "")                                                                               \
    ITERATOR1(void, mini_electron_net_continue_job, mini_electron_net_job jobPtr, "")                                                                                                     \
    ITERATOR1(const mini_electron_slist*, mini_electron_net_get_raw_http_head_in_blink_thread, mini_electron_net_job jobPtr, "")                                                                           \
    ITERATOR1(const mini_electron_slist*, mini_electron_net_get_raw_response_head_in_blink_thread, mini_electron_net_job jobPtr, "")                                                                       \
    ITERATOR1(void, mini_electron_net_hold_job_to_asyn_commit, mini_electron_net_job jobPtr, "")                                                                                             \
    ITERATOR1(void, mini_electron_net_cancel_request, mini_electron_net_job jobPtr, "")                                                                                                   \
    ITERATOR3(void, mini_electron_net_on_response, mini_electron_web_view webviewHandle, mini_electron_net_response_callback callback, void* param, "注意此接口的回调是在另外个线程")                   \
                                                                                                                                                               \
                                                                                                                                                               \
                                                                                                                                                               \
    ITERATOR1(mini_electron_post_body_elements*, mini_electron_net_get_post_body, mini_electron_net_job jobPtr, "")                                                                                      \
    ITERATOR2(mini_electron_post_body_elements*, mini_electron_net_create_post_body_elements, mini_electron_web_view webView, size_t length, "")                                                          \
    ITERATOR1(void, mini_electron_net_free_post_body_elements, mini_electron_post_body_elements* elements, "")                                                                               \
    ITERATOR1(mini_electron_post_body_element*, mini_electron_net_create_post_body_element, mini_electron_web_view webView, "")                                                                           \
    ITERATOR1(void, mini_electron_net_free_post_body_element, mini_electron_post_body_element* element, "")                                                                                  \
                                                                                                                                                               \
    ITERATOR1(mini_electron_request_type, mini_electron_net_get_request_method, mini_electron_net_job jobPtr, "")                                                                                       \
    ITERATOR2(void, mini_electron_set_view_proxy, mini_electron_web_view webView, const mini_electron_proxy* proxy, "")                                                                               \
    ITERATOR2(void, mini_electron_net_set_mime_type, mini_electron_net_job jobPtr, const char* type, "")                                                                                   \
    ITERATOR1(const char*, mini_electron_net_get_mime_type, mini_electron_net_job jobPtr, "只能在blink线程调用（非主线程）")                                                               \
    ITERATOR3(const utf8*, mini_electron_net_get_http_header_field, mini_electron_net_job job, const char* key, BOOL fromRequestOrResponse, "")                                             \
    ITERATOR4(void, mini_electron_net_set_http_header_field, mini_electron_net_job jobPtr, const WCHAR* key, const WCHAR* value, BOOL response, "")                                         \
    ITERATOR4(void, mini_electron_net_set_http_header_field_utf8, mini_electron_net_job jobPtr, const utf8* key, const utf8* value, BOOL response, "")                                       \
                                                                                                                                                               \
    ITERATOR2(void, mini_electron_set_context_menu_enabled, mini_electron_web_view webView, BOOL b, "")                                                                                    \
    ITERATOR2(void, mini_electron_set_navigation_to_new_window_enable, mini_electron_web_view webView, BOOL b, "")                                                                           \
    ITERATOR2(void, mini_electron_set_headless_enabled, mini_electron_web_view webView, BOOL b, "可以关闭渲染")                                                                           \
    ITERATOR3(void, mini_electron_set_context_menu_item_show, mini_electron_web_view webView, mini_electron_menu_item_id item, BOOL isShow, "设置某项menu是否显示")                                       \
                                                                                                                                                               \
    ITERATOR2(void, mini_electron_set_handle, mini_electron_web_view webView, HWND wnd, "")                                                                                              \
    ITERATOR1(void*, mini_electron_get_platform_window_handle, mini_electron_web_view webView, "linux下是gtk句柄，windows下是hwnd")                                                        \
    ITERATOR1(HWND, mini_electron_get_host_hwnd, mini_electron_web_view webView, "linux下永远返回nullptr，windows下是hwnd")                                                               \
    ITERATOR2(void, mini_electron_set_transparent, mini_electron_web_view webviewHandle, BOOL transparent, "")                                                                           \
    ITERATOR2(void, mini_electron_set_view_settings, mini_electron_web_view webviewHandle, const mini_electron_view_settings* settings, "")                                                            \
                                                                                                                                                               \
    ITERATOR3(void, mini_electron_set_cookie, mini_electron_web_view webView, const utf8* url, const utf8* cookie,                                                                       \
        "cookie格式必须是:PRODUCTINFO=webxpress; domain=.fidelity.com; path=/; secure")                                                                        \
    ITERATOR2(void, mini_electron_set_cookie_enabled, mini_electron_web_view webView, BOOL enable, "")                                                                                    \
    ITERATOR2(void, mini_electron_set_cookie_jar_path, mini_electron_web_view webView, const WCHAR* path, "")                                                                              \
    ITERATOR2(void, mini_electron_set_cookie_jar_full_path, mini_electron_web_view webView, const WCHAR* path, "")                                                                          \
    ITERATOR2(void, mini_electron_set_local_storage_full_path, mini_electron_web_view webView, const WCHAR* path, "")                                                                       \
    ITERATOR1(const utf8*, mini_electron_get_title, mini_electron_web_view webView, "")                                                                                                  \
    ITERATOR2(void, mini_electron_set_window_title, mini_electron_web_view webView, const utf8* title, "")                                                                                \
    ITERATOR2(void, mini_electron_set_window_title_w, mini_electron_web_view webView, const WCHAR* title, "")                                                                              \
    ITERATOR1(const utf8*, mini_electron_get_url, mini_electron_web_view webView, "")                                                                                                    \
    ITERATOR1(int, mini_electron_get_cursor_info_type, mini_electron_web_view webView, "")                                                                                                 \
    ITERATOR2(void, mini_electron_set_user_agent, mini_electron_web_view webView, const utf8* userAgent, "")                                                                              \
    ITERATOR2(void, mini_electron_set_zoom_factor, mini_electron_web_view webView, float factor, "")                                                                                      \
    ITERATOR1(float, mini_electron_get_zoom_factor, mini_electron_web_view webView, "")                                                                                                   \
                                                                                                                                                               \
    ITERATOR2(void, mini_electron_set_resource_gc, mini_electron_web_view webView, int intervalSec, "")                                                                                   \
                                                                                                                                                               \
    ITERATOR1(BOOL, mini_electron_is_loading, mini_electron_web_view webView, "")                                                                                                        \
    ITERATOR2(BOOL, mini_electron_can_go_back_or_forward, mini_electron_web_view webView, BOOL isGoBack, "")                                                                                \
    ITERATOR3(void, mini_electron_can_go_back, mini_electron_web_view webView, mini_electron_can_go_back_forward_callback callback, void* param, "")                                                      \
    ITERATOR3(void, mini_electron_can_go_forward, mini_electron_web_view webView, mini_electron_can_go_back_forward_callback callback, void* param, "")                                                   \
    ITERATOR3(void, mini_electron_get_cookie, mini_electron_web_view webView, mini_electron_get_cookie_callback callback, void* param, "")                                                             \
    ITERATOR1(const utf8*, mini_electron_get_cookie_on_blink_thread, mini_electron_web_view webView, "")                                                                                    \
    ITERATOR1(void, mini_electron_clear_cookie, mini_electron_web_view webView, "")                                                                                                      \
                                                                                                                                                               \
    ITERATOR3(void, mini_electron_resize, mini_electron_web_view webView, int w, int h, "")                                                                                             \
    ITERATOR2(void, mini_electron_get_size, mini_electron_web_view webView, mini_electron_rect* rc, "")                                                                                              \
    ITERATOR2(BOOL, mini_electron_get_window_rect, mini_electron_web_view webview, mini_electron_rect* rc, "")                                                                                        \
                                                                                                                                                               \
    ITERATOR3(void, mini_electron_on_navigation, mini_electron_web_view webView, mini_electron_navigation_callback callback, void* param, "")                                                         \
    ITERATOR3(void, mini_electron_on_create_view, mini_electron_web_view webView, mini_electron_create_view_callback callback, void* param, "")                                                         \
    ITERATOR3(void, mini_electron_on_document_ready, mini_electron_web_view webView, mini_electron_document_ready_callback callback, void* param, "")                                                   \
    ITERATOR3(void, mini_electron_on_paint_updated, mini_electron_web_view webView, mini_electron_paint_updated_callback callback, void* callbackParam, "")                                             \
    ITERATOR3(void, mini_electron_on_paint_bit_updated, mini_electron_web_view webView, mini_electron_paint_bit_updated_callback callback, void* callbackParam, "")                                       \
    ITERATOR3(void, mini_electron_on_load_url_begin, mini_electron_web_view webView, mini_electron_load_url_begin_callback callback, void* callbackParam, "")                                             \
    ITERATOR3(void, mini_electron_on_load_url_end, mini_electron_web_view webView, mini_electron_load_url_end_callback callback, void* callbackParam, "")                                                 \
    ITERATOR3(void, mini_electron_on_load_url_fail, mini_electron_web_view webView, mini_electron_load_url_fail_callback callback, void* callbackParam, "")                                               \
    ITERATOR3(void, mini_electron_on_title_changed, mini_electron_web_view webView, mini_electron_title_changed_callback callback, void* callbackParam, "")                                             \
    ITERATOR3(void, mini_electron_on_url_changed, mini_electron_web_view webView, mini_electron_url_changed_callback callback, void* callbackParam, "")                                                 \
    ITERATOR3(void, mini_electron_on_loading_finish, mini_electron_web_view webView, mini_electron_loading_finish_callback callback, void* param, "")                                                   \
    ITERATOR3(void, mini_electron_on_download, mini_electron_web_view webView, mini_electron_download_callback callback, void* param, "")                                                             \
    ITERATOR3(void, mini_electron_on_download_in_blink_thread, mini_electron_web_view webView, mini_electron_download_in_blink_thread_callback callback, void* param, "")                                   \
    ITERATOR3(void, mini_electron_on_alert_box, mini_electron_web_view webView, mini_electron_alert_box_callback callback, void* param, "")                                                             \
    ITERATOR3(void, mini_electron_on_confirm_box, mini_electron_web_view webView, mini_electron_confirm_box_callback callback, void* param, "")                                                         \
    ITERATOR3(void, mini_electron_on_prompt_box, mini_electron_web_view webView, mini_electron_prompt_box_callback callback, void* param, "")                                                           \
    ITERATOR3(void, mini_electron_on_console, mini_electron_web_view webView, mini_electron_console_callback callback, void* param, "")                                                               \
    ITERATOR3(BOOL, mini_electron_on_close, mini_electron_web_view webView, mini_electron_close_callback callback, void* param, "")                                                                   \
    ITERATOR3(void, mini_electron_on_did_create_script_context, mini_electron_web_view webView, mini_electron_did_create_script_context_callback callback, void* callbackParam, "")                         \
    ITERATOR3(void, mini_electron_on_will_release_script_context, mini_electron_web_view webView, mini_electron_will_release_script_context_callback callback, void* callbackParam, "")                     \
    ITERATOR3(void, mini_electron_on_image_buffer_to_data_url, mini_electron_web_view webView, mini_electron_image_buffer_to_data_url_callback callback, void* callbackParam, "")                             \
                                                                                                                                                               \
    ITERATOR1(void, mini_electron_go_back, mini_electron_web_view webView, "")                                                                                                           \
    ITERATOR1(void, mini_electron_go_forward, mini_electron_web_view webView, "")                                                                                                        \
    ITERATOR2(void, mini_electron_navigate_at_index, mini_electron_web_view webView, int index, "")                                                                                       \
    ITERATOR1(int, mini_electron_get_navigate_index, mini_electron_web_view webView, "")                                                                                                  \
    ITERATOR1(void, mini_electron_stop_loading, mini_electron_web_view webView, "")                                                                                                      \
    ITERATOR1(void, mini_electron_reload, mini_electron_web_view webView, "")                                                                                                           \
    ITERATOR2(void, mini_electron_perform_cookie_command, mini_electron_web_view webView, mini_electron_cookie_command command, "")                                                                    \
                                                                                                                                                               \
    ITERATOR1(void, mini_electron_editor_select_all, mini_electron_web_view webView, "")                                                                                                  \
    ITERATOR1(void, mini_electron_editor_copy, mini_electron_web_view webView, "")                                                                                                       \
    ITERATOR1(void, mini_electron_editor_cut, mini_electron_web_view webView, "")                                                                                                        \
    ITERATOR1(void, mini_electron_editor_paste, mini_electron_web_view webView, "")                                                                                                      \
    ITERATOR1(void, mini_electron_editor_delete, mini_electron_web_view webView, "")                                                                                                     \
    ITERATOR1(void, mini_electron_editor_undo, mini_electron_web_view webView, "")                                                                                                       \
                                                                                                                                                               \
    ITERATOR5(BOOL, mini_electron_fire_mouse_event, mini_electron_web_view webView, unsigned int message, int x, int y, unsigned int flags, "")                                           \
    ITERATOR4(BOOL, mini_electron_fire_context_menu_event, mini_electron_web_view webView, int x, int y, unsigned int flags, "")                                                           \
    ITERATOR5(BOOL, mini_electron_fire_mouse_wheel_event, mini_electron_web_view webView, int x, int y, int delta, unsigned int flags, "")                                                 \
    ITERATOR4(BOOL, mini_electron_fire_key_up_event, mini_electron_web_view webView, unsigned int virtualKeyCode, unsigned int flags, BOOL systemKey, "")                                  \
    ITERATOR4(BOOL, mini_electron_fire_key_down_event, mini_electron_web_view webView, unsigned int virtualKeyCode, unsigned int flags, BOOL systemKey, "")                                \
    ITERATOR4(BOOL, mini_electron_fire_key_press_event, mini_electron_web_view webView, unsigned int charCode, unsigned int flags, BOOL systemKey, "")                                     \
    ITERATOR6(BOOL, mini_electron_fire_windows_message, mini_electron_web_view webView, HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam, LRESULT* result, "")                       \
                                                                                                                                                               \
    ITERATOR1(void, mini_electron_set_focus, mini_electron_web_view webView, "")                                                                                                         \
    ITERATOR1(void, mini_electron_kill_focus, mini_electron_web_view webView, "")                                                                                                        \
                                                                                                                                                               \
    ITERATOR2(void, mini_electron_show_window, mini_electron_web_view webview, int show, "")                                                                                             \
                                                                                                                                                               \
    ITERATOR2(void, mini_electron_load_url, mini_electron_web_view webView, const utf8* url, "")                                                                                         \
    ITERATOR3(void, mini_electron_load_html_with_base_url, mini_electron_web_view webView, const utf8* html, const utf8* baseUrl, "")                                                       \
    ITERATOR4(void, mini_electron_post_url, mini_electron_web_view webView, const utf8* url, const char* postData, int postLen, "")                                                      \
                                                                                                                                                               \
    ITERATOR1(HDC, mini_electron_get_locked_view_dc, mini_electron_web_view webView, "")                                                                                                   \
    ITERATOR1(void, mini_electron_unlock_view_dc, mini_electron_web_view webView, "")                                                                                                     \
                                                                                                                                                               \
    ITERATOR1(void, mini_electron_wake, mini_electron_web_view webView, "")                                                                                                             \
                                                                                                                                                               \
    ITERATOR2(double, mini_electron_js_to_double, mini_electron_js_exec_state es, mini_electron_js_value v, "")                                                                                         \
    ITERATOR2(BOOL, mini_electron_js_to_boolean, mini_electron_js_exec_state es, mini_electron_js_value v, "")                                                                                          \
    ITERATOR2(const utf8*, mini_electron_js_to_string, mini_electron_js_exec_state es, mini_electron_js_value v, "")                                                                                    \
    ITERATOR2(mini_electron_web_frame_handle, mini_electron_js_to_web_frame_handle, mini_electron_js_exec_state es, mini_electron_js_value v, "")                                                                       \
    ITERATOR2(mini_electron_web_frame_handle, mini_electron_get_parent_web_frame_handle, mini_electron_web_view webView, mini_electron_web_frame_handle frame, "")                                                      \
    ITERATOR2(mini_electron_js_type, mini_electron_get_js_value_type, mini_electron_js_exec_state es, mini_electron_js_value v, "")                                                                                   \
    ITERATOR2(void, mini_electron_js_value_add_ref, mini_electron_js_exec_state es, mini_electron_js_value v, "")                                                                                        \
    ITERATOR2(void, mini_electron_js_value_deref, mini_electron_js_exec_state es, mini_electron_js_value v, "")                                                                                         \
    ITERATOR3(void, mini_electron_on_js_query, mini_electron_web_view webView, mini_electron_js_query_callback callback, void* param, "")                                                               \
    ITERATOR3(void, mini_electron_on_js_query_ex, mini_electron_web_view webView, mini_electron_js_query_ex_callback callback, void* param, "")                                                           \
    ITERATOR4(void, mini_electron_response_query, mini_electron_web_view webView, int64_t queryId, int customMsg, const utf8* response, "")                                              \
    ITERATOR7(void, mini_electron_run_js, mini_electron_web_view webView, mini_electron_web_frame_handle frameId, const utf8* script, BOOL isInClosure, mini_electron_run_js_callback callback, void* param,         \
        void* unuse, "")                                                                                                                                       \
    ITERATOR4(mini_electron_js_value, mini_electron_run_js_sync, mini_electron_web_view webView, mini_electron_web_frame_handle frameId, const utf8* script, BOOL isInClosure, "")                                   \
    ITERATOR1(mini_electron_web_frame_handle, mini_electron_web_frame_get_main_frame, mini_electron_web_view webView, "")                                                                                 \
    ITERATOR2(BOOL, mini_electron_is_main_frame, mini_electron_web_view webView, mini_electron_web_frame_handle frameId, "")                                                                            \
    ITERATOR2(void, mini_electron_set_node_js_enable, mini_electron_web_view webView, BOOL b, "")                                                                                          \
                                                                                                                                                               \
    ITERATOR5(void, mini_electron_set_device_parameter, mini_electron_web_view webView, const char* device, const char* paramStr, int paramInt, float paramFloat, "")                     \
                                                                                                                                                               \
    ITERATOR4(void, mini_electron_get_content_as_markup, mini_electron_web_view webView, mini_electron_get_content_as_markup_callback calback, void* param, mini_electron_web_frame_handle frameId, "")                  \
    ITERATOR3(void, mini_electron_get_source, mini_electron_web_view webView, mini_electron_get_source_callback calback, void* param, "")                                                              \
    ITERATOR2(mini_electron_mem_buf*, mini_electron_get_window_screenshot_sync, mini_electron_web_view webView, mini_electron_image_format format, "")                                                               \
    ITERATOR1(mini_electron_string_ptr, mini_electron_get_source_sync, mini_electron_web_view webView, "")                                                                                             \
    ITERATOR3(void, mini_electron_util_serialize_to_mhtml, mini_electron_web_view webView, mini_electron_get_source_callback calback, void* param, "")                                                   \
    ITERATOR3(BOOL, mini_electron_util_print, mini_electron_web_view webView, mini_electron_web_frame_handle frameId, const mini_electron_print_settings* printParams, "")                                          \
    ITERATOR2(mini_electron_mem_buf*, mini_electron_util_base64_encode_buffer, const void* str, int len, "对二进制数据进行basee64")                                                        \
    ITERATOR1(const utf8*, mini_electron_util_decode_url_escape, const utf8* url, "")                                                                                         \
    ITERATOR1(const utf8*, mini_electron_util_encode_url_escape, const utf8* url, "")                                                                                         \
    ITERATOR1(const mini_electron_mem_buf*, mini_electron_util_create_v8_snapshot, const utf8* str, "")                                                                                    \
    ITERATOR5(void, mini_electron_util_print_to_pdf, mini_electron_web_view webView, mini_electron_web_frame_handle frameId, const mini_electron_print_settings* settings, mini_electron_print_pdf_data_callback callback,           \
        void* param, "")                                                                                                                                       \
    ITERATOR5(void, mini_electron_util_print_to_bitmap, mini_electron_web_view webView, mini_electron_web_frame_handle frameId, const mini_electron_screenshot_settings* settings, mini_electron_print_bitmap_callback callback,    \
        void* param, "")                                                                                                                                       \
    ITERATOR4(void, mini_electron_util_screenshot, mini_electron_web_view webView, const mini_electron_screenshot_settings* settings, mini_electron_on_screenshot callback, void* param, "")                       \
    ITERATOR2(BOOL, mini_electron_utils_silent_print, mini_electron_web_view webView, const char* settings, "")                                                                           \
                                                                                                                                                               \
    ITERATOR9(mini_electron_download_opt, mini_electron_popup_dialog_and_download, mini_electron_web_view webView, const mini_electron_dialog_options* dialogOpt, size_t contentLength, const char* url,             \
        const char* mime, const char* disposition, mini_electron_net_job job, mini_electron_net_job_data_bind* dataBind, mini_electron_download_bind* callbackBind, "")                                 \
    ITERATOR10(mini_electron_download_opt, mini_electron_download_by_path, mini_electron_web_view webView, const mini_electron_download_options* downloadOptions, const WCHAR* path, size_t expectedContentLength,  \
        const char* url, const char* mime, const char* disposition, mini_electron_net_job job, mini_electron_net_job_data_bind* dataBind, mini_electron_download_bind* callbackBind, "")                \
    ITERATOR10(mini_electron_download_opt, mini_electron_download_by_utf8_path, mini_electron_web_view webView, const mini_electron_download_options* downloadOptions, const char* path,                             \
        size_t expectedContentLength, const char* url, const char* mime, const char* disposition, mini_electron_net_job job, mini_electron_net_job_data_bind* dataBind,                    \
        mini_electron_download_bind* callbackBind, "")                                                                                                                      \
                                                                                                                                                               \
    ITERATOR3(void, mini_electron_get_pdf_page_data, mini_electron_web_view webView, mini_electron_on_get_pdf_page_data_callback callback, void* param, "")                                                 \
                                                                                                                                                               \
    ITERATOR3(mini_electron_mem_buf*, mini_electron_create_mem_buf, mini_electron_web_view webView, void* buf, size_t length, "")                                                                      \
    ITERATOR1(void, mini_electron_free_mem_buf, mini_electron_mem_buf* buf, "")                                                                                                           \
                                                                                                                                                               \
    ITERATOR4(void, mini_electron_plugin_list_builder_add_plugin, void* builder, const utf8* name, const utf8* description, const utf8* fileName, "")                          \
    ITERATOR3(void, mini_electron_plugin_list_builder_add_media_type_to_last_plugin, void* builder, const utf8* name, const utf8* description, "")                                 \
    ITERATOR2(void, mini_electron_plugin_list_builder_add_file_extension_to_last_media_type, void* builder, const utf8* fileExtension, "")                                          \
                                                                                                                                                               \
    ITERATOR0(void, mini_electron_enable_high_dpi_support, "")                                                                                                                \
    ITERATOR0(void, mini_electron_run_message_loop, "")                                                                                                                      \
    ITERATOR0(void, mini_electron_exit_message_loop, "")                                                                                                                     \
    ITERATOR3(void, mini_electron_on_load_url_finish, mini_electron_web_view webView, mini_electron_load_url_finish_callback callback, void* callbackParam, "")                                           \
    ITERATOR3(void, mini_electron_on_load_url_headers_received, mini_electron_web_view webView, mini_electron_load_url_headers_received_callback callback, void* callbackParam, "")                         \
    ITERATOR3(void, mini_electron_on_document_ready_in_blink_thread, mini_electron_web_view webView, mini_electron_document_ready_callback callback, void* param, "")                                      \
    ITERATOR2(void, mini_electron_util_set_default_printer_settings, mini_electron_web_view webView, const mini_electron_default_printer_settings* setting, "")                                           \
    ITERATOR1(int, mini_electron_get_content_width, mini_electron_web_view webView, "")                                                                                                   \
    ITERATOR1(int, mini_electron_get_content_height, mini_electron_web_view webView, "")                                                                                                  \
    ITERATOR0(mini_electron_web_view, mini_electron_get_web_view_for_current_context, "") \
    ITERATOR0(mini_electron_web_frame_handle, mini_electron_get_web_frame_for_current_context, "") \
    ITERATOR5(BOOL, mini_electron_register_embedder_custom_element, mini_electron_web_view webviewHandle, mini_electron_web_frame_handle frameId, const char* name, void* options, void* outResult, "")  \
    ITERATOR3(void, mini_electron_on_node_create_process, mini_electron_web_view webviewHandle, mini_electron_node_on_create_process_callback callback, void* param, "")                                   \
    ITERATOR2(mini_electron_js_exec_state, mini_electron_get_global_exec_by_frame, mini_electron_web_view webView, mini_electron_web_frame_handle frameId, "")                                                          \
    ITERATOR2(void*, mini_electron_js_to_v8_value, mini_electron_js_exec_state es, mini_electron_js_value v, "")                                                                                         \
    ITERATOR3(void, mini_electron_on_thread_idle, mini_electron_thread_callback callback, void* param1, void* param2, "")                                                                 \
    ITERATOR3(void, mini_electron_on_blink_thread_init, mini_electron_thread_callback callback, void* param1, void* param2, "")                                                            \
    ITERATOR3(void, mini_electron_call_blink_thread_async, mini_electron_thread_callback callback, void* param1, void* param2, "")                                                         \
    ITERATOR3(void, mini_electron_call_blink_thread_sync, mini_electron_thread_callback callback, void* param1, void* param2, "")                                                          \
    ITERATOR3(void, mini_electron_call_ui_thread_sync, mini_electron_thread_callback callback, void* param1, void* param2, "")                                                             \
    ITERATOR3(void, mini_electron_call_ui_thread_async, mini_electron_thread_callback callback, void* param1, void* param2, "")                                                            \
    ITERATOR3(void, mini_electron_set_user_key_value, mini_electron_web_view webView, const char* key, void* value, "")                                                                    \
    ITERATOR2(void*, mini_electron_get_user_key_value, mini_electron_web_view webView, const char* key, "")                                                                                \
    ITERATOR2(void, mini_electron_go_to_offset, mini_electron_web_view webView, int offset, "")                                                                                           \
    ITERATOR2(void, mini_electron_go_to_index, mini_electron_web_view webView, int index, "")                                                                                             \
    ITERATOR1(void, mini_electron_editor_redo, mini_electron_web_view webView, "")                                                                                                       \
    ITERATOR1(void, mini_electron_editor_un_select, mini_electron_web_view webView, "")                                                                                                   \
    ITERATOR0(v8Isolate, mini_electron_get_blink_main_thread_isolate, "")                                                                                                      \
    ITERATOR3(void, mini_electron_insert_css_by_frame, mini_electron_web_view webView, mini_electron_web_frame_handle frameId, const utf8* cssText, "")                                                  \
    ITERATOR6(void, mini_electron_insert_css_by_frame_with_result, mini_electron_web_view webView, mini_electron_web_frame_handle frameId, const utf8* cssText, int cssOrigin, mini_electron_insert_css_by_frame_result_callback callback, void* param, "") \
    ITERATOR3(void, mini_electron_web_frame_get_main_world_script_context, mini_electron_web_view webView, mini_electron_web_frame_handle frameId, v8ContextPtr contextOut, "")                             \
    ITERATOR1(const char*, mini_electron_net_get_referrer, mini_electron_net_job jobPtr, "获取request的referrer")                                                                         \
    ITERATOR2(void, mini_electron_post_to_ui_thread, mini_electron_on_call_ui_thread callback, void* param, "")                                                                              \
    ITERATOR3(void, mini_electron_post_to_ui_thread_delay, mini_electron_on_call_ui_thread callback, void* param, size_t millisecond, "")                                                     \
    ITERATOR2(void, mini_electron_set_editable, mini_electron_web_view webView, bool editable, "")                                                                                       \
    ITERATOR2(void, mini_electron_set_language, mini_electron_web_view webView, const char* language, "")                                                                                       \
    ITERATOR2(int, mini_electron_query_state, mini_electron_web_view webviewHandle, const char* type, "")

MINI_ELECTRON_EXTERN_C MINI_ELECTRON_DLLEXPORT void MINI_ELECTRON_CALL_TYPE mini_electron_init(const mini_electron_settings* settings);

MINI_ELECTRON_FOR_EACH_DEFINE_FUNCTION(MINI_ELECTRON_DECLARE_ITERATOR0, MINI_ELECTRON_DECLARE_ITERATOR1, MINI_ELECTRON_DECLARE_ITERATOR2, MINI_ELECTRON_DECLARE_ITERATOR3, MINI_ELECTRON_DECLARE_ITERATOR4, MINI_ELECTRON_DECLARE_ITERATOR5,
    MINI_ELECTRON_DECLARE_ITERATOR6, MINI_ELECTRON_DECLARE_ITERATOR7, MINI_ELECTRON_DECLARE_ITERATOR8, MINI_ELECTRON_DECLARE_ITERATOR9, MINI_ELECTRON_DECLARE_ITERATOR10, MINI_ELECTRON_DECLARE_ITERATOR11)


#endif // MINI_ELECTRON_DEFINE_H
