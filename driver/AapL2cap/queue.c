/*++

Module Name:

    queue.c

Abstract:

    Read/write functionality for the AapL2cap client.
    Derived from the WDK bthecho sample (bthcli/sys/queue.c).

    ReadFile  -> BRB_L2CA_ACL_TRANSFER, ACL_TRANSFER_DIRECTION_IN |
                 ACL_SHORT_TRANSFER_OK: one ReadFile returns one L2CAP SDU
                 (possibly shorter than the buffer).
    WriteFile -> BRB_L2CA_ACL_TRANSFER, ACL_TRANSFER_DIRECTION_OUT: the
                 whole buffer is sent as one SDU.

    Requests are tracked per connection while in flight so that a remote
    disconnect can cancel them; such cancelled requests complete with
    STATUS_DEVICE_NOT_CONNECTED instead of STATUS_CANCELLED.

Environment:

    Kernel mode only

--*/

#include "aapl2cap.h"
#include "device.h"
#include "queue.h"

#if defined(EVENT_TRACING)
#include "queue.tmh"
#endif

void
AapL2capReadWriteCompletion(
    _In_ WDFREQUEST  Request,
    _In_ WDFIOTARGET  Target,
    _In_ PWDF_REQUEST_COMPLETION_PARAMS  Params,
    _In_ WDFCONTEXT  Context
    )
/*++
Description:

    Completion routine for read/write requests. Context is the transfer
    BRB (part of the request context).

--*/
{
    struct _BRB_L2CA_ACL_TRANSFER *brb;
    PAAPL2CAP_REQUEST_CONTEXT reqCtx;
    PAAPL2CAP_CONNECTION connection;
    NTSTATUS status;
    size_t information;

    UNREFERENCED_PARAMETER(Target);

    brb = (struct _BRB_L2CA_ACL_TRANSFER *) Context;

    NT_ASSERT((brb != NULL));

    reqCtx = GetRequestContext(Request);
    connection = reqCtx->Connection;

    status = Params->IoStatus.Status;

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_READ,
        "I/O completion, request 0x%p, status: %!STATUS!, bytes %d",
        Request, status, brb->BufferSize);

    //
    // Remove from the in-flight list before completing.
    //
    AapL2capConnectionObjectUntrackTransfer(Request);

    //
    // If the transfer was cancelled because the channel went away, report
    // that instead of a bare STATUS_CANCELLED so the application can tell
    // "AirPods disconnected" apart from its own CancelIo.
    //
    if (!NT_SUCCESS(status) && connection != NULL)
    {
        AAPL2CAP_CONNECTION_STATE state;

        WdfSpinLockAcquire(connection->ConnectionLock);
        state = connection->ConnectionState;
        WdfSpinLockRelease(connection->ConnectionLock);

        if (state != ConnectionStateConnected)
        {
            status = STATUS_DEVICE_NOT_CONNECTED;
        }
    }

    //
    // Bytes read/written are contained in Brb->BufferSize
    //
    information = NT_SUCCESS(status) ? brb->BufferSize : 0;

    WdfRequestCompleteWithInformation(
        Request,
        status,
        information
        );
}

static
VOID
AapL2capQueueTransfer(
    _In_ WDFQUEUE  Queue,
    _In_ WDFREQUEST  Request,
    _In_ ULONG  TransferFlags
    )
