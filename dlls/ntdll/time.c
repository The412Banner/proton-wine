/*
 * Nt time functions.
 *
 * RtlTimeToTimeFields, RtlTimeFieldsToTime and defines are taken from ReactOS and
 * adapted to wine with special permissions of the author. This code is
 * Copyright 2002 Rex Jolliff (rex@lvcablemodem.com)
 *
 * Copyright 1999 Juergen Schmied
 * Copyright 2007 Dmitry Timoshkov
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
#include <errno.h>
#include <string.h>
#include <limits.h>
#include <time.h>
#if defined(__x86_64__) && !defined(__arm64ec__)
#include <x86intrin.h>
#include <cpuid.h>
#endif

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "ddk/wdm.h"
#include "wine/exception.h"
#include "wine/debug.h"
#include "ntdll_misc.h"

WINE_DEFAULT_DEBUG_CHANNEL(ntdll);

#define TICKSPERSEC        10000000
#define TICKSPERMSEC       10000
#define SECSPERDAY         86400
#define SECSPERHOUR        3600
#define SECSPERMIN         60
#define MINSPERHOUR        60
#define HOURSPERDAY        24
#define EPOCHWEEKDAY       1  /* Jan 1, 1601 was Monday */
#define DAYSPERWEEK        7
#define MONSPERYEAR        12
#define DAYSPERQUADRICENTENNIUM (365 * 400 + 97)
#define DAYSPERNORMALQUADRENNIUM (365 * 4 + 1)

/* 1601 to 1970 is 369 years plus 89 leap days */
#define SECS_1601_TO_1970  ((369 * 365 + 89) * (ULONGLONG)SECSPERDAY)
#define TICKS_1601_TO_1970 (SECS_1601_TO_1970 * TICKSPERSEC)
/* 1601 to 1980 is 379 years plus 91 leap days */
#define SECS_1601_TO_1980  ((379 * 365 + 91) * (ULONGLONG)SECSPERDAY)
#define TICKS_1601_TO_1980 (SECS_1601_TO_1980 * TICKSPERSEC)


static const int MonthLengths[2][MONSPERYEAR] =
{
	{ 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 },
	{ 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 }
};

static inline BOOL IsLeapYear(int Year)
{
    return Year % 4 == 0 && (Year % 100 != 0 || Year % 400 == 0);
}


/******************************************************************************
 *       RtlTimeToTimeFields [NTDLL.@]
 *
 * Convert a time into a TIME_FIELDS structure.
 *
 * PARAMS
 *   liTime     [I] Time to convert.
 *   TimeFields [O] Destination for the converted time.
 *
 * RETURNS
 *   Nothing.
 */
VOID WINAPI RtlTimeToTimeFields(
	const LARGE_INTEGER *liTime,
	PTIME_FIELDS TimeFields)
{
	int SecondsInDay;
        long int cleaps, years, yearday, months;
	long int Days;
	LONGLONG Time;

	/* Extract millisecond from time and convert time into seconds */
	TimeFields->Milliseconds =
            (CSHORT) (( liTime->QuadPart % TICKSPERSEC) / TICKSPERMSEC);
	Time = liTime->QuadPart / TICKSPERSEC;

	/* The native version of RtlTimeToTimeFields does not take leap seconds
	 * into account */

	/* Split the time into days and seconds within the day */
	Days = Time / SECSPERDAY;
	SecondsInDay = Time % SECSPERDAY;

	/* compute time of day */
	TimeFields->Hour = (CSHORT) (SecondsInDay / SECSPERHOUR);
	SecondsInDay = SecondsInDay % SECSPERHOUR;
	TimeFields->Minute = (CSHORT) (SecondsInDay / SECSPERMIN);
	TimeFields->Second = (CSHORT) (SecondsInDay % SECSPERMIN);

	/* compute day of week */
	TimeFields->Weekday = (CSHORT) ((EPOCHWEEKDAY + Days) % DAYSPERWEEK);

        /* compute year, month and day of month. */
        cleaps=( 3 * ((4 * Days + 1227) / DAYSPERQUADRICENTENNIUM) + 3 ) / 4;
        Days += 28188 + cleaps;
        years = (20 * Days - 2442) / (5 * DAYSPERNORMALQUADRENNIUM);
        yearday = Days - (years * DAYSPERNORMALQUADRENNIUM)/4;
        months = (64 * yearday) / 1959;
        /* the result is based on a year starting on March.
         * To convert take 12 from Januari and Februari and
         * increase the year by one. */
        if( months < 14 ) {
            TimeFields->Month = months - 1;
            TimeFields->Year = years + 1524;
        } else {
            TimeFields->Month = months - 13;
            TimeFields->Year = years + 1525;
        }
        /* calculation of day of month is based on the wonderful
         * sequence of INT( n * 30.6): it reproduces the 
         * 31-30-31-30-31-31 month lengths exactly for small n's */
        TimeFields->Day = yearday - (1959 * months) / 64 ;
        return;
}

