// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#import <AppKit/AppKit.h>
#import <objc/runtime.h>

#include "platform/macos/electron/mac_platform_apis.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "runtime/electron/common/api/native_image.h"
#include "third_party/libnode/src/node.h"
#include "third_party/libnode/src/node_buffer.h"
#include "v8.h"

namespace mini_electron::electron {
namespace {

v8::Local<v8::String> String(v8::Isolate* isolate, const char* value) {
  return v8::String::NewFromUtf8(isolate, value).ToLocalChecked();
}

v8::Local<v8::String> String(v8::Isolate* isolate, const std::string& value) {
  return v8::String::NewFromUtf8(isolate, value.data(), v8::NewStringType::kNormal,
                                 static_cast<int>(value.size())).ToLocalChecked();
}

std::string Utf8(v8::Isolate* isolate, v8::Local<v8::Value> value) {
  v8::String::Utf8Value text(isolate, value);
  return *text ? std::string(*text, text.length()) : std::string();
}

NSString* NSStringFrom(const std::string& value) {
  return [NSString stringWithUTF8String:value.c_str()] ?: @"";
}

void Set(v8::Local<v8::Context> context, v8::Local<v8::Object> object,
         const char* name, v8::Local<v8::Value> value) {
  object->Set(context, String(context->GetIsolate(), name), value).Check();
}

void Method(v8::Local<v8::Context> context, v8::Local<v8::Object> object,
            const char* name, v8::FunctionCallback callback) {
  Set(context, object, name,
      v8::Function::New(context, callback).ToLocalChecked());
}

v8::Local<v8::Value> Get(v8::Local<v8::Context> context,
                         v8::Local<v8::Object> object, const char* name) {
  v8::Local<v8::Value> value;
  return object->Get(context, String(context->GetIsolate(), name)).ToLocal(&value)
             ? value
             : v8::Undefined(context->GetIsolate()).As<v8::Value>();
}

bool Boolean(v8::Local<v8::Context> context, v8::Local<v8::Object> object,
             const char* name, bool fallback) {
  v8::Local<v8::Value> value = Get(context, object, name);
  return value->IsBoolean() ? value->BooleanValue(context->GetIsolate()) : fallback;
}

std::string Text(v8::Local<v8::Context> context, v8::Local<v8::Object> object,
                 const char* name) {
  v8::Local<v8::Value> value = Get(context, object, name);
  return value->IsString() ? Utf8(context->GetIsolate(), value) : std::string();
}

v8::Local<v8::Promise> Resolved(v8::Local<v8::Context> context,
                                v8::Local<v8::Value> value) {
  v8::Local<v8::Promise::Resolver> resolver =
      v8::Promise::Resolver::New(context).ToLocalChecked();
  resolver->Resolve(context, value).Check();
  return resolver->GetPromise();
}

NSImage* ImageFrom(v8::Isolate* isolate, v8::Local<v8::Value> value) {
  if (value->IsString()) {
    NSString* path = NSStringFrom(Utf8(isolate, value));
    NSImage* image = [[NSImage alloc] initWithContentsOfFile:path];
    if ([[path.lastPathComponent stringByDeletingPathExtension]
            hasSuffix:@"Template"])
      [image setTemplate:YES];
    return image;
  }
  if (!value->IsObject())
    return nil;
  atom::NativeImage* native = atom::NativeImage::GetSelf(value.As<v8::Object>());
  if (!native || native->isEmpty())
    return nil;
  v8::Local<v8::Object> png = native->toPNG();
  if (!node::Buffer::HasInstance(png))
    return nil;
  NSData* data = [NSData dataWithBytes:node::Buffer::Data(png)
                                length:node::Buffer::Length(png)];
  NSImage* image = [[NSImage alloc] initWithData:data];
  [image setTemplate:native->isTemplateImage()];
  return image;
}

class MacMenu;

@interface MiniElectronMenuAction : NSObject
- (instancetype)initWithMenu:(MacMenu*)menu item:(v8::Local<v8::Object>)item;
- (void)invoke:(id)sender;
@end

class MacMenu {
 public:
  MacMenu(v8::Isolate* isolate, v8::Local<v8::Object> wrapper)
      : isolate_(isolate), menu_([[NSMenu alloc] init]) {
    wrapper_.Reset(isolate, wrapper);
    wrapper_.SetWeak(this, Weak, v8::WeakCallbackType::kParameter);
  }
  ~MacMenu() {
    [menu_ removeAllItems];
#if !__has_feature(objc_arc)
    [menu_ release];
#endif
  }

