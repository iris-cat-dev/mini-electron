// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>

#include "mini_ui/ui.h"

namespace {

void Log(std::string_view message)
{
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!output || output == INVALID_HANDLE_VALUE)
        output = GetStdHandle(STD_ERROR_HANDLE);
    if (output && output != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(output, message.data(), static_cast<DWORD>(message.size()), &written, nullptr);
        WriteFile(output, "\n", 1, &written, nullptr);
    }
}

double Now()
{
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

unsigned Modifiers()
{
    unsigned result = 0;
    if (GetKeyState(VK_SHIFT) & 0x8000) result |= mini_ui::shift_key;
    if (GetKeyState(VK_CONTROL) & 0x8000) result |= mini_ui::control_key;
    if (GetKeyState(VK_MENU) & 0x8000) result |= mini_ui::alt_key;
    if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) result |= mini_ui::meta_key;
    if (GetKeyState(VK_CAPITAL) & 1) result |= mini_ui::caps_lock;
    if (GetKeyState(VK_NUMLOCK) & 1) result |= mini_ui::num_lock;
    return result;
}

struct Application {
    std::unique_ptr<mini_ui::View> view;
    std::filesystem::path resource_root;
    std::filesystem::path capture;
    HWND window = nullptr;
    float scale = 1;
    char16_t high_surrogate = 0;
    bool smoke = false;
    bool failed = false;

    bool Report(const mini_ui::Error& error)
    {
        failed = true;
        Log("ERROR: " + error.message);
        return false;
    }

    bool Advance(mini_ui::View& target)
    {
        mini_ui::Error error;
        return target.Tick(Now(), &error) || Report(error);
    }

    bool Expect(mini_ui::View& target, std::string_view expression)
    {
        std::string result;
        mini_ui::Error error;
        if (!target.Evaluate(expression, "ui-example-smoke.js", &result, &error))
            return Report(error);
        if (result == "true")
            return true;
        failed = true;
        Log("FAIL: " + std::string(expression) + " => " + result);
        return false;
    }

    bool Wait(mini_ui::View& target, std::string_view expression)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        do {
            if (!Advance(target))
                return false;
            std::string result;
            mini_ui::Error error;
            if (!target.Evaluate(expression, "ui-example-wait.js", &result, &error))
                return Report(error);
            if (result == "true")
                return true;
            Sleep(1);
        } while (std::chrono::steady_clock::now() < deadline);
        return Expect(target, expression);
    }

    void Text(char16_t character)
    {
        if (!view)
            return;
        if (character >= 0xd800 && character <= 0xdbff) {
            high_surrogate = character;
            return;
        }
        mini_ui::KeyEvent event;
        event.type = mini_ui::KeyEvent::Type::text;
        event.modifiers = Modifiers();
        if (high_surrogate) {
            if (character >= 0xdc00 && character <= 0xdfff)
                event.text.push_back(high_surrogate);
            else
                event.text.push_back(u'\ufffd');
            high_surrogate = 0;
        }
        event.text.push_back(character);
        view->Key(event);
    }
};

bool SaveBitmap(const std::filesystem::path& path, mini_ui::FrameView frame)
{
    const std::uint64_t bytes = std::uint64_t(frame.width) * frame.height * 4;
    if (!frame.pixels || frame.width <= 0 || frame.height <= 0
        || bytes > std::numeric_limits<DWORD>::max() - sizeof(BITMAPFILEHEADER) - sizeof(BITMAPINFOHEADER))
        return false;
    BITMAPFILEHEADER file_header {};
    file_header.bfType = 0x4d42;
    file_header.bfOffBits = sizeof(file_header) + sizeof(BITMAPINFOHEADER);
    file_header.bfSize = file_header.bfOffBits + static_cast<DWORD>(bytes);
    BITMAPINFOHEADER bitmap {};
    bitmap.biSize = sizeof(bitmap);
    bitmap.biWidth = frame.width;
    bitmap.biHeight = -frame.height;
    bitmap.biPlanes = 1;
    bitmap.biBitCount = 32;
    bitmap.biCompression = BI_RGB;
    bitmap.biSizeImage = static_cast<DWORD>(bytes);
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(&file_header), sizeof(file_header));
    output.write(reinterpret_cast<const char*>(&bitmap), sizeof(bitmap));
    for (int y = 0; y < frame.height; ++y) {
        output.write(reinterpret_cast<const char*>(frame.pixels + y * frame.stride),
            std::streamsize(frame.width) * 4);
    }
    return output.good();
}