/******************************************************************************
 *       RtlTimeFieldsToTime [NTDLL.@]
 *
 * Convert a TIME_FIELDS structure into a time.
 *
 * PARAMS
 *   ftTimeFields [I] TIME_FIELDS structure to convert.
 *   Time         [O] Destination for the converted time.
 *
 * RETURNS
 *   Success: TRUE.
 *   Failure: FALSE.
 */
BOOLEAN WINAPI RtlTimeFieldsToTime(
	PTIME_FIELDS tfTimeFields,
	PLARGE_INTEGER Time)
{
        int month, year, cleaps, day;

	/* FIXME: normalize the TIME_FIELDS structure here */
        /* No, native just returns 0 (error) if the fields are not */
        if( tfTimeFields->Milliseconds< 0 || tfTimeFields->Milliseconds > 999 ||
                tfTimeFields->Second < 0 || tfTimeFields->Second > 59 ||
                tfTimeFields->Minute < 0 || tfTimeFields->Minute > 59 ||
                tfTimeFields->Hour < 0 || tfTimeFields->Hour > 23 ||
                tfTimeFields->Month < 1 || tfTimeFields->Month > 12 ||
                tfTimeFields->Day < 1 ||
                tfTimeFields->Day > MonthLengths
                    [ tfTimeFields->Month ==2 || IsLeapYear(tfTimeFields->Year)]
                    [ tfTimeFields->Month - 1] ||
                tfTimeFields->Year < 1601 )
            return FALSE;

        /* now calculate a day count from the date
         * First start counting years from March. This way the leap days
         * are added at the end of the year, not somewhere in the middle.
         * Formula's become so much less complicate that way.
         * To convert: add 12 to the month numbers of Jan and Feb, and 
         * take 1 from the year */
        if(tfTimeFields->Month < 3) {
            month = tfTimeFields->Month + 13;
            year = tfTimeFields->Year - 1;
        } else {
            month = tfTimeFields->Month + 1;
            year = tfTimeFields->Year;
        }
        cleaps = (3 * (year / 100) + 3) / 4;   /* nr of "century leap years"*/
        day =  (36525 * year) / 100 - cleaps + /* year * dayperyr, corrected */
                 (1959 * month) / 64 +         /* months * daypermonth */
                 tfTimeFields->Day -          /* day of the month */
                 584817 ;                      /* zero that on 1601-01-01 */
        /* done */
        
        Time->QuadPart = (((((LONGLONG) day * HOURSPERDAY +
            tfTimeFields->Hour) * MINSPERHOUR +
            tfTimeFields->Minute) * SECSPERMIN +
            tfTimeFields->Second ) * 1000 +
            tfTimeFields->Milliseconds ) * TICKSPERMSEC;

        return TRUE;
}


/******************************************************************************
 *        RtlLocalTimeToSystemTime [NTDLL.@]
 *
 * Convert a local time into system time.
 *
 * PARAMS
 *   LocalTime  [I] Local time to convert.
 *   SystemTime [O] Destination for the converted time.
 *
 * RETURNS
 *   Success: STATUS_SUCCESS.
 *   Failure: An NTSTATUS error code indicating the problem.
 */
NTSTATUS WINAPI RtlLocalTimeToSystemTime( const LARGE_INTEGER *LocalTime,
                                          PLARGE_INTEGER SystemTime)
{
    SYSTEM_TIMEOFDAY_INFORMATION info;

    TRACE("(%p, %p)\n", LocalTime, SystemTime);

    NtQuerySystemInformation( SystemTimeOfDayInformation, &info, sizeof(info), NULL );
    SystemTime->QuadPart = LocalTime->QuadPart + info.TimeZoneBias.QuadPart;
    return STATUS_SUCCESS;
}

