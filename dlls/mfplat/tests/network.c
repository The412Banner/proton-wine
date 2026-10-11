/*
 * Media Foundation network source resolver tests
 *
 * Copyright 2026 GloriousEggroll
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
 */

#define COBJMACROS
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "winsock2.h"
#include "mfapi.h"
#include "mfidl.h"
#include "wine/test.h"

struct network_test
{
    IMFAsyncCallback IMFAsyncCallback_iface;
    LONG refcount;
    IMFSourceResolver *resolver;
    HANDLE reply, complete;
    SOCKET listener;
    BOOL rtsp, blocked_begin;
    char request[2048];
};

static struct network_test *impl_from_IMFAsyncCallback(IMFAsyncCallback *iface)
{
    return CONTAINING_RECORD(iface, struct network_test, IMFAsyncCallback_iface);
}

static HRESULT WINAPI callback_QueryInterface(IMFAsyncCallback *iface, REFIID iid, void **out)
{
    *out = NULL;
    if (!IsEqualIID(iid, &IID_IUnknown) && !IsEqualIID(iid, &IID_IMFAsyncCallback))
        return E_NOINTERFACE;
    *out = iface;
    IMFAsyncCallback_AddRef(iface);
    return S_OK;
}

static ULONG WINAPI callback_AddRef(IMFAsyncCallback *iface)
{
    return InterlockedIncrement(&impl_from_IMFAsyncCallback(iface)->refcount);
}

static ULONG WINAPI callback_Release(IMFAsyncCallback *iface)
{
    struct network_test *test = impl_from_IMFAsyncCallback(iface);
    ULONG refcount = InterlockedDecrement(&test->refcount);

    if (!refcount)
    {
        IMFSourceResolver_Release(test->resolver);
        closesocket(test->listener);
        CloseHandle(test->reply);
        CloseHandle(test->complete);
        free(test);
    }
    return refcount;
}

static HRESULT WINAPI callback_GetParameters(IMFAsyncCallback *iface, DWORD *flags, DWORD *queue)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI callback_Invoke(IMFAsyncCallback *iface, IMFAsyncResult *result)
{
    struct network_test *test = impl_from_IMFAsyncCallback(iface);
    IUnknown *object = NULL;
    MF_OBJECT_TYPE type;
    HRESULT hr;

    hr = IMFSourceResolver_EndCreateObjectFromURL(test->resolver, result, &type, &object);
    ok(FAILED(hr), "Expected a failed network request, got %#lx.\n", hr);
    if (object) IUnknown_Release(object);
    SetEvent(test->complete);
    return S_OK;
}

static const IMFAsyncCallbackVtbl callback_vtbl =
{
    callback_QueryInterface,
    callback_AddRef,
    callback_Release,
    callback_GetParameters,
    callback_Invoke,
};

static DWORD WINAPI server_thread(void *arg)
{
    static const char http_reply[] = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    static const char rtsp_reply[] = "RTSP/1.0 404 Not Found\r\nCSeq: 1\r\nContent-Length: 0\r\n\r\n";
    struct network_test *test = arg;
    struct timeval timeout = {5, 0};
    DWORD receive_timeout = 3000;
    SOCKET client;
    fd_set sockets;
    int size = 0, ret;

    FD_ZERO(&sockets);
    FD_SET(test->listener, &sockets);
    if (select(0, &sockets, NULL, NULL, &timeout) != 1) goto done;
    if ((client = accept(test->listener, NULL, NULL)) == INVALID_SOCKET) goto done;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (char *)&receive_timeout, sizeof(receive_timeout));
    while (size < sizeof(test->request) - 1)
    {
        if ((ret = recv(client, test->request + size, sizeof(test->request) - 1 - size, 0)) <= 0) break;
        size += ret;
        test->request[size] = 0;
        if (strstr(test->request, "\r\n\r\n")) break;
    }

    /* Do not answer until BeginCreateObjectFromURL has returned. The timeout
     * also lets a broken synchronous implementation finish instead of hanging. */
    test->blocked_begin = WaitForSingleObject(test->reply, 2000) != WAIT_OBJECT_0;
    if (test->rtsp) send(client, rtsp_reply, sizeof(rtsp_reply) - 1, 0);
    else send(client, http_reply, sizeof(http_reply) - 1, 0);
    shutdown(client, SD_BOTH);
    closesocket(client);
done:
    IMFAsyncCallback_Release(&test->IMFAsyncCallback_iface);
    return 0;
}

