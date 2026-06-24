/*
 * DMO wrapper filter
 *
 * Copyright (C) 2019 Zebediah Figura
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

#include "qasf_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(quartz);

struct buffer
{
    IMediaBuffer IMediaBuffer_iface;
    IMediaSample *sample;
    DWORD len;
};

struct dmo_wrapper_source
{
    struct strmbase_source pin;
    IQualityControl IQualityControl_iface;
    IQualityControl *qc_sink;
    struct buffer buffer;
    struct strmbase_passthrough passthrough;
    BOOL send_media_type;
    BOOL send_discontinuity;
};

struct dmo_wrapper
{
    struct strmbase_filter filter;
    IDMOWrapperFilter IDMOWrapperFilter_iface;
    IAMFilterMiscFlags IAMFilterMiscFlags_iface;

    IUnknown *dmo;

    DWORD sink_count, source_count;
    struct strmbase_sink *sinks;
    struct dmo_wrapper_source *sources;
    DMO_OUTPUT_DATA_BUFFER *buffers;
    struct buffer input_buffer;
};

static struct buffer *impl_from_IMediaBuffer(IMediaBuffer *iface)
{
    return CONTAINING_RECORD(iface, struct buffer, IMediaBuffer_iface);
}

static HRESULT WINAPI buffer_QueryInterface(IMediaBuffer *iface, REFIID iid, void **out)
{
    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IMediaBuffer))
    {
        IMediaBuffer_AddRef(iface);
        *out = iface;
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI buffer_AddRef(IMediaBuffer *iface)
{
    TRACE("iface %p.\n", iface);
    return 2;
}

static ULONG WINAPI buffer_Release(IMediaBuffer *iface)
{
    TRACE("iface %p.\n", iface);
    return 1;
}

static HRESULT WINAPI buffer_SetLength(IMediaBuffer *iface, DWORD len)
{
    struct buffer *buffer = impl_from_IMediaBuffer(iface);

    TRACE("iface %p, len %lu.\n", iface, len);

    buffer->len = len;
    return S_OK;
}

static HRESULT WINAPI buffer_GetMaxLength(IMediaBuffer *iface, DWORD *len)
{
    struct buffer *buffer = impl_from_IMediaBuffer(iface);

    TRACE("iface %p, len %p.\n", iface, len);

    *len = IMediaSample_GetSize(buffer->sample);
    return S_OK;
}

static HRESULT WINAPI buffer_GetBufferAndLength(IMediaBuffer *iface, BYTE **data, DWORD *len)
{
    struct buffer *buffer = impl_from_IMediaBuffer(iface);

    TRACE("iface %p, data %p, len %p.\n", iface, data, len);

    if (len)
        *len = buffer->len;
    if (data)
        return IMediaSample_GetPointer(buffer->sample, data);
    return S_OK;
}

static const IMediaBufferVtbl buffer_vtbl =
{
    buffer_QueryInterface,
    buffer_AddRef,
    buffer_Release,
    buffer_SetLength,
    buffer_GetMaxLength,
    buffer_GetBufferAndLength,
};

static inline struct dmo_wrapper *impl_from_strmbase_filter(struct strmbase_filter *iface)
{
    return CONTAINING_RECORD(iface, struct dmo_wrapper, filter);
}

static inline struct strmbase_sink *impl_sink_from_strmbase_pin(struct strmbase_pin *iface)
{
    return CONTAINING_RECORD(iface, struct strmbase_sink, pin);
}

static HRESULT dmo_wrapper_sink_query_interface(struct strmbase_pin *iface, REFIID iid, void **out)
{
    struct strmbase_sink *sink = impl_sink_from_strmbase_pin(iface);

    if (IsEqualGUID(iid, &IID_IMemInputPin))
        *out = &sink->IMemInputPin_iface;
    else
        return E_NOINTERFACE;

    IUnknown_AddRef((IUnknown *)*out);
    return S_OK;
}

static HRESULT dmo_wrapper_sink_query_accept(struct strmbase_pin *iface, const AM_MEDIA_TYPE *mt)
{
    struct dmo_wrapper *filter = impl_from_strmbase_filter(iface->filter);
    IMediaObject *dmo;
    HRESULT hr;

    IUnknown_QueryInterface(filter->dmo, &IID_IMediaObject, (void **)&dmo);

    hr = IMediaObject_SetInputType(dmo, impl_sink_from_strmbase_pin(iface) - filter->sinks,
            (const DMO_MEDIA_TYPE *)mt, DMO_SET_TYPEF_TEST_ONLY);

    IMediaObject_Release(dmo);

    return hr;
}

static HRESULT dmo_wrapper_sink_get_media_type(struct strmbase_pin *iface, unsigned int index, AM_MEDIA_TYPE *mt)
{
    struct dmo_wrapper *filter = impl_from_strmbase_filter(iface->filter);
    IMediaObject *dmo;
    HRESULT hr;

    IUnknown_QueryInterface(filter->dmo, &IID_IMediaObject, (void **)&dmo);

    hr = IMediaObject_GetInputType(dmo, impl_sink_from_strmbase_pin(iface) - filter->sinks,
            index, (DMO_MEDIA_TYPE *)mt);

    IMediaObject_Release(dmo);

    return hr == S_OK ? S_OK : VFW_S_NO_MORE_ITEMS;
}

static HRESULT dmo_wrapper_sink_connect(struct strmbase_sink *iface, IPin *peer, const AM_MEDIA_TYPE *mt)
{
    struct dmo_wrapper *filter = impl_from_strmbase_filter(iface->pin.filter);
    IMediaObject *dmo;
    HRESULT hr;

    IUnknown_QueryInterface(filter->dmo, &IID_IMediaObject, (void **)&dmo);

    hr = IMediaObject_SetInputType(dmo, iface - filter->sinks, (const DMO_MEDIA_TYPE *)mt, 0);

    IMediaObject_Release(dmo);
    return hr;
}

static void dmo_wrapper_sink_disconnect(struct strmbase_sink *iface)
{
    struct dmo_wrapper *filter = impl_from_strmbase_filter(iface->pin.filter);
    IMediaObject *dmo;

    IUnknown_QueryInterface(filter->dmo, &IID_IMediaObject, (void **)&dmo);

    IMediaObject_SetInputType(dmo, iface - filter->sinks, NULL, DMO_SET_TYPEF_CLEAR);

    IMediaObject_Release(dmo);
}

static void release_output_samples(struct dmo_wrapper *filter)
{
    DWORD i;

    for (i = 0; i < filter->source_count; ++i)
    {
        if (filter->sources[i].buffer.sample)
        {
            IMediaSample_Release(filter->sources[i].buffer.sample);
            filter->sources[i].buffer.sample = NULL;
        }
    }
}

static HRESULT get_output_samples(struct dmo_wrapper *filter)
{
    HRESULT hr;
    DWORD i;

    for (i = 0; i < filter->source_count; ++i)
    {
        if (filter->sources[i].pin.pin.peer)
        {
            if (FAILED(hr = IMemAllocator_GetBuffer(filter->sources[i].pin.pAllocator,
                    &filter->sources[i].buffer.sample, NULL, NULL, 0)))
            {
                ERR("Failed to get sample for source %lu, hr %#lx.\n", i, hr);
                release_output_samples(filter);
                return hr;
            }
            filter->buffers[i].pBuffer = &filter->sources[i].buffer.IMediaBuffer_iface;
            filter->sources[i].buffer.len = 0;
            IMediaSample_SetActualDataLength(filter->sources[i].buffer.sample, 0);
            IMediaSample_SetTime(filter->sources[i].buffer.sample, NULL, NULL);
            IMediaSample_SetSyncPoint(filter->sources[i].buffer.sample, FALSE);
            IMediaSample_SetDiscontinuity(filter->sources[i].buffer.sample, FALSE);
            IMediaSample_SetPreroll(filter->sources[i].buffer.sample, FALSE);
            IMediaSample_SetMediaTime(filter->sources[i].buffer.sample, NULL, NULL);
            IMediaSample_SetMediaType(filter->sources[i].buffer.sample, NULL);
        }
        else
            filter->buffers[i].pBuffer = NULL;
    }

    return S_OK;
}

static HRESULT process_output(struct dmo_wrapper *filter, IMediaObject *dmo)
{
    DMO_OUTPUT_DATA_BUFFER *buffers = filter->buffers;
    HRESULT hr = S_OK;
    DWORD status, i;
    BOOL got_output, audio_only;

    audio_only = TRUE;
    for (i = 0; i < filter->source_count; ++i)
    {
        if (filter->sources[i].pin.pin.peer
                && !IsEqualGUID(&filter->sources[i].pin.pin.mt.majortype, &MEDIATYPE_Audio))
        {
            audio_only = FALSE;
            break;
        }
    }

    do
    {
        if (FAILED(hr = get_output_samples(filter)))
            return hr;

        hr = IMediaObject_ProcessOutput(dmo, DMO_PROCESS_OUTPUT_DISCARD_WHEN_NO_BUFFER,
                filter->source_count, buffers, &status);
        if (hr != S_OK)
        {
            release_output_samples(filter);
            break;
        }

        got_output = FALSE;
        for (i = 0; i < filter->source_count; ++i)
        {
            IMediaSample *sample = filter->sources[i].buffer.sample;

            if (!buffers[i].pBuffer)
                continue;

            IMediaSample_SetActualDataLength(sample, filter->sources[i].buffer.len);

            if (IMediaSample_GetActualDataLength(sample))
                got_output = TRUE;

            if (buffers[i].dwStatus & DMO_OUTPUT_DATA_BUFFERF_TIME)
            {
                if (buffers[i].dwStatus & DMO_OUTPUT_DATA_BUFFERF_TIMELENGTH)
                {
                    REFERENCE_TIME stop = buffers[i].rtTimestamp + buffers[i].rtTimelength;
                    IMediaSample_SetTime(sample, &buffers[i].rtTimestamp, &stop);
                }
                else
                    IMediaSample_SetTime(sample, &buffers[i].rtTimestamp, NULL);
            }

            if (buffers[i].dwStatus & DMO_OUTPUT_DATA_BUFFERF_SYNCPOINT)
                IMediaSample_SetSyncPoint(sample, TRUE);

            if (buffers[i].dwStatus & DMO_OUTPUT_DATA_BUFFERF_DISCONTINUITY)
                IMediaSample_SetDiscontinuity(sample, TRUE);

            if (IMediaSample_GetActualDataLength(sample))
            {
                if (filter->sources[i].send_discontinuity)
                {
                    IMediaSample_SetDiscontinuity(sample, TRUE);
                    filter->sources[i].send_discontinuity = FALSE;
                }

                if (filter->sources[i].send_media_type)
                {
                    IMediaSample_SetMediaType(sample, &filter->sources[i].pin.pin.mt);
                    filter->sources[i].send_media_type = FALSE;
                }

                if ((hr = IMemInputPin_Receive(filter->sources[i].pin.pMemInputPin, sample)) != S_OK)
                {
                    WARN("Downstream sink returned %#lx.\n", hr);
                    release_output_samples(filter);
                    return hr;
                }
            }
        }

        release_output_samples(filter);

        if (audio_only && !got_output)
            break;
    } while (1);

    if (hr == S_FALSE)
        return S_OK;
    return hr;
}

static HRESULT WINAPI dmo_wrapper_sink_Receive(struct strmbase_sink *iface, IMediaSample *sample)
{
    struct dmo_wrapper *filter = impl_from_strmbase_filter(iface->pin.filter);
    DWORD index = iface - filter->sinks;
    REFERENCE_TIME start = 0, stop = 0;
    IMediaObject *dmo;
    DWORD flags = 0;
    HRESULT hr;

    if (filter->filter.state == State_Stopped)
        return VFW_E_WRONG_STATE;
    if (iface->flushing)
        return S_FALSE;

    IUnknown_QueryInterface(filter->dmo, &IID_IMediaObject, (void **)&dmo);

    if (IMediaSample_IsDiscontinuity(sample) == S_OK)
    {
        if (FAILED(hr = IMediaObject_Discontinuity(dmo, index)))
        {
            ERR("Discontinuity() failed, hr %#lx.\n", hr);
            goto out;
        }

        /* Calling Discontinuity() might change the DMO's mind about whether it
         * has more data to process. The DirectX documentation explicitly
         * states that we should call ProcessOutput() again in this case. */
        if ((hr = process_output(filter, dmo)) != S_OK)
            goto out;
    }

    if (IMediaSample_IsSyncPoint(sample) == S_OK)
        flags |= DMO_INPUT_DATA_BUFFERF_SYNCPOINT;

    if (SUCCEEDED(hr = IMediaSample_GetTime(sample, &start, &stop)))
    {
        flags |= DMO_INPUT_DATA_BUFFERF_TIME | DMO_INPUT_DATA_BUFFERF_TIMELENGTH;
        if (hr == VFW_S_NO_STOP_TIME)
            stop = start + 1;
    }

    filter->input_buffer.sample = sample;
    filter->input_buffer.len = IMediaSample_GetActualDataLength(sample);
    if (FAILED(hr = IMediaObject_ProcessInput(dmo, index,
            &filter->input_buffer.IMediaBuffer_iface, flags, start, stop - start)))
    {
        ERR("ProcessInput() failed, hr %#lx.\n", hr);
        goto out;
    }

    hr = process_output(filter, dmo);

