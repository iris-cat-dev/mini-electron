// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#include "runtime/storage/stock_profile_migrator.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "base/base64.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "build/build_config.h"
#include "third_party/leveldb/include/leveldb/db.h"
#include "third_party/sqlite/sqlite3.h"

#if BUILDFLAG(IS_WIN)
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#elif BUILDFLAG(IS_MAC)
#include <stdio.h>
#include <Security/Security.h>
#include "third_party/openssl/openssl/include/openssl/evp.h"
#elif BUILDFLAG(IS_LINUX)
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
#include "third_party/openssl/openssl/include/openssl/sha.h"

namespace mini_electron {
namespace {

constexpr int64_t kWindowsEpochOffsetSeconds = 11644473600LL;
constexpr size_t kDomainHashSize = 32;

bool RenameExclusively(const base::FilePath& source,
    const base::FilePath& destination)
{
#if BUILDFLAG(IS_WIN)
    return ::MoveFileW(source.value().c_str(), destination.value().c_str()) != 0;
#elif BUILDFLAG(IS_MAC)
    return renamex_np(source.value().c_str(), destination.value().c_str(),
               RENAME_EXCL) == 0;
#elif BUILDFLAG(IS_LINUX) && defined(SYS_renameat2)
    constexpr unsigned int kRenameNoReplace = 1;
    return syscall(SYS_renameat2, AT_FDCWD, source.value().c_str(),
               AT_FDCWD, destination.value().c_str(),
               kRenameNoReplace) == 0;
#else
    return false;
#endif
}

bool WriteAtomically(const base::FilePath& destination,
    std::string_view contents, std::string* error)
{
    if (base::PathExists(destination))
        return true;
    if (!base::CreateDirectory(destination.DirName())) {
        *error = "Cannot create migration destination";
        return false;
    }
    base::ScopedTempDir stagingContainer;
    if (!stagingContainer.CreateUniqueTempDirUnderPath(destination.DirName())) {
        *error = "Cannot create migration output staging directory";
        return false;
    }
    base::FilePath temporary =
        stagingContainer.GetPath().AppendASCII("output");
    if (!base::WriteFile(temporary, contents)) {
        *error = "Cannot write migration output";
        return false;
    }
    if (RenameExclusively(temporary, destination))
        return true;
    if (base::PathExists(destination))
        return true;
    *error = "Cannot commit migration output";
    return false;
}

bool CopySqliteSnapshot(const base::FilePath& source,
    const base::FilePath& destination, std::string* error)
{
    sqlite3* sourceDatabase = nullptr;
    sqlite3* destinationDatabase = nullptr;
    if (sqlite3_open_v2(source.AsUTF8Unsafe().c_str(), &sourceDatabase,
            SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK ||
        sqlite3_open_v2(destination.AsUTF8Unsafe().c_str(), &destinationDatabase,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
        if (sourceDatabase)
            sqlite3_close(sourceDatabase);
        if (destinationDatabase)
            sqlite3_close(destinationDatabase);
        *error = "Cannot open Electron Cookies database for snapshot";
        return false;
    }
    sqlite3_backup* backup = sqlite3_backup_init(
        destinationDatabase, "main", sourceDatabase, "main");
    bool copied = backup && sqlite3_backup_step(backup, -1) == SQLITE_DONE;
    if (backup)
        sqlite3_backup_finish(backup);
    int destinationStatus = sqlite3_errcode(destinationDatabase);
    sqlite3_close(sourceDatabase);
    sqlite3_close(destinationDatabase);
    if (!copied || destinationStatus != SQLITE_OK) {
        base::DeleteFile(destination);
        *error = "Cannot snapshot Electron Cookies database";
        return false;
    }
    return true;
}

#if BUILDFLAG(IS_WIN)
bool UnprotectDpapi(std::string_view encrypted, std::string* plaintext)
{
    DATA_BLOB input {
        static_cast<DWORD>(encrypted.size()),
        reinterpret_cast<BYTE*>(const_cast<char*>(encrypted.data()))
    };
    DATA_BLOB output {};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
            CRYPTPROTECT_UI_FORBIDDEN, &output))
        return false;
    plaintext->assign(reinterpret_cast<const char*>(output.pbData), output.cbData);
    LocalFree(output.pbData);
    return true;
}

bool DecryptAesGcm(std::string_view encrypted, std::string_view key,
    std::string* plaintext)
{
    if (encrypted.size() < 3 + 12 + 16 || key.size() != 32)
        return false;
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE keyHandle = nullptr;
    DWORD objectSize = 0, copied = 0;
    std::vector<unsigned char> keyObject;
    bool success = false;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM,
            nullptr, 0) < 0 ||
        BCryptSetProperty(algorithm, BCRYPT_CHAINING_MODE,
            reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
            sizeof(BCRYPT_CHAIN_MODE_GCM), 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &copied, 0) < 0)
        goto cleanup;
    keyObject.resize(objectSize);
    if (BCryptGenerateSymmetricKey(algorithm, &keyHandle, keyObject.data(),
            objectSize, reinterpret_cast<PUCHAR>(const_cast<char*>(key.data())),
            static_cast<ULONG>(key.size()), 0) < 0)
        goto cleanup;
    {
        std::string_view nonce = encrypted.substr(3, 12);
        std::string_view ciphertext = encrypted.substr(15, encrypted.size() - 31);
        std::string_view tag = encrypted.substr(encrypted.size() - 16);
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO auth;
        BCRYPT_INIT_AUTH_MODE_INFO(auth);
        auth.pbNonce = reinterpret_cast<PUCHAR>(const_cast<char*>(nonce.data()));
        auth.cbNonce = static_cast<ULONG>(nonce.size());
        auth.pbTag = reinterpret_cast<PUCHAR>(const_cast<char*>(tag.data()));
        auth.cbTag = static_cast<ULONG>(tag.size());
        plaintext->resize(ciphertext.size());
        ULONG written = 0;
        success = BCryptDecrypt(keyHandle,
            reinterpret_cast<PUCHAR>(const_cast<char*>(ciphertext.data())),
            static_cast<ULONG>(ciphertext.size()), &auth, nullptr, 0,
            reinterpret_cast<PUCHAR>(plaintext->data()),
            static_cast<ULONG>(plaintext->size()), &written, 0) >= 0;
        if (success)
            plaintext->resize(written);
    }
cleanup:
    if (keyHandle)
        BCryptDestroyKey(keyHandle);
    if (algorithm)
        BCryptCloseAlgorithmProvider(algorithm, 0);
    return success;
}

