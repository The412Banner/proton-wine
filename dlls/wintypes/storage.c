/*
 * Copyright 2022-2024 Zhiyi Zhang for CodeWeavers
 * Copyright 2025 Jactry Zeng for CodeWeavers
 * Copyright 2026 Conor McCarthy for CodeWeavers
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

#include "private.h"
#include "robuffer.h"

WINE_DEFAULT_DEBUG_CHANNEL(wintypes);

/* Synchronous wrapper for async operations returned by IOutputStream async methods.
 * The IAsyncInfo object is queried from the inner operation. */

#define DEFINE_IASYNCOPERATION(iface_type, cs_type_str, impl_type, result_type,                                   \
        handler_iface_type, inner_op_iface_type, outer_handler_iface_type)                                        \
struct impl_type                                                                                                  \
{                                                                                                                 \
    iface_type iface_type##_iface;                                                                                \
    handler_iface_type handler_iface_type##_iface;                                                                \
    IAsyncInfo *async_inner;                                                                                      \
    LONG refcount;                                                                                                \
    struct data_writer *data_writer;                                                                              \
    outer_handler_iface_type *outer_handler;                                                                      \
    AsyncStatus status;                                                                                           \
    result_type result;                                                                                           \
};                                                                                                                \
                                                                                                                  \
static inline struct impl_type *impl_from_##handler_iface_type(handler_iface_type *iface)                         \
{                                                                                                                 \
    return CONTAINING_RECORD(iface, struct impl_type, handler_iface_type##_iface);                                \
}                                                                                                                 \
                                                                                                                  \
static HRESULT WINAPI impl_type##_handler_QueryInterface(handler_iface_type *iface, REFIID iid, void **out)       \
{                                                                                                                 \
    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IAgileObject) ||                                 \
        IsEqualGUID(iid, &IID_##handler_iface_type))                                                              \
    {                                                                                                             \
        IUnknown_AddRef(iface);                                                                                   \
        *out = iface;                                                                                             \
        return S_OK;                                                                                              \
    }                                                                                                             \
                                                                                                                  \
    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));                                   \
    *out = NULL;                                                                                                  \
    return E_NOINTERFACE;                                                                                         \
}                                                                                                                 \
                                                                                                                  \
static ULONG WINAPI impl_type##_handler_AddRef(handler_iface_type *iface)                                         \
{                                                                                                                 \
    struct impl_type *impl = impl_from_##handler_iface_type(iface);                                               \
    return iface_type##_AddRef(&impl->iface_type##_iface);                                                        \
}                                                                                                                 \
                                                                                                                  \
static ULONG WINAPI impl_type##_handler_Release(handler_iface_type *iface)                                        \
{                                                                                                                 \
    struct impl_type *impl = impl_from_##handler_iface_type(iface);                                               \
    return iface_type##_Release(&impl->iface_type##_iface);                                                       \
}                                                                                                                 \
                                                                                                                  \
