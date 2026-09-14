/*++

Module Name:

    client.c

Abstract:

    L2CAP client functionality: profile driver interface query, remote
    address retrieval (IOCTL_INTERNAL_BTHENUM_GET_DEVINFO) and channel
    open to the hardcoded AAP PSM.

    Derived from the WDK bthecho sample (bthcli/sys/client.c).

    Changes vs. the sample:
      - SDP lookup (IOCTL_BTH_SDP_CONNECT / SERVICE_ATTRIBUTE_SEARCH and
        the SDP tree parsing) removed; PSM is AAPL2CAP_REMOTE_PSM.
      - The basic BRB_L2CA_OPEN_CHANNEL is used instead of the enhanced
        variant, so no ERTM / streaming mode is ever proposed to the
        AirPods; basic mode is what an H2-based peer is known to accept.
      - ChannelFlags request an encrypted link (CF_LINK_ENCRYPTED).
      - The remote-disconnect indication cancels in-flight transfers.

Environment:

    Kernel mode only

--*/

#include "aapl2cap.h"
#include "device.h"
#include "client.h"

#if defined(EVENT_TRACING)
#include "client.tmh"
#endif

#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, AapL2capBthQueryInterfaces)
#endif

//
// Channel parameters
//
#define AAPL2CAP_CHANNEL_FLAGS          (CF_ROLE_EITHER | CF_LINK_ENCRYPTED)
#define AAPL2CAP_MTU_MIN                L2CAP_MIN_MTU       // 48
#define AAPL2CAP_MTU_PREFERRED          L2CAP_DEFAULT_MTU   // 672
#define AAPL2CAP_INCOMING_QUEUE_DEPTH   10

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
AapL2capBthQueryInterfaces(
    _In_ PAAPL2CAP_CLIENT_CONTEXT DevCtx
    )
/*++

Description:

    Query the profile driver interface from bthport.

--*/
{
    NTSTATUS status;

    PAGED_CODE();

    status = WdfFdoQueryForInterface(
        DevCtx->Header.Device,
        &GUID_BTHDDI_PROFILE_DRIVER_INTERFACE,
        (PINTERFACE) (&DevCtx->Header.ProfileDrvInterface),
        sizeof(DevCtx->Header.ProfileDrvInterface),
        BTHDDI_PROFILE_DRIVER_INTERFACE_VERSION_FOR_QI,
        NULL
        );

    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP,
            "QueryInterface failed for Interface profile driver interface, version %d, Status code %!STATUS!\n",
            BTHDDI_PROFILE_DRIVER_INTERFACE_VERSION_FOR_QI,
            status);

        goto exit;
    }

exit:
    return status;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
AapL2capRetrieveServerBthAddress(
    _In_ PAAPL2CAP_CLIENT_CONTEXT DevCtx
    )
/*++

Description:

    Retrieve the remote (AirPods) BTH_ADDR from BTHENUM. The PDO we sit on
    was enumerated for exactly one paired remote device, so this is how
    the driver learns which device to connect to.

--*/
{
    NTSTATUS status, statusReuse;
    WDF_MEMORY_DESCRIPTOR outMemDesc;
    WDF_REQUEST_REUSE_PARAMS ReuseParams;
    BTH_DEVICE_INFO serverDeviceInfo;

    WDF_REQUEST_REUSE_PARAMS_INIT(&ReuseParams, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_NOT_SUPPORTED);
    statusReuse = WdfRequestReuse(DevCtx->Header.Request, &ReuseParams);
    NT_ASSERT(NT_SUCCESS(statusReuse));
    UNREFERENCED_PARAMETER(statusReuse);

    RtlZeroMemory( &serverDeviceInfo, sizeof(serverDeviceInfo) );

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(
        &outMemDesc,
        &serverDeviceInfo,
        sizeof(serverDeviceInfo)
        );

    status = WdfIoTargetSendInternalIoctlSynchronously(
        DevCtx->Header.IoTarget,
        DevCtx->Header.Request,
        IOCTL_INTERNAL_BTHENUM_GET_DEVINFO,
        NULL,   //inMemDesc
        &outMemDesc,
        NULL,   //sendOptions
        NULL    //bytesReturned
        );

    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP,
            "Failed to obtain server device info, Status code %!STATUS!\n", status);

        goto exit;
    }

    DevCtx->ServerBthAddress = serverDeviceInfo.address;

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_PNP,
        "Remote BTH_ADDR: %012I64x", DevCtx->ServerBthAddress);

exit:
    return status;
}

EVT_WDF_REQUEST_COMPLETION_ROUTINE
AapL2capRemoteConnectCompletion;

