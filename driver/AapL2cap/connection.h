/*++

Module Name:

    connection.h

Abstract:

    Declaration of the connection object which represents one L2CAP
    channel to the AirPods (one per open file handle).

    Derived from the WDK bthecho sample (common/inc/connection.h); the
    server-only continuous reader was removed and a list of in-flight
    transfer requests was added so that a remote disconnect can fail
    pending reads instead of leaving them hanging.

Environment:

    Kernel mode

--*/

#pragma once

typedef struct _AAPL2CAP_CONNECTION * PAAPL2CAP_CONNECTION;

//
// Connection state
//
typedef enum _AAPL2CAP_CONNECTION_STATE {
    ConnectionStateUnitialized = 0,
    ConnectionStateInitialized,
    ConnectionStateConnecting,
    ConnectionStateConnected,
    ConnectionStateConnectFailed,
    ConnectionStateDisconnecting,
    ConnectionStateDisconnected
} AAPL2CAP_CONNECTION_STATE, *PAAPL2CAP_CONNECTION_STATE;

//
// Connection data structure for one L2CAP channel
//
typedef struct _AAPL2CAP_CONNECTION {

    PAAPL2CAP_DEVICE_CONTEXT_HEADER         DevCtxHdr;

    AAPL2CAP_CONNECTION_STATE               ConnectionState;

    //
    // Connection lock, used to synchronize access to this structure
    // and to the PendingTransfers list.
    //
    WDFSPINLOCK                             ConnectionLock;

    USHORT                                  OutMTU;
    USHORT                                  InMTU;

    L2CAP_CHANNEL_HANDLE                    ChannelHandle;
    BTH_ADDR                                RemoteAddress;

    //
    // Preallocated Brb, Request used for disconnect
    //
    struct _BRB                             ConnectDisconnectBrb;
    WDFREQUEST                              ConnectDisconnectRequest;

    //
    // Event used to wait for disconnection.
    // It is non-signaled while the connection is in the
    // ConnectionStateDisconnecting transitionary state and signaled otherwise.
    //
    KEVENT                                  DisconnectEvent;

    //
    // Read/write requests currently sent down to bthport for this channel.
    // Protected by ConnectionLock. Entries are AAPL2CAP_REQUEST_CONTEXT.
    //
    LIST_ENTRY                              PendingTransfers;

} AAPL2CAP_CONNECTION, *PAAPL2CAP_CONNECTION;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(AAPL2CAP_CONNECTION, GetConnectionObjectContext)

//
// Context attached to every WDFREQUEST delivered to this driver
// (Create, Read, Write). The BRB lives here so that no per-request
// allocation is needed; the list entry tracks in-flight transfers.
//
typedef struct _AAPL2CAP_REQUEST_CONTEXT {

    //
    // BRB used for this request (open channel or ACL transfer)
    //
    BRB                     Brb;

    //
    // Link in AAPL2CAP_CONNECTION::PendingTransfers (read/write only)
    //
    LIST_ENTRY              ListEntry;

    //
    // Connection this transfer belongs to (read/write only)
    //
    PAAPL2CAP_CONNECTION    Connection;

    //
    // TRUE while ListEntry is linked into PendingTransfers
    //
    BOOLEAN                 Linked;

    //
    // TRUE once AapL2capConnectionObjectCancelTransfers has picked this
    // request (avoids cancelling the same request twice)
    //
    BOOLEAN                 CancelRequested;

} AAPL2CAP_REQUEST_CONTEXT, *PAAPL2CAP_REQUEST_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(AAPL2CAP_REQUEST_CONTEXT, GetRequestContext)

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS
AapL2capConnectionObjectCreate(
    _In_ PAAPL2CAP_DEVICE_CONTEXT_HEADER DevCtxHdr,
    _In_ WDFOBJECT ParentObject,
    _Out_ WDFOBJECT*  ConnectionObject
    );

_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN
AapL2capConnectionObjectRemoteDisconnect(
    _In_ PAAPL2CAP_DEVICE_CONTEXT_HEADER DevCtxHdr,
    _In_ PAAPL2CAP_CONNECTION Connection
    );

_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
AapL2capConnectionObjectRemoteDisconnectSynchronously(
    _In_ PAAPL2CAP_DEVICE_CONTEXT_HEADER DevCtxHdr,
    _In_ PAAPL2CAP_CONNECTION Connection
    );

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS
AapL2capConnectionObjectFormatRequestForL2CaTransfer(
    _In_ PAAPL2CAP_CONNECTION Connection,
    _In_ WDFREQUEST Request,
    _In_ struct _BRB_L2CA_ACL_TRANSFER * Brb,
    _In_ WDFMEMORY Memory,
    _In_ ULONG TransferFlags //flags include direction of transfer
    );

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
AapL2capConnectionObjectTrackTransfer(
    _In_ PAAPL2CAP_CONNECTION Connection,
    _In_ WDFREQUEST Request
    );

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
AapL2capConnectionObjectUntrackTransfer(
    _In_ WDFREQUEST Request
    );

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
AapL2capConnectionObjectCancelTransfers(
    _In_ PAAPL2CAP_CONNECTION Connection
    );

EVT_WDF_OBJECT_CONTEXT_CLEANUP AapL2capEvtConnectionObjectCleanup;