static HRESULT impl_type##_handle_completion(struct impl_type *impl)                                              \
{                                                                                                                 \
    if (impl->status && impl->outer_handler)                                                                      \
        return outer_handler_iface_type##_Invoke(impl->outer_handler, &impl->iface_type##_iface, impl->status);   \
    return S_OK;                                                                                                  \
}                                                                                                                 \
static HRESULT WINAPI impl_type##_handler_Invoke(handler_iface_type *iface,                                       \
        inner_op_iface_type *operation, AsyncStatus status)                                                       \
{                                                                                                                 \
    struct impl_type *impl = impl_from_##handler_iface_type(iface);                                               \
    HRESULT hr;                                                                                                   \
    if (!status) return S_OK;                                                                                     \
    if (status == Completed && FAILED(hr = inner_op_iface_type##_GetResults(operation, &impl->result)))           \
        status = Error;                                                                                           \
    impl->status = status;                                                                                        \
    data_writer_async_complete(impl->data_writer);                                                                \
    return impl_type##_handle_completion(impl);                                                                   \
}                                                                                                                 \
                                                                                                                  \
static handler_iface_type##Vtbl impl_type##_handler_vtbl =                                                        \
{                                                                                                                 \
    impl_type##_handler_QueryInterface,                                                                           \
    impl_type##_handler_AddRef,                                                                                   \
    impl_type##_handler_Release,                                                                                  \
    impl_type##_handler_Invoke,                                                                                   \
};                                                                                                                \
                                                                                                                  \
static inline struct impl_type *impl_from_##iface_type(iface_type *iface)                                         \
{                                                                                                                 \
    return CONTAINING_RECORD(iface, struct impl_type, iface_type##_iface);                                        \
}                                                                                                                 \
static HRESULT WINAPI impl_type##_QueryInterface(iface_type *iface, REFIID iid, void **out)                       \
{                                                                                                                 \
    struct impl_type *impl = impl_from_##iface_type(iface);                                                       \
                                                                                                                  \
    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);                                         \
                                                                                                                  \
    if (IsEqualGUID(iid, &IID_IUnknown) ||                                                                        \
        IsEqualGUID(iid, &IID_IInspectable) ||                                                                    \
        IsEqualGUID(iid, &IID_IAgileObject) ||                                                                    \
        IsEqualGUID(iid, &IID_##iface_type))                                                                      \
    {                                                                                                             \
        iface_type##_AddRef((*out = &impl->iface_type##_iface));                                                  \
        return S_OK;                                                                                              \
    }                                                                                                             \
                                                                                                                  \
    if (IsEqualGUID(iid, &IID_IAsyncInfo))                                                                        \
    {                                                                                                             \
        IAsyncInfo_AddRef(*out = impl->async_inner);                                                              \
        return S_OK;                                                                                              \
    }                                                                                                             \
                                                                                                                  \
    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));                                   \
    *out = NULL;                                                                                                  \
    return E_NOINTERFACE;                                                                                         \
}                                                                                                                 \
static ULONG WINAPI impl_type##_AddRef(iface_type *iface)                                                         \
{                                                                                                                 \
    struct impl_type *impl = impl_from_##iface_type(iface);                                                       \
    ULONG ref = InterlockedIncrement(&impl->refcount);                                                            \
    TRACE("iface %p, ref %lu.\n", iface, ref);                                                                    \
    return ref;                                                                                                   \
}                                                                                                                 \
static ULONG WINAPI impl_type##_Release(iface_type *iface)                                                        \
{                                                                                                                 \
    struct impl_type *impl = impl_from_##iface_type(iface);                                                       \
    ULONG ref = InterlockedDecrement(&impl->refcount);                                                            \
    TRACE("iface %p, ref %lu.\n", iface, ref);                                                                    \
                                                                                                                  \
    if (!ref)                                                                                                     \
    {                                                                                                             \
        IDataWriter_Release(&impl->data_writer->IDataWriter_iface);                                               \
        IAsyncInfo_Release(impl->async_inner);                                                                    \
        if (impl->outer_handler)                                                                                  \
            outer_handler_iface_type##_Release(impl->outer_handler);                                              \
        free(impl);                                                                                               \
    }                                                                                                             \
                                                                                                                  \
    return ref;                                                                                                   \
}                                                                                                                 \
static HRESULT WINAPI impl_type##_GetIids(iface_type *iface, ULONG *iid_count, IID **iids)                        \
{                                                                                                                 \
    FIXME("iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids);                                     \
    return E_NOTIMPL;                                                                                             \
}                                                                                                                 \
static HRESULT WINAPI impl_type##_GetRuntimeClassName(iface_type *iface, HSTRING *class_name)                     \
{                                                                                                                 \
    return WindowsCreateString(L"Windows.Foundation.IAsyncOperation`1<"cs_type_str">",                            \
                                ARRAY_SIZE(L"Windows.Foundation.IAsyncOperation`1<"cs_type_str">"),               \
                                class_name);                                                                      \
}                                                                                                                 \
static HRESULT WINAPI impl_type##_GetTrustLevel(iface_type *iface, TrustLevel *trust_level)                       \
{                                                                                                                 \
    FIXME("iface %p, trust_level %p stub!\n", iface, trust_level);                                                \
    return E_NOTIMPL;                                                                                             \
}                                                                                                                 \
static HRESULT WINAPI impl_type##_put_Completed(iface_type *iface, outer_handler_iface_type *handler)             \
{                                                                                                                 \
    struct impl_type *impl = impl_from_##iface_type(iface);                                                       \
    TRACE("iface %p, handler %p.\n", iface, handler);                                                             \
    impl->outer_handler = handler;                                                                                \
    outer_handler_iface_type##_AddRef(impl->outer_handler);                                                       \
    return impl_type##_handle_completion(impl);                                                                   \
}                                                                                                                 \
static HRESULT WINAPI impl_type##_get_Completed(iface_type *iface, outer_handler_iface_type **handler)            \
{                                                                                                                 \
    struct impl_type *impl = impl_from_##iface_type(iface);                                                       \
    TRACE("iface %p, handler %p.\n", iface, handler);                                                             \
    if ((*handler = impl->outer_handler))                                                                         \
        outer_handler_iface_type##_AddRef(*handler);                                                              \
    return S_OK;                                                                                                  \
}                                                                                                                 \
static HRESULT WINAPI impl_type##_GetResults(iface_type *iface, result_type *results)                             \
{                                                                                                                 \
    struct impl_type *impl = impl_from_##iface_type(iface);                                                       \
                                                                                                                  \
    TRACE("iface %p, results %p.\n", iface, results);                                                             \
                                                                                                                  \
    if (impl->status != Completed)                                                                                \
        return E_ILLEGAL_METHOD_CALL;                                                                             \
                                                                                                                  \
    *results = impl->result;                                                                                      \
    return S_OK;                                                                                                  \
}                                                                                                                 \
static const struct iface_type##Vtbl impl_type##_vtbl =                                                           \
{                                                                                                                 \
    /* IUnknown methods */                                                                                        \
    impl_type##_QueryInterface,                                                                                   \
    impl_type##_AddRef,                                                                                           \
    impl_type##_Release,                                                                                          \
    /* IInspectable methods */                                                                                    \
    impl_type##_GetIids,                                                                                          \
    impl_type##_GetRuntimeClassName,                                                                              \
    impl_type##_GetTrustLevel,                                                                                    \
    /* IAsyncOperation<iface_type> */                                                                             \
    impl_type##_put_Completed,                                                                                    \
    impl_type##_get_Completed,                                                                                    \
    impl_type##_GetResults,                                                                                       \
};                                                                                                                \
HRESULT async_operation_##result_type##_create(struct data_writer *data_writer,                                   \
        inner_op_iface_type *inner_op, iface_type **out)                                                          \
{                                                                                                                 \
    struct impl_type *impl;                                                                                       \
    HRESULT hr;                                                                                                   \
                                                                                                                  \
    *out = NULL;                                                                                                  \
    if (!(impl = calloc(1, sizeof(*impl)))) return E_OUTOFMEMORY;                                                 \
                                                                                                                  \
    impl->iface_type##_iface.lpVtbl = &impl_type##_vtbl;                                                          \
    impl->handler_iface_type##_iface.lpVtbl = &impl_type##_handler_vtbl;                                          \
    impl->data_writer = data_writer;                                                                              \
    impl->refcount = 1;                                                                                           \
                                                                                                                  \
    if (FAILED(hr = inner_op_iface_type##_QueryInterface(inner_op, &IID_IAsyncInfo, (void **)&impl->async_inner)) \
            || FAILED(hr = inner_op_iface_type##_put_Completed(inner_op, &impl->handler_iface_type##_iface)))     \
    {                                                                                                             \
        if (impl->async_inner)                                                                                    \
            IAsyncInfo_Release(impl->async_inner);                                                                \
        free(impl);                                                                                               \
        return hr;                                                                                                \
    }                                                                                                             \
                                                                                                                  \
    IDataWriter_AddRef(&data_writer->IDataWriter_iface);                                                          \
                                                                                                                  \
    *out = &impl->iface_type##_iface;                                                                             \
    TRACE("created IAsyncOperation %p\n", *out);                                                                  \
    return S_OK;                                                                                                  \
}                                                                                                                 \

static HRESULT buffer_create(UINT32 capacity, IBuffer **out)
{
    IBufferFactory *buffer_factory;
    HRESULT hr;

    IActivationFactory_QueryInterface(buffer_activation_factory, &IID_IBufferFactory, (void **)&buffer_factory);
    hr = IBufferFactory_Create(buffer_factory, capacity, out);
    IBufferFactory_Release(buffer_factory);
    return hr;
}

struct data_writer
{
    IDataWriter IDataWriter_iface;
    LONG ref;

    IOutputStream *stream;
    IBuffer *buffer;
    byte *data;
    BOOL storing;

    UnicodeEncoding encoding;
    ByteOrder byte_order;
};

static HRESULT data_writer_async_complete(struct data_writer *impl);

DEFINE_IASYNCOPERATION(IAsyncOperation_UINT32, "IAsyncOperation<UInt32>", async_uint32, UINT32, \
        IAsyncOperationWithProgressCompletedHandler_UINT32_UINT32, \
        IAsyncOperationWithProgress_UINT32_UINT32, \
        IAsyncOperationCompletedHandler_UINT32)

static HRESULT data_writer_init_buffer(struct data_writer *impl, UINT32 extra_capacity)
{
    /* Native capacity starts at 0x88 */
    UINT32 new_capacity, capacity = max(extra_capacity, 0x88), pos = 0;
    IBufferByteAccess *access;
    IBuffer *buffer;
    HRESULT hr;
    byte *data;

    if (impl->buffer)
    {
        IBuffer_get_Capacity(impl->buffer, &capacity);
        IBuffer_get_Length(impl->buffer, &pos);

        new_capacity = pos + extra_capacity;
        if (new_capacity < pos || new_capacity < extra_capacity)
            return E_OUTOFMEMORY;
        if (new_capacity <= capacity)
            return S_OK;

        /* If allocation size grows by 50%, the sixth allocation can fit in freed
         * memory of the first four if they were contiguous. This matches native. */
        capacity = max(new_capacity, capacity + capacity / 2u);
    }

    if (FAILED(hr = buffer_create(capacity, &buffer)))
        return hr;

    IBuffer_QueryInterface(buffer, &IID_IBufferByteAccess, (void **)&access);
    IBufferByteAccess_Buffer(access, &data);
    IBufferByteAccess_Release(access);

    if (impl->buffer)
    {
        memcpy(data, impl->data, pos);
        IBuffer_Release(impl->buffer);
    }

    impl->buffer = buffer;
    impl->data = data;

    return S_OK;
}

static struct data_writer *impl_from_IDataWriter(IDataWriter *iface)
{
    return CONTAINING_RECORD(iface, struct data_writer, IDataWriter_iface);
}

static HRESULT WINAPI data_writer_QueryInterface(IDataWriter *iface, REFIID iid, void **out)
{
    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown)
            || IsEqualGUID(iid, &IID_IInspectable)
            || IsEqualGUID(iid, &IID_IAgileObject)
            || IsEqualGUID(iid, &IID_IDataWriter))
    {
        *out = iface;
        IDataWriter_AddRef(iface);
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI data_writer_AddRef(IDataWriter *iface)
{
    struct data_writer *impl = impl_from_IDataWriter(iface);
    ULONG ref = InterlockedIncrement(&impl->ref);
    TRACE("iface %p, ref %lu.\n", iface, ref);
    return ref;
}

static ULONG WINAPI data_writer_Release(IDataWriter *iface)
{
    struct data_writer *impl = impl_from_IDataWriter(iface);
    ULONG ref = InterlockedDecrement(&impl->ref);

    TRACE("iface %p, ref %lu.\n", iface, ref);

    if (!ref)
    {
        if (impl->stream)
        {
            IClosable *closable;
            HRESULT hr;

            if (SUCCEEDED(hr = IOutputStream_QueryInterface(impl->stream, &IID_IClosable, (void **)&closable)))
            {
                hr = IClosable_Close(closable);
                IClosable_Release(closable);
            }

            if (FAILED(hr))
                WARN("Failed to close stream, hr %#lx.\n", hr);

            IOutputStream_Release(impl->stream);
        }
        if (impl->buffer)
            IBuffer_Release(impl->buffer);
        free(impl);
    }

    return ref;
}

static HRESULT WINAPI data_writer_GetIids(IDataWriter *iface, ULONG *iid_count, IID **iids)
{
    FIXME("iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids);
    return E_NOTIMPL;
}

static HRESULT WINAPI data_writer_GetRuntimeClassName(IDataWriter *iface, HSTRING *class_name)
{
    FIXME("iface %p, class_name %p stub!\n", iface, class_name);
    return E_NOTIMPL;
}

static HRESULT WINAPI data_writer_GetTrustLevel(IDataWriter *iface, TrustLevel *trust_level)
{
    FIXME("iface %p, trust_level %p stub!\n", iface, trust_level);
    return E_NOTIMPL;
}

static HRESULT WINAPI data_writer_get_UnstoredBufferLength(IDataWriter *iface, UINT32 *value)
{
    struct data_writer *impl = impl_from_IDataWriter(iface);

    TRACE("iface %p, value %p.\n", iface, value);

    if (!value) return E_POINTER;
    *value = 0;
    if (impl->buffer) IBuffer_get_Length(impl->buffer, value);
    return S_OK;
}

static HRESULT WINAPI data_writer_get_UnicodeEncoding(IDataWriter *iface, UnicodeEncoding *value)
{
    struct data_writer *impl = impl_from_IDataWriter(iface);

    TRACE("iface %p, value %p.\n", iface, value);

    if (!value) return E_POINTER;
    *value = impl->encoding;
    return S_OK;
}

static HRESULT WINAPI data_writer_put_UnicodeEncoding(IDataWriter *iface, UnicodeEncoding value)
{
    struct data_writer *impl = impl_from_IDataWriter(iface);

    TRACE("iface %p, value %u.\n", iface, value);

    if (value > UnicodeEncoding_Utf16BE) return E_INVALIDARG;
    impl->encoding = value;
    return S_OK;
}

static HRESULT WINAPI data_writer_get_ByteOrder(IDataWriter *iface, ByteOrder *value)
{
    struct data_writer *impl = impl_from_IDataWriter(iface);

    TRACE("iface %p, value %p.\n", iface, value);

    if (!value) return E_POINTER;
    *value = impl->byte_order;
    return S_OK;
}

static HRESULT WINAPI data_writer_put_ByteOrder(IDataWriter *iface, ByteOrder value)
{
    struct data_writer *impl = impl_from_IDataWriter(iface);

    TRACE("iface %p, value %u.\n", iface, value);

    if (value > ByteOrder_BigEndian) return E_INVALIDARG;
    impl->byte_order = value;
    return S_OK;
}

static HRESULT data_writer_write(struct data_writer *impl, const void *value, UINT32 size)
{
    UINT32 pos = 0;
    HRESULT hr;

    if (FAILED(hr = data_writer_init_buffer(impl, size)))
        return hr;

    IBuffer_get_Length(impl->buffer, &pos);
    memcpy(&impl->data[pos], value, size);
    IBuffer_put_Length(impl->buffer, pos + size);
    return S_OK;
}

/* Write a little-endian scalar honouring the writer's byte order. */
static HRESULT data_writer_write_scalar(struct data_writer *impl, const void *value, UINT32 size)
{
    const BYTE *src = value;
    BYTE tmp[8];
    UINT32 i;

    if (impl->byte_order == ByteOrder_LittleEndian)
        return data_writer_write(impl, value, size);

    for (i = 0; i < size; i++) tmp[i] = src[size - 1 - i];
    return data_writer_write(impl, tmp, size);
}

static HRESULT data_writer_write_buffer(struct data_writer *impl, IBuffer *buffer, UINT32 start, UINT32 count)
{
    IBufferByteAccess *access;
    UINT32 length;
    HRESULT hr;
    BYTE *data;

    if (!buffer) return E_POINTER;
    if (FAILED(hr = IBuffer_get_Length(buffer, &length))) return hr;
    if (start > length || count > length - start) return E_BOUNDS;
    if (FAILED(hr = IBuffer_QueryInterface(buffer, &IID_IBufferByteAccess, (void **)&access))) return hr;
    if (SUCCEEDED(hr = IBufferByteAccess_Buffer(access, &data)))
        hr = data_writer_write(impl, data + start, count);
    IBufferByteAccess_Release(access);
    return hr;
}

/* Encodes value into the writer's encoding; with write FALSE only the length is computed. */
static HRESULT data_writer_encode_string(struct data_writer *impl, HSTRING value, BOOL write, UINT32 *code_unit_count)
{
    UINT32 len, i, size;
    const WCHAR *str = WindowsGetStringRawBuffer(value, &len);
    HRESULT hr = S_OK;
    BYTE *bytes;

    if (!code_unit_count) return E_POINTER;

    if (impl->encoding == UnicodeEncoding_Utf8)
    {
        size = len ? WideCharToMultiByte(CP_UTF8, 0, str, len, NULL, 0, NULL, NULL) : 0;
        *code_unit_count = size;
        if (!write || !size) return S_OK;
        if (!(bytes = malloc(size))) return E_OUTOFMEMORY;
        WideCharToMultiByte(CP_UTF8, 0, str, len, (char *)bytes, size, NULL, NULL);
    }
    else
    {
        *code_unit_count = len;
        size = len * sizeof(WCHAR);
        if (!write || !size) return S_OK;
        if (!(bytes = malloc(size))) return E_OUTOFMEMORY;
        memcpy(bytes, str, size);
        if (impl->encoding == UnicodeEncoding_Utf16BE)
            for (i = 0; i < size; i += 2) { BYTE b = bytes[i]; bytes[i] = bytes[i + 1]; bytes[i + 1] = b; }
    }

    hr = data_writer_write(impl, bytes, size);
    free(bytes);
    return hr;
}

static HRESULT WINAPI data_writer_WriteByte(IDataWriter *iface, BYTE value)
{
    struct data_writer *impl = impl_from_IDataWriter(iface);

    TRACE("iface %p, value %u.\n", iface, value);

    return data_writer_write(impl, &value, sizeof(value));
}

static HRESULT WINAPI data_writer_WriteBytes(IDataWriter *iface, UINT32 value_size, BYTE *value)
{
    struct data_writer *impl = impl_from_IDataWriter(iface);

    TRACE("iface %p, value_size %u, value %p.\n", iface, value_size, value);

    return data_writer_write(impl, value, value_size);
}

static HRESULT WINAPI data_writer_WriteBuffer(IDataWriter *iface, IBuffer *buffer)
{
    struct data_writer *impl = impl_from_IDataWriter(iface);
    UINT32 length = 0;

    TRACE("iface %p, buffer %p.\n", iface, buffer);

    if (!buffer) return E_POINTER;
    IBuffer_get_Length(buffer, &length);
    return data_writer_write_buffer(impl, buffer, 0, length);
}

static HRESULT WINAPI data_writer_WriteBufferRange(IDataWriter *iface, IBuffer *buffer, UINT32 start, UINT32 count)
{
    struct data_writer *impl = impl_from_IDataWriter(iface);

    TRACE("iface %p, buffer %p, start %u, count %u.\n", iface, buffer, start, count);

    return data_writer_write_buffer(impl, buffer, start, count);
}

static HRESULT WINAPI data_writer_WriteBoolean(IDataWriter *iface, boolean value)
{
    struct data_writer *impl = impl_from_IDataWriter(iface);
    BYTE byte = value ? 1 : 0;

    TRACE("iface %p, value %u.\n", iface, value);

    return data_writer_write(impl, &byte, sizeof(byte));
}

static HRESULT WINAPI data_writer_WriteGuid(IDataWriter *iface, GUID value)
{
    struct data_writer *impl = impl_from_IDataWriter(iface);
    HRESULT hr;

    TRACE("iface %p, value %s.\n", iface, debugstr_guid(&value));

    if (FAILED(hr = data_writer_write_scalar(impl, &value.Data1, sizeof(value.Data1)))) return hr;
    if (FAILED(hr = data_writer_write_scalar(impl, &value.Data2, sizeof(value.Data2)))) return hr;
    if (FAILED(hr = data_writer_write_scalar(impl, &value.Data3, sizeof(value.Data3)))) return hr;
    return data_writer_write(impl, value.Data4, sizeof(value.Data4));
}

static HRESULT WINAPI data_writer_WriteInt16(IDataWriter *iface, INT16 value)
{
    struct data_writer *impl = impl_from_IDataWriter(iface);

    TRACE("iface %p, value %d.\n", iface, value);

    return data_writer_write_scalar(impl, &value, sizeof(value));
}

static HRESULT WINAPI data_writer_WriteInt32(IDataWriter *iface, INT32 value)
{
    struct data_writer *impl = impl_from_IDataWriter(iface);

    TRACE("iface %p, value %d.\n", iface, value);

    return data_writer_write_scalar(impl, &value, sizeof(value));
}

static HRESULT WINAPI data_writer_WriteInt64(IDataWriter *iface, INT64 value)
{
    struct data_writer *impl = impl_from_IDataWriter(iface);

    TRACE("iface %p, value %I64d.\n", iface, value);

    return data_writer_write_scalar(impl, &value, sizeof(value));
}

static HRESULT WINAPI data_writer_WriteUInt16(IDataWriter *iface, UINT16 value)
{
    struct data_writer *impl = impl_from_IDataWriter(iface);

    TRACE("iface %p, value %u.\n", iface, value);

    return data_writer_write_scalar(impl, &value, sizeof(value));
}

static HRESULT WINAPI data_writer_WriteUInt32(IDataWriter *iface, UINT32 value)
{
    struct data_writer *impl = impl_from_IDataWriter(iface);

    TRACE("iface %p, value %u.\n", iface, value);

    return data_writer_write_scalar(impl, &value, sizeof(value));
}

static HRESULT WINAPI data_writer_WriteUInt64(IDataWriter *iface, UINT64 value)
{
    struct data_writer *impl = impl_from_IDataWriter(iface);

    TRACE("iface %p, value %I64u.\n", iface, value);

    return data_writer_write_scalar(impl, &value, sizeof(value));
}

static HRESULT WINAPI data_writer_WriteSingle(IDataWriter *iface, FLOAT value)
{
    struct data_writer *impl = impl_from_IDataWriter(iface);

    TRACE("iface %p, value %.7f.\n", iface, value);

    return data_writer_write_scalar(impl, &value, sizeof(value));
}

static HRESULT WINAPI data_writer_WriteDouble(IDataWriter *iface, DOUBLE value)
{
    struct data_writer *impl = impl_from_IDataWriter(iface);

    TRACE("iface %p, value %.15f.\n", iface, value);

    return data_writer_write_scalar(impl, &value, sizeof(value));
}

static HRESULT WINAPI data_writer_WriteDateTime(IDataWriter *iface, DateTime value)
{
    struct data_writer *impl = impl_from_IDataWriter(iface);

    TRACE("iface %p, value %I64d.\n", iface, value.UniversalTime);

    return data_writer_write_scalar(impl, &value.UniversalTime, sizeof(value.UniversalTime));
}

static HRESULT WINAPI data_writer_WriteTimeSpan(IDataWriter *iface, TimeSpan value)
{
    struct data_writer *impl = impl_from_IDataWriter(iface);

    TRACE("iface %p, value %I64d.\n", iface, value.Duration);

    return data_writer_write_scalar(impl, &value.Duration, sizeof(value.Duration));
}

static HRESULT WINAPI data_writer_WriteString(IDataWriter *iface, HSTRING value, UINT32 *code_unit_count)
{
    struct data_writer *impl = impl_from_IDataWriter(iface);

    TRACE("iface %p, value %s, code_unit_count %p.\n", iface, debugstr_hstring(value), code_unit_count);

    return data_writer_encode_string(impl, value, TRUE, code_unit_count);
}

static HRESULT WINAPI data_writer_MeasureString(IDataWriter *iface, HSTRING value, UINT32 *code_unit_count)
{
    struct data_writer *impl = impl_from_IDataWriter(iface);

    TRACE("iface %p, value %s, code_unit_count %p.\n", iface, debugstr_hstring(value), code_unit_count);

    return data_writer_encode_string(impl, value, FALSE, code_unit_count);
}

static HRESULT data_writer_async_complete(struct data_writer *impl)
{
    impl->storing = FALSE;
    return S_OK;
}

static HRESULT WINAPI data_writer_StoreAsync(IDataWriter *iface, IAsyncOperation_UINT32 **operation)
{
    IAsyncOperationWithProgress_UINT32_UINT32 *inner_operation;
    struct data_writer *impl = impl_from_IDataWriter(iface);
    HRESULT hr;

    TRACE("iface %p, operation %p.\n", iface, operation);

    *operation = NULL;

    if (!impl->stream)
        return HRESULT_FROM_WIN32(ERROR_INVALID_OPERATION);

    if (FAILED(hr = IOutputStream_WriteAsync(impl->stream, impl->buffer, &inner_operation)))
        return hr;
    IBuffer_Release(impl->buffer);
    impl->buffer = NULL;
    impl->storing = TRUE;

    hr = async_operation_UINT32_create(impl, inner_operation, operation);
    IAsyncOperationWithProgress_UINT32_UINT32_Release(inner_operation);

    if (SUCCEEDED(hr))
        hr = data_writer_init_buffer(impl, 0);

    return hr;
}

static HRESULT WINAPI data_writer_FlushAsync(IDataWriter *iface, IAsyncOperation_boolean **operation)
{
    struct data_writer *impl = impl_from_IDataWriter(iface);

    FIXME("iface %p, operation %p stub!\n", iface, operation);

    *operation = NULL;

    if (!impl->stream)
        return HRESULT_FROM_WIN32(ERROR_INVALID_OPERATION);

    return E_NOTIMPL;
}

static HRESULT WINAPI data_writer_DetachBuffer(IDataWriter *iface, IBuffer **buffer)
{
    struct data_writer *impl = impl_from_IDataWriter(iface);

    TRACE("iface %p, buffer %p.\n", iface, buffer);

    *buffer = impl->buffer;
    impl->buffer = NULL;
    return data_writer_init_buffer(impl, 0);
}

static HRESULT WINAPI data_writer_DetachStream(IDataWriter *iface, IOutputStream **output_stream)
{
    struct data_writer *impl = impl_from_IDataWriter(iface);

    TRACE("iface %p, output_stream %p.\n", iface, output_stream);

    *output_stream = NULL;

    if (impl->storing)
        return HRESULT_FROM_WIN32(ERROR_INVALID_OPERATION);

    *output_stream = impl->stream;
    impl->stream = NULL;
    return S_OK;
}

static const struct IDataWriterVtbl data_writer_vtbl =
{
    /* IUnknown methods */
    data_writer_QueryInterface,
    data_writer_AddRef,
    data_writer_Release,
    /* IInspectable methods */
    data_writer_GetIids,
    data_writer_GetRuntimeClassName,
    data_writer_GetTrustLevel,
    /* IDataWriter */
    data_writer_get_UnstoredBufferLength,
    data_writer_get_UnicodeEncoding,
    data_writer_put_UnicodeEncoding,
    data_writer_get_ByteOrder,
    data_writer_put_ByteOrder,
    data_writer_WriteByte,
    data_writer_WriteBytes,
    data_writer_WriteBuffer,
    data_writer_WriteBufferRange,
    data_writer_WriteBoolean,
    data_writer_WriteGuid,
    data_writer_WriteInt16,
    data_writer_WriteInt32,
    data_writer_WriteInt64,
    data_writer_WriteUInt16,
    data_writer_WriteUInt32,
    data_writer_WriteUInt64,
    data_writer_WriteSingle,
    data_writer_WriteDouble,
    data_writer_WriteDateTime,
    data_writer_WriteTimeSpan,
    data_writer_WriteString,
    data_writer_MeasureString,
    data_writer_StoreAsync,
    data_writer_FlushAsync,
    data_writer_DetachBuffer,
    data_writer_DetachStream,
};

static HRESULT data_writer_create(IOutputStream *output_stream, IDataWriter **out)
{
    struct data_writer *impl;
    HRESULT hr;

    *out = NULL;
    if (!(impl = calloc(1, sizeof(*impl))))
        return E_OUTOFMEMORY;

    impl->IDataWriter_iface.lpVtbl = &data_writer_vtbl;
    impl->stream = output_stream;
    impl->ref = 1;
    impl->encoding = UnicodeEncoding_Utf8;
    impl->byte_order = ByteOrder_BigEndian;

    if (FAILED(hr = data_writer_init_buffer(impl, 0)))
    {
        free(impl);
        return hr;
    }

    if (output_stream)
        IOutputStream_AddRef(output_stream);

    *out = &impl->IDataWriter_iface;
    return S_OK;
}

struct data_writer_factory
{
    IActivationFactory IActivationFactory_iface;
    IDataWriterFactory IDataWriterFactory_iface;
    LONG ref;
};

static inline struct data_writer_factory *impl_data_writer_factory_from_IActivationFactory(IActivationFactory *iface)
{
    return CONTAINING_RECORD(iface, struct data_writer_factory, IActivationFactory_iface);
}

static inline struct data_writer_factory *impl_data_writer_factory_from_IDataWriterFactory(IDataWriterFactory *iface)
{
    return CONTAINING_RECORD(iface, struct data_writer_factory, IDataWriterFactory_iface);
}

static HRESULT STDMETHODCALLTYPE data_writer_activation_factory_QueryInterface(IActivationFactory *iface, REFIID iid,
        void **out)
{
    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown)
            || IsEqualGUID(iid, &IID_IInspectable)
            || IsEqualGUID(iid, &IID_IAgileObject)
            || IsEqualGUID(iid, &IID_IActivationFactory))
    {
        IUnknown_AddRef(iface);
        *out = iface;
        return S_OK;
    }

    if (IsEqualGUID(iid, &IID_IDataWriterFactory))
    {
        struct data_writer_factory *impl = impl_data_writer_factory_from_IActivationFactory(iface);
        IDataWriterFactory_AddRef(&impl->IDataWriterFactory_iface);
        *out = &impl->IDataWriterFactory_iface;
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE data_writer_activation_factory_AddRef(IActivationFactory *iface)
{
    struct data_writer_factory *impl = impl_data_writer_factory_from_IActivationFactory(iface);
    ULONG ref = InterlockedIncrement(&impl->ref);
    TRACE("iface %p, ref %lu.\n", iface, ref);
    return ref;
}

static ULONG STDMETHODCALLTYPE data_writer_activation_factory_Release(IActivationFactory *iface)
{
    struct data_writer_factory *impl = impl_data_writer_factory_from_IActivationFactory(iface);
    ULONG ref = InterlockedDecrement(&impl->ref);
    TRACE("iface %p, ref %lu.\n", iface, ref);
    return ref;
}

static HRESULT STDMETHODCALLTYPE data_writer_activation_factory_GetIids(IActivationFactory *iface, ULONG *iid_count,
        IID **iids)
{
    FIXME("iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE data_writer_activation_factory_GetRuntimeClassName(IActivationFactory *iface,
        HSTRING *class_name)
{
    FIXME("iface %p, class_name %p stub!\n", iface, class_name);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE data_writer_activation_factory_GetTrustLevel(IActivationFactory *iface,
        TrustLevel *trust_level)
{
    FIXME("iface %p, trust_level %p stub!\n", iface, trust_level);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE data_writer_activation_factory_ActivateInstance(IActivationFactory *iface,
        IInspectable **instance)
{
    IDataWriter *data_writer;
    HRESULT hr;

    TRACE("iface %p, instance %p.\n", iface, instance);

    if (FAILED(hr = data_writer_create(NULL, &data_writer)))
        return hr;

    hr = IDataWriter_QueryInterface(data_writer, &IID_IInspectable, (void **)instance);
    IDataWriter_Release(data_writer);

    return hr;
}

static const struct IActivationFactoryVtbl data_writer_activation_factory_vtbl =
{
    data_writer_activation_factory_QueryInterface,
    data_writer_activation_factory_AddRef,
    data_writer_activation_factory_Release,
    /* IInspectable methods */
    data_writer_activation_factory_GetIids,
    data_writer_activation_factory_GetRuntimeClassName,
    data_writer_activation_factory_GetTrustLevel,
    /* IActivationFactory methods */
    data_writer_activation_factory_ActivateInstance,
};

static HRESULT STDMETHODCALLTYPE data_writer_factory_QueryInterface(IDataWriterFactory *iface, REFIID iid, void **out)
{
    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown)
            || IsEqualGUID(iid, &IID_IInspectable)
            || IsEqualGUID(iid, &IID_IAgileObject)
            || IsEqualGUID(iid, &IID_IDataWriterFactory))
    {
        IUnknown_AddRef(iface);
        *out = iface;
        return S_OK;
    }

    if (IsEqualGUID(iid, &IID_IActivationFactory))
    {
        struct data_writer_factory *impl = impl_data_writer_factory_from_IDataWriterFactory(iface);
        IActivationFactory_AddRef(&impl->IActivationFactory_iface);
        *out = &impl->IActivationFactory_iface;
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE data_writer_factory_AddRef(IDataWriterFactory *iface)
{
    struct data_writer_factory *impl = impl_data_writer_factory_from_IDataWriterFactory(iface);
    ULONG ref = InterlockedIncrement(&impl->ref);
    TRACE("iface %p, ref %lu.\n", iface, ref);
    return ref;
}

static ULONG STDMETHODCALLTYPE data_writer_factory_Release(IDataWriterFactory *iface)
{
    struct data_writer_factory *impl = impl_data_writer_factory_from_IDataWriterFactory(iface);
    ULONG ref = InterlockedDecrement(&impl->ref);
    TRACE("iface %p, ref %lu.\n", iface, ref);
    return ref;
}

static HRESULT STDMETHODCALLTYPE data_writer_factory_GetIids(IDataWriterFactory *iface, ULONG *iid_count,
        IID **iids)
{
    FIXME("iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE data_writer_factory_GetRuntimeClassName(IDataWriterFactory *iface,
        HSTRING *class_name)
{
    FIXME("iface %p, class_name %p stub!\n", iface, class_name);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE data_writer_factory_GetTrustLevel(IDataWriterFactory *iface,
        TrustLevel *trust_level)
{
    FIXME("iface %p, trust_level %p stub!\n", iface, trust_level);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE data_writer_factory_CreateDataWriter(IDataWriterFactory *iface,
        IOutputStream *output_stream, IDataWriter **data_writer)
{
    TRACE("iface %p, output_stream %p, data_writer %p.\n", iface, output_stream, data_writer);
    return data_writer_create(output_stream, data_writer);
}

static const struct IDataWriterFactoryVtbl data_writer_factory_vtbl =
{
    data_writer_factory_QueryInterface,
    data_writer_factory_AddRef,
    data_writer_factory_Release,
    /* IInspectable methods */
    data_writer_factory_GetIids,
    data_writer_factory_GetRuntimeClassName,
    data_writer_factory_GetTrustLevel,
    /* IDataWriterFactory methods */
    data_writer_factory_CreateDataWriter,
};

struct data_writer_factory data_writer_factory =
{
    {&data_writer_activation_factory_vtbl},
    {&data_writer_factory_vtbl},
    1
};

IActivationFactory *data_writer_activation_factory = &data_writer_factory.IActivationFactory_iface;

struct data_reader
{
    IDataReader IDataReader_iface;
    IClosable IClosable_iface;
    LONG ref;

    IBuffer *buffer;
    BYTE *data;
    UINT32 length;
    UINT32 pos;

    UnicodeEncoding encoding;
    ByteOrder byte_order;
    InputStreamOptions options;
};

static struct data_reader *impl_from_IDataReader(IDataReader *iface)
{
    return CONTAINING_RECORD(iface, struct data_reader, IDataReader_iface);
}

static HRESULT WINAPI data_reader_QueryInterface(IDataReader *iface, REFIID iid, void **out)
{
    struct data_reader *impl = impl_from_IDataReader(iface);

    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown)
            || IsEqualGUID(iid, &IID_IInspectable)
            || IsEqualGUID(iid, &IID_IAgileObject)
            || IsEqualGUID(iid, &IID_IDataReader))
    {
        *out = iface;
        IDataReader_AddRef(iface);
        return S_OK;
    }

    if (IsEqualGUID(iid, &IID_IClosable))
    {
        *out = &impl->IClosable_iface;
        IDataReader_AddRef(iface);
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI data_reader_AddRef(IDataReader *iface)
{
    struct data_reader *impl = impl_from_IDataReader(iface);
    ULONG ref = InterlockedIncrement(&impl->ref);
    TRACE("iface %p, ref %lu.\n", iface, ref);
    return ref;
}

static ULONG WINAPI data_reader_Release(IDataReader *iface)
{
    struct data_reader *impl = impl_from_IDataReader(iface);
    ULONG ref = InterlockedDecrement(&impl->ref);

    TRACE("iface %p, ref %lu.\n", iface, ref);

    if (!ref)
    {
        if (impl->buffer)
            IBuffer_Release(impl->buffer);
        free(impl);
    }

    return ref;
}

static HRESULT WINAPI data_reader_GetIids(IDataReader *iface, ULONG *iid_count, IID **iids)
{
    FIXME("iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids);
    return E_NOTIMPL;
}

static HRESULT WINAPI data_reader_GetRuntimeClassName(IDataReader *iface, HSTRING *class_name)
{
    static const WCHAR name[] = L"Windows.Storage.Streams.DataReader";

    TRACE("iface %p, class_name %p.\n", iface, class_name);

    return WindowsCreateString(name, ARRAY_SIZE(name) - 1, class_name);
}

static HRESULT WINAPI data_reader_GetTrustLevel(IDataReader *iface, TrustLevel *trust_level)
{
    FIXME("iface %p, trust_level %p stub!\n", iface, trust_level);
    return E_NOTIMPL;
}

static HRESULT WINAPI data_reader_get_UnconsumedBufferLength(IDataReader *iface, UINT32 *value)
{
    struct data_reader *impl = impl_from_IDataReader(iface);

    TRACE("iface %p, value %p.\n", iface, value);

    if (!value) return E_POINTER;
    *value = impl->length - impl->pos;
    return S_OK;
}

static HRESULT WINAPI data_reader_get_UnicodeEncoding(IDataReader *iface, UnicodeEncoding *value)
{
    struct data_reader *impl = impl_from_IDataReader(iface);

    TRACE("iface %p, value %p.\n", iface, value);

    if (!value) return E_POINTER;
    *value = impl->encoding;
    return S_OK;
}

static HRESULT WINAPI data_reader_put_UnicodeEncoding(IDataReader *iface, UnicodeEncoding value)
{
    struct data_reader *impl = impl_from_IDataReader(iface);

    TRACE("iface %p, value %u.\n", iface, value);

    if (value > UnicodeEncoding_Utf16BE) return E_INVALIDARG;
    impl->encoding = value;
    return S_OK;
}

static HRESULT WINAPI data_reader_get_ByteOrder(IDataReader *iface, ByteOrder *value)
{
    struct data_reader *impl = impl_from_IDataReader(iface);

    TRACE("iface %p, value %p.\n", iface, value);

    if (!value) return E_POINTER;
    *value = impl->byte_order;
    return S_OK;
}

static HRESULT WINAPI data_reader_put_ByteOrder(IDataReader *iface, ByteOrder value)
{
    struct data_reader *impl = impl_from_IDataReader(iface);

    TRACE("iface %p, value %u.\n", iface, value);

    if (value > ByteOrder_BigEndian) return E_INVALIDARG;
    impl->byte_order = value;
    return S_OK;
}

static HRESULT WINAPI data_reader_get_InputStreamOptions(IDataReader *iface, InputStreamOptions *value)
{
    struct data_reader *impl = impl_from_IDataReader(iface);

    TRACE("iface %p, value %p.\n", iface, value);

    if (!value) return E_POINTER;
    *value = impl->options;
    return S_OK;
}

static HRESULT WINAPI data_reader_put_InputStreamOptions(IDataReader *iface, InputStreamOptions value)
{
    struct data_reader *impl = impl_from_IDataReader(iface);

    TRACE("iface %p, value %#x.\n", iface, value);

    impl->options = value;
    return S_OK;
}

static HRESULT data_reader_read(struct data_reader *impl, void *value, UINT32 size)
{
    if (!impl->data && size) return HRESULT_FROM_WIN32(ERROR_INVALID_OPERATION);
    if (size > impl->length - impl->pos) return E_BOUNDS;
    memcpy(value, impl->data + impl->pos, size);
    impl->pos += size;
    return S_OK;
}

/* Read a scalar into little-endian host order, honouring the reader's byte order. */
static HRESULT data_reader_read_scalar(struct data_reader *impl, void *value, UINT32 size)
{
    BYTE *bytes = value, tmp;
    UINT32 i;
    HRESULT hr;

    if (FAILED(hr = data_reader_read(impl, value, size))) return hr;
    if (impl->byte_order == ByteOrder_BigEndian)
        for (i = 0; i < size / 2; i++)
        {
            tmp = bytes[i];
            bytes[i] = bytes[size - 1 - i];
            bytes[size - 1 - i] = tmp;
        }
    return S_OK;
}

static HRESULT WINAPI data_reader_ReadByte(IDataReader *iface, BYTE *value)
{
    struct data_reader *impl = impl_from_IDataReader(iface);

    TRACE("iface %p, value %p.\n", iface, value);

    if (!value) return E_POINTER;
    return data_reader_read(impl, value, sizeof(*value));
}

static HRESULT WINAPI data_reader_ReadBytes(IDataReader *iface, UINT32 value_size, BYTE *value)
{
    struct data_reader *impl = impl_from_IDataReader(iface);

    TRACE("iface %p, value_size %u, value %p.\n", iface, value_size, value);

    if (!value && value_size) return E_POINTER;
    return data_reader_read(impl, value, value_size);
}

static HRESULT WINAPI data_reader_ReadBuffer(IDataReader *iface, UINT32 length, IBuffer **buffer)
{
    struct data_reader *impl = impl_from_IDataReader(iface);
    IBufferByteAccess *access;
    IBuffer *out;
    HRESULT hr;
    BYTE *data;

    TRACE("iface %p, length %u, buffer %p.\n", iface, length, buffer);

    if (!buffer) return E_POINTER;
    *buffer = NULL;
    if (length > impl->length - impl->pos) return E_BOUNDS;

    if (FAILED(hr = buffer_create(length, &out))) return hr;
    if (SUCCEEDED(hr = IBuffer_QueryInterface(out, &IID_IBufferByteAccess, (void **)&access)))
    {
        if (SUCCEEDED(hr = IBufferByteAccess_Buffer(access, &data)))
            hr = data_reader_read(impl, data, length);
        IBufferByteAccess_Release(access);
    }
    if (SUCCEEDED(hr)) hr = IBuffer_put_Length(out, length);
    if (FAILED(hr))
    {
        IBuffer_Release(out);
        return hr;
    }

    *buffer = out;
    return S_OK;
}

static HRESULT WINAPI data_reader_ReadBoolean(IDataReader *iface, boolean *value)
{
    struct data_reader *impl = impl_from_IDataReader(iface);
    BYTE byte;
    HRESULT hr;

    TRACE("iface %p, value %p.\n", iface, value);

    if (!value) return E_POINTER;
    if (FAILED(hr = data_reader_read(impl, &byte, sizeof(byte)))) return hr;
    *value = !!byte;
    return S_OK;
}

static HRESULT WINAPI data_reader_ReadGuid(IDataReader *iface, GUID *value)
{
    struct data_reader *impl = impl_from_IDataReader(iface);
    UINT32 start = impl->pos;
    HRESULT hr;

    TRACE("iface %p, value %p.\n", iface, value);

    if (!value) return E_POINTER;
    if (impl->length - impl->pos < sizeof(*value)) return E_BOUNDS;
    if (FAILED(hr = data_reader_read_scalar(impl, &value->Data1, sizeof(value->Data1))) ||
        FAILED(hr = data_reader_read_scalar(impl, &value->Data2, sizeof(value->Data2))) ||
        FAILED(hr = data_reader_read_scalar(impl, &value->Data3, sizeof(value->Data3))) ||
        FAILED(hr = data_reader_read(impl, value->Data4, sizeof(value->Data4))))
        impl->pos = start;
    return hr;
}

#define DEFINE_DATA_READER_READ_SCALAR(name, type)                                   \
static HRESULT WINAPI data_reader_Read##name(IDataReader *iface, type *value)        \
{                                                                                    \
    struct data_reader *impl = impl_from_IDataReader(iface);                         \
                                                                                     \
    TRACE("iface %p, value %p.\n", iface, value);                                    \
                                                                                     \
    if (!value) return E_POINTER;                                                    \
    return data_reader_read_scalar(impl, value, sizeof(*value));                     \
}

DEFINE_DATA_READER_READ_SCALAR(Int16, INT16)
DEFINE_DATA_READER_READ_SCALAR(Int32, INT32)
DEFINE_DATA_READER_READ_SCALAR(Int64, INT64)
DEFINE_DATA_READER_READ_SCALAR(UInt16, UINT16)
DEFINE_DATA_READER_READ_SCALAR(UInt32, UINT32)
DEFINE_DATA_READER_READ_SCALAR(UInt64, UINT64)
DEFINE_DATA_READER_READ_SCALAR(Single, FLOAT)
DEFINE_DATA_READER_READ_SCALAR(Double, DOUBLE)

static HRESULT WINAPI data_reader_ReadString(IDataReader *iface, UINT32 code_unit_count, HSTRING *value)
{
    struct data_reader *impl = impl_from_IDataReader(iface);
    UINT32 size, i, len;
    const BYTE *src;
    WCHAR *str;
    HRESULT hr;

    TRACE("iface %p, code_unit_count %u, value %p.\n", iface, code_unit_count, value);

    if (!value) return E_POINTER;
    *value = NULL;

    size = impl->encoding == UnicodeEncoding_Utf8 ? code_unit_count : code_unit_count * sizeof(WCHAR);
    if (impl->encoding != UnicodeEncoding_Utf8 && code_unit_count > UINT_MAX / sizeof(WCHAR)) return E_BOUNDS;
    if (size > impl->length - impl->pos) return E_BOUNDS;
    if (!size) return S_OK;
    src = impl->data + impl->pos;

    if (impl->encoding == UnicodeEncoding_Utf8)
    {
        len = MultiByteToWideChar(CP_UTF8, 0, (const char *)src, size, NULL, 0);
        if (!(str = malloc(len * sizeof(WCHAR)))) return E_OUTOFMEMORY;
        MultiByteToWideChar(CP_UTF8, 0, (const char *)src, size, str, len);
    }
    else
    {
        len = code_unit_count;
        if (!(str = malloc(size))) return E_OUTOFMEMORY;
        for (i = 0; i < len; i++)
            str[i] = impl->encoding == UnicodeEncoding_Utf16BE ? (src[2 * i] << 8) | src[2 * i + 1]
                                                               : src[2 * i] | (src[2 * i + 1] << 8);
    }

    if (SUCCEEDED(hr = WindowsCreateString(str, len, value)))
        impl->pos += size;
    free(str);
    return hr;
}

static HRESULT WINAPI data_reader_ReadDateTime(IDataReader *iface, DateTime *value)
{
    struct data_reader *impl = impl_from_IDataReader(iface);

    TRACE("iface %p, value %p.\n", iface, value);

    if (!value) return E_POINTER;
    return data_reader_read_scalar(impl, &value->UniversalTime, sizeof(value->UniversalTime));
}

static HRESULT WINAPI data_reader_ReadTimeSpan(IDataReader *iface, TimeSpan *value)
{
    struct data_reader *impl = impl_from_IDataReader(iface);

    TRACE("iface %p, value %p.\n", iface, value);

    if (!value) return E_POINTER;
    return data_reader_read_scalar(impl, &value->Duration, sizeof(value->Duration));
}

static HRESULT WINAPI data_reader_LoadAsync(IDataReader *iface, UINT32 count, IAsyncOperation_UINT32 **operation)
{
    FIXME("iface %p, count %u, operation %p stub!\n", iface, count, operation);
    return E_NOTIMPL;
}

static HRESULT WINAPI data_reader_DetachBuffer(IDataReader *iface, IBuffer **buffer)
{
    struct data_reader *impl = impl_from_IDataReader(iface);

    TRACE("iface %p, buffer %p.\n", iface, buffer);

    if (!buffer) return E_POINTER;
    *buffer = impl->buffer;
    impl->buffer = NULL;
    impl->data = NULL;
    impl->length = impl->pos = 0;
    return S_OK;
}

static HRESULT WINAPI data_reader_DetachStream(IDataReader *iface, IInputStream **stream)
{
    FIXME("iface %p, stream %p stub!\n", iface, stream);

    if (!stream) return E_POINTER;
    *stream = NULL;
    return S_OK;
}

static const struct IDataReaderVtbl data_reader_vtbl =
{
    data_reader_QueryInterface,
    data_reader_AddRef,
    data_reader_Release,
    /* IInspectable methods */
    data_reader_GetIids,
    data_reader_GetRuntimeClassName,
    data_reader_GetTrustLevel,
    /* IDataReader methods */
    data_reader_get_UnconsumedBufferLength,
    data_reader_get_UnicodeEncoding,
    data_reader_put_UnicodeEncoding,
    data_reader_get_ByteOrder,
    data_reader_put_ByteOrder,
    data_reader_get_InputStreamOptions,
    data_reader_put_InputStreamOptions,
    data_reader_ReadByte,
    data_reader_ReadBytes,
    data_reader_ReadBuffer,
    data_reader_ReadBoolean,
    data_reader_ReadGuid,
    data_reader_ReadInt16,
    data_reader_ReadInt32,
    data_reader_ReadInt64,
    data_reader_ReadUInt16,
    data_reader_ReadUInt32,
    data_reader_ReadUInt64,
    data_reader_ReadSingle,
    data_reader_ReadDouble,
    data_reader_ReadString,
    data_reader_ReadDateTime,
    data_reader_ReadTimeSpan,
    data_reader_LoadAsync,
    data_reader_DetachBuffer,
    data_reader_DetachStream,
};

DEFINE_IINSPECTABLE(data_reader_closable, IClosable, struct data_reader, IDataReader_iface)

static HRESULT WINAPI data_reader_closable_Close(IClosable *iface)
{
    struct data_reader *impl = impl_from_IClosable(iface);

    TRACE("iface %p.\n", iface);

    if (impl->buffer)
        IBuffer_Release(impl->buffer);
    impl->buffer = NULL;
    impl->data = NULL;
    impl->length = impl->pos = 0;
    return S_OK;
}

static const struct IClosableVtbl data_reader_closable_vtbl =
{
    data_reader_closable_QueryInterface,
    data_reader_closable_AddRef,
    data_reader_closable_Release,
    /* IInspectable methods */
    data_reader_closable_GetIids,
    data_reader_closable_GetRuntimeClassName,
    data_reader_closable_GetTrustLevel,
    /* IClosable methods */
    data_reader_closable_Close,
};

static HRESULT data_reader_create(IBuffer *buffer, IDataReader **out)
{
    struct data_reader *impl;
    IBufferByteAccess *access;
    HRESULT hr;

    *out = NULL;
    if (!(impl = calloc(1, sizeof(*impl))))
        return E_OUTOFMEMORY;

    impl->IDataReader_iface.lpVtbl = &data_reader_vtbl;
    impl->IClosable_iface.lpVtbl = &data_reader_closable_vtbl;
    impl->ref = 1;
    impl->encoding = UnicodeEncoding_Utf8;
    impl->byte_order = ByteOrder_BigEndian;

    if (FAILED(hr = IBuffer_get_Length(buffer, &impl->length)) ||
        FAILED(hr = IBuffer_QueryInterface(buffer, &IID_IBufferByteAccess, (void **)&access)))
    {
        free(impl);
        return hr;
    }
    hr = IBufferByteAccess_Buffer(access, &impl->data);
    IBufferByteAccess_Release(access);
    if (FAILED(hr))
    {
        free(impl);
        return hr;
    }

    IBuffer_AddRef((impl->buffer = buffer));
    *out = &impl->IDataReader_iface;
    return S_OK;
}

struct data_reader_statics
{
    IActivationFactory IActivationFactory_iface;
    IDataReaderStatics IDataReaderStatics_iface;
    IDataReaderFactory IDataReaderFactory_iface;
    LONG ref;
};

static inline struct data_reader_statics *data_reader_statics_from_IActivationFactory(IActivationFactory *iface)
{
    return CONTAINING_RECORD(iface, struct data_reader_statics, IActivationFactory_iface);
}

static HRESULT WINAPI data_reader_factory_QueryInterface(IActivationFactory *iface, REFIID iid, void **out)
{
    struct data_reader_statics *impl = data_reader_statics_from_IActivationFactory(iface);

    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown)
            || IsEqualGUID(iid, &IID_IInspectable)
            || IsEqualGUID(iid, &IID_IAgileObject)
            || IsEqualGUID(iid, &IID_IActivationFactory))
    {
        IActivationFactory_AddRef((*out = &impl->IActivationFactory_iface));
        return S_OK;
    }

    if (IsEqualGUID(iid, &IID_IDataReaderStatics))
    {
        IActivationFactory_AddRef(iface);
        *out = &impl->IDataReaderStatics_iface;
        return S_OK;
    }

    if (IsEqualGUID(iid, &IID_IDataReaderFactory))
    {
        IActivationFactory_AddRef(iface);
        *out = &impl->IDataReaderFactory_iface;
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI data_reader_factory_AddRef(IActivationFactory *iface)
{
    struct data_reader_statics *impl = data_reader_statics_from_IActivationFactory(iface);
    ULONG ref = InterlockedIncrement(&impl->ref);
    TRACE("iface %p, ref %lu.\n", iface, ref);
    return ref;
}

static ULONG WINAPI data_reader_factory_Release(IActivationFactory *iface)
{
    struct data_reader_statics *impl = data_reader_statics_from_IActivationFactory(iface);
    ULONG ref = InterlockedDecrement(&impl->ref);
    TRACE("iface %p, ref %lu.\n", iface, ref);
    return ref;
}

static HRESULT WINAPI data_reader_factory_GetIids(IActivationFactory *iface, ULONG *iid_count, IID **iids)
{
    FIXME("iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids);
    return E_NOTIMPL;
}

static HRESULT WINAPI data_reader_factory_GetRuntimeClassName(IActivationFactory *iface, HSTRING *class_name)
{
    FIXME("iface %p, class_name %p stub!\n", iface, class_name);
    return E_NOTIMPL;
}

static HRESULT WINAPI data_reader_factory_GetTrustLevel(IActivationFactory *iface, TrustLevel *trust_level)
{
    FIXME("iface %p, trust_level %p stub!\n", iface, trust_level);
    return E_NOTIMPL;
}

static HRESULT WINAPI data_reader_factory_ActivateInstance(IActivationFactory *iface, IInspectable **instance)
{
    FIXME("iface %p, instance %p stub!\n", iface, instance);
    return E_NOTIMPL;
}

static const struct IActivationFactoryVtbl data_reader_factory_vtbl =
{
    data_reader_factory_QueryInterface,
    data_reader_factory_AddRef,
    data_reader_factory_Release,
    /* IInspectable methods */
    data_reader_factory_GetIids,
    data_reader_factory_GetRuntimeClassName,
    data_reader_factory_GetTrustLevel,
    /* IActivationFactory methods */
    data_reader_factory_ActivateInstance,
};

DEFINE_IINSPECTABLE(data_reader_statics, IDataReaderStatics, struct data_reader_statics, IActivationFactory_iface)

static HRESULT WINAPI data_reader_statics_FromBuffer(IDataReaderStatics *iface, IBuffer *buffer, IDataReader **data_reader)
{
    TRACE("iface %p, buffer %p, data_reader %p.\n", iface, buffer, data_reader);

    if (!data_reader) return E_POINTER;
    if (!buffer)
    {
        *data_reader = NULL;
        return E_INVALIDARG;
    }
    return data_reader_create(buffer, data_reader);
}

static const struct IDataReaderStaticsVtbl data_reader_statics_vtbl =
{
    data_reader_statics_QueryInterface,
    data_reader_statics_AddRef,
    data_reader_statics_Release,
    /* IInspectable methods */
    data_reader_statics_GetIids,
    data_reader_statics_GetRuntimeClassName,
    data_reader_statics_GetTrustLevel,
    /* IDataReaderStatics methods */
    data_reader_statics_FromBuffer,
};

DEFINE_IINSPECTABLE(data_reader_factory2, IDataReaderFactory, struct data_reader_statics, IActivationFactory_iface)

static HRESULT WINAPI data_reader_factory2_CreateDataReader(IDataReaderFactory *iface, IInputStream *input_stream,
        IDataReader **data_reader)
{
    FIXME("iface %p, input_stream %p, data_reader %p stub!\n", iface, input_stream, data_reader);
    return E_NOTIMPL;
}

static const struct IDataReaderFactoryVtbl data_reader_factory2_vtbl =
{
    data_reader_factory2_QueryInterface,
    data_reader_factory2_AddRef,
    data_reader_factory2_Release,
    /* IInspectable methods */
    data_reader_factory2_GetIids,
    data_reader_factory2_GetRuntimeClassName,
    data_reader_factory2_GetTrustLevel,
    /* IDataReaderFactory methods */
    data_reader_factory2_CreateDataReader,
};

static struct data_reader_statics data_reader_statics =
{
    {&data_reader_factory_vtbl},
    {&data_reader_statics_vtbl},
    {&data_reader_factory2_vtbl},
    1
};

IActivationFactory *data_reader_activation_factory = &data_reader_statics.IActivationFactory_iface;
