// Copyright 2026 mini-electron contributors.
#include "runtime/engine/renderer/webgl_context_provider.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_angle.h>
#include <GLES3/gl31.h>
#include <GLES2/gl2ext.h>
#include <GLES2/gl2ext_angle.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/functional/callback.h"
#include "base/logging.h"
#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/task/single_thread_task_runner.h"
#include "build/build_config.h"
#include "gpu/GLES2/gl2extchromium.h"
#include "gpu/command_buffer/client/context_support.h"
#include "gpu/command_buffer/client/gl_helper.h"
#include "gpu/command_buffer/client/gles2_interface.h"
#include "gpu/command_buffer/common/capabilities.h"
#include "gpu/config/gpu_feature_info.h"
#include "third_party/blink/public/platform/web_string.h"

namespace content {
namespace {

[[noreturn]] void Unsupported(const char* operation)
{
    LOG(FATAL) << "Direct WebGL has no command-buffer/shared-image contract for " << operation;
}

template <typename Proc> Proc Resolve(const char* name)
{
    auto proc = reinterpret_cast<Proc>(eglGetProcAddress(name));
    CHECK(proc) << "Missing pinned ANGLE entrypoint: " << name;
    return proc;
}

bool HasExtension(std::string_view list, std::string_view name)
{
    size_t offset = 0;
    while ((offset = list.find(name, offset)) != std::string_view::npos) {
        const size_t end = offset + name.size();
        if ((offset == 0 || list[offset - 1] == ' ') && (end == list.size() || list[end] == ' '))
            return true;
        offset = end;
    }
    return false;
}

struct DisplayState {
    EGLDisplay display = EGL_NO_DISPLAY;
    std::string extensions;
};

DisplayState& Display()
{
    // EGL termination would invalidate still-live worker contexts during shutdown.
    // The ANGLE display/device intentionally has process lifetime.
    static auto* state = new DisplayState;
    return *state;
}

std::atomic<GLuint> g_flush_serial { 1 };

// A direct, independent EGL context. The generated methods are ordinary native
// GL dispatch, not a GLES2InterfaceStub or a command-buffer implementation.
class DirectGLES2 final : public gpu::gles2::GLES2Interface, public gpu::ContextSupport {
public:
    DirectGLES2(EGLContext context, EGLSurface surface, int version)
        : context_(context), surface_(surface), version_(version) { }

    ~DirectGLES2() override
    {
        CHECK(!bound_ || owner_ == std::this_thread::get_id());
        if (eglGetCurrentContext() == context_)
            eglMakeCurrent(Display().display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(Display().display, context_);
        eglDestroySurface(Display().display, surface_);
    }

    bool Bind()
    {
        if (bound_ && owner_ != std::this_thread::get_id())
            return false;
        owner_ = std::this_thread::get_id();
        bound_ = true;
        return MakeCurrent();
    }

    bool MakeCurrent()
    {
        CHECK(bound_ && owner_ == std::this_thread::get_id());
        if (lost_)
            return false;
        if (eglGetCurrentContext() != context_ && !eglMakeCurrent(Display().display, surface_, surface_, context_)) {
            ReportError("eglMakeCurrent failed", eglGetError());
            MarkLost(GL_UNKNOWN_CONTEXT_RESET_KHR);
            return false;
        }
        return true;
    }

    // Initialization temporarily binds without acquiring sequence ownership. This
    // is critical for providers constructed on the main thread for worker Blink.
    bool Initialize(gpu::Capabilities* caps)
    {
        for (const char* extension : { "GL_OES_packed_depth_stencil", "GL_OES_rgb8_rgba8",
                 "GL_ANGLE_framebuffer_blit", "GL_ANGLE_framebuffer_multisample",
                 "GL_CHROMIUM_copy_texture", "GL_CHROMIUM_lose_context", "GL_KHR_robustness", "GL_KHR_debug",
                 "GL_NV_pixel_buffer_object", "GL_EXT_map_buffer_range", "GL_OES_mapbuffer" }) {
            const auto* available = reinterpret_cast<const char*>(glGetString(GL_REQUESTABLE_EXTENSIONS_ANGLE));
            if (available && HasExtension(available, extension))
                Resolve<PFNGLREQUESTEXTENSIONANGLEPROC>("glRequestExtensionANGLE")(extension);
        }
        RefreshExtensions();
        if (!HasExtension(extensions_, "GL_OES_packed_depth_stencil") ||
            !HasExtension(extensions_, "GL_KHR_robustness") ||
            !HasExtension(extensions_, "GL_ANGLE_request_extension") ||
            !HasExtension(extensions_, "GL_CHROMIUM_copy_texture"))
            return false;
        GLboolean robust_access = GL_FALSE;
        glGetBooleanv(GL_CONTEXT_ROBUST_ACCESS_EXT, &robust_access);
        if (!robust_access)
            return false;
        if (version_ == 3) {
            GLint major = 0, minor = 0;
            glGetIntegerv(GL_MAJOR_VERSION, &major);
            glGetIntegerv(GL_MINOR_VERSION, &minor);
            if (major != 3 || minor != 0)
                return false;
        } else {
            const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
            if (!version || std::strncmp(version, "OpenGL ES 2.0", 13) != 0)
                return false;
        }
        if (HasExtension(extensions_, "GL_KHR_debug")) {
            Resolve<PFNGLDEBUGMESSAGECALLBACKKHRPROC>("glDebugMessageCallbackKHR")(&DebugMessage, this);
            glEnable(GL_DEBUG_OUTPUT_KHR);
            glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS_KHR);
        }
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &caps->max_texture_size);
        caps->max_copy_texture_chromium_size = caps->max_texture_size;
        caps->texture_format_bgra8888 = HasExtension(extensions_, "GL_EXT_texture_format_BGRA8888");
        caps->texture_rg = version_ == 3 || HasExtension(extensions_, "GL_EXT_texture_rg");
        caps->texture_half_float_linear = HasExtension(extensions_, "GL_OES_texture_half_float_linear");
        caps->texture_norm16 = HasExtension(extensions_, "GL_EXT_texture_norm16");
        caps->gpu_memory_buffer_formats.Clear();
        caps->context_supports_distance_field_text = false;
        return glGetError() == GL_NO_ERROR;
    }

