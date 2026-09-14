/*++

Module Name:

    aapl2cap_public.h

Abstract:

    Definitions shared between the AapL2cap KMDF driver and user-mode
    clients (aapctl). The driver exposes one device interface per
    "AAP Server" BTHENUM PDO; opening a handle on that interface opens
    an L2CAP channel to PSM 0x1001 (Apple Accessory Protocol) on the
    remote AirPods.

    Usage model:
        - CreateFile(interface path, GENERIC_READ|GENERIC_WRITE, ...)
          -> BRB_L2CA_OPEN_CHANNEL to PSM AAPL2CAP_REMOTE_PSM
        - WriteFile  -> one L2CAP SDU (BRB_L2CA_ACL_TRANSFER OUT)
        - ReadFile   -> one L2CAP SDU (BRB_L2CA_ACL_TRANSFER IN, short OK)
        - CloseHandle -> BRB_L2CA_CLOSE_CHANNEL

--*/

#pragma once

//
// Device interface exposed by the AapL2cap client device.
// {0907FADE-E50E-4873-B132-D1BE9BDDAC55}
//
DEFINE_GUID(AAPL2CAP_DEVICE_INTERFACE,
    0x0907fade, 0xe50e, 0x4873, 0xb1, 0x32, 0xd1, 0xbe, 0x9b, 0xdd, 0xac, 0x55);

//
// Remote L2CAP PSM used by the Apple Accessory Protocol.
//
#define AAPL2CAP_REMOTE_PSM             0x1001

//
// Largest L2CAP SDU the driver negotiates for the inbound direction.
// A ReadFile buffer of at least this size guarantees that no SDU is
// truncated.
//
#define AAPL2CAP_MAX_SDU                672
