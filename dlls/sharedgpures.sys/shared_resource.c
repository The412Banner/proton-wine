/*
 * Shared GPU resource metadata, for the sharing protocol of earlier Proton releases.
 *
 * Proton 9 and 10 shared Vulkan memory between D3D devices through this driver: winevulkan
 * created a resource here, the client's handle was a handle to this device, and D3D runtimes
 * (DXVK before 3.0) kept the texture's description with it through the SET/GET_METADATA
 * controls - D3D9 writes it, D3D11's OpenSharedResource reads it back. Wine 11 shares memory
 * through D3DKMT objects instead (win32u), where a KMT handle is a global D3DKMT handle, and
 * those runtimes could no longer reach this device: the description was never written, and a
 * D3D9-rendered video never reached its D3D11 consumer (Ninja Gaiden Sigma, DXVK 2.4.1: the
 * game skips its cutscenes).
 *
 * This driver keeps that protocol working on top of the new sharing: a resource is identified
 * by the KMT handle (or name) the runtime opens it with, and only the description is kept
 * here; the memory itself is imported by the runtime through win32u, which takes the same KMT
 * handle. Nothing holds a description's resource alive on its behalf - the writer closes its
 * device handle before the reader opens its own - so a description outlives the handles that
 * made it; a resource whose KMT handle is reused writes a new description before any reader
 * asks for it.
 *
 * Copyright 2021 Derek Lesho (the original driver)
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

#define NONAMELESSUNION
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "winioctl.h"

#include "ddk/wdm.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(sharedgpures);

static DRIVER_OBJECT *sharedgpures_driver;

struct shared_resource
{
    unsigned int kmt_handle;   /* the D3DKMT global handle the resource is opened by, or 0 */
    WCHAR *name;               /* the name it is opened by instead, or NULL */
    void *metadata;
    SIZE_T metadata_size;
    unsigned int open_count;
};

static struct shared_resource *resource_pool;
static unsigned int resource_pool_size;
static unsigned int resource_count;

/* An entry for a KMT handle or a name: the existing one, or a new one. Entries are never
 * freed (see the top of the file); a few dozen bytes each, one per shared texture ever made. */
static struct shared_resource *lookup_or_add_resource(unsigned int kmt_handle, const WCHAR *name)
{
    struct shared_resource *res;
    unsigned int i;

    for (i = 0; i < resource_count; i++)
    {
        res = &resource_pool[i];
        if (kmt_handle ? res->kmt_handle == kmt_handle : (res->name && name && !wcscmp(res->name, name)))
            return res;
    }

    if (resource_count == resource_pool_size)
    {
        unsigned int new_size = resource_pool_size + 256;
        struct shared_resource *expanded = ExAllocatePoolWithTag(NonPagedPool, new_size * sizeof(*expanded), 0);

        if (!expanded)
            return NULL;
        if (resource_pool)
        {
            memcpy(expanded, resource_pool, resource_pool_size * sizeof(*expanded));
            ExFreePoolWithTag(resource_pool, 0);
        }
        memset(&expanded[resource_pool_size], 0, 256 * sizeof(*expanded));
        resource_pool = expanded;
        resource_pool_size = new_size;
    }

    res = &resource_pool[resource_count];
    memset(res, 0, sizeof(*res));
    res->kmt_handle = kmt_handle;
    if (name)
    {
        SIZE_T len = (wcslen(name) + 1) * sizeof(WCHAR);
        if (!(res->name = ExAllocatePoolWithTag(NonPagedPool, len, 0)))
            return NULL;
        memcpy(res->name, name, len);
    }
    resource_count++;
    TRACE("New resource %p: kmt handle %#x, name %s.\n", res, kmt_handle, debugstr_w(name));
    return res;
}

/* The file object's resource: FsContext holds the pool index + 1, 0 for a handle that has
 * not opened one. */
static struct shared_resource *file_resource(FILE_OBJECT *file)
{
    UINT_PTR idx = (UINT_PTR)file->FsContext;

