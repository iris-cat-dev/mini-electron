#include <cstring>
#include <limits>
#include <string>

#include "build/build_config.h"
#include "src/runtime/electron/common/node_register_help.h"
#include "src/runtime/electron/common/gin_helper/dictionary.h"
#include "src/runtime/electron/common/gin_helper/error_thrower.h"
#include "third_party/libnode/src/node_binding.h"
#include "third_party/libnode/src/node_buffer.h"

#if BUILDFLAG(IS_WIN)
#include <windows.h>
#include <wincrypt.h>
#include "src/runtime/electron/browser/api/app.h"
#endif

namespace atom {
namespace {

#if BUILDFLAG(IS_WIN)
bool protectData(const char* data, size_t size, DATA_BLOB* output)
{
    if (size > std::numeric_limits<DWORD>::max())
        return false;
    DATA_BLOB input { static_cast<DWORD>(size),
        reinterpret_cast<BYTE*>(const_cast<char*>(data)) };
    // Omit CRYPTPROTECT_LOCAL_MACHINE: protection belongs to the current user.
    return ::CryptProtectData(&input, nullptr, nullptr, nullptr, nullptr,
        CRYPTPROTECT_UI_FORBIDDEN, output) != FALSE;
}

bool unprotectData(const char* data, size_t size, DATA_BLOB* output)
{
    if (size > std::numeric_limits<DWORD>::max())
        return false;
    DATA_BLOB input { static_cast<DWORD>(size),
        reinterpret_cast<BYTE*>(const_cast<char*>(data)) };
    return ::CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
        CRYPTPROTECT_UI_FORBIDDEN, output) != FALSE;
}

bool probeEncryption()
{
    constexpr char probe[] = "mini-electron safeStorage probe";
    DATA_BLOB encrypted {};
    if (!protectData(probe, sizeof(probe), &encrypted))
        return false;
    DATA_BLOB decrypted {};
    bool available = unprotectData(reinterpret_cast<const char*>(encrypted.pbData),
        encrypted.cbData, &decrypted);
    available = available && decrypted.cbData == sizeof(probe)
        && std::memcmp(decrypted.pbData, probe, sizeof(probe)) == 0;
    if (decrypted.pbData) {
        ::SecureZeroMemory(decrypted.pbData, decrypted.cbData);
        ::LocalFree(decrypted.pbData);
    }
    ::LocalFree(encrypted.pbData);
    return available;
}
#endif

void setUsePlainTextEncryption(bool)
{
    // No plaintext or fixed-key fallback, including on unsupported platforms.
}

bool isEncryptionAvailable()
{
#if BUILDFLAG(IS_WIN)
    const App* app = App::getInstance();
    if (!app || !app->isReadyApi())
        return false;
    static const bool available = probeEncryption();
    return available;
#else
    return false;
#endif
}

v8::Local<v8::Value> encryptString(v8::Isolate* isolate, const std::string& plaintext)
{
    if (!isEncryptionAvailable()) {
        gin_helper::ErrorThrower(isolate).ThrowError("Error while encrypting the text provided to safeStorage.encryptString. Encryption is not available.");
        return {};
    }

#if BUILDFLAG(IS_WIN)
    DATA_BLOB encrypted {};
    if (!protectData(plaintext.data(), plaintext.size(), &encrypted)) {
        gin_helper::ErrorThrower(isolate).ThrowError("Error while encrypting the text provided to safeStorage.encryptString.");
        return {};
    }

    // The format is the native, authenticated DPAPI blob, not a v10/v11 payload.
    // Transfer the OS allocation to the Buffer without copying ciphertext.
    return node::Buffer::New(isolate, reinterpret_cast<char*>(encrypted.pbData),
        encrypted.cbData, [](char* data, void*) { ::LocalFree(data); },
        nullptr).ToLocalChecked();
#else
    return {};
#endif
}

v8::Local<v8::Value> decryptString(v8::Isolate* isolate, v8::Local<v8::Value> buffer)
{
    if (!isEncryptionAvailable()) {
        gin_helper::ErrorThrower(isolate).ThrowError("Error while decrypting the ciphertext provided to "
                                                     "safeStorage.decryptString. Decryption is not available.");
        return {};
    }

    if (!node::Buffer::HasInstance(buffer)) {
        gin_helper::ErrorThrower(isolate).ThrowError("Expected the first argument of decryptString() to be a buffer");
        return {};
    }

    const size_t size = node::Buffer::Length(buffer);
    if (size == 0)
        return v8::String::Empty(isolate);

#if BUILDFLAG(IS_WIN)
    DATA_BLOB decrypted {};
    if (!unprotectData(node::Buffer::Data(buffer), size, &decrypted)) {
        gin_helper::ErrorThrower(isolate).ThrowError("Error while decrypting the ciphertext provided to safeStorage.decryptString.");
        return {};
    }

    v8::Local<v8::String> plaintext;
    bool converted = decrypted.cbData == 0;
    if (converted) {
        plaintext = v8::String::Empty(isolate);
    } else if (decrypted.cbData <= v8::String::kMaxLength) {
        // Explicit byte length preserves embedded NULs and UTF-8 strings.
        converted = v8::String::NewFromUtf8(isolate,
            reinterpret_cast<const char*>(decrypted.pbData),
            v8::NewStringType::kNormal, static_cast<int>(decrypted.cbData)).ToLocal(&plaintext);
    } else {
        gin_helper::ErrorThrower(isolate).ThrowError("Error while decrypting the ciphertext provided to safeStorage.decryptString. Text is too long.");
    }
    ::SecureZeroMemory(decrypted.pbData, decrypted.cbData);
    ::LocalFree(decrypted.pbData);
    if (!converted)
        return {};
    return plaintext;
#else
    return {};
#endif
}

} // namespace

static void initializeApiSafeStorage(v8::Local<v8::Object> exports, v8::Local<v8::Value> unused, v8::Local<v8::Context> context, void* priv)
{
    v8::Isolate* isolate = context->GetIsolate();
    gin_helper::Dictionary dict(isolate, exports);
    dict.SetMethodT("decryptString", &decryptString);
    dict.SetMethodT("encryptString", &encryptString);
    dict.SetMethodT("isEncryptionAvailable", &isEncryptionAvailable);
    dict.SetMethodT("setUsePlainTextEncryption", &setUsePlainTextEncryption);
}

static const char SafeStorageSricpt[] = "exports = {};";

static NodeNative nativeSafeStorageNative { "ApiSafeStorage", SafeStorageSricpt, sizeof(SafeStorageSricpt) - 1 };

NODE_MODULE_CONTEXT_AWARE_BUILTIN_SCRIPT_MANUAL(electron_browser_safe_storage, initializeApiSafeStorage, &nativeSafeStorageNative)

} // atom