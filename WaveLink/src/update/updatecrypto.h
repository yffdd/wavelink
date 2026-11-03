#pragma once
// Windows system cryptography only: no OpenSSL runtime or private key in the app.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <vector>
#include <string>

namespace UpdateCrypto {
inline bool verify(const std::vector<unsigned char> &publicBlob,
                   const std::vector<unsigned char> &digest,
                   const std::vector<unsigned char> &signature) {
    BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_KEY_HANDLE key = nullptr;
    bool ok = false;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_RSA_ALGORITHM, nullptr, 0) >= 0) {
        BCRYPT_RSAKEY_BLOB header{};
        if (publicBlob.size() >= sizeof(header)) memcpy(&header, publicBlob.data(), sizeof(header));
        if (header.Magic == BCRYPT_RSAPUBLIC_MAGIC && header.BitLength >= 2048 && header.BitLength <= 8192 &&
            publicBlob.size() >= sizeof(BCRYPT_RSAKEY_BLOB) &&
            BCryptImportKeyPair(algorithm, nullptr, BCRYPT_RSAPUBLIC_BLOB, &key,
                const_cast<PUCHAR>(publicBlob.data()), ULONG(publicBlob.size()), 0) >= 0) {
            BCRYPT_PKCS1_PADDING_INFO padding{BCRYPT_SHA256_ALGORITHM};
            ok = BCryptVerifySignature(key, &padding, const_cast<PUCHAR>(digest.data()), ULONG(digest.size()),
                const_cast<PUCHAR>(signature.data()), ULONG(signature.size()), BCRYPT_PAD_PKCS1) >= 0;
        }
    }
    if (key) BCryptDestroyKey(key);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok;
}
inline bool hashFile(HANDLE file, std::wstring &hex) {
    BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = false; DWORD size = 0, result = 0; unsigned char digest[32];
    std::vector<unsigned char> object, buffer(64 * 1024);
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return false;
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&size), sizeof(size), &result, 0) >= 0) {
        object.resize(size);
        if (BCryptCreateHash(algorithm, &hash, object.data(), size, nullptr, 0, 0) >= 0) {
            LARGE_INTEGER zero{}; ok = SetFilePointerEx(file, zero, nullptr, FILE_BEGIN);
            while (ok) {
                DWORD read = 0;
                if (!ReadFile(file, buffer.data(), DWORD(buffer.size()), &read, nullptr)) { ok = false; break; }
                if (!read) break;
                ok = BCryptHashData(hash, buffer.data(), read, 0) >= 0;
            }
            if (ok) ok = BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0;
        }
    }
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (ok) {
        static const wchar_t digits[] = L"0123456789abcdef"; hex.clear();
        for (unsigned char byte : digest) { hex += digits[byte >> 4]; hex += digits[byte & 15]; }
    }
    return ok;
}
}
#endif
