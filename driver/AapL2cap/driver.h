/*++

Module Name:

    driver.h

Abstract:

    Driver object related declarations for the AapL2cap driver.

Environment:

    Kernel mode only

--*/

#pragma once

DRIVER_INITIALIZE DriverEntry;

EVT_WDF_DRIVER_DEVICE_ADD AapL2capEvtDriverDeviceAdd;

EVT_WDF_OBJECT_CONTEXT_CLEANUP AapL2capEvtDriverCleanup;
