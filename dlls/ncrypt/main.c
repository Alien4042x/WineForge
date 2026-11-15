/*
 * New cryptographic library (ncrypt.dll)
 *
 * Copyright 2016 Alex Henrie
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdarg.h>
#include <stdlib.h>

#include "ntstatus.h"
#include "windef.h"
#include "winbase.h"
#include "winreg.h"
#include "wincrypt.h"
#include "ncrypt.h"
#include "bcrypt.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(ncrypt);

enum algid
{
    /* symmetric */
    AES,
    /* asymmetric */
    RSA,
    DSA,
    ECDSA,
    ECDSA_P256,
    ECDSA_P384,
    ECDSA_P521,
};

enum object_type
{
    KEY,
    PROVIDER,
};

struct object_property
{
    WCHAR *key;
    DWORD  value_size;
    void  *value;
};

struct object
{
    enum object_type        type;
    LONG                    refs;
    DWORD                   num_properties;
    struct object_property *properties;
};

struct key
{
    struct object     obj;
    enum algid        algid;
    BCRYPT_KEY_HANDLE handle;
    WCHAR            *name;
    BOOL              overwrite;
};

struct provider
{
    struct object obj;
    BOOL software;
};

static SECURITY_STATUS map_ntstatus(NTSTATUS status)
{
    switch (status)
    {
    case STATUS_INVALID_HANDLE:    return NTE_INVALID_HANDLE;
    case STATUS_INVALID_SIGNATURE: return NTE_BAD_SIGNATURE;
    case STATUS_SUCCESS:           return ERROR_SUCCESS;
    case STATUS_INVALID_PARAMETER: return NTE_INVALID_PARAMETER;
    case STATUS_NO_MEMORY:         return NTE_NO_MEMORY;
    case STATUS_NOT_SUPPORTED:     return NTE_NOT_SUPPORTED;
    case NTE_BAD_DATA:             return NTE_BAD_DATA;
    case STATUS_BUFFER_TOO_SMALL:  return NTE_BUFFER_TOO_SMALL;
    default:
        FIXME("unhandled status %#lx\n", status);
        return NTE_INTERNAL_ERROR;
    }
}

static struct key *allocate_key(void)
{
    struct key *ret;
    if (!(ret = calloc(1, sizeof(*ret)))) return NULL;
    ret->obj.type = KEY;
    ret->obj.refs = 1;
    return ret;
}

static struct provider *allocate_provider(void)
{
    struct provider *ret;
    if (!(ret = calloc(1, sizeof(*ret)))) return NULL;
    ret->obj.type = PROVIDER;
    ret->obj.refs = 1;
    return ret;
}

static void provider_addref(NCRYPT_PROV_HANDLE handle)
{
    struct provider *provider = (struct provider *)handle;
    InterlockedIncrement(&provider->obj.refs);
}

static struct object_property *get_object_property(struct object *object, const WCHAR *name)
{
    unsigned int i;
    for (i = 0; i < object->num_properties; i++)
    {
        struct object_property *property = &object->properties[i];
        if (!lstrcmpW(property->key, name)) return property;
    }
    return NULL;
}

struct object_property *add_object_property(struct object *object, const WCHAR *name)
{
    struct object_property *property;

    if (!object->num_properties)
    {
        if (!(object->properties = malloc(sizeof(*property))))
        {
            ERR("Error allocating memory.\n");
            return NULL;
        }
        property = &object->properties[object->num_properties++];
    }
    else
    {
        struct object_property *tmp;
        if (!(tmp = realloc(object->properties, sizeof(*property) * (object->num_properties + 1))))
        {
            ERR("Error allocating memory.\n");
            return NULL;
        }
        object->properties = tmp;
        property = &object->properties[object->num_properties++];
    }

    memset(property, 0, sizeof(*property));
    if (!(property->key = wcsdup(name)))
    {
        ERR("Error allocating memory.\n");
        return NULL;
    }

    return property;
}

static SECURITY_STATUS set_object_property(struct object *object, const WCHAR *name, BYTE *value, DWORD value_size)
{
    struct object_property *property = get_object_property(object, name);
    void *tmp;

    if (!property && !(property = add_object_property(object, name))) return NTE_NO_MEMORY;

    property->value_size = value_size;
    if (!(tmp = realloc(property->value, value_size)))
    {
        ERR("Error allocating memory.\n");
        free(property->key);
        property->key = NULL;
        return NTE_NO_MEMORY;
    }

    property->value = tmp;
    memcpy(property->value, value, value_size);

    return ERROR_SUCCESS;
}

static struct key *create_key_object(enum algid algid, NCRYPT_PROV_HANDLE provider)
{
    NCRYPT_SUPPORTED_LENGTHS supported_lengths = {512, 16384, 8, 1024};
    struct key *key;
    DWORD dw_value;

    if (!(key = allocate_key())) return NULL;