static void test_network_url(const WCHAR *scheme, const WCHAR *path, BOOL rtsp)
{
    struct sockaddr_in address = {0};
    struct network_test *test;
    int size = sizeof(address), ret;
    WCHAR url[256];
    HANDLE thread;
    DWORD wait;
    HRESULT hr;

    winetest_push_context("%s%s", wine_dbgstr_w(scheme), wine_dbgstr_w(path));
    test = calloc(1, sizeof(*test));
    test->IMFAsyncCallback_iface.lpVtbl = &callback_vtbl;
    test->refcount = 1;
    test->rtsp = rtsp;
    test->listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(test->listener != INVALID_SOCKET, "Failed to create socket, error %d.\n", WSAGetLastError());
    if (test->listener == INVALID_SOCKET)
    {
        free(test);
        goto done;
    }
    test->reply = CreateEventW(NULL, TRUE, FALSE, NULL);
    test->complete = CreateEventW(NULL, TRUE, FALSE, NULL);
    hr = MFCreateSourceResolver(&test->resolver);
    ok(hr == S_OK, "Failed to create resolver, hr %#lx.\n", hr);
    if (FAILED(hr))
    {
        closesocket(test->listener);
        CloseHandle(test->reply);
        CloseHandle(test->complete);
        free(test);
        goto done;
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ret = bind(test->listener, (struct sockaddr *)&address, sizeof(address));
    ok(!ret, "Failed to bind, error %d.\n", WSAGetLastError());
    if (ret) goto release;
    ret = getsockname(test->listener, (struct sockaddr *)&address, &size);
    ok(!ret, "Failed to get socket address, error %d.\n", WSAGetLastError());
    if (ret) goto release;
    ret = listen(test->listener, 1);
    ok(!ret, "Failed to listen, error %d.\n", WSAGetLastError());
    if (ret) goto release;

    swprintf(url, ARRAY_SIZE(url), L"%s://127.0.0.1:%u%s", scheme, ntohs(address.sin_port), path);
    IMFAsyncCallback_AddRef(&test->IMFAsyncCallback_iface);
    thread = CreateThread(NULL, 0, server_thread, test, 0, NULL);
    ok(!!thread, "Failed to create server thread.\n");
    if (!thread)
    {
        IMFAsyncCallback_Release(&test->IMFAsyncCallback_iface);
        goto release;
    }
    hr = IMFSourceResolver_BeginCreateObjectFromURL(test->resolver, url, MF_RESOLUTION_MEDIASOURCE,
            NULL, NULL, &test->IMFAsyncCallback_iface, NULL);
    SetEvent(test->reply);
    ok(hr == S_OK, "BeginCreateObjectFromURL failed, hr %#lx.\n", hr);
    wait = WaitForSingleObject(thread, 10000);
    ok(wait == WAIT_OBJECT_0, "Server thread did not finish, wait %#lx.\n", wait);
    if (wait == WAIT_OBJECT_0)
    {
        ok(!test->blocked_begin, "BeginCreateObjectFromURL waited for a network response.\n");
        ok(!!test->request[0], "No request reached the server.\n");
        if (rtsp)
            ok(!!strstr(test->request, " rtsp://127.0.0.1:"), "Unexpected RTSP request %s.\n", test->request);
        else
            ok(!!strstr(test->request, "HTTP/1."), "Unexpected HTTP request %s.\n", test->request);
    }
    CloseHandle(thread);
    if (SUCCEEDED(hr))
    {
        wait = WaitForSingleObject(test->complete, 10000);
        ok(wait == WAIT_OBJECT_0, "Missing completion callback, wait %#lx.\n", wait);
    }
release:
    IMFAsyncCallback_Release(&test->IMFAsyncCallback_iface);
done:
    winetest_pop_context();
}

START_TEST(network)
{
    WSADATA data;
    HRESULT hr;
    int ret;

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    ret = WSAStartup(MAKEWORD(2, 2), &data);
    ok(!ret, "WSAStartup failed, error %d.\n", ret);
    hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    ok(hr == S_OK, "MFStartup failed, hr %#lx.\n", hr);
    if (!ret && SUCCEEDED(hr))
    {
        test_network_url(L"http", L"/live.ts", FALSE);
        test_network_url(L"HTTP", L"/live", FALSE);
        test_network_url(L"http", L"/movie.mkv", FALSE);
        test_network_url(L"http", L"/movie.mp4", FALSE);
        test_network_url(L"http", L"/live.m3u8", FALSE);
        test_network_url(L"rtsp", L"/live", TRUE);
        test_network_url(L"rtspt", L"/live", TRUE);
        test_network_url(L"RTSPT", L"/live", TRUE);
        test_network_url(L"rtspu", L"/live", TRUE);
    }
    if (SUCCEEDED(hr)) MFShutdown();
    if (!ret) WSACleanup();
    CoUninitialize();
}
