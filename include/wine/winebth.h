/*
 * Wine-specific IOCTL definitions for interfacing with winebth.sys
 *
 * Copyright 2025 Vibhav Pant
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
 *
 */

#ifndef __WINEBTH_H__
#define __WINEBTH_H__

/* Set the discoverability or connectable flag for a local radio. Enabling discoverability will also enable incoming
 * connections, while disabling incoming connections disables discoverability as well. */
#define IOCTL_WINEBTH_RADIO_SET_FLAG           CTL_CODE(FILE_DEVICE_BLUETOOTH, 0xa3, METHOD_BUFFERED, FILE_ANY_ACCESS)
/* Start device inquiry for a local radio. */
#define IOCTL_WINEBTH_RADIO_START_DISCOVERY    CTL_CODE(FILE_DEVICE_BLUETOOTH, 0xa6, METHOD_BUFFERED, FILE_ANY_ACCESS)
/* Stop device inquiry for a local radio. */
#define IOCTL_WINEBTH_RADIO_STOP_DISCOVERY     CTL_CODE(FILE_DEVICE_BLUETOOTH, 0xa7, METHOD_BUFFERED, FILE_ANY_ACCESS)
/* Ask the system's Bluetooth service to send all incoming authentication requests to Wine. */
#define IOCTL_WINEBTH_AUTH_REGISTER            CTL_CODE(FILE_DEVICE_BLUETOOTH, 0xa8, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_WINEBTH_RADIO_SEND_AUTH_RESPONSE CTL_CODE(FILE_DEVICE_BLUETOOTH, 0xa9, METHOD_BUFFERED, FILE_ANY_ACCESS)
/* Initiate the authentication procedure with a remote device. */
#define IOCTL_WINEBTH_RADIO_START_AUTH         CTL_CODE(FILE_DEVICE_BLUETOOTH, 0xaa, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_WINEBTH_RADIO_REMOVE_DEVICE      CTL_CODE(FILE_DEVICE_BLUETOOTH, 0xab, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* Get all primary GATT services for the LE device. */
#define IOCTL_WINEBTH_LE_DEVICE_GET_GATT_SERVICES CTL_CODE(FILE_DEVICE_BLUETOOTH, 0xc0, METHOD_BUFFERED, FILE_ANY_ACCESS)
/* Get all characteristics for a GATT service */
#define IOCTL_WINEBTH_LE_DEVICE_GET_GATT_CHARACTERISTICS CTL_CODE(FILE_DEVICE_BLUETOOTH, 0xc1, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* Read the associated value for a GATT characteristic */
#define IOCTL_WINEBTH_GATT_SERVICE_READ_CHARACTERISITIC_VALUE CTL_CODE(FILE_DEVICE_BLUETOOTH, 0xd0, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* Write a GATT characteristic value. */
#define IOCTL_WINEBTH_GATT_SERVICE_WRITE_CHARACTERISTIC_VALUE CTL_CODE(FILE_DEVICE_BLUETOOTH, 0xd1, METHOD_BUFFERED, FILE_ANY_ACCESS)
/* Enable or disable value change notifications for a GATT characteristic. */
#define IOCTL_WINEBTH_GATT_SERVICE_SET_CHARACTERISTIC_NOTIFY CTL_CODE(FILE_DEVICE_BLUETOOTH, 0xd2, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* Get the last received LE advertisement for every remote device currently in range. */
#define IOCTL_WINEBTH_RADIO_GET_LE_ADVERTISEMENTS CTL_CODE(FILE_DEVICE_BLUETOOTH, 0xe0, METHOD_BUFFERED, FILE_ANY_ACCESS)

DEFINE_GUID( GUID_WINEBTH_AUTHENTICATION_REQUEST, 0xca67235f, 0xf621, 0x4c27, 0x85, 0x65, 0xa4,
             0xd5, 0x5e, 0xa1, 0x26, 0xe8 );
/* Custom GATT service event raised whenever a characteristic value changes. The event data is a
 * struct winebth_gatt_value_changed. */
DEFINE_GUID( GUID_WINEBTH_GATT_VALUE_CHANGED, 0x6f0c2d13, 0x4c8e, 0x4b6a, 0x9a, 0x3f, 0x2c, 0x1e, 0x8d, 0x77, 0x51, 0x04 );
/* Custom radio event raised whenever an LE advertisement is received from a remote device. The event data is a
 * struct winebth_le_advertisement. */