    switch (algid)
    {
    case RSA:
        key->algid = RSA;
        set_object_property(&key->obj, NCRYPT_ALGORITHM_PROPERTY, (BYTE *)BCRYPT_RSA_ALGORITHM,
                            sizeof(BCRYPT_RSA_ALGORITHM));
        set_object_property(&key->obj, NCRYPT_ALGORITHM_GROUP_PROPERTY, (BYTE *)BCRYPT_RSA_ALGORITHM,
                            sizeof(BCRYPT_RSA_ALGORITHM));
        set_object_property(&key->obj, NCRYPT_LENGTHS_PROPERTY, (BYTE *)&supported_lengths,
                            sizeof(supported_lengths));
        dw_value = 128;
        set_object_property(&key->obj, NCRYPT_BLOCK_LENGTH_PROPERTY, (BYTE *)&dw_value, sizeof(dw_value));
        dw_value = 128;
        set_object_property(&key->obj, BCRYPT_SIGNATURE_LENGTH, (BYTE *)&dw_value, sizeof(dw_value));
        break;

    case ECDSA_P256:
        key->algid = ECDSA_P256;
        set_object_property(&key->obj, NCRYPT_ALGORITHM_PROPERTY, (BYTE *)BCRYPT_ECDSA_P256_ALGORITHM,
                            sizeof(BCRYPT_ECDSA_P256_ALGORITHM));
        set_object_property(&key->obj, NCRYPT_ALGORITHM_GROUP_PROPERTY, (BYTE *)BCRYPT_ECDSA_ALGORITHM,
                            sizeof(BCRYPT_ECDSA_ALGORITHM));
        break;

    case ECDSA_P384:
        key->algid = ECDSA_P384;
        set_object_property(&key->obj, NCRYPT_ALGORITHM_PROPERTY, (BYTE *)BCRYPT_ECDSA_P384_ALGORITHM,
                            sizeof(BCRYPT_ECDSA_P384_ALGORITHM));
        set_object_property(&key->obj, NCRYPT_ALGORITHM_GROUP_PROPERTY, (BYTE *)BCRYPT_ECDSA_ALGORITHM,
                            sizeof(BCRYPT_ECDSA_ALGORITHM));
        break;

    case ECDSA_P521:
        key->algid = ECDSA_P521;
        set_object_property(&key->obj, NCRYPT_ALGORITHM_PROPERTY, (BYTE *)BCRYPT_ECDSA_P521_ALGORITHM,
                            sizeof(BCRYPT_ECDSA_P521_ALGORITHM));
        set_object_property(&key->obj, NCRYPT_ALGORITHM_GROUP_PROPERTY, (BYTE *)BCRYPT_ECDSA_ALGORITHM,
                            sizeof(BCRYPT_ECDSA_ALGORITHM));
        break;

    default:
        ERR("Invalid algid %#x\n", algid);
        return NULL;
    }

    dw_value = 0;
    set_object_property(&key->obj, NCRYPT_EXPORT_POLICY_PROPERTY, (BYTE *)&dw_value, sizeof(dw_value));
    dw_value = NCRYPT_ALLOW_ALL_USAGES;
    set_object_property(&key->obj, NCRYPT_KEY_USAGE_PROPERTY, (BYTE *)&dw_value, sizeof(dw_value));
    dw_value = 0;
    set_object_property(&key->obj, NCRYPT_KEY_TYPE_PROPERTY, (BYTE *)&dw_value, sizeof(dw_value));
    provider_addref(provider);
    set_object_property(&key->obj, NCRYPT_PROVIDER_HANDLE_PROPERTY, (BYTE *)&provider, sizeof(provider));
    return key;
}

/* WineForge-Internal: crypto/cng-p256-persistence-v1.
 * Keep current-user software P-256 keys protected across process lifetimes.
 * Other providers, algorithms and machine-key storage retain their existing path. */
static const WCHAR p256_storage_path[] = L"Software\\Wine\\Crypto\\CNG\\P256";

struct stored_p256_key
{
    DWORD magic;
    DWORD version;
    DWORD blob_size;
    DWORD export_policy;
    BYTE blob[];
};

static SECURITY_STATUS storage_error(LONG error)
{
    if (!error) return ERROR_SUCCESS;
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return NTE_BAD_KEYSET;
    if (error == ERROR_ACCESS_DENIED) return NTE_PERM;
    return NTE_FAIL;
}

static SECURITY_STATUS p256_storage_name(const WCHAR *name, WCHAR output[65])
{
    static const WCHAR hex[] = L"0123456789abcdef";
    BYTE hash[32];
    SIZE_T length;
    unsigned int i;
    NTSTATUS status;

    if (!name || !(length = wcslen(name)) || length > 512) return NTE_INVALID_PARAMETER;
    status = BCryptHash(BCRYPT_SHA256_ALG_HANDLE, NULL, 0, (BYTE *)name,
                        length * sizeof(WCHAR), hash, sizeof(hash));
    if (status) return map_ntstatus(status);
    for (i = 0; i < sizeof(hash); ++i)
    {
        output[2 * i] = hex[hash[i] >> 4];
        output[2 * i + 1] = hex[hash[i] & 15];
    }
    output[64] = 0;
    return ERROR_SUCCESS;
}

