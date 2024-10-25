/*
 * Author: Rick Wertenbroek
 */
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <unistd.h>

#include <leechcore_device.h>

#define IO_URING_QUEUE_DEPTH 256
#define IO_URING_BLOCK_SZ    4096

/* Almost empty context */
typedef struct tdDEVICE_CONTEXT_GENERIC {
    PLC_CONTEXT ctxLC;
    SIZE_T cb; /* Size of memory region (starts at 0) */
    int fd;
} DEVICE_CONTEXT_GENERIC, *PDEVICE_CONTEXT_GENERIC;

VOID DeviceGENERIC_Read(_In_ PLC_CONTEXT ctxLC, _In_ DWORD cpMEMs, _Inout_ PPMEM_SCATTER ppMEMs)
{
    PDEVICE_CONTEXT_GENERIC ctx = (PDEVICE_CONTEXT_GENERIC)ctxLC->hDevice;
    PMEM_SCATTER pMEM;
    DWORD i;
    int ret = 0;

    for(i = 0; i < cpMEMs; i++) {
        pMEM = ppMEMs[i];
        //lcprintf(ctxLC, "GENERIC: Address of memory to read : %#016llx, #bytes : %u, data in pb : %s\n", pMEM->qwA, pMEM->cb, (pMEM->f ? "yes" : "no"));
        if(pMEM->f || MEM_SCATTER_ADDR_ISINVALID(pMEM)) {
            //lcprintf(ctxLC, "GENERIC: ERROR: pMEM->f is set or invalid address\n");
            continue;
        }
        if(pMEM->qwA + pMEM->cb > ctx->cb) {
            //lcprintf(ctxLC, "GENERIC: ERROR: OOB access\n");
            continue;
        }
        /* Do the actual transfer here e.g., :
        memcpy(pMEM->pb, <source memory base> + pMEM->qwA, pMEM->cb);
        */
        ret = pread(ctx->fd, pMEM->pb, pMEM->cb, pMEM->qwA);

        if(ret < 0) {
            //lcprintf(ctxLC, "GENERIC: ERROR: Cannot read: %s\n", strerror(-ret));
            continue;
        } else {
            pMEM->f = true; /* Successful read */
        }
    }
}

VOID DeviceGENERIC_Write(_In_ PLC_CONTEXT ctxLC, _In_ DWORD cpMEMs, _Inout_ PPMEM_SCATTER ppMEMs)
{
    PDEVICE_CONTEXT_GENERIC ctx = (PDEVICE_CONTEXT_GENERIC)ctxLC->hDevice;
    PMEM_SCATTER pMEM;
    DWORD i;
    int ret = 0;

    for(i = 0; i < cpMEMs; i++) {
        pMEM = ppMEMs[i];
        //lcprintf(ctxLC, "GENERIC: Address of memory to write : %#016llx, #bytes : %u, data in pb : %s\n", pMEM->qwA, pMEM->cb, (pMEM->f ? "yes" : "no"));
        if(pMEM->f || MEM_SCATTER_ADDR_ISINVALID(pMEM)) {
            //lcprintf(ctxLC, "GENERIC: ERROR: pMEM->f is set or invalid address\n");
            continue;
        }
        if(pMEM->qwA + pMEM->cb > ctx->cb) {
            //lcprintf(ctxLC, "GENERIC: ERROR: OOB access\n");
            continue;
        }
        /* Do the actual transfer here e.g., :
        memcpy(<destination memory base> + pMEM->qwA, pMEM->pb, pMEM->cb);
        */
        ret = pwrite(ctx->fd, pMEM->pb, pMEM->cb, pMEM->qwA);

        if(ret < 0) {
            //lcprintf(ctxLC, "GENERIC: ERROR: Cannot write: %s\n", strerror(-ret));
            continue;
        } else {
            pMEM->f = true; /* Successful write */
        }
    }
}

static
VOID DeviceGENERIC_RW_Aggregated(_In_ PLC_CONTEXT ctxLC, _In_ DWORD cpMEMs, _Inout_ PPMEM_SCATTER ppMEMs, _In_ int isWrite)
{
    PDEVICE_CONTEXT_GENERIC ctx = (PDEVICE_CONTEXT_GENERIC)ctxLC->hDevice;
    PMEM_SCATTER pMEM;
    DWORD i;
    int ret = 0;
    BOOL phys_scattered = false;
    BOOL virt_scattered = false;
    QWORD qwLast = ppMEMs[0]->qwA + ppMEMs[0]->cb;
    PBYTE pbLast = ppMEMs[0]->pb + ppMEMs[0]->cb;
    QWORD qwTotalLen = 0;

    /* Check if scatterlist is contiguous */
    for(i = 1; i < cpMEMs; i++) {
        pMEM = ppMEMs[i];
        if(pMEM->f || MEM_SCATTER_ADDR_ISINVALID(pMEM)) {
            //lcprintf(ctxLC, "GENERIC: ERROR: pMEM->f is set or invalid address\n");
            phys_scattered = true;
            break;
        }
        if(pMEM->qwA + pMEM->cb > ctx->cb) {
            //lcprintf(ctxLC, "GENERIC: ERROR: OOB access\n");
            phys_scattered = true;
            break;
        }
        if(qwLast != pMEM->qwA) {
            phys_scattered = true;
            break;
        }
        qwLast = pMEM->qwA + pMEM->cb;
    }

    /* Check if buffers are contiguous */
    for(i = 1; i < cpMEMs; i++) {
        pMEM = ppMEMs[i];
        if(pbLast != pMEM->pb) {
            virt_scattered = true;
            break;
        }
        pbLast = pMEM->pb + pMEM->cb;
    }

    //lcprintf(ctxLC, "GENERIC: phys read is %s virt dest is %s\n", (phys_scattered ? "scattered" : "contiguous"), (virt_scattered ? "scattered" : "contiguous"));

    /* If the transfer is scattered, sequentially read chunks */
    if(phys_scattered || virt_scattered || cpMEMs == 1) {
        if(isWrite) {
            DeviceGENERIC_Write(ctxLC, cpMEMs, ppMEMs);
        } else {
            DeviceGENERIC_Read(ctxLC, cpMEMs, ppMEMs);
        }
        return;
    }

    for(i = 0; i < cpMEMs; i++) {
        pMEM = ppMEMs[i];
        qwTotalLen += pMEM->cb;
    }

    /* Prepare the single read transfer to a big buffer */
    pMEM = ppMEMs[0];
    //lcprintf(ctxLC, "GENERIC: Address of memory to %s : %#016llx, #bytes : %llu\n", (direction == IORING_OP_READ) ? "read" : "write", pMEM->qwA, qwTotalLen);
    if(isWrite) {
        ret = pwrite(ctx->fd, pMEM->pb, qwTotalLen, pMEM->qwA);
    } else {
        ret = pread(ctx->fd, pMEM->pb, qwTotalLen, pMEM->qwA);
    }
    if (ret < 0) {
        //lcprintf(ctxLC, "GENERIC: ERROR: Cannot read/write: %s\n", strerror(-ret));
        return;
    } else {
        for(i = 0; i < cpMEMs; i++) {
                ppMEMs[i]->f = true; /* Successful r/w */
        }
    }
}