/******************************************************************************
 *       RtlSystemTimeToLocalTime [NTDLL.@]
 *
 * Convert a system time into a local time.
 *
 * PARAMS
 *   SystemTime [I] System time to convert.
 *   LocalTime  [O] Destination for the converted time.
 *
 * RETURNS
 *   Success: STATUS_SUCCESS.
 *   Failure: An NTSTATUS error code indicating the problem.
 */
NTSTATUS WINAPI RtlSystemTimeToLocalTime( const LARGE_INTEGER *SystemTime,
                                          PLARGE_INTEGER LocalTime )
{
    SYSTEM_TIMEOFDAY_INFORMATION info;

    TRACE("(%p, %p)\n", SystemTime, LocalTime);

    NtQuerySystemInformation( SystemTimeOfDayInformation, &info, sizeof(info), NULL );
    LocalTime->QuadPart = SystemTime->QuadPart - info.TimeZoneBias.QuadPart;
    return STATUS_SUCCESS;
}

/******************************************************************************
 *       RtlTimeToSecondsSince1970 [NTDLL.@]
 *
 * Convert a time into a count of seconds since 1970.
 *
 * PARAMS
 *   Time    [I] Time to convert.
 *   Seconds [O] Destination for the converted time.
 *
 * RETURNS
 *   Success: TRUE.
 *   Failure: FALSE, if the resulting value will not fit in a DWORD.
 */
BOOLEAN WINAPI RtlTimeToSecondsSince1970( const LARGE_INTEGER *Time, LPDWORD Seconds )
{
    ULONGLONG tmp = Time->QuadPart / TICKSPERSEC - SECS_1601_TO_1970;
    if (tmp > 0xffffffff) return FALSE;
    *Seconds = tmp;
    return TRUE;
}

/******************************************************************************
 *       RtlTimeToSecondsSince1980 [NTDLL.@]
 *
 * Convert a time into a count of seconds since 1980.
 *
 * PARAMS
 *   Time    [I] Time to convert.
 *   Seconds [O] Destination for the converted time.
 *
 * RETURNS
 *   Success: TRUE.
 *   Failure: FALSE, if the resulting value will not fit in a DWORD.
 */
BOOLEAN WINAPI RtlTimeToSecondsSince1980( const LARGE_INTEGER *Time, LPDWORD Seconds )
{
    ULONGLONG tmp = Time->QuadPart / TICKSPERSEC - SECS_1601_TO_1980;
    if (tmp > 0xffffffff) return FALSE;
    *Seconds = tmp;
    return TRUE;
}

/******************************************************************************
 *       RtlSecondsSince1970ToTime [NTDLL.@]
 *
 * Convert a count of seconds since 1970 to a time.
 *
 * PARAMS
 *   Seconds [I] Time to convert.
 *   Time    [O] Destination for the converted time.
 *
 * RETURNS
 *   Nothing.
 */
void WINAPI RtlSecondsSince1970ToTime( DWORD Seconds, LARGE_INTEGER *Time )
{
    Time->QuadPart = Seconds * (ULONGLONG)TICKSPERSEC + TICKS_1601_TO_1970;
}

/******************************************************************************
 *       RtlSecondsSince1980ToTime [NTDLL.@]
 *
 * Convert a count of seconds since 1980 to a time.
 *
 * PARAMS
 *   Seconds [I] Time to convert.
 *   Time    [O] Destination for the converted time.
 *
 * RETURNS
 *   Nothing.
 */
void WINAPI RtlSecondsSince1980ToTime( DWORD Seconds, LARGE_INTEGER *Time )
{
    Time->QuadPart = Seconds * (ULONGLONG)TICKSPERSEC + TICKS_1601_TO_1980;
}

/******************************************************************************
 *       RtlTimeToElapsedTimeFields [NTDLL.@]
 *
 * Convert a time to a count of elapsed seconds.
 *
 * PARAMS
 *   Time       [I] Time to convert.
 *   TimeFields [O] Destination for the converted time.
 *
 * RETURNS
 *   Nothing.
 */
void WINAPI RtlTimeToElapsedTimeFields( const LARGE_INTEGER *Time, PTIME_FIELDS TimeFields )
{
    LONGLONG time;
    INT rem;

    time = Time->QuadPart / TICKSPERSEC;
    TimeFields->Milliseconds = (Time->QuadPart % TICKSPERSEC) / TICKSPERMSEC;

    /* time is now in seconds */
    TimeFields->Year  = 0;
    TimeFields->Month = 0;
    TimeFields->Day   = time / SECSPERDAY;

    /* rem is now the remaining seconds in the last day */
    rem = time % SECSPERDAY;
    TimeFields->Second = rem % 60;
    rem /= 60;
    TimeFields->Minute = rem % 60;
    TimeFields->Hour = rem / 60;
}

