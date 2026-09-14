/*++

Module Name:

    connection.c

Abstract:

    Implementation of the connection object which represents one L2CAP
    channel.

    A WDFOBJECT is created per connection in AapL2capConnectionObjectCreate;
    AAPL2CAP_CONNECTION is maintained as the context of this object. The
    object is parented to the file object so that it goes away with the
    handle.

    Passive dispose is used so that the cleanup callback can wait for the
    disconnect to complete.

    Derived from the WDK bthecho sample (common/lib/connection.c) with the
    server-only continuous reader removed and in-flight transfer tracking
    added.

Environment:

    Kernel mode

--*/

#include "aapl2cap.h"

#if defined(EVENT_TRACING)
#include "connection.tmh"
#endif

#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, AapL2capEvtConnectionObjectCleanup)
#pragma alloc_text (PAGE, AapL2capConnectionObjectRemoteDisconnectSynchronously)
#endif

#pragma warning(push)
#pragma warning(disable:28118) // this callback will run at IRQL=PASSIVE_LEVEL
_Use_decl_annotations_
VOID
AapL2capEvtConnectionObjectCleanup(
    WDFOBJECT  ConnectionObject
    )
/*++

Description:

    Invoked by the framework when the connection object gets deleted
    (explicitly or because the parent file object is deleted).

    Because ExecutionLevel is passive for the connection object this runs
    at passive level and can wait for the disconnect to complete.

--*/
{
    PAAPL2CAP_CONNECTION connection = GetConnectionObjectContext(ConnectionObject);

    PAGED_CODE();

    KeWaitForSingleObject(&connection->DisconnectEvent,
        Executive,
        KernelMode,
        FALSE,
        NULL);

    if (connection->ConnectDisconnectRequest != NULL)
    {
        WdfObjectDelete(connection->ConnectDisconnectRequest);
        connection->ConnectDisconnectRequest = NULL;
    }
}
#pragma warning(pop) // enable 28118 again

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS
AapL2capConnectionObjectInit(
    _In_ WDFOBJECT ConnectionObject,
    _In_ PAAPL2CAP_DEVICE_CONTEXT_HEADER DevCtxHdr
    )
/*++

Description:

    Initializes the connection object context.
    Invoked by AapL2capConnectionObjectCreate.

--*/
{
    NTSTATUS status;
    WDF_OBJECT_ATTRIBUTES attributes;
    PAAPL2CAP_CONNECTION connection = GetConnectionObjectContext(ConnectionObject);

    connection->DevCtxHdr = DevCtxHdr;

    connection->ConnectionState = ConnectionStateInitialized;

    InitializeListHead(&connection->PendingTransfers);

    //
    // Initialize event (signaled: not disconnecting)
    //
    KeInitializeEvent(&connection->DisconnectEvent, NotificationEvent, TRUE);

    //
    // Initialize spinlock
    //
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = ConnectionObject;

    status = WdfSpinLockCreate(
                               &attributes,
                               &connection->ConnectionLock
                               );
    if (!NT_SUCCESS(status))
    {
        goto exit;
    }

    //
    // Create disconnect request
    //
    status = WdfRequestCreate(
        &attributes,
        DevCtxHdr->IoTarget,
        &connection->ConnectDisconnectRequest
        );

    if (!NT_SUCCESS(status))
    {
        goto exit;
    }

exit:
    return status;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS
AapL2capConnectionObjectCreate(
    _In_ PAAPL2CAP_DEVICE_CONTEXT_HEADER DevCtxHdr,
    _In_ WDFOBJECT ParentObject,
    _Out_ WDFOBJECT*  ConnectionObject
    )
/*++

Description:

    Creates a connection object parented to ParentObject (the file object).

--*/
{
    NTSTATUS status;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDFOBJECT connectionObject = NULL;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, AAPL2CAP_CONNECTION);
    attributes.ParentObject = ParentObject;
    attributes.EvtCleanupCallback = AapL2capEvtConnectionObjectCleanup;

    //
    // Passive execution level so that cleanup can wait for the disconnect
    // to complete.
    //
    attributes.ExecutionLevel = WdfExecutionLevelPassive;

    status = WdfObjectCreate(
        &attributes,
        &connectionObject
        );

    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_CONNECT,
            "WdfObjectCreate for connection object failed, Status code %!STATUS!\n", status);

        goto exit;
    }

    status = AapL2capConnectionObjectInit(connectionObject, DevCtxHdr);

    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_CONNECT,
            "Context initialize for connection object failed, ConnectionObject 0x%p, Status code %!STATUS!\n",
            connectionObject,
            status
            );

        goto exit;
    }

    *ConnectionObject = connectionObject;

