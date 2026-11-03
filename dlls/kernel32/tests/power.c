/*
 * Unit tests for power management functions
 *
 * Copyright (c) 2019 Alex Henrie
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

#include "wine/test.h"

void test_GetSystemPowerStatus(void)
{
    SYSTEM_POWER_STATUS ps;
    BOOL ret;
    BYTE capacity_flags, expected_capacity_flags;

    if (0) /* crashes */
        GetSystemPowerStatus(NULL);

    memset(&ps, 0x23, sizeof(ps));
    ret = GetSystemPowerStatus(&ps);
    ok(ret == TRUE, "expected TRUE\n");

    if (ps.BatteryFlag == BATTERY_FLAG_UNKNOWN)
    {
        skip("GetSystemPowerStatus not implemented or not working\n");
        return;
    }
    else if (ps.BatteryFlag != BATTERY_FLAG_NO_BATTERY)
    {
        trace("battery detected\n");
        expected_capacity_flags = 0;
        if (ps.BatteryLifePercent > 66)
            expected_capacity_flags |= BATTERY_FLAG_HIGH;
        if (ps.BatteryLifePercent < 33)
            expected_capacity_flags |= BATTERY_FLAG_LOW;
        if (ps.BatteryLifePercent < 5)
            expected_capacity_flags |= BATTERY_FLAG_CRITICAL;
        capacity_flags = (ps.BatteryFlag & ~BATTERY_FLAG_CHARGING);
        ok(capacity_flags == expected_capacity_flags,
           "expected %u%%-charged battery to have capacity flags 0x%02x, got 0x%02x\n",
           ps.BatteryLifePercent, expected_capacity_flags, capacity_flags);
        ok(ps.BatteryLifeTime <= ps.BatteryFullLifeTime,
           "expected BatteryLifeTime %lu to be less than or equal to BatteryFullLifeTime %lu\n",
           ps.BatteryLifeTime, ps.BatteryFullLifeTime);
        if (ps.BatteryFlag & BATTERY_FLAG_CHARGING)
        {
            ok(ps.BatteryLifeTime == BATTERY_LIFE_UNKNOWN,
               "expected BatteryLifeTime to be -1 when charging, got %lu\n", ps.BatteryLifeTime);
            ok(ps.BatteryFullLifeTime == BATTERY_LIFE_UNKNOWN,
               "expected BatteryFullLifeTime to be -1 when charging, got %lu\n", ps.BatteryFullLifeTime);
        }
    }
    else
    {
        trace("no battery detected\n");
        ok(ps.ACLineStatus == AC_LINE_ONLINE,
           "expected ACLineStatus to be 1, got %u\n", ps.ACLineStatus);
        ok(ps.BatteryLifePercent == BATTERY_PERCENTAGE_UNKNOWN,
           "expected BatteryLifePercent to be -1, got %u\n", ps.BatteryLifePercent);
        ok(ps.BatteryLifeTime == BATTERY_LIFE_UNKNOWN,
           "expected BatteryLifeTime to be -1, got %lu\n", ps.BatteryLifeTime);
        ok(ps.BatteryFullLifeTime == BATTERY_LIFE_UNKNOWN,
           "expected BatteryFullLifeTime to be -1, got %lu\n", ps.BatteryFullLifeTime);
    }
}

static DWORD WINAPI power_status_thread(void *arg)
{
    SYSTEM_POWER_STATUS ps;
    unsigned int i;
    BOOL ret;

    for (i = 0; i < 8; ++i)
    {
        memset(&ps, 0x23, sizeof(ps));
        ret = GetSystemPowerStatus(&ps);
        ok(ret, "GetSystemPowerStatus failed, error %lu\n", GetLastError());
        if (!ret) continue;

        ok(ps.ACLineStatus == AC_LINE_OFFLINE || ps.ACLineStatus == AC_LINE_ONLINE ||
           ps.ACLineStatus == AC_LINE_UNKNOWN, "Unexpected ACLineStatus %u\n", ps.ACLineStatus);
        ok(ps.BatteryLifePercent <= 100 || ps.BatteryLifePercent == BATTERY_PERCENTAGE_UNKNOWN,
           "Unexpected BatteryLifePercent %u\n", ps.BatteryLifePercent);
        ok(ps.BatteryFlag == BATTERY_FLAG_UNKNOWN ||
           !(ps.BatteryFlag & ~(BATTERY_FLAG_HIGH | BATTERY_FLAG_LOW | BATTERY_FLAG_CRITICAL |
                               BATTERY_FLAG_CHARGING | BATTERY_FLAG_NO_BATTERY)),
           "Unexpected BatteryFlag %#x\n", ps.BatteryFlag);
        ok(ps.SystemStatusFlag <= 1, "Unexpected SystemStatusFlag %u\n", ps.SystemStatusFlag);
    }
    return 0;
}

static void test_concurrent_power_status(void)
{
    HANDLE threads[4];
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(threads); ++i)
    {
        threads[i] = CreateThread(NULL, 0, power_status_thread, NULL, 0, NULL);
        ok(!!threads[i], "CreateThread failed, error %lu\n", GetLastError());
    }
    for (i = 0; i < ARRAY_SIZE(threads); ++i)
    {
        if (!threads[i]) continue;
        WaitForSingleObject(threads[i], INFINITE);
        CloseHandle(threads[i]);
    }
}

START_TEST(power)
{
    test_GetSystemPowerStatus();
    test_concurrent_power_status();
}