/***********************************************************************
 *       RtlGetSystemTimePrecise [NTDLL.@]
 *
 * Get a more accurate current system time.
 *
 * RETURNS
 *   The current system time.
 */
LONGLONG WINAPI RtlGetSystemTimePrecise( void )
{
    LONGLONG ret;

    WINE_UNIX_CALL( unix_system_time_precise, &ret );
    return ret;
}

#if defined(__x86_64__) && !defined(__arm64ec__)
/* WINEDEBUG=+qpc reports which path was chosen and why, and every later change of mind. */
WINE_DECLARE_DEBUG_CHANNEL(qpc);

/* User-mode performance counter based on the invariant TSC.
 *
 * Only used while the kernel uses the TSC as its own clocksource. The invariant TSC bit in
 * CPUID promises a constant tick rate, not that the per-core counters agree with each other;
 * keeping them aligned is the firmware's job and some machines are seconds off. Linux tests
 * that at boot (check_tsc_sync_source) and falls back to hpet or acpi_pm when it fails, as
 * Windows does when its own boot-time check fails. current_clocksource is that verdict, read
 * through the unix namespace at start-up, then on the first call made more than a second
 * after the last check, so a TSC the kernel demotes later is dropped too.
 *
 * Calibration publishes a rate once two consecutive passes agree. Each (counter, TSC) pair is
 * bracketed by two rdtscp and discarded unless both report the same processor and lie within
 * QPC_TSC_SAMPLE_CYCLES of each other.
 *
 * The hot path is rdtscp, two predictable compares and one 64x64->128 multiply, with plain
 * loads only: no locked instruction and no shared write, because an earlier version enforcing
 * monotonicity with an interlocked clamp bounced a cache line between the callers and gave no
 * gain at all. A TSC read below the calibrated base would wrap the unsigned subtraction into
 * a timestamp centuries away, so that call takes the syscall path instead.
 *
 * The decision is made at start-up; the counter is not disciplined afterwards. */
#define QPC_TSC_CALIBRATION_TICKS (TICKSPERSEC / 8)    /* 125 ms per calibration pass */
#define QPC_TSC_SAMPLE_CYCLES     50000                /* a sample pair wider than this is discarded */
#define QPC_TSC_RATE_TOLERANCE    2000                 /* two rates agree when within 1/2000 */
#define QPC_TSC_MAX_PASSES        32                   /* give up calibrating if they never do */
#define QPC_TSC_MAX_PROBES        64                   /* retries while the clocksource is unreadable */
#define QPC_TSC_CHECK_TICKS       TICKSPERSEC          /* ask the kernel again once a second */

enum
{
    QPC_TSC_DISABLED = -1,   /* the syscall path, for good */
    QPC_TSC_UNKNOWN = 0,     /* nothing probed yet */
    QPC_TSC_PROBING,         /* one thread is probing support and taking the first sample */
    QPC_TSC_CALIBRATING,     /* the base is valid, waiting for the window to elapse */
    QPC_TSC_PUBLISHING,      /* one thread owns the end of a calibration pass */
    QPC_TSC_READY,           /* the hot path is live */
};

static volatile LONG qpc_tsc_state;
static volatile LONG qpc_tsc_checking;   /* one thread asking the kernel at a time */
static BOOL      qpc_tsc_forced;         /* WINE_ENABLE_QPC_TSC: do not follow the kernel clocksource */

/* written before qpc_tsc_state releases them, then constant; qpc_tsc_base_qpc is also read by
 * calibrating callers while a pass is being closed, hence the relaxed atomics on it */
static LONGLONG  qpc_tsc_base_qpc;
static ULONGLONG qpc_tsc_base_tsc;
static ULONGLONG qpc_tsc_mult;           /* TICKSPERSEC * 2^32 / tsc_hz */
static ULONGLONG qpc_tsc_check_step;     /* QPC_TSC_CHECK_TICKS expressed in TSC cycles */