out:
    filter->input_buffer.sample = NULL;
    IMediaObject_Release(dmo);
    return hr;
}

static HRESULT dmo_wrapper_sink_eos(struct strmbase_sink *iface)
{
    struct dmo_wrapper *filter = impl_from_strmbase_filter(iface->pin.filter);
    DWORD index = iface - filter->sinks, i;
    IMediaObject *dmo;
    HRESULT hr;

    IUnknown_QueryInterface(filter->dmo, &IID_IMediaObject, (void **)&dmo);

    if (FAILED(hr = IMediaObject_Discontinuity(dmo, index)))
        ERR("Discontinuity() failed, hr %#lx.\n", hr);

    process_output(filter, dmo);

    if (FAILED(hr = IMediaObject_Flush(dmo)))
        ERR("Flush() failed, hr %#lx.\n", hr);

    for (i = 0; i < filter->source_count; ++i)
    {
        if (filter->sources[i].pin.pin.peer)
            IPin_EndOfStream(filter->sources[i].pin.pin.peer);
    }

    IMediaObject_Release(dmo);
    return hr;
}

static HRESULT dmo_wrapper_begin_flush(struct strmbase_sink *iface)
{
    struct dmo_wrapper *filter = impl_from_strmbase_filter(iface->pin.filter);
    HRESULT hr = S_OK, ret;
    DWORD i;

    for (i = 0; i < filter->source_count; ++i)
    {
        if (filter->sources[i].pin.pin.peer
                && FAILED(ret = IPin_BeginFlush(filter->sources[i].pin.pin.peer))
                && SUCCEEDED(hr))
            hr = ret;
    }

    return hr;
}