    void SetLostCallback(base::RepeatingClosure callback)
    {
        lost_callback_ = std::move(callback);
        if (lost_ && lost_callback_)
            ScheduleLostNotification();
    }

    GLenum GetGraphicsResetStatusKHR() override
    {
        if (lost_)
            return reset_status_;
        if (!MakeCurrent())
            return reset_status_;
        static const auto get_status = Resolve<PFNGLGETGRAPHICSRESETSTATUSKHRPROC>("glGetGraphicsResetStatusKHR");
        auto status = get_status();
        if (status != GL_NO_ERROR)
            MarkLost(status);
        return status;
    }

    GLenum GetError() override
    {
        if (!MakeCurrent())
            return GL_CONTEXT_LOST_KHR;
        const GLenum error = glGetError();
        if (error == GL_CONTEXT_LOST_KHR)
            MarkLost(GL_UNKNOWN_CONTEXT_RESET_KHR);
        else if (error != GL_NO_ERROR)
            ReportError("ANGLE WebGL error", error);
        return error;
    }

    const GLubyte* GetString(GLenum name) override
    {
        if (!MakeCurrent())
            return nullptr;
        return name == GL_EXTENSIONS ? reinterpret_cast<const GLubyte*>(extensions_.c_str()) : glGetString(name);
    }
    const GLubyte* GetStringi(GLenum name, GLuint index) override
    {
        if (!MakeCurrent())
            return nullptr;
        if (name != GL_EXTENSIONS)
            return glGetStringi(name, index);
        if (index >= extension_names_.size())
            return glGetStringi(name, ~GLuint { 0 }); // Native GL_INVALID_VALUE.
        return reinterpret_cast<const GLubyte*>(extension_names_[index].c_str());
    }
    void GetIntegerv(GLenum name, GLint* value) override
    {
        if (!MakeCurrent())
            return;
        if (name == GL_NUM_EXTENSIONS)
            *value = static_cast<GLint>(extension_names_.size());
        else
            glGetIntegerv(BufferTarget(name), value);
    }
    const GLchar* GetRequestableExtensionsCHROMIUM() override
    {
        if (!MakeCurrent())
            return "";
        return requestable_extensions_.c_str();
    }
    void RequestExtensionCHROMIUM(const char* name) override
    {
        if (!MakeCurrent())
            return;
        static const auto request = Resolve<PFNGLREQUESTEXTENSIONANGLEPROC>("glRequestExtensionANGLE");
        if (version_ == 3 && name && std::strcmp(name, "GL_EXT_disjoint_timer_query_webgl2") == 0)
            name = "GL_EXT_disjoint_timer_query";
        request(name);
        RefreshExtensions();
    }
    void LoseContextCHROMIUM(GLenum current, GLenum other) override
    {
        if (!MakeCurrent())
            return;
        static const auto lose = Resolve<PFNGLLOSECONTEXTCHROMIUMPROC>("glLoseContextCHROMIUM");
        lose(current, other);
        GetGraphicsResetStatusKHR();
    }