/* calibration state, owned by whoever holds QPC_TSC_PROBING or QPC_TSC_PUBLISHING */
static LONGLONG  qpc_tsc_first_qpc;      /* the very first sample, for the full-span rate */
static ULONGLONG qpc_tsc_first_tsc;
static ULONGLONG qpc_tsc_prev_hz;        /* rate measured by the previous pass */
static LONG      qpc_tsc_passes;
static LONG      qpc_tsc_probes;

/* written after publication while every caller reads it: relaxed atomics, which on x86-64
 * are plain 64-bit loads and stores, keep the compiler from tearing or caching it */
static ULONGLONG qpc_tsc_check_start;    /* TSC at the last check; the next one is due a step later */

struct qpc_tsc_sample
{
    LONGLONG  qpc;
    ULONGLONG tsc;
};

static inline ULONGLONG qpc_read_tsc(void)
{
    unsigned int aux;
    return __rdtscp( &aux );
}

static inline LONGLONG qpc_tsc_ticks( ULONGLONG tsc )
{
    return qpc_tsc_base_qpc +
           (LONGLONG)(((unsigned __int128)(tsc - qpc_tsc_base_tsc) * qpc_tsc_mult) >> 32);
}

/* Read the real counter and the TSC together, bracketed by two rdtscp so that a pair taken
 * across a preemption or a core migration can be recognised and thrown away. sample->qpc is
 * filled in either way, so the caller can still serve the counter from a rejected pair. */
static BOOL qpc_tsc_get_sample( struct qpc_tsc_sample *sample )
{
    unsigned int cpu_before, cpu_after;
    ULONGLONG before, after;
    LARGE_INTEGER now;

    before = __rdtscp( &cpu_before );
    NtQueryPerformanceCounter( &now, NULL );
    after = __rdtscp( &cpu_after );

    sample->qpc = now.QuadPart;
    sample->tsc = before + (after - before) / 2;
    return cpu_before == cpu_after && after - before <= QPC_TSC_SAMPLE_CYCLES;
}

static BOOL qpc_tsc_env_set( const WCHAR *name )
{
    UNICODE_STRING nameU, value;
    WCHAR buffer[8];

    RtlInitUnicodeString( &nameU, name );
    value.Buffer = buffer;
    value.MaximumLength = sizeof(buffer);
    value.Length = 0;
    if (RtlQueryEnvironmentVariable_U( NULL, &nameU, &value )) return FALSE;
    return value.Length && buffer[0] != '0';
}

/* Does the host kernel use the TSC as its own clocksource? 1 yes, 0 no, -1 not readable. */
static int qpc_tsc_kernel_clocksource(void)
{
    OBJECT_ATTRIBUTES attr;
    UNICODE_STRING nameU;
    IO_STATUS_BLOCK io;
    LARGE_INTEGER offset;
    HANDLE handle;
    char buffer[16];
    int ret = -1;

    RtlInitUnicodeString( &nameU,
        L"\\??\\unix\\sys\\devices\\system\\clocksource\\clocksource0\\current_clocksource" );
    InitializeObjectAttributes( &attr, &nameU, OBJ_CASE_INSENSITIVE, NULL, NULL );
    if (NtOpenFile( &handle, GENERIC_READ | SYNCHRONIZE, &attr, &io,
                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                    FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE ))
        return -1;
    offset.QuadPart = 0;
    if (!NtReadFile( handle, NULL, NULL, NULL, &io, buffer, sizeof(buffer), &offset, NULL ))
        ret = io.Information >= 3 && !memcmp( buffer, "tsc", 3 ) &&
              (io.Information == 3 || buffer[3] == '\n');
    NtClose( handle );
    return ret;
}

/* May the TSC be used? 1 yes, 0 no, -1 undecided, ask again later. */
static int qpc_tsc_usable(void)
{
    unsigned int eax, ebx, ecx, edx;
    int clocksource;

    /* escape hatch: WINE_DISABLE_QPC_TSC=1 in the environment */
    if (qpc_tsc_env_set( L"WINE_DISABLE_QPC_TSC" ))
    {
        TRACE_(qpc)( "disabled by WINE_DISABLE_QPC_TSC\n" );
        return 0;
    }

    /* invariant TSC: CPUID 0x80000007 EDX bit 8 ; rdtscp: CPUID 0x80000001 EDX bit 27 */
    __cpuid( 0x80000000, eax, ebx, ecx, edx );
    if (eax >= 0x80000007) __cpuid( 0x80000007, eax, ebx, ecx, edx );
    else edx = 0;
    if (!(edx & (1u << 8)))
    {
        TRACE_(qpc)( "no invariant TSC\n" );
        return 0;
    }
    __cpuid( 0x80000001, eax, ebx, ecx, edx );
    if (!(edx & (1u << 27)))
    {
        TRACE_(qpc)( "no rdtscp\n" );
        return 0;
    }

    if ((qpc_tsc_forced = qpc_tsc_env_set( L"WINE_ENABLE_QPC_TSC" )))
    {
        TRACE_(qpc)( "WINE_ENABLE_QPC_TSC set, not following the kernel clocksource\n" );
        return 1;
    }
    clocksource = qpc_tsc_kernel_clocksource();
    TRACE_(qpc)( "kernel clocksource is %s\n", clocksource == 1 ? "tsc" :
                 clocksource == 0 ? "not tsc, staying on the syscall path" : "not readable" );
    return clocksource;
}