exit:
    if(!NT_SUCCESS(status) && connectionObject)
    {
        WdfObjectDelete(connectionObject);
    }

    return status;
}

EVT_WDF_REQUEST_COMPLETION_ROUTINE
AapL2capConnectionObjectDisconnectCompletion;

void
AapL2capConnectionObjectDisconnectCompletion(
    _In_ WDFREQUEST  Request,
    _In_ WDFIOTARGET  Target,
    _In_ PWDF_REQUEST_COMPLETION_PARAMS  Params,
    _In_ WDFCONTEXT  Context
    )
/*++

Description:

    Completion routine for the BRB_L2CA_CLOSE_CHANNEL BRB.
    Marks the connection disconnected and signals DisconnectEvent.

--*/
{
    PAAPL2CAP_CONNECTION connection = (PAAPL2CAP_CONNECTION) Context;

    UNREFERENCED_PARAMETER(Request);
    UNREFERENCED_PARAMETER(Target);

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_CONNECT,
        "Close channel completion, status: %!STATUS!", Params->IoStatus.Status);

    WdfSpinLockAcquire(connection->ConnectionLock);
    connection->ConnectionState = ConnectionStateDisconnected;
    WdfSpinLockRelease(connection->ConnectionLock);

    //
    // Disconnect complete, set the event
    //
    KeSetEvent(
        &connection->DisconnectEvent,
        0,
        FALSE
        );
}

_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN
AapL2capConnectionObjectRemoteDisconnect(
    _In_ PAAPL2CAP_DEVICE_CONTEXT_HEADER DevCtxHdr,
    _In_ PAAPL2CAP_CONNECTION Connection
    )