    void Flush() override
    {
        if (MakeCurrent()) {
            glFlush();
            flush_serial_ = g_flush_serial.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void Finish() override { if (MakeCurrent()) glFinish(); }
    void ShallowFlushCHROMIUM() override { Flush(); }
    void ShallowFinishCHROMIUM() override { Finish(); }
    void OrderingBarrierCHROMIUM() override { Flush(); }
    void DescheduleUntilFinishedCHROMIUM() override { Finish(); }
    GLuint GetLastFlushIdCHROMIUM() override { return MakeCurrent() ? flush_serial_ : 0; }
    void ContextVisibilityHintCHROMIUM(GLboolean) override
    {
        // No command-buffer scheduler exists. Submit pending work so hidden
        // contexts do not retain an unsubmitted Metal command buffer.
        Flush();
    }
    void InvalidateReadbackBufferShadowDataCHROMIUM(GLuint) override
    {
        // This adapter never caches readback buffers in CPU shadow storage.
        // Finish provides the required visibility for following native mapping.
        Finish();
    }
    void GenSyncTokenCHROMIUM(GLbyte*) override { Unsupported("GenSyncTokenCHROMIUM"); }
    void GenUnverifiedSyncTokenCHROMIUM(GLbyte*) override { Unsupported("GenUnverifiedSyncTokenCHROMIUM"); }
    void VerifySyncTokensCHROMIUM(GLbyte**, GLsizei) override { Unsupported("VerifySyncTokensCHROMIUM"); }
    void WaitSyncTokenCHROMIUM(const GLbyte*) override { Unsupported("WaitSyncTokenCHROMIUM"); }
    void FreeSharedMemory(void*) override { Unsupported("FreeSharedMemory"); }
    GLboolean DidGpuSwitch(gl::GpuPreference*) override { return GL_FALSE; }

    void BlitFramebufferCHROMIUM(GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLbitfield mask, GLenum filter) override
    {
        if (!MakeCurrent()) return;
        if (version_ == 3)
            glBlitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, filter);
        else {
            static const auto blit = Resolve<PFNGLBLITFRAMEBUFFERANGLEPROC>("glBlitFramebufferANGLE");
            blit(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, filter);
        }
    }
    void RenderbufferStorageMultisampleCHROMIUM(GLenum target, GLsizei samples, GLenum format, GLsizei width, GLsizei height) override
    {
        if (!MakeCurrent()) return;
        if (version_ == 3)
            glRenderbufferStorageMultisample(target, samples, format, width, height);
        else {
            static const auto storage = Resolve<PFNGLRENDERBUFFERSTORAGEMULTISAMPLEANGLEPROC>("glRenderbufferStorageMultisampleANGLE");
            storage(target, samples, format, width, height);
        }
    }

    void BindBuffer(GLenum target, GLuint buffer) override { if (MakeCurrent()) glBindBuffer(BufferTarget(target), buffer); }
    void BufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage) override { if (MakeCurrent()) glBufferData(BufferTarget(target), size, data, usage); }
    void BufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void* data) override { if (MakeCurrent()) glBufferSubData(BufferTarget(target), offset, size, data); }
    void* MapBufferCHROMIUM(GLuint target, GLenum access) override
    {
        if (!MakeCurrent()) return nullptr;
        target = BufferTarget(target);
        GLint size = 0;
        glGetBufferParameteriv(target, GL_BUFFER_SIZE, &size);
        const GLbitfield flags = access == GL_READ_ONLY ? GL_MAP_READ_BIT : access == GL_WRITE_ONLY ? GL_MAP_WRITE_BIT : GL_MAP_READ_BIT | GL_MAP_WRITE_BIT;
        if (version_ == 3)
            return glMapBufferRange(target, 0, size, flags);
        static const auto map = Resolve<PFNGLMAPBUFFERRANGEEXTPROC>("glMapBufferRangeEXT");
        return map(target, 0, size, flags);
    }
    GLboolean UnmapBufferCHROMIUM(GLuint target) override
    {
        if (!MakeCurrent()) return GL_FALSE;
        if (version_ == 3) return glUnmapBuffer(BufferTarget(target));
        static const auto unmap = Resolve<PFNGLUNMAPBUFFEROESPROC>("glUnmapBufferOES");
        return unmap(BufferTarget(target));
    }

