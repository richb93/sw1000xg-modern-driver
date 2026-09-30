#include <ntddk.h>
#include <wdf.h>
#include "../hardware/sw1000xg_hw.h"
#include "../hardware/sw1000xg_trace.h"
#include "sw1000xg_assets.generated.h"

#define SWXG_MIN_BAR_LENGTH 0x3FF14u
#define SWXG_TRACE_CAPACITY 4096u

typedef struct DEVICE_CONTEXT {
    PUCHAR Registers;
    ULONG RegisterLength;
    WDFWAITLOCK HardwareLock;
    swxg_device Core;
    BOOLEAN Initialized;
#if DBG
    /* Startup MMIO trace for comparison with docs/startup-recipe.json. */
    swxg_trace Trace;
    swxg_trace_entry TraceEntries[SWXG_TRACE_CAPACITY];
#endif
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, DeviceGetContext)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD SwxgEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE SwxgEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE SwxgEvtReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY SwxgEvtD0Entry;
EVT_WDF_DEVICE_D0_EXIT SwxgEvtD0Exit;

static uint32_t CoreRead32(void *opaque, uint32_t offset)
{
    PDEVICE_CONTEXT context = opaque;
    NT_ASSERT(context->Registers != NULL);
    NT_ASSERT(offset <= context->RegisterLength - sizeof(ULONG));
    return READ_REGISTER_ULONG((volatile ULONG *)(context->Registers + offset));
}

static void CoreWrite32(void *opaque, uint32_t offset, uint32_t value)
{
    PDEVICE_CONTEXT context = opaque;
    NT_ASSERT(context->Registers != NULL);
    NT_ASSERT(offset <= context->RegisterLength - sizeof(ULONG));
    WRITE_REGISTER_ULONG((volatile ULONG *)(context->Registers + offset), value);
}

static void CoreDelayMs(void *opaque, uint32_t milliseconds)
{
    LARGE_INTEGER interval;
    UNREFERENCED_PARAMETER(opaque);
    NT_ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);
    interval.QuadPart = -((LONGLONG)milliseconds * 10 * 1000);
    (void)KeDelayExecutionThread(KernelMode, FALSE, &interval);
}

#if DBG
/* Prints the trace in the format read by tools/recipe_trace.py. Visible after
 * "ed nt!Kd_IHVDRIVER_Mask 0xF" in the kernel debugger. */
static void SwxgDumpTrace(PDEVICE_CONTEXT context, int result)
{
    size_t i;
    for (i = 0; i < context->Trace.count; ++i) {
        const swxg_trace_entry *e = &context->TraceEntries[i];
        if (e->kind == SWXG_TRACE_WRITE)
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
                       "SWXG W %05X %08X\n", e->offset, e->value);
        else if (e->kind == SWXG_TRACE_READ)
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
                       "SWXG R %05X %08X %u\n", e->offset, e->value,
                       e->repeat);
        else
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
                       "SWXG D %u\n", e->value);
    }
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "SWXG END %d %u\n",
               result, (ULONG)context->Trace.dropped);
}
#endif

NTSTATUS DriverEntry(PDRIVER_OBJECT driverObject, PUNICODE_STRING registryPath)
{
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, SwxgEvtDeviceAdd);
    return WdfDriverCreate(driverObject, registryPath,
                           WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}

NTSTATUS SwxgEvtDeviceAdd(WDFDRIVER driver, PWDFDEVICE_INIT deviceInit)
{
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDFDEVICE device;
    PDEVICE_CONTEXT context;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(driver);

    WdfDeviceInitSetExclusive(deviceInit, TRUE);
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = SwxgEvtPrepareHardware;
    pnp.EvtDeviceReleaseHardware = SwxgEvtReleaseHardware;
    pnp.EvtDeviceD0Entry = SwxgEvtD0Entry;
    pnp.EvtDeviceD0Exit = SwxgEvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(deviceInit, &pnp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DEVICE_CONTEXT);
    status = WdfDeviceCreate(&deviceInit, &attributes, &device);
    if (!NT_SUCCESS(status)) return status;
    context = DeviceGetContext(device);
    RtlZeroMemory(context, sizeof(*context));
    status = WdfWaitLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &context->HardwareLock);
    return status;
}