/*++
Description:

    Common path for read and write: format the request as an ACL transfer
    BRB on the handle's connection and send it to bthport.

--*/
{
    NTSTATUS status;
    PAAPL2CAP_CLIENT_CONTEXT DevCtx;
    WDFMEMORY memory;
    struct _BRB_L2CA_ACL_TRANSFER * brb;
    PAAPL2CAP_CONNECTION connection;
    BOOLEAN tracked = FALSE;

    connection =  GetFileContext(WdfRequestGetFileObject(Request))->Connection;

    DevCtx = GetClientDeviceContext(WdfIoQueueGetDevice(Queue));

    if (connection == NULL)
    {
        status = STATUS_DEVICE_NOT_CONNECTED;

        TraceEvents(TRACE_LEVEL_ERROR, DBG_READ,
            "No connection for request 0x%p, Status code %!STATUS!\n",
            Request, status);

        goto exit;
    }

    if ((TransferFlags & ACL_TRANSFER_DIRECTION_IN) != 0)
    {
        status = WdfRequestRetrieveOutputMemory(Request, &memory);
    }
    else
    {
        status = WdfRequestRetrieveInputMemory(Request, &memory);
    }

    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_READ,
            "WdfRequestRetrieve%sMemory failed, request 0x%p, Status code %!STATUS!\n",
            ((TransferFlags & ACL_TRANSFER_DIRECTION_IN) != 0) ? "Output" : "Input",
            Request,
            status);

        goto exit;
    }

    //
    // Get the BRB from the request context and format the request as an
    // L2CAP transfer. This fails with STATUS_DEVICE_NOT_CONNECTED if the
    // channel is no longer open.
    //
    brb = (struct _BRB_L2CA_ACL_TRANSFER *) &GetRequestContext(Request)->Brb;

    status = AapL2capConnectionObjectFormatRequestForL2CaTransfer(
        connection,
        Request,
        brb,
        memory,
        TransferFlags
        );

    if (!NT_SUCCESS(status))
    {
        goto exit;
    }

    WdfRequestSetCompletionRoutine(
        Request,
        AapL2capReadWriteCompletion,
        brb
        );

    //
    // Track before sending so a disconnect racing with the send still
    // finds the request.
    //
    AapL2capConnectionObjectTrackTransfer(connection, Request);
    tracked = TRUE;

    if (FALSE == WdfRequestSend(
        Request,
        DevCtx->Header.IoTarget,
        NULL
        ))
    {
        status = WdfRequestGetStatus(Request);

        TraceEvents(TRACE_LEVEL_ERROR, DBG_UTIL,
            "Request send failed for request 0x%p, Brb 0x%p, Status code %!STATUS!\n",
            Request,
            brb,
            status
            );

        goto exit;
    }

exit:
    if (!NT_SUCCESS(status))
    {
        if (tracked)
        {
            AapL2capConnectionObjectUntrackTransfer(Request);
        }
        WdfRequestComplete(Request, status);
    }
}

VOID
AapL2capEvtQueueIoWrite(
    _In_ WDFQUEUE  Queue,
    _In_ WDFREQUEST  Request,
    _In_ size_t  Length
    )
/*++
Description:

    Delivers a Write request: send the buffer as one L2CAP SDU.

--*/
{
    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_WRITE,
        "Write request 0x%p, %d bytes", Request, (ULONG) Length);

    if (Length == 0)
    {
        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, 0);
        return;
    }

    AapL2capQueueTransfer(Queue, Request, ACL_TRANSFER_DIRECTION_OUT);
}

VOID
AapL2capEvtQueueIoRead (
    _In_ WDFQUEUE  Queue,
    _In_ WDFREQUEST  Request,
    _In_ size_t  Length
    )
/*++
Description:

    Delivers a Read request: receive one L2CAP SDU (short transfer OK).

--*/
{
    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_READ,
        "Read request 0x%p, %d bytes", Request, (ULONG) Length);

    if (Length == 0)
    {
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }

    AapL2capQueueTransfer(Queue, Request, ACL_TRANSFER_DIRECTION_IN | ACL_SHORT_TRANSFER_OK);
}

VOID
AapL2capEvtQueueIoStop(
    _In_ WDFQUEUE  Queue,
    _In_ WDFREQUEST  Request,
    _In_ ULONG  ActionFlags
    )
/*++
Description:

    Invoked by the framework when the queue is being stopped (e.g. surprise
    removal). Cancels requests owned by the driver so the stop does not
    wait indefinitely.

--*/
{
    UNREFERENCED_PARAMETER(Queue);
    UNREFERENCED_PARAMETER(ActionFlags);

    WdfRequestCancelSentRequest(Request);
}