VOID DeviceGENERIC_WriteScatterGather(_In_ PLC_CONTEXT ctxLC, _In_ DWORD cpMEMs, _Inout_ PPMEM_SCATTER ppMEMs)
{
    DeviceGENERIC_RW_Aggregated(ctxLC, cpMEMs, ppMEMs, 1);
}

VOID DeviceGENERIC_ReadScatterGather(_In_ PLC_CONTEXT ctxLC, _In_ DWORD cpMEMs, _Inout_ PPMEM_SCATTER ppMEMs)
{
    DeviceGENERIC_RW_Aggregated(ctxLC, cpMEMs, ppMEMs, 0);
}

VOID DeviceGENERIC_Close(_Inout_ PLC_CONTEXT ctxLC)
{
    PDEVICE_CONTEXT_GENERIC ctx = (PDEVICE_CONTEXT_GENERIC)ctxLC->hDevice;

    if(ctx) {
        close(ctx->fd);
        ctxLC->hDevice = 0;
        free(ctx);
    }
}

BOOL DeviceGENERIC_Init(_Inout_ PDEVICE_CONTEXT_GENERIC ctx, _In_ PLC_DEVICE_PARAMETER_ENTRY pDevParameter)
{
    int ret = 0;

    ctx->fd = open(pDevParameter->szValue, O_RDWR | O_SYNC);
    if(ctx->fd < 0) {
        lcprintf(ctx->ctxLC, "GENERIC: ERROR: Cannot open %s\n", pDevParameter->szValue);
        return false;
    }

    return true;
}

_Success_(return) EXPORTED_FUNCTION
BOOL LcPluginCreate(_Inout_ PLC_CONTEXT ctxLC, _Out_opt_ PPLC_CONFIG_ERRORINFO ppLcCreateErrorInfo)
{
    int ret = 0;
    PDEVICE_CONTEXT_GENERIC ctx = NULL;
    PLC_DEVICE_PARAMETER_ENTRY pDevParameter = NULL;
    PLC_DEVICE_PARAMETER_ENTRY pSizeParameter = NULL;

    lcprintf(ctxLC, "DEVICE: GENERIC: Initializing\n");

    /* Sanity checks */
    if(ppLcCreateErrorInfo) { *ppLcCreateErrorInfo = NULL; }
    if(ctxLC->version != LC_CONTEXT_VERSION) { return false; }

    /* Allocate the context */
    ctx = (PDEVICE_CONTEXT_GENERIC)calloc(1, sizeof(DEVICE_CONTEXT_GENERIC));
    if(!ctx) { return false; }
    ctx->ctxLC = ctxLC;

    /* Parse parameters */
    pDevParameter = LcDeviceParameterGet(ctxLC, "dev");

    if(!pDevParameter) {
        lcprintf(ctxLC, "GENERIC: ERROR: Required parameter \"dev\" not given.\n");
        lcprintf(ctxLC, "   Example: generic://dev=<value>\n");
        goto fail;
    }

    lcprintf(ctxLC, "Dev parameter is %s\n", pDevParameter->szValue);

    pSizeParameter = LcDeviceParameterGet(ctxLC, "size");

    if(pSizeParameter) {
        ret = sscanf(pSizeParameter->szValue, "%zu", &ctx->cb);
        if (ret <= 0) {
            lcprintf(ctxLC, "GENERIC: ERROR: Failed to read \"size\" parameter\n");
            goto fail;
        }
    } else {
        ctx->cb = 0x1200000000;
    }

    if(!DeviceGENERIC_Init(ctx, pDevParameter)) { goto fail; }

    /* Assign info and handles for LeechCore */
    ctxLC->hDevice = (HANDLE)ctx;
    ctxLC->fMultiThread = false;
    ctxLC->Config.fVolatile = true;
    ctxLC->pfnClose = DeviceGENERIC_Close;
    ctxLC->pfnReadScatter = DeviceGENERIC_ReadScatterGather;
    ctxLC->pfnWriteScatter = DeviceGENERIC_WriteScatterGather;
    return true;
fail:
    free(ctx);
    return false;
}
