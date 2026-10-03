// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#import <AppKit/AppKit.h>
#import <ReactiveObjC/ReactiveObjC.h>
#import <Squirrel/Squirrel.h>

#include "platform/macos/electron/mac_auto_updater.h"

#include <string>
#include <vector>

#include "third_party/libnode/src/node.h"
#include "v8.h"

namespace mini_electron::electron {
namespace {

struct AutoUpdaterState {
  v8::Isolate* isolate = nullptr;
  v8::Global<v8::Context> context;
  v8::Global<v8::Object> wrapper;
  SQRLUpdater* updater = nil;
  RACDisposable* check = nil;
  RACDisposable* availability = nil;
  RACDisposable* install = nil;
  NSString* feed_url = nil;
};

AutoUpdaterState& State() {
  static AutoUpdaterState state;
  return state;
}

v8::Local<v8::String> String(v8::Isolate* isolate, const char* value) {
  return v8::String::NewFromUtf8(isolate, value).ToLocalChecked();
}

v8::Local<v8::String> String(v8::Isolate* isolate, NSString* value) {
  const char* utf8 = value.UTF8String ?: "";
  return v8::String::NewFromUtf8(isolate, utf8).ToLocalChecked();
}

std::string Utf8(v8::Isolate* isolate, v8::Local<v8::Value> value) {
  v8::String::Utf8Value text(isolate, value);
  return *text ? std::string(*text, text.length()) : std::string();
}

void Set(v8::Local<v8::Context> context,
         v8::Local<v8::Object> object,
         const char* name,
         v8::Local<v8::Value> value) {
  object->Set(context, String(context->GetIsolate(), name), value).Check();
}

void Method(v8::Local<v8::Context> context,
            v8::Local<v8::Object> object,
            const char* name,
            v8::FunctionCallback callback) {
  Set(context, object, name,
      v8::Function::New(context, callback).ToLocalChecked());
}

void Emit(const char* name,
          int argument_count = 0,
          v8::Local<v8::Value>* arguments = nullptr) {
  AutoUpdaterState& state = State();
  if (!state.isolate || state.context.IsEmpty() || state.wrapper.IsEmpty())
    return;
  v8::Isolate::Scope isolate_scope(state.isolate);
  v8::HandleScope handle_scope(state.isolate);
  v8::Local<v8::Context> context = state.context.Get(state.isolate);
  v8::Context::Scope context_scope(context);
  v8::Local<v8::Object> wrapper = state.wrapper.Get(state.isolate);
  v8::Local<v8::Value> emit_value;
  if (!wrapper->Get(context, String(state.isolate, "emit"))
           .ToLocal(&emit_value) ||
      !emit_value->IsFunction()) {
    return;
  }
  std::vector<v8::Local<v8::Value>> values;
  values.reserve(static_cast<size_t>(argument_count) + 1);
  values.push_back(String(state.isolate, name));
  for (int index = 0; index < argument_count; ++index)
    values.push_back(arguments[index]);
  v8::TryCatch try_catch(state.isolate);
  (void)emit_value.As<v8::Function>()->Call(
      context, wrapper, static_cast<int>(values.size()), values.data());
  if (try_catch.HasCaught())
    node::FatalException(state.isolate, try_catch);
}

void EmitError(NSError* error) {
  AutoUpdaterState& state = State();
  if (!state.isolate || state.context.IsEmpty())
    return;
  v8::Isolate::Scope isolate_scope(state.isolate);
  v8::HandleScope handle_scope(state.isolate);
  v8::Local<v8::Context> context = state.context.Get(state.isolate);
  v8::Context::Scope context_scope(context);
  NSString* description = error.localizedDescription ?: @"Update failed";
  v8::Local<v8::Value> argument =
      v8::Exception::Error(String(state.isolate, description));
  Emit("error", 1, &argument);
}

NSDictionary<NSString*, NSString*>* Headers(
    v8::Local<v8::Context> context,
    v8::Local<v8::Value> value) {
  if (!value->IsObject())
    return @{};
  v8::Isolate* isolate = context->GetIsolate();
  v8::Local<v8::Object> object = value.As<v8::Object>();
  v8::Local<v8::Array> names;
  if (!object->GetOwnPropertyNames(context).ToLocal(&names))
    return @{};
  NSMutableDictionary* result = [NSMutableDictionary dictionary];
  for (uint32_t index = 0; index < names->Length(); ++index) {
    v8::Local<v8::Value> name;
    v8::Local<v8::Value> header;
    if (!names->Get(context, index).ToLocal(&name) ||
        !object->Get(context, name).ToLocal(&header)) {
      continue;
    }
    const std::string key = Utf8(isolate, name);
    const std::string text = Utf8(isolate, header);
    result[[NSString stringWithUTF8String:key.c_str()] ?: @""] =
        [NSString stringWithUTF8String:text.c_str()] ?: @"";
  }
  return result;
}

void ReplaceUpdater(NSURLRequest* request,
                    NSDictionary<NSString*, NSString*>* headers) {
  AutoUpdaterState& state = State();
  [state.check dispose];
  [state.check release];
  state.check = nil;
  [state.availability dispose];
  [state.availability release];
  state.availability = nil;
  [state.install dispose];
  [state.install release];
  state.install = nil;
  [state.updater release];
  state.updater = nil;
  SQRLRequestForDownload request_for_download = ^NSURLRequest*(NSURL* url) {
    NSMutableURLRequest* download = [NSMutableURLRequest requestWithURL:url];
    [headers enumerateKeysAndObjectsUsingBlock:
                 ^(NSString* key, NSString* value, BOOL*) {
                   [download setValue:value forHTTPHeaderField:key];
                 }];
    return download;
  };
  state.updater = [[SQRLUpdater alloc]
      initWithUpdateRequest:request
         requestForDownload:request_for_download];
}

void SetFeedURL(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Isolate* isolate = info.GetIsolate();
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  if (!info.Length()) {
    isolate->ThrowException(v8::Exception::TypeError(
        String(isolate, "setFeedURL requires a URL")));
    return;
  }
  v8::Local<v8::Value> url_value = info[0];
  NSDictionary<NSString*, NSString*>* headers = @{};
  if (url_value->IsObject() && !url_value->IsString()) {
    v8::Local<v8::Object> options = url_value.As<v8::Object>();
    if (!options->Get(context, String(isolate, "url")).ToLocal(&url_value))
      return;
    v8::Local<v8::Value> headers_value;
    if (options->Get(context, String(isolate, "headers"))
            .ToLocal(&headers_value)) {
      headers = Headers(context, headers_value);
    }
  }
  const std::string url_text = Utf8(isolate, url_value);
  NSURL* url = [NSURL URLWithString:
      [NSString stringWithUTF8String:url_text.c_str()] ?: @""];
  if (!url || !url.scheme.length) {
    isolate->ThrowException(v8::Exception::TypeError(
        String(isolate, "setFeedURL requires an absolute URL")));
    return;
  }
  NSMutableURLRequest* request = [NSMutableURLRequest requestWithURL:url];
  [headers enumerateKeysAndObjectsUsingBlock:
               ^(NSString* key, NSString* value, BOOL*) {
                 [request setValue:value forHTTPHeaderField:key];
               }];
  @try {
    ReplaceUpdater(request, headers);
  } @catch (NSException* exception) {
    isolate->ThrowException(v8::Exception::Error(
        String(isolate, exception.reason ?: @"Unable to initialize updater")));
    return;
  }
  if (!State().updater) {
    isolate->ThrowException(v8::Exception::Error(String(
        isolate,
        "Unable to initialize the signed Squirrel.Mac updater")));
    return;
  }
  AutoUpdaterState& state = State();
  [state.feed_url release];
  state.feed_url = [[url absoluteString] copy];
}

void GetFeedURL(const v8::FunctionCallbackInfo<v8::Value>& info) {
  AutoUpdaterState& state = State();
  info.GetReturnValue().Set(
      String(info.GetIsolate(), state.feed_url ?: @""));
}

void CheckForUpdates(const v8::FunctionCallbackInfo<v8::Value>& info) {
  AutoUpdaterState& state = State();
  if (!state.updater) {
    info.GetIsolate()->ThrowException(v8::Exception::Error(
        String(info.GetIsolate(), "Update URL is not configured")));
    return;
  }
  [state.check dispose];
  [state.check release];
  state.check = nil;
  [state.availability dispose];
  [state.availability release];
  state.availability = [[[[state.updater
      rac_valuesForKeyPath:@"state" observer:nil]
      filter:^BOOL(NSNumber* value) {
        return value.unsignedIntegerValue ==
               SQRLUpdaterStateDownloadingUpdate;
      }]
      take:1]
      subscribeNext:^(id) {
        if (NSThread.isMainThread) {
          Emit("update-available");
        } else {
          dispatch_async(dispatch_get_main_queue(), ^{
            Emit("update-available");
          });
        }
      }];
  [state.availability retain];
  Emit("checking-for-update");
  __block BOOL found_update = NO;
  RACSignal* signal =
      [state.updater.checkForUpdatesCommand execute:nil];
  state.check = [[signal
      subscribeNext:^(SQRLDownloadedUpdate* downloaded) {
        found_update = YES;
        SQRLUpdate* update = downloaded.update;
        AutoUpdaterState& callback_state = State();
        if (!callback_state.isolate || callback_state.context.IsEmpty())
          return;
        v8::Isolate::Scope isolate_scope(callback_state.isolate);
        v8::HandleScope handle_scope(callback_state.isolate);
        v8::Local<v8::Context> context =
            callback_state.context.Get(callback_state.isolate);
        v8::Context::Scope context_scope(context);
        v8::Local<v8::Object> event = v8::Object::New(callback_state.isolate);
        [callback_state.availability dispose];
        NSString* date = update.releaseDate
            ? update.releaseDate.description
            : @"";
        v8::Local<v8::Value> arguments[] = {
            event,
            String(callback_state.isolate, update.releaseNotes ?: @""),
            String(callback_state.isolate, update.releaseName ?: @""),
            String(callback_state.isolate, date),
            String(callback_state.isolate,
                   update.updateURL.absoluteString ?: @"")};
        Emit("update-downloaded", 5, arguments);
      }
               error:^(NSError* error) {
                 AutoUpdaterState& callback_state = State();
                 [callback_state.availability dispose];
                 EmitError(error);
               }
           completed:^{
             AutoUpdaterState& callback_state = State();
             [callback_state.availability dispose];
             if (!found_update)
               Emit("update-not-available");
           }] retain];
}

void QuitAndInstall(const v8::FunctionCallbackInfo<v8::Value>& info) {
  AutoUpdaterState& state = State();
  if (!state.updater ||
      state.updater.state != SQRLUpdaterStateAwaitingRelaunch) {
    info.GetIsolate()->ThrowException(v8::Exception::Error(
        String(info.GetIsolate(), "No downloaded update is available")));
    return;
  }
  [state.install dispose];
  [state.install release];
  state.install = nil;
  Emit("before-quit-for-update");
  state.install = [[[state.updater relaunchToInstallUpdate]
      subscribeError:^(NSError* error) {
        EmitError(error);
      }] retain];
}

void InitAutoUpdater(v8::Local<v8::Object> exports,
                     v8::Local<v8::Value>,
                     v8::Local<v8::Context> context,
                     void*) {
  v8::Isolate* isolate = context->GetIsolate();
  AutoUpdaterState& state = State();
  state.isolate = isolate;
  state.context.Reset(isolate, context);
  v8::Local<v8::Object> updater = v8::Object::New(isolate);
  Method(context, updater, "setFeedURL", SetFeedURL);
  Method(context, updater, "getFeedURL", GetFeedURL);
  Method(context, updater, "checkForUpdates", CheckForUpdates);
  Method(context, updater, "quitAndInstall", QuitAndInstall);
  state.wrapper.Reset(isolate, updater);
  Set(context, exports, "autoUpdater", updater);
}

}  // namespace

void InstallMacAutoUpdaterBinding(node::Environment* environment) {
  node::AddLinkedBinding(environment, "electron_browser_auto_updater",
                         InitAutoUpdater, nullptr);
}

void ShutdownMacAutoUpdaterBinding() {
  AutoUpdaterState& state = State();
  [state.check dispose];
  [state.check release];
  state.check = nil;
  [state.availability dispose];
  [state.availability release];
  state.availability = nil;
  [state.install dispose];
  [state.install release];
  state.install = nil;
  [state.updater release];
  state.updater = nil;
  [state.feed_url release];
  state.feed_url = nil;
  state.wrapper.Reset();
  state.context.Reset();
  state.isolate = nullptr;
}

}  // namespace mini_electron::electron