bool LoadCookieKey(const base::FilePath& userDataRoot,
    std::string* key, std::string* error)
{
    std::string json;
    if (!base::ReadFileToString(userDataRoot.AppendASCII("Local State"), &json)) {
        *error = "Electron Local State is unreadable";
        return false;
    }
    auto parsed = base::JSONReader::Read(json);
    const std::string* encoded = parsed && parsed->is_dict()
        ? parsed->GetDict().FindStringByDottedPath("os_crypt.encrypted_key")
        : nullptr;
    std::string wrapped;
    if (!encoded || !base::Base64Decode(*encoded, &wrapped) ||
        !base::StartsWith(wrapped, "DPAPI", base::CompareCase::SENSITIVE)) {
        *error = "Electron Local State has no supported DPAPI cookie key";
        return false;
    }
    if (!UnprotectDpapi(std::string_view(wrapped).substr(5), key)) {
        *error = "Windows DPAPI could not decrypt Electron cookie key";
        return false;
    }
    return true;
}
#elif BUILDFLAG(IS_MAC)
bool ReadKeychainPassword(const base::FilePath& userDataRoot,
    std::string* password)
{
    std::string product = userDataRoot.BaseName().AsUTF8Unsafe();
    std::vector<std::string> products = {
        product, "OMP", "OMP Desktop", "Electron"
    };
    for (const std::string& name : products) {
        if (name.empty())
            continue;
        std::string service = name + " Safe Storage";
        CFStringRef serviceString = CFStringCreateWithBytes(
            kCFAllocatorDefault,
            reinterpret_cast<const UInt8*>(service.data()),
            static_cast<CFIndex>(service.size()),
            kCFStringEncodingUTF8, false);
        CFStringRef accountString = CFStringCreateWithBytes(
            kCFAllocatorDefault,
            reinterpret_cast<const UInt8*>(name.data()),
            static_cast<CFIndex>(name.size()),
            kCFStringEncodingUTF8, false);
        if (!serviceString || !accountString) {
            if (serviceString)
                CFRelease(serviceString);
            if (accountString)
                CFRelease(accountString);
            continue;
        }
        const void* keys[] = {
            kSecClass, kSecAttrService, kSecAttrAccount,
            kSecReturnData, kSecMatchLimit
        };
        const void* values[] = {
            kSecClassGenericPassword, serviceString, accountString,
            kCFBooleanTrue, kSecMatchLimitOne
        };
        CFDictionaryRef query = CFDictionaryCreate(kCFAllocatorDefault,
            keys, values, static_cast<CFIndex>(std::size(keys)),
            &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks);
        CFTypeRef result = nullptr;
        OSStatus status = errSecParam;
        if (query)
            status = SecItemCopyMatching(query, &result);
        if (query)
            CFRelease(query);
        CFRelease(serviceString);
        CFRelease(accountString);
        if (status != errSecSuccess || !result) {
            if (result)
                CFRelease(result);
            continue;
        }
        bool isData = CFGetTypeID(result) == CFDataGetTypeID();
        if (isData) {
            CFDataRef data = static_cast<CFDataRef>(result);
            CFIndex length = CFDataGetLength(data);
            if (length > 0) {
                password->assign(
                    reinterpret_cast<const char*>(CFDataGetBytePtr(data)),
                    static_cast<size_t>(length));
            } else {
                password->clear();
            }
        }
        CFRelease(result);
        if (isData)
            return true;
    }
    return false;
}