static SECURITY_STATUS p256_key_exists(const WCHAR *name, BOOL *exists)
{
    WCHAR value_name[65];
    HKEY storage;
    LONG error;
    SECURITY_STATUS status;

    *exists = FALSE;
    if ((status = p256_storage_name(name, value_name))) return status;
    error = RegOpenKeyExW(HKEY_CURRENT_USER, p256_storage_path, 0, KEY_QUERY_VALUE, &storage);
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return ERROR_SUCCESS;
    if (error) return storage_error(error);
    error = RegQueryValueExW(storage, value_name, NULL, NULL, NULL, NULL);
    RegCloseKey(storage);
    if (error == ERROR_FILE_NOT_FOUND) return ERROR_SUCCESS;
    if (error) return storage_error(error);
    *exists = TRUE;
    return ERROR_SUCCESS;
}

static HANDLE lock_p256_storage(void)
{
    HANDLE mutex = CreateMutexW(NULL, FALSE, L"Local\\Wine.NCrypt.P256.Persistence.v1");
    DWORD wait;

    if (!mutex) return NULL;
    wait = WaitForSingleObject(mutex, 5000);
    if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) return mutex;
    CloseHandle(mutex);
    return NULL;
}

static SECURITY_STATUS store_p256_key(struct key *key)
{
    struct stored_p256_key *record = NULL;
    struct object_property *policy;
    DATA_BLOB plain, protected = {0}, entropy;
    WCHAR value_name[65];
    DWORD size = 0;
    HANDLE mutex = NULL;
    HKEY storage;
    BOOL exists;
    LONG error;
    SECURITY_STATUS status;

    if ((status = p256_storage_name(key->name, value_name))) return status;
    if ((status = map_ntstatus(BCryptExportKey(key->handle, NULL, BCRYPT_ECCPRIVATE_BLOB,
                                             NULL, 0, &size, 0)))) return status;
    if (size != sizeof(BCRYPT_ECCKEY_BLOB) + 3 * 32) return NTE_BAD_DATA;
    plain.cbData = sizeof(*record) + size;
    if (!(record = calloc(1, plain.cbData))) return NTE_NO_MEMORY;
    record->magic = 0x31474e43;
    record->version = 1;
    record->blob_size = size;
    policy = get_object_property(&key->obj, NCRYPT_EXPORT_POLICY_PROPERTY);
    if (!policy || policy->value_size != sizeof(DWORD)) { status = NTE_BAD_DATA; goto done; }
    record->export_policy = *(DWORD *)policy->value;
    if ((status = map_ntstatus(BCryptExportKey(key->handle, NULL, BCRYPT_ECCPRIVATE_BLOB,
                                             record->blob, size, &size, 0)))) goto done;
    plain.pbData = (BYTE *)record;
    entropy.pbData = (BYTE *)key->name;
    entropy.cbData = (wcslen(key->name) + 1) * sizeof(WCHAR);
    if (!CryptProtectData(&plain, L"Wine CNG P-256 key", &entropy, NULL, NULL,
                         CRYPTPROTECT_UI_FORBIDDEN, &protected))
    {
        status = NTE_FAIL;
        goto done;
    }
    if (!(mutex = lock_p256_storage())) { status = NTE_FAIL; goto done; }
    if ((status = p256_key_exists(key->name, &exists))) goto done;
    if (exists && !key->overwrite) { status = NTE_EXISTS; goto done; }
    error = RegCreateKeyExW(HKEY_CURRENT_USER, p256_storage_path, 0, NULL, 0,
                            KEY_SET_VALUE, NULL, &storage, NULL);
    if (!error)
    {
        error = RegSetValueExW(storage, value_name, 0, REG_BINARY, protected.pbData, protected.cbData);
        RegCloseKey(storage);
    }
    status = storage_error(error);

done:
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    if (protected.pbData) LocalFree(protected.pbData);
    if (record) { SecureZeroMemory(record, plain.cbData); free(record); }
    return status;
}

