/*++

Module Name:

    driver.c

Abstract:

    DriverEntry and driver-level callbacks for the AapL2cap driver.
    Derived from the WDK bthecho sample (bthcli/sys/driver.c).

Environment:

    Kernel mode only

--*/

#include "aapl2cap.h"
#include "driver.h"

#if defined(EVENT_TRACING)
//
// The trace message header (.tmh) file must be included in a source file
// before any WPP macro calls and after defining WPP_CONTROL_GUIDS
// (trace.h).
//
#include "driver.tmh"
#else
#define _DRIVER_NAME_ "AapL2cap"
ULONG DebugLevel = TRACE_LEVEL_INFORMATION;
ULONG DebugFlag = 0xff;
#endif

#ifdef ALLOC_PRAGMA
#pragma alloc_text (INIT, DriverEntry)
#pragma alloc_text (PAGE, AapL2capEvtDriverCleanup)
#endif

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
/*++

Routine Description:

    DriverEntry initializes the driver and is the first routine called by
    the system after the driver is loaded.

--*/
{
    NTSTATUS status;
    WDFDRIVER driver;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_DRIVER_CONFIG DriverConfig;

    WDF_DRIVER_CONFIG_INIT(
                           &DriverConfig,
                           AapL2capEvtDriverDeviceAdd
                           );

    WPP_INIT_TRACING( DriverObject, RegistryPath );

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.EvtCleanupCallback = AapL2capEvtDriverCleanup;

    status = WdfDriverCreate(
        DriverObject,
        RegistryPath,
        &attributes,
        &DriverConfig,
        &driver
        );

    if(!NT_SUCCESS(status))
    {
        WPP_CLEANUP(DriverObject);
    }

    return status;
}

VOID
AapL2capEvtDriverCleanup(
    _In_ WDFOBJECT DriverObject
    )
/*++

Routine Description:

    Stops WPP tracing when the driver object is deleted.

--*/
{
    PAGED_CODE ();

    //
    // DriverObject remains unreferenced if EVENT_TRACING is not defined
    //
    UNREFERENCED_PARAMETER(DriverObject);

    WPP_CLEANUP( WdfDriverWdmGetDriverObject( DriverObject ) );
}

#if !defined(EVENT_TRACING)

VOID
TraceEvents (
    _In_ ULONG DebugPrintLevel,
    _In_ ULONG DebugPrintFlag,
    _Printf_format_string_
    _In_ PCSTR DebugMessage,
    ...
    )
/*++

Routine Description:

    Debug print fallback used when WPP is disabled.

--*/
{
#if DBG
#define     TEMP_BUFFER_SIZE        1024
    va_list    list;
    CHAR       debugMessageBuffer[TEMP_BUFFER_SIZE];
    NTSTATUS   status;

    va_start(list, DebugMessage);

    if (DebugMessage) {

        status = RtlStringCbVPrintfA( debugMessageBuffer,
                                      sizeof(debugMessageBuffer),
                                      DebugMessage,
                                      list );
        if(!NT_SUCCESS(status)) {

            DbgPrint (_DRIVER_NAME_": RtlStringCbVPrintfA failed 0x%x\n", status);
            va_end(list);
            return;
        }
        if (DebugPrintLevel <= TRACE_LEVEL_ERROR ||
            (DebugPrintLevel <= DebugLevel &&
             ((DebugPrintFlag & DebugFlag) == DebugPrintFlag))) {
            DbgPrint("%s %s", _DRIVER_NAME_, debugMessageBuffer);
        }
    }
    va_end(list);

    return;
#else
    UNREFERENCED_PARAMETER(DebugPrintLevel);
    UNREFERENCED_PARAMETER(DebugPrintFlag);
    UNREFERENCED_PARAMETER(DebugMessage);
#endif
}

#endif