/*++

Description:

    Sends a BRB_L2CA_CLOSE_CHANNEL for the connection and cancels any
    transfer requests still in flight so that their completion routine
    can fail them with STATUS_DEVICE_NOT_CONNECTED.

    Called from file close (passive) and from the bthport indication
    callback on remote disconnect (up to DISPATCH_LEVEL).

Return Value:

    TRUE if this call initiates the disconnect.
    FALSE if the connection was already disconnected / never connected.

--*/
{
    struct _BRB_L2CA_CLOSE_CHANNEL * disconnectBrb;

    WdfSpinLockAcquire(Connection->ConnectionLock);

    if (Connection->ConnectionState == ConnectionStateConnecting)
    {
        //
        // Connect has not completed yet. Mark disconnecting; the connect
        // completion routine will notice and send CLOSE_CHANNEL.
        //
        Connection->ConnectionState = ConnectionStateDisconnecting;

        KeClearEvent(&Connection->DisconnectEvent);

        WdfSpinLockRelease(Connection->ConnectionLock);
        return TRUE;
    }
    else if (Connection->ConnectionState != ConnectionStateConnected)
    {
        //
        // Do nothing if we are not connected
        //
        WdfSpinLockRelease(Connection->ConnectionLock);
        return FALSE;
    }

    Connection->ConnectionState = ConnectionStateDisconnecting;

    //
    // We are now sending the disconnect, so clear the event.
    //
    KeClearEvent(&Connection->DisconnectEvent);

    WdfSpinLockRelease(Connection->ConnectionLock);

    //
    // Fail in-flight reads/writes. bthport normally completes them itself
    // once the channel goes away, but cancelling here guarantees that a
    // ReadFile never hangs after the AirPods drop the channel.
    //
    AapL2capConnectionObjectCancelTransfers(Connection);

    DevCtxHdr->ProfileDrvInterface.BthReuseBrb(&Connection->ConnectDisconnectBrb, BRB_L2CA_CLOSE_CHANNEL);

    disconnectBrb = (struct _BRB_L2CA_CLOSE_CHANNEL *) &(Connection->ConnectDisconnectBrb);

    disconnectBrb->BtAddress = Connection->RemoteAddress;
    disconnectBrb->ChannelHandle = Connection->ChannelHandle;

    //
    // The BRB can fail with STATUS_DEVICE_DISCONNECT if the device is already
    // disconnected, hence we don't assert for success. If the send itself
    // fails the completion routine is not called, so signal the event here.
    //
    if (!NT_SUCCESS(AapL2capSharedSendBrbAsync(
        DevCtxHdr->IoTarget,
        Connection->ConnectDisconnectRequest,
        (PBRB) disconnectBrb,
        sizeof(*disconnectBrb),
        AapL2capConnectionObjectDisconnectCompletion,
        Connection
        )))
    {
        WdfSpinLockAcquire(Connection->ConnectionLock);
        Connection->ConnectionState = ConnectionStateDisconnected;
        WdfSpinLockRelease(Connection->ConnectionLock);

        KeSetEvent(&Connection->DisconnectEvent, 0, FALSE);
    }

    return TRUE;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
AapL2capConnectionObjectRemoteDisconnectSynchronously(
    _In_ PAAPL2CAP_DEVICE_CONTEXT_HEADER DevCtxHdr,
    _In_ PAAPL2CAP_CONNECTION Connection
    )
/*++

Description:

    Disconnects the connection and waits for the disconnect to complete.

--*/
{
    PAGED_CODE();

    AapL2capConnectionObjectRemoteDisconnect(DevCtxHdr, Connection);

    KeWaitForSingleObject(&Connection->DisconnectEvent,
        Executive,
        KernelMode,
        FALSE,
        NULL
        );
}

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS
AapL2capFormatRequestWithBrb(
    _In_ WDFIOTARGET IoTarget,
    _In_ WDFREQUEST Request,
    _In_ PBRB Brb,
    _In_ size_t BrbSize
    )
/*++

Description:

    Formats a WDFREQUEST as IOCTL_INTERNAL_BTH_SUBMIT_BRB carrying Brb.

--*/
{
    NTSTATUS status         = STATUS_SUCCESS;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDFMEMORY memoryArg1    = NULL;

    if (BrbSize <= 0)
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_UTIL,
            "BrbSize has an invalid value: %I64d\n",
            BrbSize
            );

        status = STATUS_INVALID_PARAMETER;

        goto exit;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = Request;

    status = WdfMemoryCreatePreallocated(
        &attributes,
        Brb,
        BrbSize,
        &memoryArg1
        );

    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_UTIL,
            "Creating preallocted memory for Brb 0x%p failed, Request to be formatted 0x%p, "
            "Status code %!STATUS!\n",
            Brb,
            Request,
            status
            );

        goto exit;
    }

    status = WdfIoTargetFormatRequestForInternalIoctlOthers(
        IoTarget,
        Request,
        IOCTL_INTERNAL_BTH_SUBMIT_BRB,
        memoryArg1,
        NULL, //OtherArg1Offset
        NULL, //OtherArg2
        NULL, //OtherArg2Offset
        NULL, //OtherArg4
        NULL  //OtherArg4Offset
        );

    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_UTIL,
            "Formatting request 0x%p with Brb 0x%p failed, Status code %!STATUS!\n",
            Request,
            Brb,
            status
            );

        goto exit;
    }
exit:
    return status;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS
AapL2capConnectionObjectFormatRequestForL2CaTransfer(
    _In_ PAAPL2CAP_CONNECTION Connection,
    _In_ WDFREQUEST Request,
    _In_ struct _BRB_L2CA_ACL_TRANSFER * Brb,
    _In_ WDFMEMORY Memory,
    _In_ ULONG TransferFlags //flags include direction of transfer
    )