SECURITY_STATUS WINAPI NCryptCreatePersistedKey(NCRYPT_PROV_HANDLE provider, NCRYPT_KEY_HANDLE *handle,
                                                const WCHAR *algid, const WCHAR *name, DWORD keyspec, DWORD flags)
{
    struct key *key;
    DWORD bitlen;
    NTSTATUS status;
    BCRYPT_ALG_HANDLE alg;
    BOOL persist, exists;
    SECURITY_STATUS ret;

    TRACE("(%#Ix, %p, %s, %s, %#lx, %#lx)\n", provider, handle, wine_dbgstr_w(algid), wine_dbgstr_w(name),
          keyspec, flags);

    if (!provider || ((struct provider *)provider)->obj.type != PROVIDER) return NTE_INVALID_HANDLE;
    if (!algid) return HRESULT_FROM_WIN32(RPC_X_NULL_REF_POINTER);
    if (!handle) return NTE_INVALID_PARAMETER;
    persist = name && !keyspec && !(flags & NCRYPT_MACHINE_KEY_FLAG) &&
              ((struct provider *)provider)->software && !wcsicmp(algid, BCRYPT_ECDSA_P256_ALGORITHM);
    if (persist)
    {
        if (flags & ~(NCRYPT_OVERWRITE_KEY_FLAG | NCRYPT_SILENT_FLAG)) return NTE_BAD_FLAGS;
        if ((ret = p256_key_exists(name, &exists))) return ret;
        if (exists && !(flags & NCRYPT_OVERWRITE_KEY_FLAG)) return NTE_EXISTS;
    }
    else if (name) FIXME("Persistent keys are not supported for this provider or algorithm\n");

    if (!wcsicmp(algid, BCRYPT_RSA_ALGORITHM))
    {
        if (!(key = create_key_object(RSA, provider))) return NTE_NO_MEMORY;
        alg = BCRYPT_RSA_ALG_HANDLE;
        bitlen = 1024;
    }
    else if (!wcsicmp(algid, BCRYPT_ECDSA_P256_ALGORITHM))
    {
        if (!(key = create_key_object(ECDSA_P256, provider))) return NTE_NO_MEMORY;
        alg = BCRYPT_ECDSA_P256_ALG_HANDLE;
        bitlen = 256;
    }
    else if (!wcsicmp(algid, BCRYPT_ECDSA_P384_ALGORITHM))
    {
        if (!(key = create_key_object(ECDSA_P384, provider))) return NTE_NO_MEMORY;
        alg = BCRYPT_ECDSA_P384_ALG_HANDLE;
        bitlen = 384;
    }
    else if (!wcsicmp(algid, BCRYPT_ECDSA_P521_ALGORITHM))
    {
        if (!(key = create_key_object(ECDSA_P521, provider))) return NTE_NO_MEMORY;
        alg = BCRYPT_ECDSA_P521_ALG_HANDLE;
        bitlen = 521;
    }
    else
    {
        FIXME("Algorithm not handled %s\n", wine_dbgstr_w(algid));
        return NTE_NOT_SUPPORTED;
    }

    if ((status = BCryptGenerateKeyPair(alg, &key->handle, bitlen, 0)))
    {
        free(key);
        return map_ntstatus(status);
    }
    set_object_property(&key->obj, NCRYPT_LENGTH_PROPERTY, (BYTE *)&bitlen, sizeof(bitlen));
    set_object_property(&key->obj, BCRYPT_PUBLIC_KEY_LENGTH, (BYTE *)&bitlen, sizeof(bitlen));

    if (persist)
    {
        if (!(key->name = wcsdup(name)))
        {
            NCryptFreeObject((NCRYPT_HANDLE)key);
            return NTE_NO_MEMORY;
        }
        key->overwrite = !!(flags & NCRYPT_OVERWRITE_KEY_FLAG);
        if ((ret = set_object_property(&key->obj, NCRYPT_NAME_PROPERTY, (BYTE *)name,
                                      (wcslen(name) + 1) * sizeof(WCHAR))))
        {
            NCryptFreeObject((NCRYPT_HANDLE)key);
            return ret;
        }
    }

    *handle = (NCRYPT_KEY_HANDLE)key;
    return ERROR_SUCCESS;
}

SECURITY_STATUS WINAPI NCryptDecrypt(NCRYPT_KEY_HANDLE handle, BYTE *input, DWORD insize, void *padding, BYTE *output,
                                     DWORD outsize, DWORD *result, DWORD flags)
{
    struct key *key = (struct key *)handle;
    NTSTATUS status;

    TRACE("(%#Ix, %p, %lu, %p, %p, %lu, %p, %#lx)\n", handle, input, insize, padding, output, outsize, result, flags);

    if (key->obj.type != KEY) return NTE_INVALID_HANDLE;

    status = BCryptDecrypt(key->handle, input, insize, padding, NULL, 0, output, outsize, result, flags);
    return map_ntstatus(status);
}

SECURITY_STATUS WINAPI NCryptDeleteKey(NCRYPT_KEY_HANDLE key, DWORD flags)
{
    struct key *object = (struct key *)key;
    WCHAR value_name[65];
    HANDLE mutex;
    HKEY storage;
    LONG error;
    SECURITY_STATUS status;

    if (!object || object->obj.type != KEY) return NTE_INVALID_HANDLE;
    if (object->name)
    {
        if (flags & ~NCRYPT_SILENT_FLAG) return NTE_BAD_FLAGS;
        if ((status = p256_storage_name(object->name, value_name))) return status;
        if (!(mutex = lock_p256_storage())) return NTE_FAIL;
        error = RegOpenKeyExW(HKEY_CURRENT_USER, p256_storage_path, 0, KEY_SET_VALUE, &storage);
        if (!error)
        {
            error = RegDeleteValueW(storage, value_name);
            RegCloseKey(storage);
        }
        ReleaseMutex(mutex);
        CloseHandle(mutex);
        if ((status = storage_error(error))) return status;
        return NCryptFreeObject(key);
    }
    FIXME("(%#Ix, %#lx): stub\n", key, flags);
    return NTE_NOT_SUPPORTED;
}