bool DecryptMacV10(const base::FilePath& userDataRoot,
    std::string_view encrypted, std::string* plaintext, std::string* error)
{
    if (encrypted.size() <= 3) {
        *error = "Malformed macOS v10 cookie";
        return false;
    }
    std::string password;
    if (!ReadKeychainPassword(userDataRoot, &password)) {
        *error = "Electron Safe Storage password was not found in Keychain";
        return false;
    }
    unsigned char key[16];
    const unsigned char salt[] = "saltysalt";
    if (!PKCS5_PBKDF2_HMAC_SHA1(password.data(),
            static_cast<int>(password.size()), salt, sizeof(salt) - 1,
            1003, sizeof(key), key)) {
        *error = "Cannot derive macOS Electron cookie key";
        return false;
    }
    const unsigned char iv[16] = {
        ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
        ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '
    };
    EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
    if (!context)
        return false;
    std::string_view ciphertext = encrypted.substr(3);
    plaintext->resize(ciphertext.size() + 16);
    int first = 0, final = 0;
    bool ok = EVP_DecryptInit_ex(context, EVP_aes_128_cbc(), nullptr,
                  key, iv) == 1 &&
        EVP_DecryptUpdate(context,
            reinterpret_cast<unsigned char*>(plaintext->data()), &first,
            reinterpret_cast<const unsigned char*>(ciphertext.data()),
            static_cast<int>(ciphertext.size())) == 1 &&
        EVP_DecryptFinal_ex(context,
            reinterpret_cast<unsigned char*>(plaintext->data()) + first,
            &final) == 1;
    EVP_CIPHER_CTX_free(context);
    if (ok)
        plaintext->resize(first + final);
    else
        *error = "Cannot decrypt macOS Electron cookie";
    return ok;
}
#endif

bool RemoveDomainHash(int databaseVersion, const std::string& domain,
    std::string* value, std::string* error)
{
    if (databaseVersion < 24)
        return true;
    if (value->size() < kDomainHashSize) {
        *error = "Electron cookie is missing schema-24 domain hash";
        return false;
    }
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(domain.data()),
        domain.size(), digest);
    if (std::memcmp(digest, value->data(), kDomainHashSize) != 0) {
        *error = "Electron cookie domain hash does not match its host";
        return false;
    }
    value->erase(0, kDomainHashSize);
    return true;
}