static HRESULT dmo_wrapper_end_flush(struct strmbase_sink *iface)
{
    struct dmo_wrapper *filter = impl_from_strmbase_filter(iface->pin.filter);
    IMediaObject *dmo;
    HRESULT hr;
    DWORD i;

    IUnknown_QueryInterface(filter->dmo, &IID_IMediaObject, (void **)&dmo);

    if (FAILED(hr = IMediaObject_Flush(dmo)))
        ERR("Flush() failed, hr %#lx.\n", hr);

    for (i = 0; i < filter->source_count; ++i)
    {
        if (filter->sources[i].pin.pin.peer)
            IPin_EndFlush(filter->sources[i].pin.pin.peer);
    }

    IMediaObject_Release(dmo);
    return hr;
}

static HRESULT dmo_wrapper_new_segment(struct strmbase_sink *iface,
        REFERENCE_TIME start, REFERENCE_TIME stop, double rate)
{
    struct dmo_wrapper *filter = impl_from_strmbase_filter(iface->pin.filter);
    HRESULT hr = S_OK, ret;
    DWORD i;

    TRACE("filter %p, start %s, stop %s, rate %.16e.\n",
            filter, debugstr_time(start), debugstr_time(stop), rate);

    for (i = 0; i < filter->source_count; ++i)
    {
        if (filter->sources[i].pin.pin.peer)
        {
            filter->sources[i].send_media_type = TRUE;
            filter->sources[i].send_discontinuity = TRUE;
            if (FAILED(ret = IPin_NewSegment(filter->sources[i].pin.pin.peer, start, stop, rate))
                    && SUCCEEDED(hr))
                hr = ret;
        }
    }

    return hr;
}