/*++

Description:

    Formats a request for an L2CAP ACL transfer on the connection.

Arguments:

    Connection - Connection on which the transfer will be made
    Request - Request to be formatted
    Brb - Caller-provided BRB (from the request context)
    Memory - Memory object which has the buffer for the transfer
    TransferFlags - ACL_TRANSFER_DIRECTION_xxx / ACL_SHORT_TRANSFER_OK

Return Value:

    NTSTATUS Status code. STATUS_DEVICE_NOT_CONNECTED if the channel is
    not (or no longer) open.

--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    size_t bufferSize;

    WdfSpinLockAcquire(Connection->ConnectionLock);

    if(Connection->ConnectionState != ConnectionStateConnected)
    {
        status = STATUS_DEVICE_NOT_CONNECTED;
        WdfSpinLockRelease(Connection->ConnectionLock);
        goto exit;
    }

    WdfSpinLockRelease(Connection->ConnectionLock);

    Connection->DevCtxHdr->ProfileDrvInterface.BthReuseBrb(
        (PBRB)Brb, BRB_L2CA_ACL_TRANSFER
        );

    Brb->BtAddress = Connection->RemoteAddress;
    Brb->BufferMDL = NULL;
    Brb->Buffer = WdfMemoryGetBuffer(Memory, &bufferSize);

    __analysis_assume(bufferSize <= (ULONG)(-1));
    if (bufferSize > (ULONG)(-1))
    {
        status = STATUS_BUFFER_OVERFLOW;

        TraceEvents(TRACE_LEVEL_ERROR, DBG_WRITE,
            "Buffer passed in longer than max ULONG, returning status code %!STATUS!\n", status);

        goto exit;
    }

    Brb->BufferSize = (ULONG) bufferSize;
    Brb->ChannelHandle = Connection->ChannelHandle;
    Brb->TransferFlags = TransferFlags;

    status = AapL2capFormatRequestWithBrb(
        Connection->DevCtxHdr->IoTarget,
        Request,
        (PBRB) Brb,
        sizeof(*Brb)
        );

exit:
    return status;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
AapL2capConnectionObjectTrackTransfer(
    _In_ PAAPL2CAP_CONNECTION Connection,
    _In_ WDFREQUEST Request
    )
/*++

Description:

    Adds a read/write request to the connection's in-flight list.
    Must be called before WdfRequestSend.

--*/
{
    PAAPL2CAP_REQUEST_CONTEXT reqCtx = GetRequestContext(Request);

    reqCtx->Connection = Connection;

    WdfSpinLockAcquire(Connection->ConnectionLock);
    InsertTailList(&Connection->PendingTransfers, &reqCtx->ListEntry);
    reqCtx->Linked = TRUE;
    WdfSpinLockRelease(Connection->ConnectionLock);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
AapL2capConnectionObjectUntrackTransfer(
    _In_ WDFREQUEST Request
    )
/*++

Description:

    Removes a read/write request from the in-flight list. Safe to call
    for requests that were never tracked. Must be called before the
    request is completed.

--*/
{
    PAAPL2CAP_REQUEST_CONTEXT reqCtx = GetRequestContext(Request);
    PAAPL2CAP_CONNECTION connection = reqCtx->Connection;

    if (connection == NULL)
    {
        return;
    }

    WdfSpinLockAcquire(connection->ConnectionLock);
    if (reqCtx->Linked)
    {
        RemoveEntryList(&reqCtx->ListEntry);
        reqCtx->Linked = FALSE;
    }
    WdfSpinLockRelease(connection->ConnectionLock);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
AapL2capConnectionObjectCancelTransfers(
    _In_ PAAPL2CAP_CONNECTION Connection
    )
/*++

Description:

    Requests cancellation of every read/write currently sent to bthport
    on this connection.

    WdfRequestCancelSentRequest may complete the request synchronously
    (bthport's cancel routine -> IoCompleteRequest -> our completion
    routine, which takes ConnectionLock), so it must not be called while
    holding the lock. Each request is therefore referenced under the lock,
    cancelled outside of it and dereferenced afterwards; CancelRequested
    prevents the same request from being picked twice.

--*/
{
    PLIST_ENTRY entry;

    for (;;)
    {
        WDFREQUEST toCancel = NULL;

        WdfSpinLockAcquire(Connection->ConnectionLock);

        for (entry = Connection->PendingTransfers.Flink;
             entry != &Connection->PendingTransfers;
             entry = entry->Flink)
        {
            PAAPL2CAP_REQUEST_CONTEXT reqCtx =
                CONTAINING_RECORD(entry, AAPL2CAP_REQUEST_CONTEXT, ListEntry);

            if (!reqCtx->CancelRequested)
            {
                reqCtx->CancelRequested = TRUE;
                toCancel = (WDFREQUEST) WdfObjectContextGetObject(reqCtx);
                WdfObjectReference(toCancel);
                break;
            }
        }

        WdfSpinLockRelease(Connection->ConnectionLock);

        if (toCancel == NULL)
        {
            break;
        }

        TraceEvents(TRACE_LEVEL_INFORMATION, DBG_CONNECT,
            "Cancelling in-flight transfer request 0x%p", toCancel);

        WdfRequestCancelSentRequest(toCancel);
        WdfObjectDereference(toCancel);
    }
}
