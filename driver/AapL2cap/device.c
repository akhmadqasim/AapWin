/*++

Module Name:

    device.c

Abstract:

    Device and file object related functionality for the AapL2cap client
    device. Derived from the WDK bthecho sample (bthcli/sys/device.c).

    Changes vs. the sample:
      - no SDP lookup on file create; the PSM is hardcoded (0x1001)
      - the request context is AAPL2CAP_REQUEST_CONTEXT (BRB + tracking)
      - EvtFileClose tolerates a NULL connection

Environment:

    Kernel mode only

--*/

#include "aapl2cap.h"
#include "device.h"
#include "client.h"
#include "queue.h"

#if defined(EVENT_TRACING)
#include "device.tmh"
#endif

#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, AapL2capEvtDriverDeviceAdd)
#pragma alloc_text (PAGE, AapL2capEvtDeviceFileCreate)
#pragma alloc_text (PAGE, AapL2capEvtFileClose)
#endif

NTSTATUS
AapL2capEvtDriverDeviceAdd(
    _In_ WDFDRIVER  Driver,
    _Inout_ PWDFDEVICE_INIT  DeviceInit
)
/*++
Routine Description:

    Called by the framework in response to AddDevice from the PnP manager.
    Creates the FDO on top of the BTHENUM PDO, sets up file/request
    contexts, the default I/O queue and the device interface.

--*/
{
    NTSTATUS                        status;
    WDFDEVICE                       device;
    WDF_OBJECT_ATTRIBUTES           deviceAttributes;
    WDF_PNPPOWER_EVENT_CALLBACKS    pnpPowerCallbacks;
    WDF_FILEOBJECT_CONFIG           fileobjectConfig;
    WDF_OBJECT_ATTRIBUTES           fileAttributes, requestAttributes;
    WDF_IO_QUEUE_CONFIG             ioQueueConfig;
    WDFQUEUE                        queue;
    PAAPL2CAP_CLIENT_CONTEXT        devCtx;

    UNREFERENCED_PARAMETER(Driver);

    PAGED_CODE();

    //
    // Configure Pnp/power callbacks
    //
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPowerCallbacks);
    pnpPowerCallbacks.EvtDeviceSelfManagedIoInit = AapL2capEvtDeviceSelfManagedIoInit;

    WdfDeviceInitSetPnpPowerEventCallbacks(
       DeviceInit,
       &pnpPowerCallbacks
       );

    //
    // Configure file callbacks
    //
    WDF_FILEOBJECT_CONFIG_INIT(
        &fileobjectConfig,
        AapL2capEvtDeviceFileCreate,
        AapL2capEvtFileClose,
        WDF_NO_EVENT_CALLBACK // Cleanup
        );

    //
    // Per-handle context
    //
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&fileAttributes, AAPL2CAP_CLIENT_FILE_CONTEXT);

    WdfDeviceInitSetFileObjectConfig(
        DeviceInit,
        &fileobjectConfig,
        &fileAttributes
        );

    //
    // Per-request context: holds the BRB for Create/Read/Write plus the
    // in-flight tracking fields.
    //
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(
        &requestAttributes,
        AAPL2CAP_REQUEST_CONTEXT
        );

    WdfDeviceInitSetRequestAttributes(
        DeviceInit,
        &requestAttributes
        );

    //
    // Set device attributes
    //
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, AAPL2CAP_CLIENT_CONTEXT);

    status = WdfDeviceCreate(
        &DeviceInit,
        &deviceAttributes,
        &device
        );

    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP,
            "WdfDeviceCreate failed with Status code %!STATUS!\n", status);

        goto exit;
    }

    devCtx = GetClientDeviceContext(device);

    status = AapL2capContextInit(devCtx, device);

    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP,
            "Initialization of context failed with Status code %!STATUS!\n", status);

        goto exit;
    }

    status = AapL2capBthQueryInterfaces(devCtx);
    if (!NT_SUCCESS(status))
    {
        goto exit;
    }

    //
    // Default queue: parallel, read/write only. IOCTLs are not supported
    // and will be failed by the framework.
    //
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(
        &ioQueueConfig,
        WdfIoQueueDispatchParallel
        );

    ioQueueConfig.EvtIoRead     = AapL2capEvtQueueIoRead;
    ioQueueConfig.EvtIoWrite    = AapL2capEvtQueueIoWrite;
    ioQueueConfig.EvtIoStop     = AapL2capEvtQueueIoStop;

    status = WdfIoQueueCreate(
        device,
        &ioQueueConfig,
        WDF_NO_OBJECT_ATTRIBUTES,
        &queue
        );

    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP,
                            "WdfIoQueueCreate failed  %!STATUS!\n", status);
        goto exit;
    }

    //
    // Enable the device interface so that aapctl can open a handle.
    //
    status = WdfDeviceCreateDeviceInterface(
        device,
        &AAPL2CAP_DEVICE_INTERFACE,
        NULL
        );

    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP,
            "Enabling device interface failed with Status code %!STATUS!\n", status);

        goto exit;
    }