    // GLHelper's async query denotes completion of the pixel transfer, not an
    // occlusion query. Complete it natively; SignalQuery may legally be immediate.
    void BeginQueryEXT(GLenum target, GLuint query) override
    {
        if (!MakeCurrent()) return;
        if (target == GL_ASYNC_PIXEL_PACK_COMPLETED_CHROMIUM) {
            CHECK(!pixel_pack_query_);
            pixel_pack_query_ = query;
        } else if (version_ == 3) {
            glBeginQuery(target, query);
        } else {
            static const auto begin = Resolve<PFNGLBEGINQUERYEXTPROC>("glBeginQueryEXT");
            begin(target, query);
        }
    }
    void EndQueryEXT(GLenum target) override
    {
        if (!MakeCurrent()) return;
        if (target == GL_ASYNC_PIXEL_PACK_COMPLETED_CHROMIUM) {
            CHECK(pixel_pack_query_);
            glFinish();
            pixel_pack_query_ = 0;
        } else if (version_ == 3) {
            glEndQuery(target);
        } else {
            static const auto end = Resolve<PFNGLENDQUERYEXTPROC>("glEndQueryEXT");
            end(target);
        }
    }
    void DeleteQueriesEXT(GLsizei count, const GLuint* queries) override
    {
        if (!MakeCurrent()) return;
        if (version_ == 3) {
            glDeleteQueries(count, queries);
        } else {
            static const auto remove = Resolve<PFNGLDELETEQUERIESEXTPROC>("glDeleteQueriesEXT");
            remove(count, queries);
        }
    }

    void MultiDrawElementsWEBGL(GLenum mode, const GLsizei* counts, GLenum type, const GLsizei* offsets, GLsizei draws) override
    {
        if (!MakeCurrent()) return;
        static const auto draw = Resolve<PFNGLMULTIDRAWELEMENTSANGLEPROC>("glMultiDrawElementsANGLE");
        draw(mode, counts, type, Indices(offsets, draws), draws);
    }
    void MultiDrawElementsInstancedWEBGL(GLenum mode, const GLsizei* counts, GLenum type, const GLsizei* offsets, const GLsizei* instances, GLsizei draws) override
    {
        if (!MakeCurrent()) return;
        static const auto draw = Resolve<PFNGLMULTIDRAWELEMENTSINSTANCEDANGLEPROC>("glMultiDrawElementsInstancedANGLE");
        draw(mode, counts, type, Indices(offsets, draws), instances, draws);
    }
    void MultiDrawElementsInstancedBaseVertexBaseInstanceWEBGL(GLenum mode, const GLsizei* counts, GLenum type, const GLsizei* offsets, const GLsizei* instances, const GLint* bases, const GLuint* first_instances, GLsizei draws) override
    {
        if (!MakeCurrent()) return;
        static const auto draw = Resolve<PFNGLMULTIDRAWELEMENTSINSTANCEDBASEVERTEXBASEINSTANCEANGLEPROC>("glMultiDrawElementsInstancedBaseVertexBaseInstanceANGLE");
        draw(mode, counts, type, Indices(offsets, draws), instances, bases, first_instances, draws);
    }

#include "runtime/engine/renderer/webgl_gles2_forwarding_autogen.inc"