  static MacMenu* From(v8::Local<v8::Object> object) {
    return object->InternalFieldCount() == 1
        ? static_cast<MacMenu*>(object->GetAlignedPointerFromInternalField(0))
        : nullptr;
  }
  static void Weak(const v8::WeakCallbackInfo<MacMenu>& data) {
    MacMenu* self = data.GetParameter();
    self->wrapper_.Reset();
    delete self;
  }
  static void New(const v8::FunctionCallbackInfo<v8::Value>& info) {
    if (!info.IsConstructCall()) return;
    MacMenu* self = new MacMenu(info.GetIsolate(), info.This());
    info.This()->SetAlignedPointerInInternalField(0, self);
    info.GetReturnValue().Set(info.This());
  }
  static void Insert(const v8::FunctionCallbackInfo<v8::Value>& info) {
    MacMenu* self = From(info.This());
    if (!self || info.Length() < 2 || !info[1]->IsObject()) return;
    v8::Local<v8::Context> context = info.GetIsolate()->GetCurrentContext();
    int position = info[0]->Int32Value(context).FromMaybe(self->menu_.numberOfItems);
    v8::Local<v8::Object> item = info[1].As<v8::Object>();
    const std::string type = Text(context, item, "type");
    NSMenuItem* native = nil;
    if (type == "separator") {
      native = [NSMenuItem separatorItem];
    } else {
      native = [[NSMenuItem alloc] initWithTitle:NSStringFrom(Text(context, item, "label"))
                                          action:@selector(invoke:)
                                   keyEquivalent:@""];
      MiniElectronMenuAction* action =
          [[MiniElectronMenuAction alloc] initWithMenu:self item:item];
      native.target = action;
      objc_setAssociatedObject(native, @selector(invoke:), action,
                               OBJC_ASSOCIATION_RETAIN_NONATOMIC);
      native.enabled = Boolean(context, item, "enabled", true);
      native.state = Boolean(context, item, "checked", false)
          ? NSControlStateValueOn : NSControlStateValueOff;
      v8::Local<v8::Value> submenu = Get(context, item, "submenu");
      if (submenu->IsObject()) {
        if (MacMenu* child = From(submenu.As<v8::Object>()))
          native.submenu = child->menu_;
      }
    }
    position = std::clamp(position, 0, static_cast<int>(self->menu_.numberOfItems));
    [self->menu_ insertItem:native atIndex:position];
#if !__has_feature(objc_arc)
    if (type != "separator") [native release];
#endif
  }
  static void Clear(const v8::FunctionCallbackInfo<v8::Value>& info) {
    if (MacMenu* self = From(info.This())) [self->menu_ removeAllItems];
  }
  static void Count(const v8::FunctionCallbackInfo<v8::Value>& info) {
    if (MacMenu* self = From(info.This())) info.GetReturnValue().Set(static_cast<int>(self->menu_.numberOfItems));
  }
  static void Popup(const v8::FunctionCallbackInfo<v8::Value>& info) {
    MacMenu* self = From(info.This());
    if (!self) return;
    NSPoint point = NSEvent.mouseLocation;
    if (info.Length() >= 2) {
      point.x = info[0]->NumberValue(info.GetIsolate()->GetCurrentContext()).FromMaybe(point.x);
      point.y = info[1]->NumberValue(info.GetIsolate()->GetCurrentContext()).FromMaybe(point.y);
    }
    [self->menu_ popUpMenuPositioningItem:nil atLocation:point inView:nil];
  }
  static void SetApplicationMenu(const v8::FunctionCallbackInfo<v8::Value>& info) {
    if (!info.Length() || info[0]->IsNullOrUndefined()) {
      NSApp.mainMenu = nil;
      return;
    }
    if (info[0]->IsObject()) {
      if (MacMenu* menu = From(info[0].As<v8::Object>())) NSApp.mainMenu = menu->menu_;
    }
  }
  void Invoke(v8::Local<v8::Object> item) {
    v8::HandleScope scope(isolate_);
    v8::Local<v8::Context> context = isolate_->GetCurrentContext();
    v8::Local<v8::Value> click = Get(context, item, "click");
    if (!click->IsFunction()) return;
    v8::Local<v8::Value> arguments[] = {
        v8::Object::New(isolate_), item, v8::Undefined(isolate_), v8::Undefined(isolate_)};
    (void)click.As<v8::Function>()->Call(context, item, 4, arguments);
  }

