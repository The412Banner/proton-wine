/*
 * Bluetooth DualSense and DualShock 4 audio for winepipewire.drv
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

/* A Bluetooth Sony controller endpoint, for the device list. */
struct bt_endpoint
{
    const char *name;     /* device string */
    const char *display;  /* UTF-8 */
    EDataFlow flow;
    EndpointFormFactor form;
    UINT32 rate, channels, channel_mask;
    UINT16 vendor_id, product_id;
    GUID container_id;
};

typedef void (*bt_endpoint_cb)(const struct bt_endpoint *endpoint, void *context);

/* Bluetooth stream handles have the low bit set. */
static inline BOOL bt_is_stream_handle(stream_handle h)
{
    return h & 1;
}

extern void bt_enum_endpoints(bt_endpoint_cb callback, void *context);
extern unsigned int bt_hidraw_signature(void);
extern void bt_latch_windows_names(void);
extern BOOL bt_windows_audio_mode(void);
extern BOOL bt_use_windows_names(void);
extern BOOL bt_is_device(const char *device);
extern HRESULT bt_is_format_supported(const char *device, EDataFlow flow, const WAVEFORMATEX *fmt);

extern NTSTATUS bt_create_stream(struct create_stream_params *params);
extern NTSTATUS bt_release_stream(struct release_stream_params *params);
extern NTSTATUS bt_start(struct start_params *params);
extern NTSTATUS bt_stop(struct stop_params *params);
extern NTSTATUS bt_reset(struct reset_params *params);
extern NTSTATUS bt_timer_loop(struct timer_loop_params *params);
extern NTSTATUS bt_get_render_buffer(struct get_render_buffer_params *params);
extern NTSTATUS bt_release_render_buffer(struct release_render_buffer_params *params);
extern NTSTATUS bt_get_capture_buffer(struct get_capture_buffer_params *params);
extern NTSTATUS bt_release_capture_buffer(struct release_capture_buffer_params *params);
extern NTSTATUS bt_get_buffer_size(struct get_buffer_size_params *params);
extern NTSTATUS bt_get_latency(struct get_latency_params *params);
extern NTSTATUS bt_get_current_padding(struct get_current_padding_params *params);
extern NTSTATUS bt_get_next_packet_size(struct get_next_packet_size_params *params);
extern NTSTATUS bt_get_frequency(struct get_frequency_params *params);
extern NTSTATUS bt_get_position(struct get_position_params *params);
extern NTSTATUS bt_set_volumes(struct set_volumes_params *params);
extern NTSTATUS bt_set_event_handle(struct set_event_handle_params *params);
extern NTSTATUS bt_set_sample_rate(struct set_sample_rate_params *params);
extern NTSTATUS bt_is_started(struct is_started_params *params);
