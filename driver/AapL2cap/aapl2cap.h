/*++

Module Name:

    aapl2cap.h

Abstract:

    Main private header for the AapL2cap driver: common includes, the
    device context header shared by all modules and the helper routines
    used to talk to the Bluetooth profile driver interface.

    Derived from the WDK bthecho sample (common/inc/clisrv.h).

Environment:

    Kernel mode

--*/

#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <initguid.h>
#include <ntstrsafe.h>
#include <bthdef.h>
#include <ntintsafe.h>
#include <bthguid.h>
#include <bthioctl.h>
#include <bthddi.h>

#include "trace.h"
#include "aapl2cap_public.h"

#define POOLTAG_AAPL2CAP 'p2aA'

//
// Device context header (kept separate from the device context so that the
// connection code only depends on this part).
//
typedef struct _AAPL2CAP_DEVICE_CONTEXT_HEADER
{
    //
    // Framework device this context is associated with
    //
    WDFDEVICE Device;

    //
    // Default I/O target (the BTHENUM PDO -> bthport)
    //
    WDFIOTARGET IoTarget;

    //
    // Profile driver interface which contains profile driver DDI
    //
    BTH_PROFILE_DRIVER_INTERFACE ProfileDrvInterface;

    //
    // Local Bluetooth Address
    //
    BTH_ADDR LocalBthAddr;

    //
    // Preallocated request to be reused during initialization phase.
    // Access to this request is not synchronized.
    //
    WDFREQUEST Request;
} AAPL2CAP_DEVICE_CONTEXT_HEADER, *PAAPL2CAP_DEVICE_CONTEXT_HEADER;

#include "connection.h"

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS
AapL2capSharedDeviceContextHeaderInit(
    PAAPL2CAP_DEVICE_CONTEXT_HEADER Header,
    WDFDEVICE Device
    );

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
AapL2capSharedRetrieveLocalInfo(
    _In_ PAAPL2CAP_DEVICE_CONTEXT_HEADER DevCtxHdr
    );

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
AapL2capSharedSendBrbSynchronously(
    _In_ WDFIOTARGET IoTarget,
    _In_ WDFREQUEST Request,
    _In_ PBRB Brb,
    _In_ ULONG BrbSize
    );

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS
AapL2capSharedSendBrbAsync(
    _In_ WDFIOTARGET IoTarget,
    _In_ WDFREQUEST Request,
    _In_ PBRB Brb,
    _In_ size_t BrbSize,
    _In_ PFN_WDF_REQUEST_COMPLETION_ROUTINE ComplRoutine,
    _In_opt_ WDFCONTEXT Context
    );