SECURITY_STATUS WINAPI NCryptEncrypt(NCRYPT_KEY_HANDLE handle, BYTE *input, DWORD insize, void *padding, BYTE *output,
                                     DWORD outsize, DWORD *result, DWORD flags)
{
    struct key *key = (struct key *)handle;
    NTSTATUS status;

    TRACE("(%#Ix, %p, %lu, %p, %p, %lu, %p, %#lx)\n", handle, input, insize, padding, output, outsize, result, flags);

    if (flags & ~(NCRYPT_NO_PADDING_FLAG | NCRYPT_PAD_OAEP_FLAG | NCRYPT_PAD_PKCS1_FLAG | NCRYPT_SILENT_FLAG))
    {
        FIXME("Flags %lx not supported\n", flags);
        return NTE_BAD_FLAGS;
    }
    if (key->obj.type != KEY) return NTE_INVALID_HANDLE;

    status = BCryptEncrypt(key->handle, input, insize, padding, NULL, 0, output, outsize, result, flags);
    return map_ntstatus(status);
}

SECURITY_STATUS WINAPI NCryptEnumAlgorithms(NCRYPT_PROV_HANDLE provider, DWORD alg_ops,
                                            DWORD *alg_count, NCryptAlgorithmName **alg_list, DWORD flags)
{
    FIXME("(%#Ix, %#lx, %p, %p, %#lx): stub\n", provider, alg_ops, alg_count, alg_list, flags);
    return NTE_NOT_SUPPORTED;
}

SECURITY_STATUS WINAPI NCryptEnumKeys(NCRYPT_PROV_HANDLE provider, const WCHAR *scope,
                                      NCryptKeyName **key_name, PVOID *enum_state, DWORD flags)
{
    FIXME("(%#Ix, %p, %p, %p, %#lx): stub\n", provider, scope, key_name, enum_state, flags);
    return NTE_NOT_SUPPORTED;
}

SECURITY_STATUS WINAPI NCryptFinalizeKey(NCRYPT_KEY_HANDLE handle, DWORD flags)
{
    struct key *key = (struct key *)handle;
    DWORD key_length;
    struct object_property *prop;
    NTSTATUS status;

    TRACE("(%#Ix, %#lx)\n", handle, flags);

    if (!key || key->obj.type != KEY) return NTE_INVALID_HANDLE;
    if (key->name && (flags & ~NCRYPT_SILENT_FLAG)) return NTE_BAD_FLAGS;

    if (!(prop = get_object_property(&key->obj, NCRYPT_LENGTH_PROPERTY))) return NTE_INVALID_HANDLE;
    key_length = *(DWORD *)prop->value;
    status = BCryptSetProperty(key->handle, BCRYPT_KEY_LENGTH, (UCHAR *)&key_length, sizeof(key_length), 0);
    if (status != STATUS_SUCCESS)
    {
        ERR("Error setting key length property\n");
        return map_ntstatus(status);
    }

    status = BCryptFinalizeKeyPair(key->handle, 0);
    if (status != STATUS_SUCCESS)
    {
        ERR("Error finalizing key pair\n");
        return map_ntstatus(status);
    }

    return key->name ? store_p256_key(key) : ERROR_SUCCESS;
}

SECURITY_STATUS WINAPI NCryptFreeBuffer(PVOID buf)
{
    FIXME("(%p): stub\n", buf);
    return NTE_NOT_SUPPORTED;
}

SECURITY_STATUS WINAPI NCryptFreeObject(NCRYPT_HANDLE handle)
{
    struct object *object = (struct object *)handle;
    SECURITY_STATUS ret = STATUS_SUCCESS;
    unsigned int i;

    TRACE("(%#Ix)\n", handle);

    if (!object)
    {
        WARN("invalid handle %#Ix\n", handle);
        return NTE_INVALID_HANDLE;
    }

    switch (object->type)
    {
    case KEY:
    {
        struct key *key = (struct key *)object;
        if ((ret = map_ntstatus( BCryptDestroyKey(key->handle) ))) return ret;
        free(key->name);
        break;
    }
    case PROVIDER:
        if (InterlockedDecrement(&object->refs)) return 0;
        break;

    default:
        WARN("invalid handle %#Ix\n", handle);
        return NTE_INVALID_HANDLE;
    }

    for (i = 0; i < object->num_properties; i++)
    {
        if (!wcscmp(object->properties[i].key, NCRYPT_PROVIDER_HANDLE_PROPERTY))
            NCryptFreeObject(*(NCRYPT_HANDLE *)object->properties[i].value);

        free(object->properties[i].key);
        free(object->properties[i].value);
    }
    free(object->properties);
    free(object);
    return ret;
}