    // Only real completion of local GL work is supported. There is deliberately
    // no sync-token namespace, shared-image manager, transfer cache, or GrContext.
    void FlushPendingWork() override { Flush(); }
    void SignalQuery(uint32_t, base::OnceClosure callback) override { Finish(); std::move(callback).Run(); }
    void SignalSyncToken(const gpu::SyncToken&, base::OnceClosure) override { Unsupported("SignalSyncToken"); }
    bool IsSyncTokenSignaled(const gpu::SyncToken&) override { Unsupported("IsSyncTokenSignaled"); }
    void GetGpuFence(uint32_t, base::OnceCallback<void(std::unique_ptr<gfx::GpuFence>)>) override { Unsupported("GetGpuFence"); }
    void SetAggressivelyFreeResources(bool aggressive) override { if (aggressive) Flush(); }
    uint64_t ShareGroupTracingGUID() const override { return reinterpret_cast<uintptr_t>(context_); }
    void SetErrorMessageCallback(base::RepeatingCallback<void(const char*, int32_t)> callback) override { error_callback_ = std::move(callback); }
    bool ThreadSafeShallowLockDiscardableTexture(uint32_t) override { Unsupported("ThreadSafeShallowLockDiscardableTexture"); }
    void CompleteLockDiscardableTexureOnContextThread(uint32_t) override { Unsupported("CompleteLockDiscardableTexureOnContextThread"); }
    bool ThreadsafeDiscardableTextureIsDeletedForTracing(uint32_t) override { Unsupported("ThreadsafeDiscardableTextureIsDeletedForTracing"); }
    void* MapTransferCacheEntry(uint32_t) override { Unsupported("MapTransferCacheEntry"); }
    void UnmapAndCreateTransferCacheEntry(uint32_t, uint32_t) override { Unsupported("UnmapAndCreateTransferCacheEntry"); }
    bool ThreadsafeLockTransferCacheEntry(uint32_t, uint32_t) override { Unsupported("ThreadsafeLockTransferCacheEntry"); }
    void UnlockTransferCacheEntries(const std::vector<std::pair<uint32_t, uint32_t>>&) override { Unsupported("UnlockTransferCacheEntries"); }
    void DeleteTransferCacheEntry(uint32_t, uint32_t) override { Unsupported("DeleteTransferCacheEntry"); }
    unsigned int GetTransferBufferFreeSize() const override { Unsupported("GetTransferBufferFreeSize"); }
    bool IsJpegDecodeAccelerationSupported() const override { return false; }
    bool IsWebPDecodeAccelerationSupported() const override { return false; }
    bool CanDecodeWithHardwareAcceleration(const cc::ImageHeaderMetadata*) const override { return false; }
    bool HasGrContextSupport() const override { return false; }
    void SetGrContext(GrDirectContext*) override { Unsupported("SetGrContext"); }
    void WillCallGLFromSkia() override { Unsupported("WillCallGLFromSkia"); }
    void DidCallGLFromSkia() override { Unsupported("DidCallGLFromSkia"); }

private:
    static GLenum BufferTarget(GLenum target)
    {
        switch (target) {
        case GL_PIXEL_PACK_TRANSFER_BUFFER_CHROMIUM: return GL_PIXEL_PACK_BUFFER;
        case GL_PIXEL_UNPACK_TRANSFER_BUFFER_CHROMIUM: return GL_PIXEL_UNPACK_BUFFER;
        case GL_PIXEL_PACK_TRANSFER_BUFFER_BINDING_CHROMIUM: return GL_PIXEL_PACK_BUFFER_BINDING;
        case GL_PIXEL_UNPACK_TRANSFER_BUFFER_BINDING_CHROMIUM: return GL_PIXEL_UNPACK_BUFFER_BINDING;
        default: return target;
        }
    }
    const void* const* Indices(const GLsizei* offsets, GLsizei count)
    {
        if (count <= 0) return nullptr; // Let ANGLE validate negative draw counts.
        indices_.resize(count);
        for (GLsizei i = 0; i < count; ++i)
            indices_[i] = reinterpret_cast<const void*>(static_cast<intptr_t>(offsets[i]));
        return indices_.data();
    }
    void RefreshExtensions()
    {
        const char* native = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
        extensions_ = native ? native : "";
        const char* requestable = reinterpret_cast<const char*>(glGetString(GL_REQUESTABLE_EXTENSIONS_ANGLE));
        requestable_extensions_ = requestable ? requestable : "";
        auto add = [this](const char* name) {
            if (!HasExtension(extensions_, name)) { extensions_ += ' '; extensions_ += name; }
        };
        if (version_ == 3) {
            add("GL_OES_packed_depth_stencil");
            add("GL_OES_rgb8_rgba8");
            if (HasExtension(extensions_, "GL_EXT_disjoint_timer_query"))
                add("GL_EXT_disjoint_timer_query_webgl2");
            if (HasExtension(requestable_extensions_, "GL_EXT_disjoint_timer_query")
                && !HasExtension(requestable_extensions_, "GL_EXT_disjoint_timer_query_webgl2"))
                requestable_extensions_ += " GL_EXT_disjoint_timer_query_webgl2";
        }
        if (version_ == 3 || (HasExtension(extensions_, "GL_ANGLE_framebuffer_blit") && HasExtension(extensions_, "GL_ANGLE_framebuffer_multisample")))
            add("GL_CHROMIUM_framebuffer_multisample");
        extension_names_.clear();
        size_t offset = 0;
        while (offset < extensions_.size()) {
            auto end = extensions_.find(' ', offset);
            if (end == std::string::npos) end = extensions_.size();
            if (end != offset) extension_names_.emplace_back(extensions_.substr(offset, end - offset));
            offset = end + 1;
        }
    }
    static void GL_APIENTRY DebugMessage(GLenum, GLenum, GLuint id, GLenum, GLsizei, const GLchar* message, const void* user)
    {
        static_cast<DirectGLES2*>(const_cast<void*>(user))->ReportError(message, id);
    }
    void ReportError(const char* message, int32_t id)
    {
        if (error_callback_) error_callback_.Run(message, id);
    }
    void ScheduleLostNotification()
    {
        if (!lost_callback_ || loss_notification_pending_)
            return;
        // Blink tears down its DrawingBuffer from this callback. Defer that
        // teardown until the current GLES/ANGLE dispatch has unwound.
        loss_notification_pending_ = true;
        base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
            base::BindOnce(&DirectGLES2::NotifyLost, weak_factory_.GetWeakPtr()));
    }
    void NotifyLost()
    {
        loss_notification_pending_ = false;
        auto callback = lost_callback_;
        if (callback)
            callback.Run();
    }
    void MarkLost(GLenum status)
    {
        if (lost_) return;
        lost_ = true;
        reset_status_ = status;
        ScheduleLostNotification();
    }

    const EGLContext context_;
    const EGLSurface surface_;
    const int version_;
    std::thread::id owner_;
    bool bound_ = false;
    bool lost_ = false;
    bool loss_notification_pending_ = false;
    GLenum reset_status_ = GL_NO_ERROR;
    GLuint flush_serial_ = 0;
    GLuint pixel_pack_query_ = 0;
    std::string extensions_;
    std::string requestable_extensions_;
    std::vector<std::string> extension_names_;
    std::vector<const void*> indices_;
    base::RepeatingClosure lost_callback_;
    base::RepeatingCallback<void(const char*, int32_t)> error_callback_;
    base::WeakPtrFactory<DirectGLES2> weak_factory_ { this };
};