_IRQL_requires_max_(DISPATCH_LEVEL)
void
AapL2capIndicationCallback(
    _In_opt_ PVOID Context,
    _In_ INDICATION_CODE Indication,
    _In_ PINDICATION_PARAMETERS Parameters
    )
/*++

Description:

    Indication callback registered with the open channel BRB. Only
    CALLBACK_DISCONNECT is requested, so IndicationRemoteDisconnect is
    the interesting case: the AirPods (or the link) went away, so we close
    our side, which also cancels any pending reads.

--*/
{
    PAAPL2CAP_CONNECTION connection = (PAAPL2CAP_CONNECTION) Context;

    UNREFERENCED_PARAMETER(Parameters);

    if (connection == NULL)
    {
        return;
    }

    switch(Indication)
    {
        //
        // No references are taken: the connection is scoped within the
        // file object lifetime.
        //
        case IndicationAddReference:
        case IndicationReleaseReference:
            break;
        case IndicationRemoteConnect:
        {
            //
            // We never accept connections
            //
            NT_ASSERT(FALSE);
            break;
        }
        case IndicationRemoteDisconnect:
        {
            TraceEvents(TRACE_LEVEL_INFORMATION, DBG_CONNECT,
                "Remote disconnect indication, Connection 0x%p", connection);

            AapL2capConnectionObjectRemoteDisconnect(
                connection->DevCtxHdr,
                connection
                );

            break;
        }
        default:
            break;
    }
}

VOID
AapL2capRemoteConnectCompletion(
    _In_ WDFREQUEST  Request,
    _In_ WDFIOTARGET  Target,
    _In_ PWDF_REQUEST_COMPLETION_PARAMS  Params,
    _In_ WDFCONTEXT  Context
    )
/*++
Description:

    Completion routine for the Create request which was formatted as an
    open channel BRB. Completes the Create request.

    Context is the BRB (part of the request context, no explicit free).
    The connection is stored in the BRB's ClientContext[0].

--*/
{
    NTSTATUS status;
    struct _BRB_L2CA_OPEN_CHANNEL *brb;
    PAAPL2CAP_CLIENT_CONTEXT DevCtx;
    PAAPL2CAP_CONNECTION connection;
    BOOLEAN disconnectRequested = FALSE;

    DevCtx = GetClientDeviceContext(WdfIoTargetGetDevice(Target));

    status = Params->IoStatus.Status;

    brb = (struct _BRB_L2CA_OPEN_CHANNEL *) Context;

    connection = (PAAPL2CAP_CONNECTION) brb->Hdr.ClientContext[0];

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_CONNECT,
        "Open channel completion, status: %!STATUS!, response 0x%x",
        status, brb->Response);

    WdfSpinLockAcquire(connection->ConnectionLock);

    //
    // A remote-disconnect indication could in theory have arrived before
    // this completion; RemoteDisconnect then left the state at
    // Disconnecting and cleared DisconnectEvent.
    //
    disconnectRequested = (connection->ConnectionState == ConnectionStateDisconnecting);

    if(NT_SUCCESS(status))
    {
        connection->OutMTU = brb->OutResults.Params.Mtu;
        connection->InMTU = brb->InResults.Params.Mtu;
        connection->ChannelHandle = brb->ChannelHandle;
        connection->RemoteAddress = brb->BtAddress;

        connection->ConnectionState = ConnectionStateConnected;
    }
    else
    {
        connection->ConnectionState = ConnectionStateConnectFailed;
    }

    WdfSpinLockRelease(connection->ConnectionLock);

    if (NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, DBG_CONNECT,
            "Channel established, OutMTU %d, InMTU %d",
            connection->OutMTU, connection->InMTU);

        if (disconnectRequested)
        {
            //
            // Channel is already gone from the remote's point of view.
            // Close our side and fail the create.
            //
            AapL2capConnectionObjectRemoteDisconnect(&(DevCtx->Header), connection);
            status = STATUS_DEVICE_NOT_CONNECTED;
        }
        else
        {
            //
            // Post processing in device.c (publish connection in file context)
            //
            status = AapL2capConnectionStateConnected(WdfRequestGetFileObject(Request), connection);
            if (!NT_SUCCESS(status))
            {
                AapL2capConnectionObjectRemoteDisconnect(
                    &(DevCtx->Header),
                    connection
                    );
            }
        }
    }
    else if (disconnectRequested)
    {
        //
        // Nothing to close, but RemoteDisconnect cleared the event.
        //
        KeSetEvent(&connection->DisconnectEvent, 0, FALSE);
    }

    //
    // Complete the Create request. On failure the framework deletes the
    // file object and with it the connection object; its cleanup waits
    // for any close-channel BRB issued above.
    //
    WdfRequestComplete(Request, status);

    return;
}

