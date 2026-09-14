/*++

Module Name:

    client.h

Abstract:

    Declarations for the L2CAP client side (query interfaces, remote
    address retrieval, channel open).

Environment:

    Kernel mode only

--*/

#pragma once

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
AapL2capBthQueryInterfaces(
    _In_ PAAPL2CAP_CLIENT_CONTEXT DevCtx
    );

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
AapL2capRetrieveServerBthAddress(
    _In_ PAAPL2CAP_CLIENT_CONTEXT DevCtx
    );

_IRQL_requires_same_
NTSTATUS
AapL2capOpenRemoteConnection(
    _In_ PAAPL2CAP_CLIENT_CONTEXT DevCtx,
    _In_ WDFFILEOBJECT FileObject,
    _In_ WDFREQUEST Request
    );