class WebGLContextProvider final : public blink::WebGraphicsContext3DProvider {
public:
    explicit WebGLContextProvider(std::unique_ptr<DirectGLES2> gl) : gl_(std::move(gl))
    {
        preferences_.max_active_webgl_contexts = 16;
        preferences_.max_active_webgl_contexts_on_worker = 4;
        for (auto& status : features_.status_values)
            status = gpu::kGpuFeatureStatusDisabled;
        features_.status_values[gpu::GPU_FEATURE_TYPE_ACCELERATED_GL] = gpu::kGpuFeatureStatusEnabled;
#if BUILDFLAG(IS_MAC)
        features_.status_values[gpu::GPU_FEATURE_TYPE_METAL] = gpu::kGpuFeatureStatusEnabled;
#endif
        features_.status_values[gpu::GPU_FEATURE_TYPE_ACCELERATED_WEBGL] = gpu::kGpuFeatureStatusEnabled;
        features_.status_values[gpu::GPU_FEATURE_TYPE_ACCELERATED_WEBGL2] = gpu::kGpuFeatureStatusEnabled;
    }
    ~WebGLContextProvider() override
    {
        gl_->SetLostCallback({});
        gl_->SetErrorMessageCallback({});
        helper_.reset();
    }
    gpu::Capabilities& MutableCapabilities() { return caps_; }
    gpu::InterfaceBase* InterfaceBase() override { return gl_.get(); }
    gpu::gles2::GLES2Interface* ContextGL() override { return gl_.get(); }
    gpu::ContextSupport* ContextSupport() override { return gl_.get(); }
    gpu::raster::RasterInterface* RasterInterface() override { return nullptr; }
    gpu::webgpu::WebGPUInterface* WebGPUInterface() override { return nullptr; }
    gpu::SharedImageInterface* SharedImageInterface() override { return nullptr; }
    GrDirectContext* GetGrContext() override { return nullptr; }
    bool IsContextLost() override { return gl_->GetGraphicsResetStatusKHR() != GL_NO_ERROR; }
    bool BindToCurrentSequence() override { return gl_->Bind(); }
    const gpu::Capabilities& GetCapabilities() const override { return caps_; }
    const gpu::GpuFeatureInfo& GetGpuFeatureInfo() const override { return features_; }
    const blink::WebglPreferences& GetWebglPreferences() const override { return preferences_; }
    gpu::GLHelper* GetGLHelper() override
    {
        if (!helper_) helper_ = std::make_unique<gpu::GLHelper>(gl_.get(), gl_.get());
        return helper_.get();
    }
    void SetLostContextCallback(base::RepeatingClosure callback) override { gl_->SetLostCallback(std::move(callback)); }
    void SetErrorMessageCallback(base::RepeatingCallback<void(const char*, int32_t)> callback) override { gl_->SetErrorMessageCallback(std::move(callback)); }
    cc::ImageDecodeCache* ImageDecodeCache(SkColorType) override { Unsupported("ImageDecodeCache (no GrContext)"); }
    void CopyVideoFrame(media::PaintCanvasVideoRenderer*, media::VideoFrame*, cc::PaintCanvas*) override { Unsupported("CopyVideoFrame (no GrContext)"); }
    viz::RasterContextProvider* RasterContextProvider() const override { return nullptr; }
    unsigned int GetGrGLTextureFormat(viz::SharedImageFormat) const override { Unsupported("GetGrGLTextureFormat (no shared images)"); }
private:
    std::unique_ptr<DirectGLES2> gl_;
    gpu::Capabilities caps_;
    gpu::GpuFeatureInfo features_;
    blink::WebglPreferences preferences_;
    // Destroy helper GL resources before destroying their owning context.
    std::unique_ptr<gpu::GLHelper> helper_;
};

}  // namespace