SECURITY_STATUS WINAPI NCryptGetProperty(NCRYPT_HANDLE handle, const WCHAR *name, BYTE *output,
                                         DWORD outsize, DWORD *result, DWORD flags)
{
    struct object *object = (struct object *)handle;
    const struct object_property *property;

    TRACE("(%#Ix, %s, %p, %lu, %p, %#lx)\n", handle, wine_dbgstr_w(name), output, outsize, result, flags);
    if (flags) FIXME("flags %#lx not supported\n", flags);

    if (!object) return NTE_INVALID_HANDLE;
    if (!(property = get_object_property(object, name))) return NTE_INVALID_PARAMETER;

    *result = property->value_size;
    if (!output) return ERROR_SUCCESS;
    if (outsize < property->value_size) return NTE_BUFFER_TOO_SMALL;

    memcpy(output, property->value, property->value_size);
    if (!wcscmp(name, NCRYPT_PROVIDER_HANDLE_PROPERTY))
        provider_addref(*(NCRYPT_PROV_HANDLE*)property->value);
    return ERROR_SUCCESS;
}

SECURITY_STATUS WINAPI NCryptImportKey(NCRYPT_PROV_HANDLE provider, NCRYPT_KEY_HANDLE decrypt_key,
                                       const WCHAR *type, NCryptBufferDesc *params, NCRYPT_KEY_HANDLE *handle,
                                       BYTE *data, DWORD datasize, DWORD flags)
{
    BCRYPT_KEY_BLOB *header = (BCRYPT_KEY_BLOB *)data;
    struct key *key;
    NTSTATUS status;
    ULONG bitlen, i;
    BCRYPT_ALG_HANDLE alg;

    TRACE("(%#Ix, %#Ix, %s, %p, %p, %p, %lu, %#lx)\n", provider, decrypt_key, wine_dbgstr_w(type),
          params, handle, data, datasize, flags);

    if (decrypt_key)
    {
        FIXME("Key blob decryption not implemented\n");
        return NTE_NOT_SUPPORTED;
    }
    if (params)
    {
        for (i = 0; i < params->cBuffers; i++)
            FIXME("unsupported parameter %lu\n", params->pBuffers[i].BufferType);
    }
    if (flags == NCRYPT_SILENT_FLAG)
    {
        FIXME("Silent flag not implemented\n");
    }
    else if (flags)
    {
        WARN("Invalid flags %#lx\n", flags);
        return NTE_BAD_FLAGS;
    }
    if (!header) return NTE_INVALID_PARAMETER;

    switch (header->Magic)
    {
    case BCRYPT_RSAFULLPRIVATE_MAGIC:
    case BCRYPT_RSAPRIVATE_MAGIC:
    case BCRYPT_RSAPUBLIC_MAGIC:
    {
        BCRYPT_RSAKEY_BLOB *rsablob = (BCRYPT_RSAKEY_BLOB *)data;
        if (!(key = create_key_object(RSA, provider))) return NTE_NO_MEMORY;
        alg = BCRYPT_RSA_ALG_HANDLE;
        bitlen = rsablob->BitLength;
        break;
    }
    case BCRYPT_ECDSA_PUBLIC_P256_MAGIC:
    case BCRYPT_ECDSA_PRIVATE_P256_MAGIC:
    {
        if (!(key = create_key_object(ECDSA_P256, provider))) return NTE_NO_MEMORY;
        alg = BCRYPT_ECDSA_P256_ALG_HANDLE;
        bitlen = 256;
        break;
    }
    case BCRYPT_ECDSA_PUBLIC_P384_MAGIC:
    case BCRYPT_ECDSA_PRIVATE_P384_MAGIC:
    {
        if (!(key = create_key_object(ECDSA_P384, provider))) return NTE_NO_MEMORY;
        alg = BCRYPT_ECDSA_P384_ALG_HANDLE;
        bitlen = 384;
        break;
    }
    case BCRYPT_ECDSA_PUBLIC_P521_MAGIC:
    case BCRYPT_ECDSA_PRIVATE_P521_MAGIC:
    {
        if (!(key = create_key_object(ECDSA_P521, provider))) return NTE_NO_MEMORY;
        alg = BCRYPT_ECDSA_P521_ALG_HANDLE;
        bitlen = 521;
        break;
    }
    default:
        FIXME("Unhandled key magic %#lx\n", header->Magic);
        return NTE_INVALID_PARAMETER;
    }

    if ((status = BCryptImportKeyPair(alg, NULL, type, &key->handle, data, datasize, 0)))
    {
        free(key);
        return map_ntstatus(status);
    }

    set_object_property(&key->obj, NCRYPT_LENGTH_PROPERTY, (BYTE *)&bitlen, sizeof(bitlen));
    set_object_property(&key->obj, BCRYPT_PUBLIC_KEY_LENGTH, (BYTE *)&bitlen, sizeof(bitlen));

    *handle = (NCRYPT_KEY_HANDLE)key;
    return ERROR_SUCCESS;
}