NTSTATUS SwxgEvtPrepareHardware(WDFDEVICE device, WDFCMRESLIST resourcesRaw,
                               WDFCMRESLIST resourcesTranslated)
{
    PDEVICE_CONTEXT context = DeviceGetContext(device);
    ULONG count = WdfCmResourceListGetCount(resourcesTranslated);
    ULONG i;
    NTSTATUS status = STATUS_DEVICE_CONFIGURATION_ERROR;
    swxg_io io;
    UNREFERENCED_PARAMETER(resourcesRaw);

    for (i = 0; i < count; ++i) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR descriptor =
            WdfCmResourceListGetDescriptor(resourcesTranslated, i);
        if (descriptor != NULL && descriptor->Type == CmResourceTypeMemory &&
            descriptor->u.Memory.Length >= SWXG_MIN_BAR_LENGTH) {
            context->RegisterLength = descriptor->u.Memory.Length;
            context->Registers = MmMapIoSpaceEx(descriptor->u.Memory.Start,
                descriptor->u.Memory.Length, PAGE_READWRITE | PAGE_NOCACHE);
            if (context->Registers == NULL) return STATUS_INSUFFICIENT_RESOURCES;
            status = STATUS_SUCCESS;
            break;
        }
    }
    if (!NT_SUCCESS(status)) return status;

    io.context = context;
    io.read32 = CoreRead32;
    io.write32 = CoreWrite32;
    io.delay_ms = CoreDelayMs;
#if DBG
    swxg_trace_init(&context->Trace, io, context->TraceEntries,
                    SWXG_TRACE_CAPACITY);
    io = swxg_trace_io(&context->Trace);
#endif
    swxg_init(&context->Core, io);
    return STATUS_SUCCESS;
}

/* The card loses its DSP state in D3, so startup runs on every D0 entry,
 * including resume from sleep and hibernate, not only on first start. */
NTSTATUS SwxgEvtD0Entry(WDFDEVICE device, WDF_POWER_DEVICE_STATE previousState)
{
    PDEVICE_CONTEXT context = DeviceGetContext(device);
    NTSTATUS status;
    int result;
    UNREFERENCED_PARAMETER(previousState);

    if (context->Registers == NULL) return STATUS_DEVICE_NOT_READY;

    WdfWaitLockAcquire(context->HardwareLock, NULL);
    /* No ISR is connected: keep every interrupt source disabled. */
    CoreWrite32(context, SWXG_TRPIF, 0);
#if DBG
    context->Trace.count = 0;
    context->Trace.dropped = 0;
#endif
    result = swxg_startup(&context->Core, SwxgGetStartupAssets());
#if DBG
    SwxgDumpTrace(context, result);
#endif
    if (result == SWXG_OK) {
        context->Initialized = TRUE;
        status = STATUS_SUCCESS;
    } else {
        /* Startup may have stopped part-way; leave interrupts disabled. */
        CoreWrite32(context, SWXG_TRPIF, 0);
        context->Initialized = FALSE;
        status = result == SWXG_TIMEOUT ? STATUS_IO_TIMEOUT
                                        : STATUS_INVALID_PARAMETER;
    }
    WdfWaitLockRelease(context->HardwareLock);
    return status;
}

NTSTATUS SwxgEvtD0Exit(WDFDEVICE device, WDF_POWER_DEVICE_STATE targetState)
{
    PDEVICE_CONTEXT context = DeviceGetContext(device);
    UNREFERENCED_PARAMETER(targetState);
    if (context->Registers != NULL) {
        WdfWaitLockAcquire(context->HardwareLock, NULL);
        CoreWrite32(context, SWXG_TRPIF, 0);
        context->Initialized = FALSE;
        WdfWaitLockRelease(context->HardwareLock);
    }
    return STATUS_SUCCESS;
}

NTSTATUS SwxgEvtReleaseHardware(WDFDEVICE device,
                               WDFCMRESLIST resourcesTranslated)
{
    PDEVICE_CONTEXT context = DeviceGetContext(device);
    UNREFERENCED_PARAMETER(resourcesTranslated);
    if (context->Registers != NULL) {
        MmUnmapIoSpace(context->Registers, context->RegisterLength);
        context->Registers = NULL;
        context->RegisterLength = 0;
    }
    return STATUS_SUCCESS;
}
