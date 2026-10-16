// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// BCrypt hashing (SHA-1 and SHA-256 only) over CommonCrypto.

#include <windows.h>

using BCRYPT_HANDLE = void*;
using BCRYPT_ALG_HANDLE = void*;
using BCRYPT_HASH_HANDLE = void*;
#ifndef NT_SUCCESS
#define NT_SUCCESS(status) ((static_cast<NTSTATUS>(status)) >= 0)
#endif
#define STATUS_SUCCESS (static_cast<NTSTATUS>(0x00000000))
#define STATUS_UNSUCCESSFUL (static_cast<NTSTATUS>(0xC0000001u))
#define STATUS_NOT_SUPPORTED (static_cast<NTSTATUS>(0xC00000BBu))
#define STATUS_INVALID_PARAMETER (static_cast<NTSTATUS>(0xC000000Du))
#define STATUS_BUFFER_TOO_SMALL (static_cast<NTSTATUS>(0xC0000023u))
#define BCRYPT_SHA1_ALGORITHM L"SHA1"
#define BCRYPT_SHA256_ALGORITHM L"SHA256"
#define BCRYPT_OBJECT_LENGTH L"ObjectLength"
#define BCRYPT_HASH_LENGTH L"HashDigestLength"
#define BCRYPT_ALG_HANDLE_HMAC_FLAG 0x00000008u

NTSTATUS BCryptOpenAlgorithmProvider(BCRYPT_ALG_HANDLE* algorithm, LPCWSTR id,
    LPCWSTR implementation, ULONG flags) noexcept;
NTSTATUS BCryptCloseAlgorithmProvider(BCRYPT_ALG_HANDLE algorithm, ULONG flags) noexcept;
NTSTATUS BCryptGetProperty(BCRYPT_HANDLE object, LPCWSTR property, PUCHAR output, ULONG size,
    ULONG* result, ULONG flags) noexcept;
NTSTATUS BCryptCreateHash(BCRYPT_ALG_HANDLE algorithm, BCRYPT_HASH_HANDLE* hash, PUCHAR object,
    ULONG object_size, PUCHAR secret, ULONG secret_size, ULONG flags) noexcept;
NTSTATUS BCryptHashData(BCRYPT_HASH_HANDLE hash, PUCHAR input, ULONG size, ULONG flags) noexcept;
NTSTATUS BCryptFinishHash(BCRYPT_HASH_HANDLE hash, PUCHAR output, ULONG size, ULONG flags) noexcept;
NTSTATUS BCryptDestroyHash(BCRYPT_HASH_HANDLE hash) noexcept;