static const struct strmbase_sink_ops sink_ops =
{
    .base.pin_query_interface = dmo_wrapper_sink_query_interface,
    .base.pin_query_accept = dmo_wrapper_sink_query_accept,
    .base.pin_get_media_type = dmo_wrapper_sink_get_media_type,
    .sink_connect = dmo_wrapper_sink_connect,
    .sink_disconnect = dmo_wrapper_sink_disconnect,
    .sink_eos = dmo_wrapper_sink_eos,
    .sink_begin_flush = dmo_wrapper_begin_flush,
    .sink_end_flush = dmo_wrapper_end_flush,
    .sink_new_segment = dmo_wrapper_new_segment,
    .pfnReceive = dmo_wrapper_sink_Receive,
};

static inline struct dmo_wrapper_source *impl_source_from_strmbase_pin(struct strmbase_pin *iface)
{
    return CONTAINING_RECORD(iface, struct dmo_wrapper_source, pin.pin);
}

static struct dmo_wrapper_source *impl_source_from_IQualityControl(IQualityControl *iface)
{
    return CONTAINING_RECORD(iface, struct dmo_wrapper_source, IQualityControl_iface);
}

static HRESULT dmo_wrapper_source_query_interface(struct strmbase_pin *iface, REFIID iid, void **out)
{
    struct dmo_wrapper_source *pin = impl_source_from_strmbase_pin(iface);

    if (IsEqualGUID(iid, &IID_IMediaPosition))
        *out = &pin->passthrough.IMediaPosition_iface;
    else if (IsEqualGUID(iid, &IID_IMediaSeeking))
        *out = &pin->passthrough.IMediaSeeking_iface;
    else if (IsEqualGUID(iid, &IID_IQualityControl))
        *out = &pin->IQualityControl_iface;
    else
        return E_NOINTERFACE;

    IUnknown_AddRef((IUnknown *)*out);
    return S_OK;
}

static HRESULT WINAPI dmo_wrapper_source_qc_QueryInterface(IQualityControl *iface, REFIID iid, void **out)
{
    struct dmo_wrapper_source *pin = impl_source_from_IQualityControl(iface);
    return IPin_QueryInterface(&pin->pin.pin.IPin_iface, iid, out);
}