/* Owner of QPC_TSC_PROBING: decide whether the TSC may be used at all and take the first
 * sample. Re-entrant callers see QPC_TSC_PROBING and are served by the syscall path, so it is
 * safe for this to open a file. */
static void qpc_tsc_probe(void)
{
    struct qpc_tsc_sample sample;
    int i, usable = qpc_tsc_usable();

    if (usable > 0)
    {
        for (i = 0; i < 16; i++)
        {
            if (!qpc_tsc_get_sample( &sample )) continue;
            qpc_tsc_first_qpc = sample.qpc;
            qpc_tsc_first_tsc = qpc_tsc_base_tsc = sample.tsc;
            __atomic_store_n( &qpc_tsc_base_qpc, sample.qpc, __ATOMIC_RELAXED );
            InterlockedExchange( (LONG *)&qpc_tsc_state, QPC_TSC_CALIBRATING );
            return;
        }
    }
    else if (usable < 0 && ++qpc_tsc_probes < QPC_TSC_MAX_PROBES)
    {
        /* the clocksource was not readable, most likely too early in process start-up:
         * fall back for now and look again on a later call */
        InterlockedExchange( (LONG *)&qpc_tsc_state, QPC_TSC_UNKNOWN );
        return;
    }
    TRACE_(qpc)( "using the syscall path for this process\n" );
    InterlockedExchange( (LONG *)&qpc_tsc_state, QPC_TSC_DISABLED );
}

/* End of one calibration pass, owner of QPC_TSC_PUBLISHING. */
static void qpc_tsc_calibrate( const struct qpc_tsc_sample *sample )
{
    LONGLONG dqpc = sample->qpc - qpc_tsc_base_qpc;
    ULONGLONG prev = qpc_tsc_prev_hz, hz, span_hz, delta;
    unsigned __int128 wide;

    /* another thread may have closed a pass and moved the base between the caller's check of
     * the window and its taking ownership: its sample is then stale, possibly older than the
     * new base, and nothing can be computed from it */
    if (dqpc < QPC_TSC_CALIBRATION_TICKS)
    {
        InterlockedExchange( (LONG *)&qpc_tsc_state, QPC_TSC_CALIBRATING );
        return;
    }

    wide = (unsigned __int128)(sample->tsc - qpc_tsc_base_tsc) * TICKSPERSEC / (ULONGLONG)dqpc;
    hz = wide > ~0ull ? ~0ull : (ULONGLONG)wide;
    qpc_tsc_prev_hz = 0;
    if (hz > 100000000ull && hz < 20000000000ull)   /* 100 MHz .. 20 GHz : sane */
    {
        delta = prev > hz ? prev - hz : hz - prev;
        if (prev && delta * QPC_TSC_RATE_TOLERANCE <= prev)
        {
            /* the passes agree. The rate over the whole span since the first sample is the
             * more accurate one, so prefer it, but only while it agrees with this pass to the
             * same tolerance */
            wide = (unsigned __int128)(sample->tsc - qpc_tsc_first_tsc) * TICKSPERSEC
                   / (ULONGLONG)(sample->qpc - qpc_tsc_first_qpc);
            span_hz = wide > ~0ull ? ~0ull : (ULONGLONG)wide;
            delta = span_hz > hz ? span_hz - hz : hz - span_hz;
            if (delta * QPC_TSC_RATE_TOLERANCE <= hz) hz = span_hz;

            /* rebase on this sample so the switch is continuous, arm the first check, then
             * publish (release) */
            qpc_tsc_mult = (ULONGLONG)(((unsigned __int128)TICKSPERSEC << 32) / hz);
            qpc_tsc_base_tsc = sample->tsc;
            qpc_tsc_check_step = hz * QPC_TSC_CHECK_TICKS / TICKSPERSEC;
            __atomic_store_n( &qpc_tsc_base_qpc, sample->qpc, __ATOMIC_RELAXED );
            __atomic_store_n( &qpc_tsc_check_start, sample->tsc, __ATOMIC_RELAXED );
            TRACE_(qpc)( "calibrated at %I64u Hz after %ld passes, using the TSC\n",
                         hz, qpc_tsc_passes + 1 );
            InterlockedExchange( (LONG *)&qpc_tsc_state, QPC_TSC_READY );
            return;
        }
        qpc_tsc_prev_hz = hz;
    }
    if (++qpc_tsc_passes >= QPC_TSC_MAX_PASSES)
    {
        WARN_(qpc)( "no two calibration passes agreed in %d tries, using the syscall path\n",
                    QPC_TSC_MAX_PASSES );
        InterlockedExchange( (LONG *)&qpc_tsc_state, QPC_TSC_DISABLED );
        return;
    }
    qpc_tsc_base_tsc = sample->tsc;
    __atomic_store_n( &qpc_tsc_base_qpc, sample->qpc, __ATOMIC_RELAXED );
    InterlockedExchange( (LONG *)&qpc_tsc_state, QPC_TSC_CALIBRATING );
}