_IRQL_requires_same_
NTSTATUS
AapL2capOpenRemoteConnection(
    _In_ PAAPL2CAP_CLIENT_CONTEXT DevCtx,
    _In_ WDFFILEOBJECT FileObject,
    _In_ WDFREQUEST Request
    )
/*++

Description:

    Invoked by AapL2capEvtDeviceFileCreate. Creates the connection object
    and sends BRB_L2CA_OPEN_CHANNEL to the AirPods on the AAP PSM using
    the Create request.

--*/
{
    NTSTATUS status;
    WDFOBJECT connectionObject;
    struct _BRB_L2CA_OPEN_CHANNEL *brb = NULL;
    PAAPL2CAP_CONNECTION connection = NULL;
    PAAPL2CAP_CLIENT_FILE_CONTEXT fileCtx = GetFileContext(FileObject);

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_CONNECT,
        "Connect request to %012I64x PSM 0x%04x",
        DevCtx->ServerBthAddress, fileCtx->ServerPsm);

    //
    // Create the connection object (parented to the file object)
    //
    status = AapL2capConnectionObjectCreate(
        &DevCtx->Header,
        FileObject, //parent
        &connectionObject
        );

    if (!NT_SUCCESS(status))
    {
        goto exit;
    }

    connection = GetConnectionObjectContext(connectionObject);

    connection->ConnectionState = ConnectionStateConnecting;

    //
    // Get the BRB from the request context and initialize it as a
    // BRB_L2CA_OPEN_CHANNEL (basic, not enhanced)
    //
    brb = (struct _BRB_L2CA_OPEN_CHANNEL *) &GetRequestContext(Request)->Brb;

    DevCtx->Header.ProfileDrvInterface.BthReuseBrb(
        (PBRB)brb,
        BRB_L2CA_OPEN_CHANNEL
        );

    brb->Hdr.ClientContext[0] = connection;
    brb->BtAddress = DevCtx->ServerBthAddress;
    brb->Psm = fileCtx->ServerPsm;

    //
    // Either role; require an encrypted (and therefore authenticated)
    // baseband link - the AirPods are paired so this is satisfied.
    //
    brb->ChannelFlags = AAPL2CAP_CHANNEL_FLAGS;

    //
    // Outbound config request: the MTU we can receive.
    //
    brb->ConfigOut.Flags = CFG_MTU;
    brb->ConfigOut.Mtu.Min = AAPL2CAP_MTU_MIN;
    brb->ConfigOut.Mtu.Max = AAPL2CAP_MTU_PREFERRED;
    brb->ConfigOut.Mtu.Preferred = AAPL2CAP_MTU_PREFERRED;

    //
    // Inbound config validation: what we accept as the remote's receive
    // MTU. Allow anything from the minimum up to the L2CAP maximum so a
    // peer asking for a large MTU is not rejected.
    //
    brb->ConfigIn.Flags = CFG_MTU;
    brb->ConfigIn.Mtu.Min = AAPL2CAP_MTU_MIN;
    brb->ConfigIn.Mtu.Max = 0xFFFF;
    brb->ConfigIn.Mtu.Preferred = AAPL2CAP_MTU_PREFERRED;

    //
    // Get notification about remote disconnect
    //
    brb->CallbackFlags = CALLBACK_DISCONNECT;

    brb->Callback = &AapL2capIndicationCallback;
    brb->CallbackContext = connection;
    brb->ReferenceObject = (PVOID) WdfDeviceWdmGetDeviceObject(DevCtx->Header.Device);
    brb->IncomingQueueDepth = AAPL2CAP_INCOMING_QUEUE_DEPTH;

    status = AapL2capSharedSendBrbAsync(
        DevCtx->Header.IoTarget,
        Request,
        (PBRB) brb,
        sizeof(*brb),
        AapL2capRemoteConnectCompletion,
        brb    //Context
        );

    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_CONNECT,
            "Sending brb for opening connection failed, returning status code %!STATUS!\n", status);

        goto exit;
    }

exit:

    if(!NT_SUCCESS(status))
    {
        if (connection)
        {
            //
            // Set the right state to facilitate debugging
            //
            connection->ConnectionState = ConnectionStateConnectFailed;
        }

        //
        // Failing Create deletes the file object and with it the
        // connection object.
        //
    }

    return status;
}