bool DecryptCookie(const base::FilePath& userDataRoot, int databaseVersion,
    const std::string& domain, std::string_view encrypted,
    std::string* masterKey, std::string* plaintext, std::string* error)
{
    if (base::StartsWith(encrypted, "v20", base::CompareCase::SENSITIVE)) {
        *error = "Electron cookie uses unsupported app-bound v20 encryption";
        return false;
    }
#if BUILDFLAG(IS_WIN)
    if (base::StartsWith(encrypted, "v10", base::CompareCase::SENSITIVE) ||
        base::StartsWith(encrypted, "v11", base::CompareCase::SENSITIVE)) {
        if (masterKey->empty() &&
            !LoadCookieKey(userDataRoot, masterKey, error))
            return false;
        if (!DecryptAesGcm(encrypted, *masterKey, plaintext)) {
            *error = "Cannot AES-GCM decrypt Electron cookie";
            return false;
        }
    } else if (!UnprotectDpapi(encrypted, plaintext)) {
        *error = "Cannot DPAPI decrypt Electron cookie";
        return false;
    }
#elif BUILDFLAG(IS_MAC)
    if (!base::StartsWith(encrypted, "v10", base::CompareCase::SENSITIVE)) {
        *error = "Electron cookie uses unsupported macOS encryption version";
        return false;
    }
    if (!DecryptMacV10(userDataRoot, encrypted, plaintext, error))
        return false;
#else
    *error = "Encrypted Electron cookies are unsupported on this platform";
    return false;
#endif
    return RemoveDomainHash(databaseVersion, domain, plaintext, error);
}

struct SqliteCloser {
    void operator()(sqlite3* database) const { sqlite3_close(database); }
};
struct StatementCloser {
    void operator()(sqlite3_stmt* statement) const { sqlite3_finalize(statement); }
};

bool TableHasColumn(sqlite3* database, const char* table,
    const char* column)
{
    std::string query = "PRAGMA table_info(" + std::string(table) + ")";
    sqlite3_stmt* rawStatement = nullptr;
    if (sqlite3_prepare_v2(database, query.c_str(), -1,
            &rawStatement, nullptr) != SQLITE_OK)
        return false;
    std::unique_ptr<sqlite3_stmt, StatementCloser> statement(rawStatement);
    while (sqlite3_step(statement.get()) == SQLITE_ROW) {
        const unsigned char* name = sqlite3_column_text(statement.get(), 1);
        if (name && std::string_view(
                reinterpret_cast<const char*>(name)) == column)
            return true;
    }
    return false;
}