    if (!idx || idx > resource_count)
        return NULL;
    return &resource_pool[idx - 1];
}

/* winevulkan's part of the earlier protocol: the memory is no longer registered here. */
#define IOCTL_SHARED_GPU_RESOURCE_CREATE           CTL_CODE(FILE_DEVICE_VIDEO, 0, METHOD_BUFFERED, FILE_WRITE_ACCESS)

#define IOCTL_SHARED_GPU_RESOURCE_OPEN             CTL_CODE(FILE_DEVICE_VIDEO, 1, METHOD_BUFFERED, FILE_WRITE_ACCESS)

struct shared_resource_open
{
    unsigned int kmt_handle;
    WCHAR name[1];
};

static NTSTATUS shared_resource_open(FILE_OBJECT *file, void *buff, SIZE_T insize, IO_STATUS_BLOCK *iosb)
{
    struct shared_resource_open *input = buff;
    struct shared_resource *res;
    const WCHAR *name = NULL;

    if (insize < sizeof(*input))
        return STATUS_INFO_LENGTH_MISMATCH;

    if (!input->kmt_handle)
    {
        SIZE_T chars = (insize - offsetof(struct shared_resource_open, name)) / sizeof(WCHAR);

        if (!chars || input->name[chars - 1] || !input->name[0])
            return STATUS_INVALID_PARAMETER;
        name = input->name;
    }

    if (!(res = lookup_or_add_resource(input->kmt_handle, name)))
        return STATUS_NO_MEMORY;

    res->open_count++;
    file->FsContext = (void *)(UINT_PTR)((res - resource_pool) + 1);
    iosb->Information = 0;
    return STATUS_SUCCESS;
}

#define IOCTL_SHARED_GPU_RESOURCE_GETKMT           CTL_CODE(FILE_DEVICE_VIDEO, 2, METHOD_BUFFERED, FILE_READ_ACCESS)

static NTSTATUS shared_resource_getkmt(struct shared_resource *res, void *buff, SIZE_T outsize, IO_STATUS_BLOCK *iosb)
{
    if (outsize < sizeof(unsigned int))
        return STATUS_INFO_LENGTH_MISMATCH;
    if (!res->kmt_handle)
        return STATUS_NOT_FOUND;

    *((unsigned int *)buff) = res->kmt_handle;
    iosb->Information = sizeof(unsigned int);
    return STATUS_SUCCESS;
}

#define IOCTL_SHARED_GPU_RESOURCE_SET_METADATA           CTL_CODE(FILE_DEVICE_VIDEO, 4, METHOD_BUFFERED, FILE_WRITE_ACCESS)

static NTSTATUS shared_resource_set_metadata(struct shared_resource *res, void *buff, SIZE_T insize, IO_STATUS_BLOCK *iosb)
{
    void *metadata;

    if (!insize)
        return STATUS_INVALID_PARAMETER;
    if (!(metadata = ExAllocatePoolWithTag(NonPagedPool, insize, 0)))
        return STATUS_NO_MEMORY;
    memcpy(metadata, buff, insize);

    if (res->metadata)
        ExFreePoolWithTag(res->metadata, 0);
    res->metadata = metadata;
    res->metadata_size = insize;

    iosb->Information = 0;
    return STATUS_SUCCESS;
}

#define IOCTL_SHARED_GPU_RESOURCE_GET_METADATA           CTL_CODE(FILE_DEVICE_VIDEO, 5, METHOD_BUFFERED, FILE_READ_ACCESS)

static NTSTATUS shared_resource_get_metadata(struct shared_resource *res, void *buff, SIZE_T outsize, IO_STATUS_BLOCK *iosb)
{
    if (!res->metadata)
        return STATUS_NOT_FOUND;
    if (res->metadata_size > outsize)
        return STATUS_BUFFER_TOO_SMALL;

    memcpy(buff, res->metadata, res->metadata_size);
    iosb->Information = res->metadata_size;
    return STATUS_SUCCESS;
}