static ULONG WINAPI dmo_wrapper_source_qc_AddRef(IQualityControl *iface)
{
    struct dmo_wrapper_source *pin = impl_source_from_IQualityControl(iface);
    return IPin_AddRef(&pin->pin.pin.IPin_iface);
}

static ULONG WINAPI dmo_wrapper_source_qc_Release(IQualityControl *iface)
{
    struct dmo_wrapper_source *pin = impl_source_from_IQualityControl(iface);
    return IPin_Release(&pin->pin.pin.IPin_iface);
}

static HRESULT WINAPI dmo_wrapper_source_qc_Notify(IQualityControl *iface, IBaseFilter *sender, Quality q)
{
    struct dmo_wrapper_source *pin = impl_source_from_IQualityControl(iface);
    struct dmo_wrapper *filter = impl_from_strmbase_filter(pin->pin.pin.filter);
    IQualityControl *peer;
    HRESULT hr = S_OK;

    TRACE("filter %p, source %p, sender %p, type %#x, proportion %ld, late %s, timestamp %s.\n",
            filter, pin, sender, q.Type, q.Proportion, debugstr_time(q.Late), debugstr_time(q.TimeStamp));

    if (pin->qc_sink)
        return IQualityControl_Notify(pin->qc_sink, &filter->filter.IBaseFilter_iface, q);

    if (filter->sink_count && filter->sinks[0].pin.peer
            && SUCCEEDED(IPin_QueryInterface(filter->sinks[0].pin.peer, &IID_IQualityControl, (void **)&peer)))
    {
        hr = IQualityControl_Notify(peer, &filter->filter.IBaseFilter_iface, q);
        IQualityControl_Release(peer);
    }

    return hr;
}

static HRESULT WINAPI dmo_wrapper_source_qc_SetSink(IQualityControl *iface, IQualityControl *sink)
{
    struct dmo_wrapper_source *pin = impl_source_from_IQualityControl(iface);

    TRACE("source %p, sink %p.\n", pin, sink);

    pin->qc_sink = sink;
    return S_OK;
}

static const IQualityControlVtbl source_qc_vtbl =
{
    dmo_wrapper_source_qc_QueryInterface,
    dmo_wrapper_source_qc_AddRef,
    dmo_wrapper_source_qc_Release,
    dmo_wrapper_source_qc_Notify,
    dmo_wrapper_source_qc_SetSink,
};

static HRESULT dmo_wrapper_source_query_accept(struct strmbase_pin *iface, const AM_MEDIA_TYPE *mt)
{
    struct dmo_wrapper *filter = impl_from_strmbase_filter(iface->filter);
    DWORD index = impl_source_from_strmbase_pin(iface) - filter->sources;
    IMediaObject *dmo;
    HRESULT hr;

    IUnknown_QueryInterface(filter->dmo, &IID_IMediaObject, (void **)&dmo);

    TRACE("source %p %s:%s output %lu test accept type %p.\n", iface,
            debugstr_w(iface->filter->name), debugstr_w(iface->name), index, mt);
    strmbase_dump_media_type(mt);

    hr = IMediaObject_SetOutputType(dmo, index, (const DMO_MEDIA_TYPE *)mt, DMO_SET_TYPEF_TEST_ONLY);
    TRACE("source %p output %lu test SetOutputType returned %#lx.\n", iface, index, hr);

    IMediaObject_Release(dmo);

    return hr;
}

static HRESULT dmo_wrapper_source_get_media_type(struct strmbase_pin *iface, unsigned int index, AM_MEDIA_TYPE *mt)
{
    struct dmo_wrapper *filter = impl_from_strmbase_filter(iface->filter);
    IMediaObject *dmo;
    HRESULT hr;

    IUnknown_QueryInterface(filter->dmo, &IID_IMediaObject, (void **)&dmo);

    hr = IMediaObject_GetOutputType(dmo, impl_source_from_strmbase_pin(iface) - filter->sources,
            index, (DMO_MEDIA_TYPE *)mt);

    IMediaObject_Release(dmo);

    return hr == S_OK ? S_OK : VFW_S_NO_MORE_ITEMS;
}