SECURITY_STATUS WINAPI NCryptExportKey(NCRYPT_KEY_HANDLE handle, NCRYPT_KEY_HANDLE encrypt_key, const WCHAR *type,
                                       NCryptBufferDesc *params, BYTE *output, DWORD output_len, DWORD *ret_len,
                                       DWORD flags)
{
    struct key *key = (struct key *)handle;

    TRACE("(%#Ix, %#Ix, %s, %p, %p, %lu, %p, %#lx)\n", handle, encrypt_key, wine_dbgstr_w(type), params, output,
          output_len, ret_len, flags);

    if (encrypt_key)
    {
        FIXME("Key blob encryption not implemented\n");
        return NTE_NOT_SUPPORTED;
    }
    if (params)
    {
        FIXME("Parameter information not implemented\n");
        return NTE_NOT_SUPPORTED;
    }
    if (flags == NCRYPT_SILENT_FLAG)
    {
        FIXME("Silent flag not implemented\n");
    }

    return map_ntstatus(BCryptExportKey(key->handle, NULL, type, output, output_len, ret_len, 0));
}

SECURITY_STATUS WINAPI NCryptIsAlgSupported(NCRYPT_PROV_HANDLE provider, const WCHAR *algid, DWORD flags)
{
    static const ULONG supported = BCRYPT_CIPHER_OPERATION |\
                                   BCRYPT_ASYMMETRIC_ENCRYPTION_OPERATION |\
                                   BCRYPT_SIGNATURE_OPERATION |\
                                   BCRYPT_SECRET_AGREEMENT_OPERATION;
    BCRYPT_ALGORITHM_IDENTIFIER *list;
    ULONG i, count;
    NTSTATUS status;

    TRACE("(%#Ix, %s, %#lx)\n", provider, wine_dbgstr_w(algid), flags);

    if (!provider) return NTE_INVALID_HANDLE;
    if (!algid) return HRESULT_FROM_WIN32(RPC_X_NULL_REF_POINTER);
    if (flags == NCRYPT_SILENT_FLAG)
    {
        FIXME("Silent flag not implemented\n");
    }
    else if (flags)
    {
        WARN("Invalid flags %#lx\n", flags);
        return NTE_BAD_FLAGS;
    }
    if (!lstrcmpiW(BCRYPT_RSA_SIGN_ALGORITHM, algid)) return NTE_NOT_SUPPORTED;

    status = BCryptEnumAlgorithms(supported, &count, &list, 0);
    if (status != STATUS_SUCCESS)
    {
        ERR("Error retrieving algorithm list %#lx\n", status);
        return map_ntstatus(status);
    }

    status = STATUS_NOT_SUPPORTED;
    for (i = 0; i < count; i++)
    {
        if (!lstrcmpiW(list[i].pszName, algid))
        {
            status = STATUS_SUCCESS;
            break;
        }
    }

    BCryptFreeBuffer(list);
    return map_ntstatus(status);
}

BOOL WINAPI NCryptIsKeyHandle(NCRYPT_KEY_HANDLE hKey)
{
    FIXME("(%#Ix): stub\n", hKey);
    return FALSE;
}

SECURITY_STATUS WINAPI NCryptOpenKey(NCRYPT_PROV_HANDLE provider, NCRYPT_KEY_HANDLE *key,
                                     const WCHAR *name, DWORD keyspec, DWORD flags)
{
    WCHAR value_name[65];
    struct stored_p256_key *record;
    BCRYPT_ECCKEY_BLOB *blob;
    DATA_BLOB protected = {0}, plain = {0}, entropy;
    DWORD type, size;
    HKEY storage;
    LONG error;
    SECURITY_STATUS status;
    struct key *object;

    TRACE("(%#Ix, %p, %s, %#lx, %#lx)\n", provider, key, wine_dbgstr_w(name), keyspec, flags);
    if (!provider || ((struct provider *)provider)->obj.type != PROVIDER) return NTE_INVALID_HANDLE;
    if (!key || !name) return NTE_INVALID_PARAMETER;
    if (!((struct provider *)provider)->software || (flags & NCRYPT_MACHINE_KEY_FLAG)) return NTE_NOT_SUPPORTED;
    if (flags & ~NCRYPT_SILENT_FLAG) return NTE_BAD_FLAGS;
    if (keyspec) return NTE_NOT_SUPPORTED;
    if ((status = p256_storage_name(name, value_name))) return status;
    *key = 0;
    error = RegOpenKeyExW(HKEY_CURRENT_USER, p256_storage_path, 0, KEY_QUERY_VALUE, &storage);
    if (error) return storage_error(error);
    size = 0;
    error = RegQueryValueExW(storage, value_name, NULL, &type, NULL, &size);
    if (error) { RegCloseKey(storage); return storage_error(error); }
    if (type != REG_BINARY || !size || size > 4096) { RegCloseKey(storage); return NTE_BAD_DATA; }
    if (!(protected.pbData = malloc(size))) { RegCloseKey(storage); return NTE_NO_MEMORY; }
    protected.cbData = size;
    error = RegQueryValueExW(storage, value_name, NULL, &type, protected.pbData, &protected.cbData);
    RegCloseKey(storage);
    status = storage_error(error);
    if (status) goto done;
    if (type != REG_BINARY) { status = NTE_BAD_DATA; goto done; }
    entropy.pbData = (BYTE *)name;
    entropy.cbData = (wcslen(name) + 1) * sizeof(WCHAR);
    if (!CryptUnprotectData(&protected, NULL, &entropy, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &plain))
    {
        status = NTE_BAD_DATA;
        goto done;
    }
    record = (struct stored_p256_key *)plain.pbData;
    if (plain.cbData != sizeof(*record) + sizeof(*blob) + 3 * 32 ||
        record->magic != 0x31474e43 || record->version != 1 ||
        record->blob_size != sizeof(*blob) + 3 * 32)
    {
        status = NTE_BAD_DATA;
        goto done;
    }
    blob = (BCRYPT_ECCKEY_BLOB *)record->blob;
    if (blob->dwMagic != BCRYPT_ECDSA_PRIVATE_P256_MAGIC || blob->cbKey != 32)
    {
        status = NTE_BAD_DATA;
        goto done;
    }
    status = NCryptImportKey(provider, 0, BCRYPT_ECCPRIVATE_BLOB, NULL, key,
                             record->blob, record->blob_size, 0);
    if (status) goto done;
    object = (struct key *)*key;
    if (!(object->name = wcsdup(name))) status = NTE_NO_MEMORY;
    else status = set_object_property(&object->obj, NCRYPT_NAME_PROPERTY, (BYTE *)name,
                                      (wcslen(name) + 1) * sizeof(WCHAR));
    if (!status) status = set_object_property(&object->obj, NCRYPT_EXPORT_POLICY_PROPERTY,
                                             (BYTE *)&record->export_policy, sizeof(record->export_policy));
    if (status) { NCryptFreeObject(*key); *key = 0; }

done:
    if (plain.pbData) { SecureZeroMemory(plain.pbData, plain.cbData); LocalFree(plain.pbData); }
    free(protected.pbData);
    return status;
}