DEFINE_GUID( GUID_WINEBTH_LE_ADVERTISEMENT, 0x1c3b7a52, 0x9e4d, 0x4f0a, 0xb6, 0x2e, 0x5d, 0x0f, 0x3a, 0x71, 0xc9, 0x88 );

#define WINEBTH_AUTH_DEVICE_PATH L"\\??\\WINEBTHAUTH"

#pragma pack(push,1)

#define LOCAL_RADIO_DISCOVERABLE 0x0001
#define LOCAL_RADIO_CONNECTABLE  0x0002

struct winebth_radio_set_flag_params
{
    unsigned int flag: 2;
    unsigned int enable : 1;
};

/* Optional input for IOCTL_WINEBTH_RADIO_START_DISCOVERY. Without it, only Bluetooth Classic devices are discovered. */
struct winebth_radio_start_discovery_params
{
    unsigned int le : 1;
};

#define WINEBTH_LE_ADV_MAX_UUIDS             16
#define WINEBTH_LE_ADV_MAX_MANUFACTURER_DATA 2
#define WINEBTH_LE_ADV_MAX_SERVICE_DATA      2
#define WINEBTH_LE_ADV_MAX_DATA              128

#define WINEBTH_LE_ADV_FLAG_RANDOM_ADDRESS 0x01
#define WINEBTH_LE_ADV_FLAG_RSSI           0x02
#define WINEBTH_LE_ADV_FLAG_TX_POWER       0x04
#define WINEBTH_LE_ADV_FLAG_APPEARANCE     0x08
#define WINEBTH_LE_ADV_FLAG_NAME           0x10

struct winebth_le_manufacturer_data
{
    UINT16 company_id;
    UINT16 size;
    BYTE data[WINEBTH_LE_ADV_MAX_DATA];
};

struct winebth_le_service_data
{
    GUID uuid;
    UINT16 size;
    BYTE data[WINEBTH_LE_ADV_MAX_DATA];
};

struct winebth_le_advertisement
{
    BTH_ADDR address;
    UINT32 flags;
    INT16 rssi;
    INT16 tx_power;
    UINT16 appearance;
    CHAR name[BLUETOOTH_MAX_NAME_SIZE];
    UINT16 uuid_count;
    GUID uuids[WINEBTH_LE_ADV_MAX_UUIDS];
    UINT16 manufacturer_data_count;
    struct winebth_le_manufacturer_data manufacturer_data[WINEBTH_LE_ADV_MAX_MANUFACTURER_DATA];
    UINT16 service_data_count;
    struct winebth_le_service_data service_data[WINEBTH_LE_ADV_MAX_SERVICE_DATA];
};

struct winebth_gatt_service_write_characteristic_value_params
{
    BTH_LE_UUID uuid;
    UINT16 handle;
    unsigned int without_response : 1;
    ULONG size;
    BYTE buf[1];
};

struct winebth_gatt_service_set_characteristic_notify_params
{
    BTH_LE_UUID uuid;
    UINT16 handle;
    unsigned int enable : 1;
};

struct winebth_gatt_value_changed
{
    BTH_LE_UUID uuid;
    UINT16 handle;
    ULONG size;
    BYTE data[1];
};

struct winebth_radio_get_le_advertisements_params
{
    ULONG count;
    struct winebth_le_advertisement advertisements[1];
};

/* Associated data for GUID_WINEBTH_AUTHENTICATION_REQUEST events. */
struct winebth_authentication_request
{
    BTH_DEVICE_INFO device_info;
    BLUETOOTH_AUTHENTICATION_METHOD auth_method;
    ULONG numeric_value_or_passkey;
};

struct winebth_radio_send_auth_response_params
{
    BTH_ADDR address;
    BLUETOOTH_AUTHENTICATION_METHOD method;
    UINT32 numeric_value_or_passkey;
    unsigned int negative : 1;

    unsigned int authenticated : 1;
};

struct winebth_radio_start_auth_params
{
    BTH_ADDR address;
};

struct winebth_le_device_get_gatt_services_params
{
    ULONG count;
    BTH_LE_GATT_SERVICE services[0];
};

struct winebth_le_device_get_gatt_characteristics_params
{
    BTH_LE_GATT_SERVICE service;
    ULONG count;
    BTH_LE_GATT_CHARACTERISTIC characteristics[0];
};

struct winebth_gatt_service_read_characterisitic_value_params
{
    BTH_LE_UUID uuid;
    UINT16 handle;
    unsigned int from_device : 1;
    ULONG size;
    BYTE buf[1];
};

#pragma pack(pop)

#endif /* __WINEBTH_H__ */