static HRESULT WINAPI dmo_wrapper_source_DecideBufferSize(struct strmbase_source *iface,
        IMemAllocator *allocator, ALLOCATOR_PROPERTIES *props)
{
    struct dmo_wrapper *filter = impl_from_strmbase_filter(iface->pin.filter);
    DWORD index = impl_source_from_strmbase_pin(&iface->pin) - filter->sources;
    ALLOCATOR_PROPERTIES ret_props;
    DWORD size = 0, alignment = 0;
    IMediaObject *dmo;
    HRESULT hr;

    IUnknown_QueryInterface(filter->dmo, &IID_IMediaObject, (void **)&dmo);

    TRACE("source %p %s:%s output %lu decide buffer size, allocator %p, requested buffers %ld, size %ld, align %ld, prefix %ld.\n",
            iface, debugstr_w(iface->pin.filter->name), debugstr_w(iface->pin.name), index, allocator,
            props->cBuffers, props->cbBuffer, props->cbAlign, props->cbPrefix);
    strmbase_dump_media_type(&iface->pin.mt);

    hr = IMediaObject_SetOutputType(dmo, index, (const DMO_MEDIA_TYPE *)&iface->pin.mt, 0);
    TRACE("source %p output %lu SetOutputType returned %#lx.\n", iface, index, hr);
    if (SUCCEEDED(hr))
    {
        hr = IMediaObject_GetOutputSizeInfo(dmo, index, &size, &alignment);
        TRACE("source %p output %lu GetOutputSizeInfo returned %#lx, size %lu, alignment %lu.\n",
                iface, index, hr, size, alignment);
    }

    if (SUCCEEDED(hr))
    {
        props->cBuffers = max(props->cBuffers, 1);
        props->cbBuffer = max(max(props->cbBuffer, size), 16384);
        props->cbAlign = max(props->cbAlign, alignment);
        TRACE("source %p output %lu calling SetProperties with buffers %ld, size %ld, align %ld, prefix %ld.\n",
                iface, index, props->cBuffers, props->cbBuffer, props->cbAlign, props->cbPrefix);
        hr = IMemAllocator_SetProperties(allocator, props, &ret_props);
        TRACE("source %p output %lu SetProperties returned %#lx, actual buffers %ld, size %ld, align %ld, prefix %ld.\n",
                iface, index, hr, ret_props.cBuffers, ret_props.cbBuffer, ret_props.cbAlign, ret_props.cbPrefix);
    }

    IMediaObject_Release(dmo);

    return hr;
}

static void dmo_wrapper_source_disconnect(struct strmbase_source *iface)
{
    struct dmo_wrapper *filter = impl_from_strmbase_filter(iface->pin.filter);
    IMediaObject *dmo;

    IUnknown_QueryInterface(filter->dmo, &IID_IMediaObject, (void **)&dmo);

    IMediaObject_SetOutputType(dmo, impl_source_from_strmbase_pin(&iface->pin) - filter->sources,
            NULL, DMO_SET_TYPEF_CLEAR);

    IMediaObject_Release(dmo);
}

static const struct strmbase_source_ops source_ops =
{
    .base.pin_query_interface = dmo_wrapper_source_query_interface,
    .base.pin_query_accept = dmo_wrapper_source_query_accept,
    .base.pin_get_media_type = dmo_wrapper_source_get_media_type,
    .pfnAttemptConnection = BaseOutputPinImpl_AttemptConnection,
    .pfnDecideAllocator = BaseOutputPinImpl_DecideAllocator,
    .pfnDecideBufferSize = dmo_wrapper_source_DecideBufferSize,
    .source_disconnect = dmo_wrapper_source_disconnect,
};

static inline struct dmo_wrapper *impl_from_IDMOWrapperFilter(IDMOWrapperFilter *iface)
{
    return CONTAINING_RECORD(iface, struct dmo_wrapper, IDMOWrapperFilter_iface);
}

static HRESULT WINAPI dmo_wrapper_filter_QueryInterface(IDMOWrapperFilter *iface, REFIID iid, void **out)
{
    struct dmo_wrapper *filter = impl_from_IDMOWrapperFilter(iface);
    return IUnknown_QueryInterface(filter->filter.outer_unk, iid, out);
}

static ULONG WINAPI dmo_wrapper_filter_AddRef(IDMOWrapperFilter *iface)
{
    struct dmo_wrapper *filter = impl_from_IDMOWrapperFilter(iface);
    return IUnknown_AddRef(filter->filter.outer_unk);
}

static ULONG WINAPI dmo_wrapper_filter_Release(IDMOWrapperFilter *iface)
{
    struct dmo_wrapper *filter = impl_from_IDMOWrapperFilter(iface);
    return IUnknown_Release(filter->filter.outer_unk);
}

