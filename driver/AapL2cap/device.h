/*++

Module Name:

    device.h

Abstract:

    Context and function declarations for the AapL2cap client device.
    Derived from the WDK bthecho sample (bthcli/sys/device.h).

Environment:

    Kernel mode only

--*/

#pragma once

#include "aapl2cap.h"

typedef struct _AAPL2CAP_CLIENT_CONTEXT
{
    //
    // Common context header
    //
    AAPL2CAP_DEVICE_CONTEXT_HEADER  Header;

    //
    // Remote (AirPods) address, obtained from BTHENUM
    //
    BTH_ADDR                        ServerBthAddress;

} AAPL2CAP_CLIENT_CONTEXT, *PAAPL2CAP_CLIENT_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(AAPL2CAP_CLIENT_CONTEXT, GetClientDeviceContext)

typedef struct _AAPL2CAP_CLIENT_FILE_CONTEXT
{
    //
    // Connection (L2CAP channel) opened for this file handle.
    // NULL until the open channel BRB has completed successfully.
    //
    PAAPL2CAP_CONNECTION Connection;

    //
    // Remote PSM (always AAPL2CAP_REMOTE_PSM)
    //
    USHORT               ServerPsm;
} AAPL2CAP_CLIENT_FILE_CONTEXT, *PAAPL2CAP_CLIENT_FILE_CONTEXT;

//
// Context for file objects
//
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(AAPL2CAP_CLIENT_FILE_CONTEXT, GetFileContext);

NTSTATUS
FORCEINLINE
AapL2capContextInit(
    PAAPL2CAP_CLIENT_CONTEXT context,
    WDFDEVICE Device
    )
{
    return AapL2capSharedDeviceContextHeaderInit(&context->Header, Device);
}

EVT_WDF_DRIVER_DEVICE_ADD AapL2capEvtDriverDeviceAdd;

EVT_WDF_DEVICE_SELF_MANAGED_IO_INIT AapL2capEvtDeviceSelfManagedIoInit;

EVT_WDF_DEVICE_FILE_CREATE AapL2capEvtDeviceFileCreate;

EVT_WDF_FILE_CLOSE AapL2capEvtFileClose;

NTSTATUS
AapL2capConnectionStateConnected(
    _In_ WDFFILEOBJECT  FileObject,
    _In_ PAAPL2CAP_CONNECTION Connection
    );