/* Once a second, one caller asks the kernel whether it still uses the TSC. Returns FALSE
 * once it does not and the TSC path has been given up. */
static BOOL qpc_tsc_check( ULONGLONG tsc )
{
    BOOL ok = TRUE;

    if (InterlockedCompareExchange( (LONG *)&qpc_tsc_checking, 1, 0 )) return TRUE;
    if (__atomic_load_n( &qpc_tsc_state, __ATOMIC_ACQUIRE ) != QPC_TSC_READY) ok = FALSE;
    else if (!qpc_tsc_forced && qpc_tsc_kernel_clocksource() != 1)
    {
        WARN_(qpc)( "the kernel no longer selects the TSC, falling back to the syscall path\n" );
        InterlockedExchange( (LONG *)&qpc_tsc_state, QPC_TSC_DISABLED );
        ok = FALSE;
    }
    else __atomic_store_n( &qpc_tsc_check_start, tsc, __ATOMIC_RELAXED );
    InterlockedExchange( (LONG *)&qpc_tsc_checking, 0 );
    return ok;
}

static BOOL qpc_tsc_counter( LARGE_INTEGER *counter )
{
    /* acquire: the values published below must not be loaded before the state that releases
     * them, which the compiler would otherwise be free to do */
    LONG state = __atomic_load_n( &qpc_tsc_state, __ATOMIC_ACQUIRE );
    struct qpc_tsc_sample sample;
    ULONGLONG tsc;

    if (state == QPC_TSC_READY)
    {
        tsc = qpc_read_tsc();
        /* a core running behind the calibrated base would wrap this subtraction and hand out a
         * timestamp centuries away: this call takes the real counter instead */
        if ((LONGLONG)(tsc - qpc_tsc_base_tsc) < 0)
        {
            unsigned int cpu;

            __rdtscp( &cpu );   /* only to name the core in the message */
            WARN_(qpc)( "TSC %I64u below the calibrated base %I64u on cpu %u\n",
                        tsc, qpc_tsc_base_tsc, cpu );
            return FALSE;
        }
        /* unsigned: a TSC that went backwards wraps this and asks the kernel at once */
        if (tsc - __atomic_load_n( &qpc_tsc_check_start, __ATOMIC_RELAXED ) >= qpc_tsc_check_step &&
            !qpc_tsc_check( tsc ))
            return FALSE;
        counter->QuadPart = qpc_tsc_ticks( tsc );
        return TRUE;
    }
    if (state == QPC_TSC_DISABLED) return FALSE;

    if (state == QPC_TSC_UNKNOWN)
    {
        if (InterlockedCompareExchange( (LONG *)&qpc_tsc_state, QPC_TSC_PROBING,
                                        QPC_TSC_UNKNOWN ) == QPC_TSC_UNKNOWN)
            qpc_tsc_probe();
        return FALSE;
    }
    if (state != QPC_TSC_CALIBRATING) return FALSE;   /* another thread owns the transition */

    /* still calibrating: serve the real counter, and close the pass if the window elapsed */
    if (qpc_tsc_get_sample( &sample ) &&
        sample.qpc - __atomic_load_n( &qpc_tsc_base_qpc, __ATOMIC_RELAXED ) >= QPC_TSC_CALIBRATION_TICKS &&
        InterlockedCompareExchange( (LONG *)&qpc_tsc_state, QPC_TSC_PUBLISHING,
                                    QPC_TSC_CALIBRATING ) == QPC_TSC_CALIBRATING)
        qpc_tsc_calibrate( &sample );

    counter->QuadPart = sample.qpc;
    return TRUE;
}
#endif