static HRESULT WINAPI dmo_wrapper_filter_Init(IDMOWrapperFilter *iface, REFCLSID clsid, REFCLSID category)
{
    struct dmo_wrapper *filter = impl_from_IDMOWrapperFilter(iface);
    struct dmo_wrapper_source *sources;
    DMO_OUTPUT_DATA_BUFFER *buffers;
    DWORD input_count, output_count;
    struct strmbase_sink *sinks;
    IMediaObject *dmo;
    IUnknown *unk;
    WCHAR id[14];
    HRESULT hr;
    DWORD i;

    TRACE("filter %p, clsid %s, category %s.\n", filter, debugstr_guid(clsid), debugstr_guid(category));

    if (FAILED(hr = CoCreateInstance(clsid, &filter->filter.IUnknown_inner,
            CLSCTX_INPROC_SERVER, &IID_IUnknown, (void **)&unk)))
        return hr;

    if (FAILED(hr = IUnknown_QueryInterface(unk, &IID_IMediaObject, (void **)&dmo)))
    {
        IUnknown_Release(unk);
        return hr;
    }

    if (FAILED(IMediaObject_GetStreamCount(dmo, &input_count, &output_count)))
        input_count = output_count = 0;

    sinks = calloc(sizeof(*sinks), input_count);
    sources = calloc(sizeof(*sources), output_count);
    buffers = calloc(sizeof(*buffers), output_count);
    if (!sinks || !sources || !buffers)
    {
        free(sinks);
        free(sources);
        free(buffers);
        IMediaObject_Release(dmo);
        IUnknown_Release(unk);
        return hr;
    }

    for (i = 0; i < input_count; ++i)
    {
        swprintf(id, ARRAY_SIZE(id), L"in%u", i);
        strmbase_sink_init(&sinks[i], &filter->filter, id, &sink_ops, NULL);
    }

    for (i = 0; i < output_count; ++i)
    {
        swprintf(id, ARRAY_SIZE(id), L"out%u", i);
        strmbase_source_init(&sources[i].pin, &filter->filter, id, &source_ops);
        sources[i].IQualityControl_iface.lpVtbl = &source_qc_vtbl;
        sources[i].buffer.IMediaBuffer_iface.lpVtbl = &buffer_vtbl;

        strmbase_passthrough_init(&sources[i].passthrough, (IUnknown *)&sources[i].pin.pin.IPin_iface);
        ISeekingPassThru_Init(&sources[i].passthrough.ISeekingPassThru_iface,
                FALSE, &sinks[0].pin.IPin_iface);
    }

    EnterCriticalSection(&filter->filter.filter_cs);

    filter->dmo = unk;
    filter->sink_count = input_count;
    filter->source_count = output_count;
    filter->sinks = sinks;
    filter->sources = sources;
    filter->buffers = buffers;

    LeaveCriticalSection(&filter->filter.filter_cs);

    IMediaObject_Release(dmo);

    return S_OK;
}

static const IDMOWrapperFilterVtbl dmo_wrapper_filter_vtbl =
{
    dmo_wrapper_filter_QueryInterface,
    dmo_wrapper_filter_AddRef,
    dmo_wrapper_filter_Release,
    dmo_wrapper_filter_Init,
};

static inline struct dmo_wrapper *impl_from_IAMFilterMiscFlags(IAMFilterMiscFlags *iface)
{
    return CONTAINING_RECORD(iface, struct dmo_wrapper, IAMFilterMiscFlags_iface);
}

static HRESULT WINAPI dmo_wrapper_misc_flags_QueryInterface(IAMFilterMiscFlags *iface, REFIID iid, void **out)
{
    struct dmo_wrapper *filter = impl_from_IAMFilterMiscFlags(iface);
    return IUnknown_QueryInterface(filter->filter.outer_unk, iid, out);
}

static ULONG WINAPI dmo_wrapper_misc_flags_AddRef(IAMFilterMiscFlags *iface)
{
    struct dmo_wrapper *filter = impl_from_IAMFilterMiscFlags(iface);
    return IUnknown_AddRef(filter->filter.outer_unk);
}

static ULONG WINAPI dmo_wrapper_misc_flags_Release(IAMFilterMiscFlags *iface)
{
    struct dmo_wrapper *filter = impl_from_IAMFilterMiscFlags(iface);
    return IUnknown_Release(filter->filter.outer_unk);
}

static ULONG WINAPI dmo_wrapper_misc_flags_GetMiscFlags(IAMFilterMiscFlags *iface)
{
    TRACE("iface %p.\n", iface);

    return 0;
}

static const IAMFilterMiscFlagsVtbl dmo_wrapper_misc_flags_vtbl =
{
    dmo_wrapper_misc_flags_QueryInterface,
    dmo_wrapper_misc_flags_AddRef,
    dmo_wrapper_misc_flags_Release,
    dmo_wrapper_misc_flags_GetMiscFlags,
};

static struct strmbase_pin *dmo_wrapper_get_pin(struct strmbase_filter *iface, unsigned int index)
{
    struct dmo_wrapper *filter = impl_from_strmbase_filter(iface);

    if (index < filter->sink_count)
        return &filter->sinks[index].pin;
    else if (index < filter->sink_count + filter->source_count)
        return &filter->sources[index - filter->sink_count].pin.pin;
    return NULL;
}