bool MigrateCookies(const base::FilePath& userDataRoot,
    const base::FilePath& profilePath, std::string* error)
{
    base::FilePath output = profilePath.AppendASCII("cookie.dat");
    if (base::PathExists(output))
        return true;
    base::FilePath source = profilePath.AppendASCII("Network")
        .AppendASCII("Cookies");
    if (!base::PathExists(source))
        source = profilePath.AppendASCII("Cookies");
    if (!base::PathExists(source))
        return true;

    base::ScopedTempDir temporary;
    if (!temporary.CreateUniqueTempDir()) {
        *error = "Cannot create Cookies migration snapshot";
        return false;
    }
    base::FilePath snapshot = temporary.GetPath().AppendASCII("Cookies");
    if (!CopySqliteSnapshot(source, snapshot, error))
        return false;
    sqlite3* rawDatabase = nullptr;
    if (sqlite3_open_v2(snapshot.AsUTF8Unsafe().c_str(), &rawDatabase,
            SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        if (rawDatabase)
            sqlite3_close(rawDatabase);
        *error = "Cannot open Electron Cookies snapshot";
        return false;
    }
    std::unique_ptr<sqlite3, SqliteCloser> database(rawDatabase);
    int databaseVersion = 0;
    sqlite3_stmt* rawVersion = nullptr;
    if (sqlite3_prepare_v2(database.get(),
            "SELECT value FROM meta WHERE key='version'", -1,
            &rawVersion, nullptr) == SQLITE_OK) {
        std::unique_ptr<sqlite3_stmt, StatementCloser> version(rawVersion);
        if (sqlite3_step(version.get()) == SQLITE_ROW)
            databaseVersion = sqlite3_column_int(version.get(), 0);
    }

    sqlite3_stmt* rawStatement = nullptr;
    const char* persistenceColumn =
        TableHasColumn(database.get(), "cookies", "is_persistent")
        ? "is_persistent"
        : (TableHasColumn(database.get(), "cookies", "has_expires")
              ? "has_expires" : "1");
    std::string query =
        "SELECT host_key,path,is_secure,is_httponly,expires_utc,name,value,"
        "encrypted_value," + std::string(persistenceColumn) + " FROM cookies";
    if (sqlite3_prepare_v2(database.get(), query.c_str(), -1,
            &rawStatement, nullptr) != SQLITE_OK) {
        *error = "Electron Cookies schema is unsupported";
        return false;
    }
    std::unique_ptr<sqlite3_stmt, StatementCloser> statement(rawStatement);
    std::string netscape("# Netscape HTTP Cookie File\n");
    std::string masterKey;
    size_t imported = 0;
    int stepStatus = SQLITE_OK;
    while ((stepStatus = sqlite3_step(statement.get())) == SQLITE_ROW) {
        auto textColumn = [&](int column) {
            const unsigned char* text = sqlite3_column_text(statement.get(), column);
            int length = sqlite3_column_bytes(statement.get(), column);
            return text ? std::string(reinterpret_cast<const char*>(text), length)
                        : std::string();
        };
        std::string domain = textColumn(0);
        std::string path = textColumn(1);
        bool secure = sqlite3_column_int(statement.get(), 2) != 0;
        bool httpOnly = sqlite3_column_int(statement.get(), 3) != 0;
        int64_t chromiumExpiry = sqlite3_column_int64(statement.get(), 4);
        std::string name = textColumn(5);
        std::string value = textColumn(6);
        const void* encryptedBytes = sqlite3_column_blob(statement.get(), 7);
        int encryptedLength = sqlite3_column_bytes(statement.get(), 7);
        bool persistent = sqlite3_column_int(statement.get(), 8) != 0;
        if (value.empty() && encryptedBytes && encryptedLength) {
            std::string_view encrypted(
                static_cast<const char*>(encryptedBytes), encryptedLength);
            if (!DecryptCookie(userDataRoot, databaseVersion, domain,
                    encrypted, &masterKey, &value, error))
                return false;
        }
        if (domain.empty() || name.empty() ||
            domain.find_first_of("\t\r\n") != std::string::npos ||
            path.find_first_of("\t\r\n") != std::string::npos ||
            name.find_first_of("\t\r\n") != std::string::npos ||
            value.find_first_of("\t\r\n") != std::string::npos) {
            *error = "Electron cookie contains unsupported control characters";
            return false;
        }
        int64_t unixExpiry = !persistent || chromiumExpiry == 0 ? 0 :
            chromiumExpiry / 1000000 - kWindowsEpochOffsetSeconds;
        std::string persistedDomain = httpOnly ? "#HttpOnly_" + domain : domain;
        netscape += persistedDomain + "\t" +
            (domain.front() == '.' ? "TRUE" : "FALSE") + "\t" +
            (path.empty() ? "/" : path) + "\t" +
            (secure ? "TRUE" : "FALSE") + "\t" +
            std::to_string(std::max<int64_t>(unixExpiry, 0)) + "\t" +
            name + "\t" + value + "\n";
        ++imported;
    }
    if (stepStatus != SQLITE_DONE) {
        *error = "Cannot read Electron Cookies snapshot";
        return false;
    }
    if (!imported)
        return true;
    return WriteAtomically(output, netscape, error);
}

bool DecodeStorageString(std::string_view encoded, std::string* decoded)
{
    if (encoded.empty()) {
        decoded->clear();
        return true;
    }
    unsigned char format = static_cast<unsigned char>(encoded.front());
    encoded.remove_prefix(1);
    if (format == 1) {
        std::u16string latin1(encoded.size(), 0);
        for (size_t i = 0; i < encoded.size(); ++i)
            latin1[i] = static_cast<unsigned char>(encoded[i]);
        return base::UTF16ToUTF8(latin1.data(), latin1.size(), decoded);
    }
    if (format != 0 || encoded.size() % 2)
        return false;
    std::u16string utf16(encoded.size() / 2, 0);
    for (size_t i = 0; i < utf16.size(); ++i) {
        utf16[i] = static_cast<unsigned char>(encoded[i * 2]) |
            (static_cast<unsigned char>(encoded[i * 2 + 1]) << 8);
    }
    return base::UTF16ToUTF8(utf16.data(), utf16.size(), decoded);
}

bool IsStorageOrigin(std::string_view origin)
{
    if (!base::IsStringUTF8(origin))
        return false;
    size_t separator = origin.find("://");
    if (separator == std::string_view::npos || separator == 0 ||
        !base::IsAsciiAlpha(origin.front()))
        return false;
    for (size_t i = 1; i < separator; ++i) {
        char character = origin[i];
        if (!base::IsAsciiAlphaNumeric(character) && character != '+' &&
            character != '-' && character != '.')
            return false;
    }
    for (unsigned char character : origin) {
        if (character < 0x20 || character == 0x7f)
            return false;
    }
    return true;
}

bool MigrateLocalStorage(const base::FilePath& profilePath,
    std::string* error)
{
    base::FilePath output = profilePath.AppendASCII("BrokerStorage.json");
    if (base::PathExists(output))
        return true;
    base::FilePath source = profilePath.AppendASCII("Local Storage")
        .AppendASCII("leveldb");
    if (!base::DirectoryExists(source))
        return true;
    base::ScopedTempDir temporary;
    if (!temporary.CreateUniqueTempDir()) {
        *error = "Cannot create Local Storage migration snapshot";
        return false;
    }
    base::FilePath snapshot = temporary.GetPath().AppendASCII("leveldb");
    if (!base::CopyDirectory(source, snapshot, true)) {
        *error = "Cannot snapshot Electron Local Storage LevelDB";
        return false;
    }
    leveldb::Options options;
    options.create_if_missing = false;
    leveldb::DB* rawDatabase = nullptr;
    leveldb::Status open = leveldb::DB::Open(
        options, snapshot.AsUTF8Unsafe(), &rawDatabase);
    if (!open.ok()) {
        *error = "Cannot open Electron Local Storage snapshot: " + open.ToString();
        return false;
    }
    std::unique_ptr<leveldb::DB> database(rawDatabase);
    std::unique_ptr<leveldb::Iterator> iterator(
        database->NewIterator(leveldb::ReadOptions()));
    base::Value::Dict origins;
    size_t imported = 0;
    for (iterator->SeekToFirst(); iterator->Valid(); iterator->Next()) {
        std::string key = iterator->key().ToString();
        // Chromium's metadata entries do not use the '_' data-record prefix.
        if (key.empty() || key.front() != '_')
            continue;
        size_t separator = key.find('\0', 1);
        if (separator == std::string::npos) {
            *error = "Electron Local Storage contains a malformed data key";
            return false;
        }
        std::string origin = key.substr(1, separator - 1);
        if (!IsStorageOrigin(origin)) {
            *error = "Electron Local Storage contains a malformed origin";
            return false;
        }
        std::string storageKey;
        std::string storageValue;
        if (!DecodeStorageString(
                std::string_view(key).substr(separator + 1), &storageKey) ||
            !DecodeStorageString(iterator->value().ToString(), &storageValue)) {
            *error =
                "Electron Local Storage contains an unsupported string encoding";
            return false;
        }
        base::Value::Dict* values = origins.FindDict(origin);
        if (!values) {
            origins.Set(origin, base::Value::Dict());
            values = origins.FindDict(origin);
        }
        if (values->Find(storageKey)) {
            *error =
                "Electron Local Storage contains duplicate decoded data keys";
            return false;
        }
        values->Set(storageKey, storageValue);
        ++imported;
    }
    if (!iterator->status().ok()) {
        *error = "Cannot read Electron Local Storage snapshot: " +
            iterator->status().ToString();
        return false;
    }
    if (!imported)
        return true;
    std::string json;
    if (!base::JSONWriter::Write(origins, &json)) {
        *error = "Cannot serialize migrated Electron Local Storage";
        return false;
    }
    return WriteAtomically(output, json, error);
}

} // namespace

bool MigrateStockElectronProfile(const base::FilePath& userDataRoot,
    const base::FilePath& profilePath, std::string* error)
{
    if (!MigrateCookies(userDataRoot, profilePath, error)) {
        LOG(ERROR) << "Stock Electron cookie migration failed: " << *error;
        return false;
    }
    if (!MigrateLocalStorage(profilePath, error)) {
        LOG(ERROR) << "Stock Electron Local Storage migration failed: " << *error;
        return false;
    }
    return true;
}

} // namespace mini_electron