/******************************************************************************
 *  RtlQueryPerformanceCounter   [NTDLL.@]
 */
BOOL WINAPI DECLSPEC_HOTPATCH RtlQueryPerformanceCounter( LARGE_INTEGER *counter )
{
#if defined(__x86_64__) && !defined(__arm64ec__)
    if (qpc_tsc_counter( counter )) return TRUE;
#endif
    NtQueryPerformanceCounter( counter, NULL );
    return TRUE;
}

/******************************************************************************
 *  RtlQueryPerformanceFrequency   [NTDLL.@]
 */
BOOL WINAPI DECLSPEC_HOTPATCH RtlQueryPerformanceFrequency( LARGE_INTEGER *frequency )
{
    frequency->QuadPart = TICKSPERSEC;
    return TRUE;
}

/******************************************************************************
 * NtGetTickCount   (NTDLL.@)
 * ZwGetTickCount   (NTDLL.@)
 */
ULONG WINAPI DECLSPEC_HOTPATCH NtGetTickCount(void)
{
    /* note: we ignore TickCountMultiplier */
    return user_shared_data->TickCount.LowPart;
}

/***********************************************************************
 *      RtlQueryTimeZoneInformation [NTDLL.@]
 *
 * Get information about the current timezone.
 *
 * PARAMS
 *   tzinfo [O] Destination for the retrieved timezone info.
 *
 * RETURNS
 *   Success: STATUS_SUCCESS.
 *   Failure: An NTSTATUS error code indicating the problem.
 */
NTSTATUS WINAPI RtlQueryTimeZoneInformation(RTL_TIME_ZONE_INFORMATION *ret)
{
    return NtQuerySystemInformation( SystemCurrentTimeZoneInformation, ret, sizeof(*ret), NULL );
}

/***********************************************************************
 *      RtlQueryDynamicTimeZoneInformation [NTDLL.@]
 *
 * Get information about the current timezone.
 *
 * PARAMS
 *   tzinfo [O] Destination for the retrieved timezone info.
 *
 * RETURNS
 *   Success: STATUS_SUCCESS.
 *   Failure: An NTSTATUS error code indicating the problem.
 */
NTSTATUS WINAPI RtlQueryDynamicTimeZoneInformation(RTL_DYNAMIC_TIME_ZONE_INFORMATION *ret)
{
    return NtQuerySystemInformation( SystemDynamicTimeZoneInformation, ret, sizeof(*ret), NULL );
}

/***********************************************************************
 *       RtlSetTimeZoneInformation [NTDLL.@]
 *
 * Set the current time zone information.
 *
 * PARAMS
 *   tzinfo [I] Timezone information to set.
 *
 * RETURNS
 *   Success: STATUS_SUCCESS.
 *   Failure: An NTSTATUS error code indicating the problem.
 *
 */
NTSTATUS WINAPI RtlSetTimeZoneInformation( const RTL_TIME_ZONE_INFORMATION *tzinfo )
{
    return STATUS_PRIVILEGE_NOT_HELD;
}

/***********************************************************************
 *        RtlQueryUnbiasedInterruptTime [NTDLL.@]
 */
BOOL WINAPI RtlQueryUnbiasedInterruptTime(ULONGLONG *time)
{
    ULONG high, low;

    if (!time)
    {
        RtlSetLastWin32ErrorAndNtStatusFromNtStatus( STATUS_INVALID_PARAMETER );
        return FALSE;
    }

    do
    {
        high = user_shared_data->InterruptTime.High1Time;
        low = user_shared_data->InterruptTime.LowPart;
    }
    while (high != user_shared_data->InterruptTime.High2Time);
    /* FIXME: should probably subtract InterruptTimeBias */
    *time = (ULONGLONG)high << 32 | low;
    return TRUE;
}