bool CheckCanvasResolution(Application& app, mini_ui::View& view)
{
    return app.Wait(view, R"JS((()=>{
        const canvas=document.getElementById('plot');
        const bounds=canvas.getBoundingClientRect(), style=getComputedStyle(canvas);
        const width=bounds.width-parseFloat(style.borderLeftWidth)-parseFloat(style.borderRightWidth);
        const height=bounds.height-parseFloat(style.borderTopWidth)-parseFloat(style.borderBottomWidth);
        const ratio=devicePixelRatio;
        if (width<80 || height<80
            || canvas.width!==Math.round(width*ratio)
            || canvas.height!==Math.round(height*ratio)) return false;
        const sx=canvas.width/width, sy=canvas.height/height;
        const pixels=canvas.getContext('2d').getImageData(
            Math.floor((width-48)*sx),Math.floor(41*sy),
            Math.ceil(16*sx),Math.ceil(16*sy));
        let left=pixels.width, top=pixels.height, right=-1, bottom=-1;
        for(let y=0;y<pixels.height;++y) {
            for(let x=0;x<pixels.width;++x) {
                const i=(y*pixels.width+x)*4, data=pixels.data;
                if(data[i]<135 && data[i+1]>200 && data[i+2]>150 && data[i+2]<200) {
                    left=Math.min(left,x);right=Math.max(right,x);
                    top=Math.min(top,y);bottom=Math.max(bottom,y);
                }
            }
        }
        const dotWidth=(right-left+1)/sx, dotHeight=(bottom-top+1)/sy;
        return dotWidth>=6 && dotWidth<=10 && dotHeight>=6 && dotHeight<=10
            && Math.abs(dotWidth-dotHeight)<=1.5;
    })())JS");
}

