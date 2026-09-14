/*++

Module Name:

    queue.h

Abstract:

    Queue events for the AapL2cap client.

Environment:

    Kernel mode only

--*/

#pragma once

EVT_WDF_IO_QUEUE_IO_READ AapL2capEvtQueueIoRead;

EVT_WDF_IO_QUEUE_IO_WRITE AapL2capEvtQueueIoWrite;

EVT_WDF_IO_QUEUE_IO_STOP AapL2capEvtQueueIoStop;

EVT_WDF_REQUEST_COMPLETION_ROUTINE AapL2capReadWriteCompletion;