exit:
    //
    // On failure the framework deletes the device and all child objects.
    //
    return status;
}

NTSTATUS
AapL2capEvtDeviceSelfManagedIoInit(
    _In_ WDFDEVICE  Device
    )
/*++

Description:

    Called once by the framework; used for one-time initialization.

    The local radio address and the remote (AirPods) address do not
    change, so they are retrieved here. Retrieving the remote address does
    not require the AirPods to be in range: BTHENUM remembers it from
    pairing.

--*/
{
    NTSTATUS status;
    PAAPL2CAP_CLIENT_CONTEXT devCtx = GetClientDeviceContext(Device);

    status = AapL2capSharedRetrieveLocalInfo(&devCtx->Header);
    if (!NT_SUCCESS(status))
    {
        goto exit;
    }

    status = AapL2capRetrieveServerBthAddress(devCtx);
    if (!NT_SUCCESS(status))
    {
        goto exit;
    }

exit:
    return status;
}

NTSTATUS
AapL2capConnectionStateConnected(
    _In_ WDFFILEOBJECT  FileObject,
    _In_ PAAPL2CAP_CONNECTION Connection
    )
/*++
Description:

    Invoked by AapL2capRemoteConnectCompletion (client.c) once the channel
    is open. Publishes the connection in the file context.

--*/
{
    GetFileContext(FileObject)->Connection = Connection;

    return STATUS_SUCCESS;
}

VOID
AapL2capEvtDeviceFileCreate(
    _In_ WDFDEVICE  Device,
    _In_ WDFREQUEST  Request,
    _In_ WDFFILEOBJECT  FileObject
    )
/*++
Description:

    Invoked by the framework when an application opens a handle to our
    device. Opens an L2CAP channel to the hardcoded AAP PSM; the Create
    request is completed by the open channel completion routine.

--*/
{
    NTSTATUS status;
    PAAPL2CAP_CLIENT_CONTEXT devCtx = GetClientDeviceContext(Device);
    PAAPL2CAP_CLIENT_FILE_CONTEXT fileCtx = GetFileContext(FileObject);

    PAGED_CODE();

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_CREATE_CLOSE,
        "File create, FileObject 0x%p", FileObject);

    fileCtx->Connection = NULL;
    fileCtx->ServerPsm = AAPL2CAP_REMOTE_PSM;

    if (devCtx->ServerBthAddress == 0)
    {
        //
        // SelfManagedIoInit did not manage to obtain the remote address.
        //
        status = STATUS_DEVICE_NOT_READY;

        TraceEvents(TRACE_LEVEL_ERROR, DBG_CREATE_CLOSE,
            "Remote BTH_ADDR unknown, failing create with %!STATUS!", status);

        goto exit;
    }

    //
    // Open remote connection. The Create request itself is formatted with
    // the open channel BRB, so cancellation of Create propagates to the
    // BRB.
    //
    status = AapL2capOpenRemoteConnection(
        devCtx,
        FileObject,
        Request
        );

exit:
    //
    // On failure complete the request here; on success the completion
    // routine completes it.
    //
    if (!NT_SUCCESS(status))
    {
        WdfRequestComplete(Request, status);
    }
}

VOID
AapL2capEvtFileClose(
    _In_ WDFFILEOBJECT  FileObject
    )
/*++
Description:

    Invoked by the framework when the I/O manager sends IRP_MJ_CLOSE for a
    file. Closes the L2CAP channel synchronously (passive level).

--*/
{
    PAAPL2CAP_CLIENT_CONTEXT devCtx;
    PAAPL2CAP_CONNECTION connection;

    PAGED_CODE();

    devCtx = GetClientDeviceContext(WdfFileObjectGetDevice(FileObject));

    connection =  GetFileContext(FileObject)->Connection;

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_CREATE_CLOSE,
        "File close, FileObject 0x%p, Connection 0x%p", FileObject, connection);

    if (connection == NULL)
    {
        //
        // Channel was never established for this handle.
        //
        return;
    }

    AapL2capConnectionObjectRemoteDisconnectSynchronously(
        &(devCtx->Header),
        connection
        );
}