SECURITY_STATUS WINAPI NCryptOpenStorageProvider(NCRYPT_PROV_HANDLE *handle, const WCHAR *name, DWORD flags)
{
    struct provider *provider;

    FIXME("(%p, %s, %#lx): stub\n", handle, wine_dbgstr_w(name), flags);

    if (!(provider = allocate_provider()))
    {
        ERR("Error allocating memory.\n");
        return NTE_NO_MEMORY;
    }
    provider->software = !name || !wcsicmp(name, MS_KEY_STORAGE_PROVIDER);
    *handle = (NCRYPT_PROV_HANDLE)provider;
    return ERROR_SUCCESS;
}

SECURITY_STATUS WINAPI NCryptSetProperty(NCRYPT_HANDLE handle, const WCHAR *name, BYTE *input, DWORD insize, DWORD flags)
{
    struct object *object = (struct object *)handle;

    TRACE("(%#Ix, %s, %p, %lu, %#lx)\n", handle, wine_dbgstr_w(name), input, insize, flags);
    if (flags) FIXME("flags %#lx not supported\n", flags);

    if (!object) return NTE_INVALID_HANDLE;
    if (!wcscmp(name, NCRYPT_PROVIDER_HANDLE_PROPERTY)) return NTE_NOT_SUPPORTED;
    return set_object_property(object, name, input, insize);
}

SECURITY_STATUS WINAPI NCryptSignHash(NCRYPT_KEY_HANDLE handle, void *padding, BYTE *value, DWORD value_len,
                                      BYTE *sig, DWORD sig_len, DWORD *ret_len, DWORD flags)
{
    struct key *key = (struct key *)handle;

    TRACE("(%#Ix, %p, %p, %lu, %p, %lu, %#lx)\n", handle, padding, value, value_len, sig, sig_len, flags);
    if (flags & NCRYPT_SILENT_FLAG) FIXME("Silent flag not implemented\n");

    if (!key || key->obj.type != KEY) return NTE_INVALID_HANDLE;

    return map_ntstatus(BCryptSignHash(key->handle, padding, value, value_len, sig, sig_len, ret_len,
                                       flags & ~NCRYPT_SILENT_FLAG));
}

SECURITY_STATUS WINAPI NCryptVerifySignature(NCRYPT_KEY_HANDLE handle, void *padding, BYTE *hash, DWORD hash_size,
                                             BYTE *signature, DWORD signature_size, DWORD flags)
{
    struct key *key = (struct key *)handle;

    TRACE("(%#Ix, %p, %p, %lu, %p, %lu, %#lx)\n", handle, padding, hash, hash_size, signature,
          signature_size, flags);

    if (!hash_size || !signature_size) return NTE_INVALID_PARAMETER;
    if (!hash || !signature) return HRESULT_FROM_WIN32(RPC_X_NULL_REF_POINTER);
    if (!handle || key->obj.type != KEY) return NTE_INVALID_HANDLE;

    if (key->algid < RSA)
    {
        FIXME("Symmetric keys not supported.\n");
        return NTE_NOT_SUPPORTED;
    }

    return map_ntstatus(BCryptVerifySignature(key->handle, padding, hash, hash_size, signature, signature_size, flags));
}