static NTSTATUS WINAPI dispatch_create(DEVICE_OBJECT *device, IRP *irp)
{
    IO_STACK_LOCATION *stack = IoGetCurrentIrpStackLocation(irp);

    stack->FileObject->FsContext = NULL;
    irp->IoStatus.u.Status = STATUS_SUCCESS;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS WINAPI dispatch_close(DEVICE_OBJECT *device, IRP *irp)
{
    IO_STACK_LOCATION *stack = IoGetCurrentIrpStackLocation(irp);
    struct shared_resource *res = file_resource(stack->FileObject);

    if (res)
    {
        TRACE("Closing shared resource %p (kmt handle %#x).\n", res, res->kmt_handle);
        res->open_count--;
    }

    irp->IoStatus.u.Status = STATUS_SUCCESS;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS WINAPI dispatch_ioctl(DEVICE_OBJECT *device, IRP *irp)
{
    IO_STACK_LOCATION *stack = IoGetCurrentIrpStackLocation(irp);
    ULONG code = stack->Parameters.DeviceIoControl.IoControlCode;
    struct shared_resource *res = file_resource(stack->FileObject);
    NTSTATUS status;

    TRACE("ioctl %#lx insize %lu outsize %lu\n", code,
          stack->Parameters.DeviceIoControl.InputBufferLength,
          stack->Parameters.DeviceIoControl.OutputBufferLength);

    if (code == IOCTL_SHARED_GPU_RESOURCE_OPEN)
        status = shared_resource_open(stack->FileObject, irp->AssociatedIrp.SystemBuffer,
                                      stack->Parameters.DeviceIoControl.InputBufferLength, &irp->IoStatus);
    else if (!res)
    {
        WARN("ioctl %#lx on a handle with no resource opened.\n", code);
        status = STATUS_INVALID_HANDLE;
    }
    else switch (code)
    {
        case IOCTL_SHARED_GPU_RESOURCE_GETKMT:
            status = shared_resource_getkmt(res, irp->AssociatedIrp.SystemBuffer,
                                            stack->Parameters.DeviceIoControl.OutputBufferLength, &irp->IoStatus);
            break;
        case IOCTL_SHARED_GPU_RESOURCE_SET_METADATA:
            status = shared_resource_set_metadata(res, irp->AssociatedIrp.SystemBuffer,
                                                  stack->Parameters.DeviceIoControl.InputBufferLength, &irp->IoStatus);
            break;
        case IOCTL_SHARED_GPU_RESOURCE_GET_METADATA:
            status = shared_resource_get_metadata(res, irp->AssociatedIrp.SystemBuffer,
                                                  stack->Parameters.DeviceIoControl.OutputBufferLength, &irp->IoStatus);
            break;
        case IOCTL_SHARED_GPU_RESOURCE_CREATE:
        default:
            FIXME("ioctl %#lx not supported\n", code);
            status = STATUS_NOT_SUPPORTED;
            break;
    }

    irp->IoStatus.u.Status = status;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}

NTSTATUS WINAPI DriverEntry(DRIVER_OBJECT *driver, UNICODE_STRING *path)
{
    static const WCHAR device_nameW[] = L"\\Device\\SharedGpuResource";
    static const WCHAR link_nameW[] = L"\\??\\SharedGpuResource";
    UNICODE_STRING device_name, link_name;
    DEVICE_OBJECT *device;
    NTSTATUS status;

    sharedgpures_driver = driver;

    driver->MajorFunction[IRP_MJ_CREATE] = dispatch_create;
    driver->MajorFunction[IRP_MJ_CLOSE] = dispatch_close;
    driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = dispatch_ioctl;

    RtlInitUnicodeString(&device_name, device_nameW);
    RtlInitUnicodeString(&link_name, link_nameW);

    if ((status = IoCreateDevice(driver, 0, &device_name, 0, 0, FALSE, &device)))
        return status;

    return IoCreateSymbolicLink(&link_name, &device_name);
}
