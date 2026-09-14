/*++

Module Name:

    trace.h

Abstract:

    WPP tracing definitions for the AapL2cap driver.
    Derived from the WDK bthecho sample (common/inc/trace.h).

Environment:

    Kernel mode

--*/

#pragma once

#include <evntrace.h> // For TRACE_LEVEL definitions

#if !defined(EVENT_TRACING)

#if !defined(TRACE_LEVEL_NONE)
  #define TRACE_LEVEL_NONE          0
  #define TRACE_LEVEL_CRITICAL      1
  #define TRACE_LEVEL_FATAL         1
  #define TRACE_LEVEL_ERROR         2
  #define TRACE_LEVEL_WARNING       3
  #define TRACE_LEVEL_INFORMATION   4
  #define TRACE_LEVEL_VERBOSE       5
#endif

//
// Debug flags
//
#define DBG_INIT                0x00000001
#define DBG_PNP                 0x00000002
#define DBG_POWER               0x00000004
#define DBG_CREATE_CLOSE        0x00000010
#define DBG_IOCTL               0x00000020
#define DBG_WRITE               0x00000040
#define DBG_READ                0x00000080
#define DBG_CONNECT             0x00000200
#define DBG_UTIL                0x00000400

VOID
TraceEvents    (
    _In_ ULONG   DebugPrintLevel,
    _In_ ULONG   DebugPrintFlag,
    _Printf_format_string_
    _In_ PCSTR DebugMessage,
    ...
    );

#define WPP_INIT_TRACING(DriverObject, RegistryPath)
#define WPP_CLEANUP(DriverObject)

#else

#define WPP_CHECK_FOR_NULL_STRING  //to prevent exceptions due to NULL strings

//
// WPP control GUID for AapL2cap: {71d3ae7a-1bd6-4f8d-9b3c-7e18e5858b3c}
//
#define WPP_CONTROL_GUIDS \
    WPP_DEFINE_CONTROL_GUID(AapL2capTraceGuid,(71d3ae7a,1bd6,4f8d,9b3c,7e18e5858b3c), \
        WPP_DEFINE_BIT(DBG_INIT)             /* bit  0 = 0x00000001 */ \
        WPP_DEFINE_BIT(DBG_PNP)              /* bit  1 = 0x00000002 */ \
        WPP_DEFINE_BIT(DBG_POWER)            /* bit  2 = 0x00000004 */ \
        WPP_DEFINE_BIT(DBG_CREATE_CLOSE)     /* bit  3 = 0x00000008 */ \
        WPP_DEFINE_BIT(DBG_IOCTL)            /* bit  4 = 0x00000010 */ \
        WPP_DEFINE_BIT(DBG_WRITE)            /* bit  5 = 0x00000020 */ \
        WPP_DEFINE_BIT(DBG_READ)             /* bit  6 = 0x00000040 */ \
        WPP_DEFINE_BIT(DBG_CONNECT)          /* bit  7 = 0x00000080 */ \
        WPP_DEFINE_BIT(DBG_UTIL)             /* bit  8 = 0x00000100 */ \
        )

#define WPP_LEVEL_FLAGS_LOGGER(lvl,flags) WPP_LEVEL_LOGGER(flags)
#define WPP_LEVEL_FLAGS_ENABLED(lvl, flags) (WPP_LEVEL_ENABLED(flags) && WPP_CONTROL(WPP_BIT_ ## flags).Level  >= lvl)

#endif