bool InitializeWebGL()
{
    static std::once_flag once;
    std::call_once(once, [] {
        const EGLint platform[] = { EGL_PLATFORM_ANGLE_TYPE_ANGLE,
#if BUILDFLAG(IS_MAC)
            EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE,
#elif BUILDFLAG(IS_WIN)
            EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE,
#else
            EGL_PLATFORM_ANGLE_TYPE_OPENGLES_ANGLE,
#endif
            EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_DEVICE_TYPE_HARDWARE_ANGLE, EGL_NONE };
        auto get_display = Resolve<PFNEGLGETPLATFORMDISPLAYEXTPROC>("eglGetPlatformDisplayEXT");
        auto display = get_display(EGL_PLATFORM_ANGLE_ANGLE, nullptr, platform);
        if (display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr)) {
            LOG(ERROR) << "Cannot initialize ANGLE WebGL display: " << eglGetError();
            return;
        }
        const char* extensions = eglQueryString(display, EGL_EXTENSIONS);
        const std::string_view list = extensions ? extensions : "";
        for (const char* required : { "EGL_KHR_create_context",
                 "EGL_ANGLE_create_context_webgl_compatibility", "EGL_ANGLE_robust_resource_initialization",
                 "EGL_ANGLE_create_context_client_arrays", "EGL_CHROMIUM_create_context_bind_generates_resource",
                 "EGL_ANGLE_create_context_backwards_compatible" }) {
            if (!HasExtension(list, required)) {
                LOG(ERROR) << "ANGLE WebGL requires " << required;
                eglTerminate(display);
                return;
            }
        }
        Display().extensions = list;
        Display().display = display;
    });
    return Display().display != EGL_NO_DISPLAY;
}