bool Smoke(Application& app)
{
    auto& view = *app.view;
    for (int i = 0; i < 12; ++i) {
        if (!app.Advance(view))
            return false;
    }
    if (!app.Expect(view,
            "!!window.uiExample && uiExample.moduleLoaded && uiExample.promiseDone && uiExample.frames>0"
            " && document.querySelector('#increment')===document.getElementById('increment')"
            " && document.querySelector('.brand img').complete"
            " && document.querySelector('.brand img').naturalWidth===32"))
        return false;
    if (!app.Expect(view,
            "document.querySelector('nav a[href=\"#interaction\"]').click();true"))
        return false;
    if (!app.Advance(view)
        || !app.Expect(view,
            "location.hash==='#interaction'"
            " && document.getElementById('interaction').getBoundingClientRect().top<innerHeight"
            " && document.getElementById('interaction').getBoundingClientRect().bottom>0"))
        return false;
    if (!app.Expect(view,
            "document.querySelector('nav a[href=\"#workspace\"]').click();true")
        || !app.Advance(view)
        || !app.Expect(view, "location.hash==='#workspace' && uiExample.moduleLoaded"))
        return false;
    if (!app.Expect(view,
            "typeof process==='undefined' && typeof require==='undefined'"
            " && typeof HTMLVideoElement==='undefined' && typeof HTMLAudioElement==='undefined' && typeof AudioContext==='undefined'"
            " && typeof WebGLRenderingContext==='undefined' && typeof GPU==='undefined'"
            " && typeof indexedDB==='undefined' && typeof navigator.serviceWorker==='undefined'"
            " && typeof Worker==='undefined' && typeof WebSocket==='undefined'"
            " && typeof XMLHttpRequest==='undefined' && typeof fetch==='function'"
            " && typeof localStorage==='undefined' && typeof document.cookie==='undefined'"
            " && typeof navigator.gpu==='undefined' && typeof WebAssembly==='undefined'"
            " && typeof document.startViewTransition==='undefined'"
            " && !('onpageswap' in window) && !('onpagereveal' in window)"))
        return false;
    if (!app.Expect(view, R"JS((()=>{
            const result=window.localResourceProof={done:false,local:false,remote:false,escape:false};
            Promise.all([
              fetch('./local-data.json').then(response=>response.json()).then(value=>{
                result.local=value.title==='本地资源' && value.enabled===true
                  && value.sequence.join(',')==='3,5,8';
              }),
              fetch('https://example.invalid/forbidden').then(
                ()=>{},()=>{result.remote=true}),
              fetch('../main.cc').then(()=>{},()=>{result.escape=true})
            ]).then(()=>{result.done=true},error=>{result.error=String(error);result.done=true});
            return true;
          })())JS")
        || !app.Wait(view, "localResourceProof.done")
        || !app.Expect(view, "localResourceProof.local && localResourceProof.remote"
                            " && localResourceProof.escape && !localResourceProof.error"))
        return false;
    if (!app.Expect(view, R"JS((()=>{
            globalThis.blobProof={done:false,ok:false,error:''};
            const expected=[0,255,17,128,65];
            const url=URL.createObjectURL(new Blob([new Uint8Array(expected)],
                {type:'application/octet-stream'}));
            fetch(url).then(async response=>{
                const headers=response.status===200
                    && response.headers.get('content-type')==='application/octet-stream'
                    && response.headers.get('content-length')==='5';
                const bytes=new Uint8Array(await response.arrayBuffer());
                blobProof.ok=headers && bytes.length===expected.length
                    && bytes.every((value,index)=>value===expected[index]);
            }).catch(error=>{blobProof.error=String(error);})
              .finally(()=>{URL.revokeObjectURL(url);blobProof.done=true;});
            return true;
          })())JS")
        || !app.Wait(view, "blobProof.done")
        || !app.Expect(view, "blobProof.ok && !blobProof.error"))
        return false;
    if (!app.Expect(view, R"JS((()=>{
            const result=window.animationProof={ready:false,finished:false,interpolated:false};
            const element=document.createElement('div');
            element.style.cssText='position:absolute;left:-10000px;top:0;width:1px;height:1px';
            document.body.appendChild(element);
            const animation=element.animate([{opacity:1},{opacity:0}],{duration:100,fill:'both'});
            animation.pause();animation.currentTime=50;
            animation.ready.then(()=>{
              result.ready=true;
              result.interpolated=Math.abs(Number(getComputedStyle(element).opacity)-0.5)<0.01;
              animation.finished.then(()=>{result.finished=true;element.remove()});
              animation.finish();
            });
            return true;
          })())JS")
        || !app.Wait(view, "animationProof.finished")
        || !app.Expect(view, "animationProof.ready && animationProof.interpolated"))
        return false;
    if (!app.Expect(view, R"JS((()=>{
            const text=document.createElement('span');
            text.style.cssText='position:absolute;left:-10000px;top:0;font:medium system-ui';
            text.textContent='Hgj';
            document.body.appendChild(text);
            const range=document.createRange();
            range.selectNodeContents(text);
            const line=range.getBoundingClientRect();
            const context=document.createElement('canvas').getContext('2d');
            context.font='16px system-ui';
            const glyphs=context.measureText('Hgj');
            text.remove();
            return line.height>glyphs.actualBoundingBoxAscent
              && glyphs.actualBoundingBoxAscent>0 && glyphs.actualBoundingBoxDescent>0;
          })())JS"))
        return false;
    if (!app.Expect(view,
            "(()=>{if(!document.documentElement.matches(':root')"
            " || !document.getElementById('increment').matches(':enabled'))return false;"
            "return [':-webkit-full-page-media',':-internal-video-persistent',"
            "':-internal-video-persistent-ancestor',':future',':past',':paused',':playing',"
            "':picture-in-picture'].every(selector=>{try{document.querySelector(selector);return false}"
            "catch(error){return error.name==='SyntaxError'}})})()"))
        return false;
    if (!app.Expect(view,
            "(()=>{const descriptor=Object.getOwnPropertyDescriptor(document,'location');"
            "return descriptor && !descriptor.configurable && typeof descriptor.set==='function'"
            " && descriptor.get.call(document)===window.location"
            " && !Reflect.deleteProperty(document,'location')"
            " && !Reflect.defineProperty(document,'location',{value:'must-not-replace'})})()"))
        return false;
    if (!app.Expect(view,
            "(()=>{const key={value:41};const buffer=new ArrayBuffer(8);new Uint8Array(buffer)[0]=17;"
            "const source={map:new Map([[key,new Set([key])]]),date:new Date(1234),buffer,"
            "view:new Uint8Array(buffer),dataView:new DataView(buffer,2,4),unicode:'a\\u0000\\ud800'};source.self=source;"
            "const copy=structuredClone(source);const clonedKey=copy.map.keys().next().value;"
            "return copy!==source && copy.self===copy && copy.map.get(clonedKey).has(clonedKey)"
            " && clonedKey!==key && clonedKey.value===41 && copy.date.getTime()===1234"
            " && copy.view.buffer===copy.buffer && copy.view[0]===17 && copy.unicode===source.unicode"
            " && copy.dataView instanceof DataView && copy.dataView.buffer===copy.buffer"
            " && copy.dataView.byteOffset===2 && copy.dataView.byteLength===4})()")
        || !app.Expect(view,
            "(()=>{const source=new ArrayBuffer(4);new Uint8Array(source)[0]=42;"
            "const copy=structuredClone(source,{transfer:[source]});"
            "let rejected=false;try{structuredClone(source,{transfer:[source]})}"
            "catch(error){rejected=error instanceof DOMException && error.name==='DataCloneError'}"
            "return rejected && source.byteLength===0 && copy.byteLength===4 && new Uint8Array(copy)[0]===42})()"))
        return false;
    if (!app.Expect(view,
            "(()=>{const frame=document.createElement('iframe');frame.id='realm-probe';"
            "frame.style.display='none';frame.src='frame.html';document.body.appendChild(frame);return true})()"))
        return false;
    for (int i = 0; i < 12; ++i) {
        if (!app.Advance(view))
            return false;
    }
    if (!app.Expect(view,
            "(()=>{const frame=document.getElementById('realm-probe');"
            "if(!frame.contentWindow || typeof frame.contentWindow.getFrameRealmTag!=='function')return false;"
            "window.childProxy=frame.contentWindow;window.oldFrameFunction=childProxy.getFrameRealmTag;"
            "window.oldFrameToken=oldFrameFunction();childProxy.oldOnly=true;"
            "frame.src='frame.html?fresh=1';return true})()"))
        return false;
    for (int i = 0; i < 12; ++i) {
        if (!app.Advance(view))
            return false;
    }
    if (!app.Expect(view,
            "document.getElementById('realm-probe').contentWindow===childProxy"
            " && typeof childProxy.oldOnly==='undefined' && oldFrameFunction()===oldFrameToken"
            " && childProxy.getFrameRealmTag()!==oldFrameToken"))
        return false;

    if (!app.Expect(view,
            "(()=>{if(Reflect.preventExtensions(window)||!Object.isExtensible(window))return false;"
            "if(!Reflect.setPrototypeOf(window,Object.getPrototypeOf(window)))return false;"
            "const host=document.implementation.createHTMLDocument('host');"
            "if(Reflect.setPrototypeOf(host,host))return false;"
            "if(!Reflect.preventExtensions(host)||Object.isExtensible(host))return false;"
            "void host.body;return !Object.isExtensible(host)})()"))
        return false;
    if (!app.Expect(view,
            "(()=>{const frame=document.createElement('iframe');frame.id='opaque-probe';"
            "frame.style.display='none';frame.sandbox='allow-scripts';frame.src='frame.html';"
            "document.body.appendChild(frame);return true})()"))
        return false;
    for (int i = 0; i < 12; ++i) {
        if (!app.Advance(view))
            return false;
    }
    if (!app.Expect(view,
            "(()=>{const frame=document.getElementById('opaque-probe');const proxy=frame.contentWindow;"
            "const valid=Object.getPrototypeOf(proxy)===null&&Object.isExtensible(proxy)"
            "&&!Reflect.preventExtensions(proxy)&&Reflect.setPrototypeOf(proxy,null)"
            "&&!Reflect.setPrototypeOf(proxy,{});frame.remove();return valid})()"))
        return false;
    mini_ui::Error error;
    std::string position;
    if (!view.Evaluate("(()=>{const button=document.getElementById('increment');"
            "button.scrollIntoView({block:'center'});const r=button.getBoundingClientRect();"
            "return [r.x+r.width/2,r.y+r.height/2]})()", "ui-example-smoke.js", &position, &error))
        return app.Report(error);
    double x = 0, y = 0;
    if (std::sscanf(position.c_str(), "[%lf,%lf]", &x, &y) != 2) {
        app.failed = true;
        Log("FAIL: could not read button geometry: " + position);
        return false;
    }
    // Exercise real Win32 message entry while rAF keeps drawing to Canvas.
    // Direct Pointer(down/up) alone misses task-observer state leaked between
    // the host's WM_TIMER and the next WM_MOUSEMOVE.
    if (!app.Expect(view,
            "window.smokeHoverFrames=uiExample.frames;window.smokeHoverMoves=0;"
            "document.getElementById('increment').addEventListener('mousemove',"
            "()=>++window.smokeHoverMoves);true"))
        return false;
    struct RestoreCursor {
        POINT position {};
        ~RestoreCursor() { SetCursorPos(position.x, position.y); }
    } restore_cursor;
    POINT hover_position {
        static_cast<LONG>(x * app.scale), static_cast<LONG>(y * app.scale)
    };
    SetForegroundWindow(app.window);
    if (!GetCursorPos(&restore_cursor.position)
        || !ClientToScreen(app.window, &hover_position)
        || !SetCursorPos(hover_position.x, hover_position.y)) {
        app.failed = true;
        Log("FAIL: could not position the native cursor for hover");
        return false;
    }
    for (int i = 0; i < 16; ++i) {
        SendMessageW(app.window, WM_TIMER, 1, 0);
        SendMessageW(app.window, WM_MOUSEMOVE, 0,
            MAKELPARAM(static_cast<int>(x * app.scale), static_cast<int>(y * app.scale)));
        if (app.failed || !app.view)
            return false;
    }
    if (!app.Expect(view, "smokeHoverMoves>0")
        || !app.Expect(view, "uiExample.frames>smokeHoverFrames")
        || !app.Expect(view, "document.getElementById('increment').matches(':hover')")
        || !app.Expect(view,
            "getComputedStyle(document.getElementById('increment')).backgroundColor==='rgb(179, 192, 255)'")
        || !app.Expect(view,
            "document.getElementById('plot').getContext('2d').getImageData(1,1,1,1).data[3]===255"))
        return false;
    SendMessageW(app.window, WM_MOUSELEAVE, 0, 0);
    if (!app.Advance(view)
        || !app.Expect(view, "!document.getElementById('increment').matches(':hover')"))
        return false;
    mini_ui::PointerEvent pointer;
    pointer.x = x;
    pointer.y = y;
    pointer.type = mini_ui::PointerEvent::down;
    view.Pointer(pointer);
    pointer.type = mini_ui::PointerEvent::up;
    view.Pointer(pointer);
    if (!app.Advance(view) || !app.Expect(view, "uiExample.clicks===1 && document.getElementById('count').textContent==='1'"))
        return false;
    if (!app.Expect(view, "document.getElementById('name').focus();true"))
        return false;
    mini_ui::KeyEvent text;
    text.type = mini_ui::KeyEvent::Type::text;
    text.text = u"QuickJS";
    view.Key(text);
    if (!app.Advance(view) || !app.Expect(view,
            "document.getElementById('name').value==='QuickJS' && uiExample.input==='QuickJS'"))
        return false;
    text.text = u"\U0001f642\u4e2d\u6587";
    view.Key(text);
    if (!app.Advance(view) || !app.Expect(view,
            "document.getElementById('name').value==='QuickJS\\ud83d\\ude42\\u4e2d\\u6587'"
            " && uiExample.input==='QuickJS\\ud83d\\ude42\\u4e2d\\u6587'"))
        return false;
    if (!app.Expect(view,
            "(()=>{const select=document.createElement('select');select.id='smoke-select';"
            "select.style.cssText='position:fixed;left:400px;top:100px;width:180px;height:32px;font-size:16px;z-index:100000';"
            "select.innerHTML='<option value=a>Alpha</option><option value=b>Beta</option><option value=c>Gamma</option>';"
            "uiExample.selectChanges=0;select.addEventListener('change',()=>++uiExample.selectChanges);"
            "document.body.appendChild(select);return select.value==='a'})()"))
        return false;
    const auto open_select = [&] {
        mini_ui::PointerEvent event;
        event.x = 450;
        event.y = 116;
        event.type = mini_ui::PointerEvent::down;
        view.Pointer(event);
        event.type = mini_ui::PointerEvent::up;
        view.Pointer(event);
        return app.Advance(view);
    };
    const auto send_key = [&](unsigned key) {
        mini_ui::KeyEvent event;
        event.virtual_key = key;
        event.scan_code = MapVirtualKeyW(key, MAPVK_VK_TO_VSC);
        event.type = mini_ui::KeyEvent::Type::down;
        view.Key(event);
        event.type = mini_ui::KeyEvent::Type::up;
        view.Key(event);
    };
    if (!open_select())
        return false;
    send_key(VK_DOWN);
    send_key(VK_RETURN);
    if (!app.Advance(view) || !app.Expect(view,
            "document.getElementById('smoke-select').value==='b' && uiExample.selectChanges===1"))
        return false;
    if (!open_select())
        return false;
    send_key(VK_ESCAPE);
    if (!app.Advance(view) || !app.Expect(view,
            "document.getElementById('smoke-select').value==='b' && uiExample.selectChanges===1"))
        return false;
    if (!app.Expect(view, "document.getElementById('smoke-select').remove();true"))
        return false;
    if (!app.Expect(view, "window.beforeNavigation=17;true")
        || !view.LoadFile("index.html", &error))
        return app.Report(error);
    for (int i = 0; i < 12; ++i) {
        if (!app.Advance(view))
            return false;
    }
    if (!app.Expect(view,
            "typeof beforeNavigation==='undefined' && uiExample.clicks===0"
            " && uiExample.promiseDone && uiExample.frames>0"))
        return false;

    if (view.LoadFile("https://example.invalid/", &error)
        || error.code != mini_ui::ErrorCode::resource_denied) {
        app.failed = true;
        Log("FAIL: network navigation was not denied");
        return false;
    }
    mini_ui::ViewOptions other_options;
    other_options.resource_root = app.resource_root.parent_path() / "other-resources";
    other_options.width = 320;
    other_options.height = 200;
    auto other = mini_ui::View::Create(other_options, &error);
    if (!other || !other->LoadFile("index.html", &error))
        return app.Report(error);
    for (int i = 0; i < 6; ++i) {
        if (!app.Advance(*other))
            return false;
    }
    if (!app.Expect(*other, "window.rootMarker==='other-resource-root'"))
        return false;
    if (other->LoadFile("../resources/index.html", &error)
        || error.code != mini_ui::ErrorCode::resource_denied) {
        app.failed = true;
        Log("FAIL: second view escaped its independent resource root");
        return false;
    }
    app.view.reset();
    if (!app.Advance(*other) || !app.Expect(*other, "window.rootMarker==='other-resource-root' && document.body!==null"))
        return false;

    mini_ui::ViewOptions recreated;
    recreated.resource_root = app.resource_root;
    recreated.width = 840;
    recreated.height = 560;
    recreated.device_scale = 1.25f;
    recreated.native_window = app.window;
    app.view = mini_ui::View::Create(recreated, &error);
    if (!app.view || !app.view->LoadFile("index.html", &error))
        return app.Report(error);
    for (int i = 0; i < 12; ++i) {
        if (!app.Advance(*app.view))
            return false;
    }
    if (!app.Expect(*app.view, "uiExample.clicks===0 && uiExample.promiseDone && uiExample.frames>0"))
        return false;
    other.reset();
    struct CanvasSizeCase { int width, height; float scale; };
    for (const auto& size : {
             CanvasSizeCase { 840, 560, 1.0f }, CanvasSizeCase { 1440, 800, 1.25f },
             CanvasSizeCase { 1920, 900, 2.0f }, CanvasSizeCase { 840, 560, 1.25f } }) {
        if (!app.view->Resize(size.width, size.height, size.scale, &error))
            return app.Report(error);
        if (!CheckCanvasResolution(app, *app.view))
            return false;
    }
    // A content-box change without a window resize must also update backing.
    if (!app.Expect(*app.view,
            "document.querySelector('.canvas-panel').style.maxWidth='180px';true")
        || !CheckCanvasResolution(app, *app.view)
        || !app.Expect(*app.view,
            "document.querySelector('.canvas-panel').style.removeProperty('max-width');true")
        || !CheckCanvasResolution(app, *app.view))
        return false;
    if (!app.view->Resize(840, 560, 1.25f, &error) || !app.Advance(*app.view))
        return app.Report(error);
    mini_ui::FrameView frame = app.view->frame();
    if (!frame.pixels || frame.width != 1050 || frame.height != 700 || frame.stride < 4200) {
        app.failed = true;
        Log("FAIL: resized BGRA frame dimensions are incorrect");
        return false;
    }
    const std::uint8_t* pixel = frame.pixels + 10 * frame.stride + 10 * 4;
    if (pixel[0] != 0x25 || pixel[1] != 0x19 || pixel[2] != 0x13 || pixel[3] != 0xff) {
        app.failed = true;
        Log("FAIL: rendered CSS sidebar pixel is not the expected opaque BGRA color");
        return false;
    }
    if (!app.capture.empty() && !SaveBitmap(app.capture, frame)) {
        app.failed = true;
        Log("FAIL: could not save rendered frame");
        return false;
    }
    InvalidateRect(app.window, nullptr, FALSE);
    UpdateWindow(app.window);
    Log("PASS UI: local HTML/CSS/module/SVG, default/system-font geometry, Blob URL bytes/headers, DOM identity, Promise/rAF, clone/transfer/DataView, iframe realm/proxy navigation, anchor navigation, native mouse hover/leave and Canvas pixels, responsive Canvas backing and round marker at 1x/1.25x/2x, pointer, keyboard, select popup, BGRA, scale, two resource roots, network denial, destroy/recreate");
    return true;
}

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    auto* app = reinterpret_cast<Application*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        app = static_cast<Application*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        app->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (!app)
        return DefWindowProcW(window, message, wparam, lparam);
    switch (message) {
    case WM_SIZE:
        if (app->view && wparam != SIZE_MINIMIZED) {
            mini_ui::Error error;
            if (!app->view->Resize(static_cast<int>(LOWORD(lparam) / app->scale),
                    static_cast<int>(HIWORD(lparam) / app->scale), app->scale, &error))
                app->Report(error);
        }
        return 0;
    case WM_DPICHANGED:
        app->scale = HIWORD(wparam) / 96.0f;
        {
            const auto* rect = reinterpret_cast<const RECT*>(lparam);
            SetWindowPos(window, nullptr, rect->left, rect->top,
                rect->right - rect->left, rect->bottom - rect->top,
                SWP_NOACTIVATE | SWP_NOZORDER);
        }
        return 0;
    case WM_TIMER:
        if (app->view && !app->Advance(*app->view))
            DestroyWindow(window);
        else
            InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(window, &paint);
        mini_ui::FrameView frame = app->view ? app->view->frame() : mini_ui::FrameView {};
        RECT client;
        GetClientRect(window, &client);
        if (frame.pixels) {
            BITMAPINFO info {};
            info.bmiHeader.biSize = sizeof(info.bmiHeader);
            info.bmiHeader.biWidth = frame.width;
            info.bmiHeader.biHeight = -frame.height;
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            StretchDIBits(dc, 0, 0, client.right, client.bottom,
                0, 0, frame.width, frame.height, frame.pixels, &info, DIB_RGB_COLORS, SRCCOPY);
        } else {
            FillRect(dc, &client, GetSysColorBrush(COLOR_WINDOW));
        }
        EndPaint(window, &paint);
        return 0;
    }
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MOUSELEAVE:
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
        if (app->view) {
            mini_ui::PointerEvent event;
            POINT position { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
            event.modifiers = Modifiers();
            if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL) {
                ScreenToClient(window, &position);
                event.type = mini_ui::PointerEvent::wheel;
                const double delta = GET_WHEEL_DELTA_WPARAM(wparam) * (100.0 / WHEEL_DELTA);
                if (message == WM_MOUSEWHEEL) event.delta_y = delta;
                else event.delta_x = delta;
            } else if (message == WM_MOUSELEAVE) {
                event.type = mini_ui::PointerEvent::leave;
            } else if (message == WM_MOUSEMOVE) {
                TRACKMOUSEEVENT tracking { sizeof(tracking), TME_LEAVE, window, 0 };
                TrackMouseEvent(&tracking);
                event.type = mini_ui::PointerEvent::move;
            } else {
                const bool down = message == WM_LBUTTONDOWN || message == WM_MBUTTONDOWN || message == WM_RBUTTONDOWN;
                event.type = down ? mini_ui::PointerEvent::down : mini_ui::PointerEvent::up;
                event.button = (message == WM_MBUTTONDOWN || message == WM_MBUTTONUP) ? 1
                    : (message == WM_RBUTTONDOWN || message == WM_RBUTTONUP) ? 2 : 0;
                if (down) { SetFocus(window); SetCapture(window); }
                else ReleaseCapture();
            }
            event.x = position.x / app->scale;
            event.y = position.y / app->scale;
            app->view->Pointer(event);
        }
        return 0;
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
        if (app->view) {
            mini_ui::KeyEvent event;
            event.type = (message == WM_KEYDOWN || message == WM_SYSKEYDOWN)
                ? mini_ui::KeyEvent::Type::down : mini_ui::KeyEvent::Type::up;
            event.virtual_key = static_cast<unsigned>(wparam);
            event.scan_code = static_cast<unsigned>((lparam >> 16) & 0xff);
            if (lparam & (1LL << 24)) event.scan_code |= 0xe000;
            event.modifiers = Modifiers();
            event.repeat = (lparam & (1LL << 30)) != 0;
            app->view->Key(event);
        }
        if ((message == WM_SYSKEYDOWN || message == WM_SYSKEYUP) && wparam == VK_F4)
            return DefWindowProcW(window, message, wparam, lparam);
        return 0;
    case WM_CHAR:
        app->Text(static_cast<char16_t>(wparam));
        return 0;
    case WM_DESTROY:
        KillTimer(window, 1);
        app->view.reset();
        PostQuitMessage(app->failed ? 1 : 0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    using SetDpi = BOOL(WINAPI*)(HANDLE);
    if (auto set_dpi = reinterpret_cast<SetDpi>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext")))
        set_dpi(reinterpret_cast<HANDLE>(-4));
    else
        SetProcessDPIAware();
    Application app;
    int argc = 0;
    PWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv)
        return 1;
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view argument(argv[i]);
        if (argument == L"--smoke") app.smoke = true;
        else if (argument == L"--capture" && i + 1 < argc) app.capture = argv[++i];
        else app.resource_root = argv[i];
    }
    LocalFree(argv);
    if (app.resource_root.empty()) {
        wchar_t module[MAX_PATH];
        const DWORD size = GetModuleFileNameW(nullptr, module, MAX_PATH);
        if (!size || size == MAX_PATH)
            return 1;
        app.resource_root = std::filesystem::path(module).parent_path().parent_path().parent_path() / L"examples/ui/resources";
    }
    app.resource_root = std::filesystem::absolute(app.resource_root);
    WNDCLASSW window_class {};
    window_class.lpfnWndProc = WindowProcedure;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    window_class.lpszClassName = L"MiniUIQuickJSBlinkExample";
    if (!RegisterClassW(&window_class))
        return 1;
    HWND window = CreateWindowExW(0, window_class.lpszClassName, L"QuickJS + Blink | mini-ui",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1100, 730,
        nullptr, nullptr, instance, &app);
    if (!window)
        return 1;
    app.scale = GetDpiForWindow(window) / 96.0f;
    RECT client;
    GetClientRect(window, &client);
    mini_ui::ViewOptions options;
    options.resource_root = app.resource_root;
    options.width = static_cast<int>(client.right / app.scale);
    options.height = static_cast<int>(client.bottom / app.scale);
    options.device_scale = app.scale;
    options.native_window = window;
    mini_ui::Error error;
    app.view = mini_ui::View::Create(options, &error);
    if (!app.view || !app.view->LoadFile("index.html", &error)) {
        app.Report(error);
        DestroyWindow(window);
        return 1;
    }
    ShowWindow(window, show);
    if (app.smoke) {
        const bool success = Smoke(app);
        DestroyWindow(window);
        return success ? 0 : 1;
    }
    SetTimer(window, 1, 16, nullptr);
    MSG message;
    BOOL result;
    while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return result < 0 ? 1 : static_cast<int>(message.wParam);
}