  NSMenu* menu_;
 private:
  v8::Isolate* isolate_;
  v8::Global<v8::Object> wrapper_;
};

@implementation MiniElectronMenuAction {
  MacMenu* _menu;
  v8::Global<v8::Object> _item;
  v8::Isolate* _isolate;
}
- (instancetype)initWithMenu:(MacMenu*)menu item:(v8::Local<v8::Object>)item {
  self = [super init];
  if (self) { _menu = menu; _isolate = item->GetIsolate(); _item.Reset(_isolate, item); }
  return self;
}
- (void)invoke:(id)sender {
  v8::HandleScope scope(_isolate);
  _menu->Invoke(_item.Get(_isolate));
}
- (void)dealloc {
  _item.Reset();
#if !__has_feature(objc_arc)
  [super dealloc];
#endif
}
@end

void InitMenu(v8::Local<v8::Object> exports, v8::Local<v8::Value>,
              v8::Local<v8::Context> context, void*) {
  v8::Isolate* isolate = context->GetIsolate();
  v8::Local<v8::FunctionTemplate> type = v8::FunctionTemplate::New(isolate, MacMenu::New);
  type->SetClassName(String(isolate, "Menu"));
  type->InstanceTemplate()->SetInternalFieldCount(1);
  type->PrototypeTemplate()->Set(isolate, "_insert", v8::FunctionTemplate::New(isolate, MacMenu::Insert));
  type->PrototypeTemplate()->Set(isolate, "_clear", v8::FunctionTemplate::New(isolate, MacMenu::Clear));
  type->PrototypeTemplate()->Set(isolate, "getItemCount", v8::FunctionTemplate::New(isolate, MacMenu::Count));
  type->PrototypeTemplate()->Set(isolate, "_popup", v8::FunctionTemplate::New(isolate, MacMenu::Popup));
  v8::Local<v8::Function> constructor = type->GetFunction(context).ToLocalChecked();
  Method(context, constructor, "_setApplicationMenu", MacMenu::SetApplicationMenu);
  Set(context, exports, "Menu", constructor);
}

class MacTray;
@interface MiniElectronTrayAction : NSObject
- (instancetype)initWithTray:(MacTray*)tray;
- (void)invoke:(id)sender;
@end

class MacTray {
 public:
  MacTray(v8::Isolate* isolate, v8::Local<v8::Object> wrapper)
      : isolate_(isolate), item_([NSStatusBar.systemStatusBar statusItemWithLength:NSSquareStatusItemLength]) {
    wrapper_.Reset(isolate, wrapper);
    wrapper_.SetWeak(this, Weak, v8::WeakCallbackType::kParameter);
    target_ = [[MiniElectronTrayAction alloc] initWithTray:this];
    item_.button.target = target_;
    item_.button.action = @selector(invoke:);
    [item_.button sendActionOn:NSEventMaskLeftMouseUp |
                               NSEventMaskRightMouseUp];
  }
  ~MacTray() {
    [NSStatusBar.systemStatusBar removeStatusItem:item_];
    callback_.Reset();
#if !__has_feature(objc_arc)
    [target_ release];
#endif
  }
  static MacTray* From(v8::Local<v8::Object> object) {
    return object->InternalFieldCount() == 1
        ? static_cast<MacTray*>(object->GetAlignedPointerFromInternalField(0)) : nullptr;
  }
  static void Weak(const v8::WeakCallbackInfo<MacTray>& data) {
    MacTray* self = data.GetParameter(); self->wrapper_.Reset(); delete self;
  }
  static void New(const v8::FunctionCallbackInfo<v8::Value>& info) {
    if (!info.IsConstructCall()) return;
    MacTray* self = new MacTray(info.GetIsolate(), info.This());
    info.This()->SetAlignedPointerInInternalField(0, self);
    if (info.Length()) self->SetImageValue(info[0]);
    info.GetReturnValue().Set(info.This());
  }
  void SetImageValue(v8::Local<v8::Value> value) {
    NSImage* image = ImageFrom(isolate_, value);
    if (image) item_.button.image = image;
#if !__has_feature(objc_arc)
    [image release];
#endif
  }
  static void SetImage(const v8::FunctionCallbackInfo<v8::Value>& info) { if (MacTray* self = From(info.This()); self && info.Length()) self->SetImageValue(info[0]); }
  static void SetToolTip(const v8::FunctionCallbackInfo<v8::Value>& info) { if (MacTray* self = From(info.This()); self && info.Length()) self->item_.button.toolTip = NSStringFrom(Utf8(info.GetIsolate(), info[0])); }
  static void SetTitle(const v8::FunctionCallbackInfo<v8::Value>& info) { if (MacTray* self = From(info.This()); self && info.Length()) self->item_.button.title = NSStringFrom(Utf8(info.GetIsolate(), info[0])); }
  static void Destroy(const v8::FunctionCallbackInfo<v8::Value>& info) { if (MacTray* self = From(info.This())) { [NSStatusBar.systemStatusBar removeStatusItem:self->item_]; self->item_ = nil; } }
  static void IsDestroyed(const v8::FunctionCallbackInfo<v8::Value>& info) { MacTray* self = From(info.This()); info.GetReturnValue().Set(!self || !self->item_); }
  static void SetCallback(const v8::FunctionCallbackInfo<v8::Value>& info) { if (MacTray* self = From(info.This()); self && info.Length() && info[0]->IsFunction()) self->callback_.Reset(info.GetIsolate(), info[0].As<v8::Function>()); }
  static void SetContext(const v8::FunctionCallbackInfo<v8::Value>&) {}
  void Click(bool right_click) {
    if (callback_.IsEmpty()) return;
    v8::HandleScope scope(isolate_);
    v8::Local<v8::Context> context = isolate_->GetCurrentContext();
    v8::Local<v8::Value> argument =
        String(isolate_, right_click ? "right-click" : "click");
    (void)callback_.Get(isolate_)->Call(
        context, wrapper_.Get(isolate_), 1, &argument);
  }
 private:
  v8::Isolate* isolate_;
  NSStatusItem* item_;
  MiniElectronTrayAction* target_;
  v8::Global<v8::Object> wrapper_;
  v8::Global<v8::Function> callback_;
};

@implementation MiniElectronTrayAction { MacTray* _tray; }
- (instancetype)initWithTray:(MacTray*)tray { self = [super init]; if (self) _tray = tray; return self; }
- (void)invoke:(id)sender {
  NSEventType type = NSApp.currentEvent.type;
  _tray->Click(type == NSEventTypeRightMouseDown ||
               type == NSEventTypeRightMouseUp);
  (void)sender;
}
@end

void InitTray(v8::Local<v8::Object> exports, v8::Local<v8::Value>,
              v8::Local<v8::Context> context, void*) {
  v8::Isolate* isolate = context->GetIsolate();
  v8::Local<v8::FunctionTemplate> type = v8::FunctionTemplate::New(isolate, MacTray::New);
  type->SetClassName(String(isolate, "Tray"));
  type->InstanceTemplate()->SetInternalFieldCount(1);
  type->PrototypeTemplate()->Set(isolate, "setImage", v8::FunctionTemplate::New(isolate, MacTray::SetImage));
  type->PrototypeTemplate()->Set(isolate, "setPressedImage", v8::FunctionTemplate::New(isolate, MacTray::SetImage));
  type->PrototypeTemplate()->Set(isolate, "setToolTip", v8::FunctionTemplate::New(isolate, MacTray::SetToolTip));
  type->PrototypeTemplate()->Set(isolate, "setTitle", v8::FunctionTemplate::New(isolate, MacTray::SetTitle));
  type->PrototypeTemplate()->Set(isolate, "destroy", v8::FunctionTemplate::New(isolate, MacTray::Destroy));
  type->PrototypeTemplate()->Set(isolate, "isDestroyed", v8::FunctionTemplate::New(isolate, MacTray::IsDestroyed));
  type->PrototypeTemplate()->Set(isolate, "_setNativeMessageCallback", v8::FunctionTemplate::New(isolate, MacTray::SetCallback));
  type->PrototypeTemplate()->Set(isolate, "_setIsContextMenu", v8::FunctionTemplate::New(isolate, MacTray::SetContext));
  Set(context, exports, "Tray", type->GetFunction(context).ToLocalChecked());
}

v8::Local<v8::Object> DialogOptions(v8::Local<v8::Context> context,
                                    const v8::FunctionCallbackInfo<v8::Value>& info) {
  for (int index = 0; index < info.Length(); ++index) {
    if (info[index]->IsObject() && !info[index]->IsFunction())
      return info[index].As<v8::Object>();
  }
  return v8::Object::New(context->GetIsolate());
}

v8::Local<v8::Function> DialogCallback(v8::Isolate* isolate,
                                        const v8::FunctionCallbackInfo<v8::Value>& info) {
  for (int index = info.Length() - 1; index >= 0; --index)
    if (info[index]->IsFunction()) return info[index].As<v8::Function>();
  return {};
}

NSArray<NSString*>* RunOpenPanel(v8::Local<v8::Context> context,
                                 v8::Local<v8::Object> options, bool save) {
  NSSavePanel* panel = save ? [NSSavePanel savePanel] : [NSOpenPanel openPanel];
  const std::string title = Text(context, options, "title");
  const std::string default_path = Text(context, options, "defaultPath");
  if (!title.empty()) panel.title = NSStringFrom(title);
  if (!default_path.empty()) panel.directoryURL = [NSURL fileURLWithPath:NSStringFrom(default_path)];
  if (!save) {
    NSOpenPanel* open = (NSOpenPanel*)panel;
    v8::Local<v8::Value> properties = Get(context, options, "properties");
    if (properties->IsArray()) {
      v8::Local<v8::Array> array = properties.As<v8::Array>();
      for (uint32_t index = 0; index < array->Length(); ++index) {
        v8::Local<v8::Value> value;
        if (!array->Get(context, index).ToLocal(&value)) continue;
        const std::string property = Utf8(context->GetIsolate(), value);
        if (property == "openDirectory") open.canChooseDirectories = YES;
        if (property == "multiSelections") open.allowsMultipleSelection = YES;
        if (property == "createDirectory") open.canCreateDirectories = YES;
      }
    }
  }
  if ([panel runModal] != NSModalResponseOK) return @[];
  if (save) return @[ panel.URL.path ?: @"" ];
  NSMutableArray<NSString*>* paths = [NSMutableArray array];
  for (NSURL* url in ((NSOpenPanel*)panel).URLs) [paths addObject:url.path];
  return paths;
}

v8::Local<v8::Array> Paths(v8::Local<v8::Context> context, NSArray<NSString*>* paths) {
  v8::Local<v8::Array> result = v8::Array::New(context->GetIsolate(), paths.count);
  for (NSUInteger index = 0; index < paths.count; ++index)
    result->Set(context, index, String(context->GetIsolate(), [paths[index] UTF8String])).Check();
  return result;
}

void ShowOpen(const v8::FunctionCallbackInfo<v8::Value>& info, bool save, bool sync) {
  v8::Local<v8::Context> context = info.GetIsolate()->GetCurrentContext();
  NSArray<NSString*>* paths = RunOpenPanel(context, DialogOptions(context, info), save);
  v8::Local<v8::Array> values = Paths(context, paths);
  if (sync) {
    if (save) {
      if (paths.count)
        info.GetReturnValue().Set(String(info.GetIsolate(), [paths[0] UTF8String]));
      else
        info.GetReturnValue().Set(v8::Null(info.GetIsolate()));
    } else {
      info.GetReturnValue().Set(values);
    }
    return;
  }
  v8::Local<v8::Function> callback = DialogCallback(info.GetIsolate(), info);
  if (!callback.IsEmpty()) {
    v8::Local<v8::Value> arguments[] = { v8::Boolean::New(info.GetIsolate(), paths.count == 0), values };
    (void)callback->Call(context, info.This(), 2, arguments);
  }
}
void DialogOpen(const v8::FunctionCallbackInfo<v8::Value>& info) { ShowOpen(info, false, false); }
void DialogOpenSync(const v8::FunctionCallbackInfo<v8::Value>& info) { ShowOpen(info, false, true); }
void DialogSave(const v8::FunctionCallbackInfo<v8::Value>& info) { ShowOpen(info, true, false); }
void DialogSaveSync(const v8::FunctionCallbackInfo<v8::Value>& info) { ShowOpen(info, true, true); }

int RunMessage(v8::Local<v8::Context> context, v8::Local<v8::Object> options) {
  NSAlert* alert = [[NSAlert alloc] init];
  alert.messageText = NSStringFrom(Text(context, options, "title"));
  std::string message = Text(context, options, "message");
  std::string detail = Text(context, options, "detail");
  alert.informativeText = NSStringFrom(detail.empty() ? message : message + "\n" + detail);
  v8::Local<v8::Value> buttons = Get(context, options, "buttons");
  if (buttons->IsArray()) {
    v8::Local<v8::Array> array = buttons.As<v8::Array>();
    for (uint32_t index = 0; index < array->Length(); ++index) {
      v8::Local<v8::Value> value;
      if (array->Get(context, index).ToLocal(&value)) [alert addButtonWithTitle:NSStringFrom(Utf8(context->GetIsolate(), value))];
    }
  }
  if (!alert.buttons.count) [alert addButtonWithTitle:@"OK"];
  NSInteger response = [alert runModal] - NSAlertFirstButtonReturn;
#if !__has_feature(objc_arc)
  [alert release];
#endif
  return std::max<NSInteger>(0, response);
}
void DialogMessageSync(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Local<v8::Context> context = info.GetIsolate()->GetCurrentContext();
  info.GetReturnValue().Set(RunMessage(context, DialogOptions(context, info)));
}
void DialogMessage(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Local<v8::Context> context = info.GetIsolate()->GetCurrentContext();
  v8::Local<v8::Object> result = v8::Object::New(info.GetIsolate());
  Set(context, result, "response", v8::Integer::New(info.GetIsolate(), RunMessage(context, DialogOptions(context, info))));
  Set(context, result, "checkboxChecked", v8::Boolean::New(info.GetIsolate(), false));
  info.GetReturnValue().Set(Resolved(context, result));
}
void DialogError(const v8::FunctionCallbackInfo<v8::Value>& info) {
  NSAlert* alert = [[NSAlert alloc] init];
  alert.alertStyle = NSAlertStyleCritical;
  alert.messageText = info.Length() ? NSStringFrom(Utf8(info.GetIsolate(), info[0])) : @"Error";
  alert.informativeText = info.Length() > 1 ? NSStringFrom(Utf8(info.GetIsolate(), info[1])) : @"";
  [alert runModal];
#if !__has_feature(objc_arc)
  [alert release];
#endif
}
void DialogNew(const v8::FunctionCallbackInfo<v8::Value>& info) { if (info.IsConstructCall()) info.GetReturnValue().Set(info.This()); }

void InitDialog(v8::Local<v8::Object> exports, v8::Local<v8::Value>,
                v8::Local<v8::Context> context, void*) {
  v8::Isolate* isolate = context->GetIsolate();
  v8::Local<v8::FunctionTemplate> type = v8::FunctionTemplate::New(isolate, DialogNew);
  type->SetClassName(String(isolate, "Dialog"));
  type->PrototypeTemplate()->Set(isolate, "_showOpenDialog", v8::FunctionTemplate::New(isolate, DialogOpen));
  type->PrototypeTemplate()->Set(isolate, "_showOpenDialogSync", v8::FunctionTemplate::New(isolate, DialogOpenSync));
  type->PrototypeTemplate()->Set(isolate, "_showSaveDialog", v8::FunctionTemplate::New(isolate, DialogSave));
  type->PrototypeTemplate()->Set(isolate, "_showSaveDialogSync", v8::FunctionTemplate::New(isolate, DialogSaveSync));
  type->PrototypeTemplate()->Set(isolate, "_showMessageBox", v8::FunctionTemplate::New(isolate, DialogMessage));
  type->PrototypeTemplate()->Set(isolate, "_showMessageBoxSync", v8::FunctionTemplate::New(isolate, DialogMessageSync));
  type->PrototypeTemplate()->Set(isolate, "_showErrorBox", v8::FunctionTemplate::New(isolate, DialogError));
  Set(context, exports, "Dialog", type->GetFunction(context).ToLocalChecked());
}

void ShellOpenExternal(const v8::FunctionCallbackInfo<v8::Value>& info) {
  bool opened = info.Length() && [NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:NSStringFrom(Utf8(info.GetIsolate(), info[0]))]];
  info.GetReturnValue().Set(Resolved(info.GetIsolate()->GetCurrentContext(), v8::Boolean::New(info.GetIsolate(), opened)));
}
void ShellOpenPath(const v8::FunctionCallbackInfo<v8::Value>& info) {
  bool opened = info.Length() && [NSWorkspace.sharedWorkspace openURL:[NSURL fileURLWithPath:NSStringFrom(Utf8(info.GetIsolate(), info[0]))]];
  info.GetReturnValue().Set(Resolved(info.GetIsolate()->GetCurrentContext(), String(info.GetIsolate(), opened ? "" : "Failed to open path")));
}
void ShellShowItem(const v8::FunctionCallbackInfo<v8::Value>& info) {
  if (info.Length()) [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:@[[NSURL fileURLWithPath:NSStringFrom(Utf8(info.GetIsolate(), info[0]))]]];
}
void ShellTrash(const v8::FunctionCallbackInfo<v8::Value>& info) {
  bool success = false;
  if (info.Length()) {
    NSURL* url =
        [NSURL fileURLWithPath:NSStringFrom(Utf8(info.GetIsolate(), info[0]))];
    NSError* error = nil;
    success = [[NSFileManager defaultManager] trashItemAtURL:url
                                            resultingItemURL:nil
                                                       error:&error];
  }
  info.GetReturnValue().Set(Resolved(
      info.GetIsolate()->GetCurrentContext(),
      v8::Boolean::New(info.GetIsolate(), success)));
}
void ShellBeep(const v8::FunctionCallbackInfo<v8::Value>&) { NSBeep(); }
void InitShell(v8::Local<v8::Object> exports, v8::Local<v8::Value>,
               v8::Local<v8::Context> context, void*) {
  v8::Local<v8::Object> shell = v8::Object::New(context->GetIsolate());
  Method(context, shell, "openExternal", ShellOpenExternal);
  Method(context, shell, "openPath", ShellOpenPath);
  Method(context, shell, "showItemInFolder", ShellShowItem);
  Method(context, shell, "trashItem", ShellTrash);
  Method(context, shell, "beep", ShellBeep);
  Set(context, exports, "Shell", shell);
}

v8::Local<v8::Object> Display(v8::Local<v8::Context> context, NSScreen* screen) {
  v8::Isolate* isolate = context->GetIsolate();
  v8::Local<v8::Object> display = v8::Object::New(isolate);
  NSNumber* number = screen.deviceDescription[@"NSScreenNumber"];
  Set(context, display, "id", v8::Integer::New(isolate, number.intValue));
  NSRect frame = screen.frame;
  NSRect work = screen.visibleFrame;
  const CGFloat primary_top = NSMaxY(NSScreen.screens.firstObject.frame);
  frame.origin.y = primary_top - NSMaxY(frame);
  work.origin.y = primary_top - NSMaxY(work);
  auto rect = [&](NSRect value) {
    v8::Local<v8::Object> result = v8::Object::New(isolate);
    Set(context, result, "x", v8::Integer::New(isolate, value.origin.x));
    Set(context, result, "y", v8::Integer::New(isolate, value.origin.y));
    Set(context, result, "width", v8::Integer::New(isolate, value.size.width));
    Set(context, result, "height", v8::Integer::New(isolate, value.size.height));
    return result;
  };
  Set(context, display, "bounds", rect(frame));
  Set(context, display, "workArea", rect(work));
  Set(context, display, "scaleFactor", v8::Number::New(isolate, screen.backingScaleFactor));
  return display;
}
void ScreenPrimary(const v8::FunctionCallbackInfo<v8::Value>& info) { info.GetReturnValue().Set(Display(info.GetIsolate()->GetCurrentContext(), NSScreen.mainScreen)); }
void ScreenAll(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Local<v8::Context> context = info.GetIsolate()->GetCurrentContext();
  NSArray<NSScreen*>* screens = NSScreen.screens;
  v8::Local<v8::Array> result = v8::Array::New(info.GetIsolate(), screens.count);
  for (NSUInteger index = 0; index < screens.count; ++index) result->Set(context, index, Display(context, screens[index])).Check();
  info.GetReturnValue().Set(result);
}
void ScreenCursor(const v8::FunctionCallbackInfo<v8::Value>& info) {
  NSPoint point = NSEvent.mouseLocation;
  point.y = NSMaxY(NSScreen.screens.firstObject.frame) - point.y;
  v8::Local<v8::Object> result = v8::Object::New(info.GetIsolate());
  Set(info.GetIsolate()->GetCurrentContext(), result, "x", v8::Integer::New(info.GetIsolate(), point.x));
  Set(info.GetIsolate()->GetCurrentContext(), result, "y", v8::Integer::New(info.GetIsolate(), point.y));
  info.GetReturnValue().Set(result);
}
void ScreenNew(const v8::FunctionCallbackInfo<v8::Value>& info) { if (info.IsConstructCall()) info.GetReturnValue().Set(info.This()); }
void InitScreen(v8::Local<v8::Object> exports, v8::Local<v8::Value>,
                v8::Local<v8::Context> context, void*) {
  v8::Isolate* isolate = context->GetIsolate();
  v8::Local<v8::FunctionTemplate> type = v8::FunctionTemplate::New(isolate, ScreenNew);
  type->SetClassName(String(isolate, "Screen"));
  type->PrototypeTemplate()->Set(isolate, "getPrimaryDisplay", v8::FunctionTemplate::New(isolate, ScreenPrimary));
  type->PrototypeTemplate()->Set(isolate, "getAllDisplays", v8::FunctionTemplate::New(isolate, ScreenAll));
  type->PrototypeTemplate()->Set(isolate, "getCursorScreenPoint", v8::FunctionTemplate::New(isolate, ScreenCursor));
  Set(context, exports, "Screen", type->GetFunction(context).ToLocalChecked());
}

}  // namespace

void InstallMacPlatformApiBindings(node::Environment* environment) {
  node::AddLinkedBinding(environment, "electron_browser_menu", InitMenu, nullptr);
  node::AddLinkedBinding(environment, "electron_browser_tray", InitTray, nullptr);
  node::AddLinkedBinding(environment, "electron_browser_dialog", InitDialog, nullptr);
  node::AddLinkedBinding(environment, "electron_common_shell", InitShell, nullptr);
  node::AddLinkedBinding(environment, "electron_common_screen", InitScreen, nullptr);
}

}  // namespace mini_electron::electron