std::unique_ptr<blink::WebGraphicsContext3DProvider> CreateWebGLContextProvider(
    const blink::Platform::ContextAttributes& attributes,
    blink::Platform::GraphicsInfo* info)
{
    auto fail = [info](const char* reason) -> std::unique_ptr<blink::WebGraphicsContext3DProvider> {
        if (info) info->error_message = blink::WebString::FromUTF8(reason);
        LOG(ERROR) << reason << " (EGL error " << eglGetError() << ')';
        return nullptr;
    };
    if (attributes.context_type != blink::Platform::kWebGL1ContextType && attributes.context_type != blink::Platform::kWebGL2ContextType)
        return fail("Direct provider only supports WebGL contexts");
    if (attributes.enable_raster_interface || attributes.support_grcontext)
        return fail("Direct WebGL does not provide a RasterInterface or GrContext");
    if (!InitializeWebGL()) return fail("ANGLE WebGL initialization failed");
    const int version = attributes.context_type == blink::Platform::kWebGL2ContextType ? 3 : 2;
    const EGLint config_attributes[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
        version == 3 ? EGL_OPENGL_ES3_BIT_KHR : EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
    EGLConfig config = nullptr;
    EGLint count = 0;
    if (!eglChooseConfig(Display().display, config_attributes, &config, 1, &count) || count != 1)
        return fail("ANGLE has no compatible WebGL EGL config");
    std::vector<EGLint> context_attributes { EGL_CONTEXT_MAJOR_VERSION_KHR, version, EGL_CONTEXT_MINOR_VERSION_KHR, 0,
        EGL_CONTEXT_OPENGL_BACKWARDS_COMPATIBLE_ANGLE, EGL_FALSE,
        EGL_CONTEXT_WEBGL_COMPATIBILITY_ANGLE, EGL_TRUE,
        EGL_ROBUST_RESOURCE_INITIALIZATION_ANGLE, EGL_TRUE,
        EGL_CONTEXT_OPENGL_ROBUST_ACCESS, EGL_TRUE,
        EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY_KHR, EGL_LOSE_CONTEXT_ON_RESET_KHR,
        EGL_CONTEXT_CLIENT_ARRAYS_ENABLED_ANGLE, EGL_FALSE,
        EGL_CONTEXT_BIND_GENERATES_RESOURCE_CHROMIUM, EGL_FALSE };
    if (HasExtension(Display().extensions, "EGL_ANGLE_power_preference")) {
        context_attributes.push_back(EGL_POWER_PREFERENCE_ANGLE);
        context_attributes.push_back(attributes.prefer_low_power_gpu ? EGL_LOW_POWER_ANGLE : EGL_HIGH_POWER_ANGLE);
    }
    context_attributes.push_back(EGL_NONE);
    if (!eglBindAPI(EGL_OPENGL_ES_API)) return fail("Cannot bind EGL GLES API");
    const EGLContext context = eglCreateContext(Display().display, config, EGL_NO_CONTEXT, context_attributes.data());
    if (context == EGL_NO_CONTEXT) return fail("Cannot create robust exact-version WebGL EGL context");
    const EGLint surface_attributes[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    const EGLSurface surface = eglCreatePbufferSurface(Display().display, config, surface_attributes);
    if (surface == EGL_NO_SURFACE) {
        eglDestroyContext(Display().display, context);
        return fail("Cannot create WebGL EGL backing surface");
    }
    const EGLDisplay previous_display = eglGetCurrentDisplay();
    const EGLContext previous_context = eglGetCurrentContext();
    const EGLSurface previous_draw = eglGetCurrentSurface(EGL_DRAW);
    const EGLSurface previous_read = eglGetCurrentSurface(EGL_READ);
    auto adapter = std::make_unique<DirectGLES2>(context, surface, version);
    auto* direct = adapter.get();
    auto provider = std::make_unique<WebGLContextProvider>(std::move(adapter));
    bool success = eglMakeCurrent(Display().display, surface, surface, context) && direct->Initialize(&provider->MutableCapabilities());
    if (success && info) {
        info->using_gpu_compositing = false;
        info->using_passthrough_command_decoder = false;
        info->reset_notification_strategy = GL_LOSE_CONTEXT_ON_RESET_KHR;
#if BUILDFLAG(IS_MAC)
        info->angle_implementation = gl::ANGLEImplementation::kMetal;
#elif BUILDFLAG(IS_WIN)
        info->angle_implementation = gl::ANGLEImplementation::kD3D11;
#endif
        info->vendor_info = blink::WebString::FromUTF8(reinterpret_cast<const char*>(glGetString(GL_VENDOR)));
        info->renderer_info = blink::WebString::FromUTF8(reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
        info->driver_version = blink::WebString::FromUTF8(reinterpret_cast<const char*>(glGetString(GL_VERSION)));
    }
    // Restore an existing provider's context, including its read/draw surfaces;
    // leave the new context detached for BindToCurrentSequence on its real owner.
    if (!eglMakeCurrent(previous_context == EGL_NO_CONTEXT ? Display().display : previous_display,
            previous_draw, previous_read, previous_context))
        success = false;
    if (!success) return fail("ANGLE WebGL capabilities or mandatory extensions unavailable");
    return provider;
}

}  // namespace content
