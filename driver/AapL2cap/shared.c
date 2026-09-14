/*++

Module Name:

    shared.c

Abstract:

    Helper routines for talking to bthport through the BTHENUM PDO:
    context header init, local address retrieval and BRB submission.

    Derived from the WDK bthecho sample (common/lib/clisrv.c). The
    IOCTL_BTH_GET_HOST_SUPPORTED_FEATURES query was dropped because the
    driver no longer negotiates ERTM.

Environment:

    Kernel mode

--*/

#include "aapl2cap.h"

#if defined(EVENT_TRACING)
#include "shared.tmh"
#endif

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS
AapL2capSharedDeviceContextHeaderInit(
    PAAPL2CAP_DEVICE_CONTEXT_HEADER Header,
    WDFDEVICE Device
    )
/*++

Description:

    Initializes the device context header and preallocates the request
    used during initialization.

--*/
{
    NTSTATUS status;
    WDF_OBJECT_ATTRIBUTES attributes;

    Header->Device = Device;

    Header->IoTarget = WdfDeviceGetIoTarget(Device);

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = Device;

    status = WdfRequestCreate(
        &attributes,
        Header->IoTarget,
        &Header->Request
        );

    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP,
            "Failed to pre-allocate request in device context, Status code %!STATUS!\n", status);

        goto exit;
    }

exit:
    return status;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
AapL2capSharedRetrieveLocalInfo(
    _In_ PAAPL2CAP_DEVICE_CONTEXT_HEADER DevCtxHdr
    )
/*++

Description:

    Retrieves the local Bluetooth radio address (informational only).

--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    struct _BRB_GET_LOCAL_BD_ADDR * brb = NULL;

    brb = (struct _BRB_GET_LOCAL_BD_ADDR *)
        DevCtxHdr->ProfileDrvInterface.BthAllocateBrb(
            BRB_HCI_GET_LOCAL_BD_ADDR,
            POOLTAG_AAPL2CAP
            );

    if(brb == NULL)
    {
        status = STATUS_INSUFFICIENT_RESOURCES;

        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP,
            "Failed to allocate brb BRB_HCI_GET_LOCAL_BD_ADDR, returning status code %!STATUS!\n", status);

        goto exit;
    }

    status = AapL2capSharedSendBrbSynchronously(
        DevCtxHdr->IoTarget,
        DevCtxHdr->Request,
        (PBRB) brb,
        sizeof(*brb)
        );

    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP,
            "Retrieving local bth address failed, Status code %!STATUS!\n", status);

        goto exit1;
    }

    DevCtxHdr->LocalBthAddr = brb->BtAddress;

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_PNP,
        "Local BTH_ADDR: %012I64x", DevCtxHdr->LocalBthAddr);

exit1:
    DevCtxHdr->ProfileDrvInterface.BthFreeBrb((PBRB)brb);
exit:
    return status;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS
AapL2capSharedSendBrbAsync(
    _In_ WDFIOTARGET IoTarget,
    _In_ WDFREQUEST Request,
    _In_ PBRB Brb,
    _In_ size_t BrbSize,
    _In_ PFN_WDF_REQUEST_COMPLETION_ROUTINE ComplRoutine,
    _In_opt_ WDFCONTEXT Context
    )
/*++

Routine Description:

    Formats a request with a BRB and sends it asynchronously.

Return Value:

    Success implies that the request was sent and the completion routine
    will be called; failure implies it was not sent and the caller must
    complete the request.

Notes:

    Does not call WdfRequestReuse on Request; the caller must do so
    when reusing a request.

--*/
{
    NTSTATUS status         = STATUS_SUCCESS;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDFMEMORY memoryArg1    = NULL;

    if (BrbSize <= 0)
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_UTIL,
            "BrbSize has invalid value: %I64d\n",
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

    WdfRequestSetCompletionRoutine(
        Request,
        ComplRoutine,
        Context
        );

    if (FALSE == WdfRequestSend(
        Request,
        IoTarget,
        NULL
        ))
    {
        status = WdfRequestGetStatus(Request);

        TraceEvents(TRACE_LEVEL_ERROR, DBG_UTIL,
            "Request send failed for request 0x%p, Brb 0x%p, Status code %!STATUS!\n",
            Request,
            Brb,
            status
            );

        goto exit;
    }

exit:
    return status;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
AapL2capSharedSendBrbSynchronously(
    _In_ WDFIOTARGET IoTarget,
    _In_ WDFREQUEST Request,
    _In_ PBRB Brb,
    _In_ ULONG BrbSize
    )
/*++

Routine Description:

    Formats a request with a BRB and sends it synchronously.
    Calls WdfRequestReuse on Request first.

--*/
{
    NTSTATUS status;
    WDF_REQUEST_REUSE_PARAMS reuseParams;
    WDF_MEMORY_DESCRIPTOR OtherArg1Desc;

    WDF_REQUEST_REUSE_PARAMS_INIT(
        &reuseParams,
        WDF_REQUEST_REUSE_NO_FLAGS,
        STATUS_NOT_SUPPORTED
        );

    status = WdfRequestReuse(Request, &reuseParams);
    if (!NT_SUCCESS(status))
    {
        goto exit;
    }

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(
                        &OtherArg1Desc,
                        Brb,
                        BrbSize
                        );

    status = WdfIoTargetSendInternalIoctlOthersSynchronously(
        IoTarget,
        Request,
        IOCTL_INTERNAL_BTH_SUBMIT_BRB,
        &OtherArg1Desc,
        NULL, //OtherArg2
        NULL, //OtherArg4
        NULL, //RequestOptions
        NULL  //BytesReturned
        );

exit:
    return status;
}