static void dmo_wrapper_destroy(struct strmbase_filter *iface)
{
    struct dmo_wrapper *filter = impl_from_strmbase_filter(iface);
    DWORD i;

    if (filter->dmo)
        IUnknown_Release(filter->dmo);
    for (i = 0; i < filter->sink_count; ++i)
        strmbase_sink_cleanup(&filter->sinks[i]);
    for (i = 0; i < filter->source_count; ++i)
    {
        strmbase_passthrough_cleanup(&filter->sources[i].passthrough);
        strmbase_source_cleanup(&filter->sources[i].pin);
    }
    free(filter->sinks);
    free(filter->sources);
    strmbase_filter_cleanup(&filter->filter);
    free(filter);
}

static HRESULT dmo_wrapper_query_interface(struct strmbase_filter *iface, REFIID iid, void **out)
{
    struct dmo_wrapper *filter = impl_from_strmbase_filter(iface);

    if (IsEqualGUID(iid, &IID_IDMOWrapperFilter))
    {
        *out = &filter->IDMOWrapperFilter_iface;
        IUnknown_AddRef((IUnknown *)*out);
        return S_OK;
    }
    else if (IsEqualGUID(iid, &IID_IAMFilterMiscFlags))
    {
        *out = &filter->IAMFilterMiscFlags_iface;
        IUnknown_AddRef((IUnknown *)*out);
        return S_OK;
    }

    if (filter->source_count && IsEqualGUID(iid, &IID_IMediaPosition))
    {
        *out = &filter->sources[0].passthrough.IMediaPosition_iface;
        IUnknown_AddRef((IUnknown *)*out);
        return S_OK;
    }
    else if (filter->source_count && IsEqualGUID(iid, &IID_IMediaSeeking))
    {
        *out = &filter->sources[0].passthrough.IMediaSeeking_iface;
        IUnknown_AddRef((IUnknown *)*out);
        return S_OK;
    }

    if (filter->dmo && !IsEqualGUID(iid, &IID_IUnknown))
        return IUnknown_QueryInterface(filter->dmo, iid, out);
    return E_NOINTERFACE;
}

static HRESULT dmo_wrapper_init_stream(struct strmbase_filter *iface)
{
    struct dmo_wrapper *filter = impl_from_strmbase_filter(iface);
    IMediaObject *dmo;
    HRESULT hr;
    DWORD i;

    if (!filter->dmo)
        return E_FAIL;

    IUnknown_QueryInterface(filter->dmo, &IID_IMediaObject, (void **)&dmo);

    hr = IMediaObject_AllocateStreamingResources(dmo);
    if (FAILED(hr) && hr != E_NOTIMPL)
    {
        IMediaObject_Release(dmo);
        return hr;
    }

    for (i = 0; i < filter->source_count; ++i)
    {
        if (filter->sources[i].pin.pin.peer)
            IMemAllocator_Commit(filter->sources[i].pin.pAllocator);
    }

    IMediaObject_Release(dmo);
    return S_OK;
}

static HRESULT dmo_wrapper_cleanup_stream(struct strmbase_filter *iface)
{
    struct dmo_wrapper *filter = impl_from_strmbase_filter(iface);
    IMediaObject *dmo;
    DWORD i;

    if (!filter->dmo)
        return E_FAIL;

    IUnknown_QueryInterface(filter->dmo, &IID_IMediaObject, (void **)&dmo);

    EnterCriticalSection(&filter->filter.stream_cs);
    for (i = 0; i < filter->source_count; ++i)
    {
        if (filter->sources[i].pin.pin.peer)
            IMemAllocator_Decommit(filter->sources[i].pin.pAllocator);
    }

    IMediaObject_Flush(dmo);
    IMediaObject_FreeStreamingResources(dmo);

    IMediaObject_Release(dmo);
    LeaveCriticalSection(&filter->filter.stream_cs);
    return S_OK;
}

static struct strmbase_filter_ops filter_ops =
{
    .filter_get_pin = dmo_wrapper_get_pin,
    .filter_destroy = dmo_wrapper_destroy,
    .filter_query_interface = dmo_wrapper_query_interface,
    .filter_init_stream = dmo_wrapper_init_stream,
    .filter_cleanup_stream = dmo_wrapper_cleanup_stream,
};

HRESULT dmo_wrapper_create(IUnknown *outer, IUnknown **out)
{
    struct dmo_wrapper *object;

    if (!(object = calloc(sizeof(*object), 1)))
        return E_OUTOFMEMORY;

    /* Always pass NULL as the outer object; see test_aggregation(). */
    strmbase_filter_init(&object->filter, NULL, &CLSID_DMOWrapperFilter, &filter_ops);

    object->IDMOWrapperFilter_iface.lpVtbl = &dmo_wrapper_filter_vtbl;
    object->IAMFilterMiscFlags_iface.lpVtbl = &dmo_wrapper_misc_flags_vtbl;

    object->input_buffer.IMediaBuffer_iface.lpVtbl = &buffer_vtbl;

    TRACE("Created DMO wrapper %p.\n", object);
    *out = &object->filter.IUnknown_inner;

    return S_OK;
}
