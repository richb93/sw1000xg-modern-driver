#include <ntddk.h>
#include <wdf.h>
#include "../hardware/sw1000xg_hw.h"
#include "sw1000xg_assets.generated.h"

#define SWXG_MIN_BAR_LENGTH 0x3FF14u

typedef struct DEVICE_CONTEXT {
    PUCHAR Registers;
    ULONG RegisterLength;
    WDFWAITLOCK HardwareLock;
    swxg_device Core;
    BOOLEAN Initialized;
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, DeviceGetContext)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD SwxgEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE SwxgEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE SwxgEvtReleaseHardware;

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
    swxg_init(&context->Core, io);

    WdfWaitLockAcquire(context->HardwareLock, NULL);
    status = swxg_startup(&context->Core, SwxgGetStartupAssets());
    if (status == SWXG_OK) {
        context->Initialized = TRUE;
        status = STATUS_SUCCESS;
    } else if (status == SWXG_TIMEOUT) {
        status = STATUS_IO_TIMEOUT;
    } else {
        status = STATUS_INVALID_PARAMETER;
    }
    WdfWaitLockRelease(context->HardwareLock);

    if (!NT_SUCCESS(status)) {
        MmUnmapIoSpace(context->Registers, context->RegisterLength);
        context->Registers = NULL;
        context->RegisterLength = 0;
    }
    return status;
}

NTSTATUS SwxgEvtReleaseHardware(WDFDEVICE device,
                               WDFCMRESLIST resourcesTranslated)
{
    PDEVICE_CONTEXT context = DeviceGetContext(device);
    UNREFERENCED_PARAMETER(resourcesTranslated);
    if (context->Registers != NULL) {
        WdfWaitLockAcquire(context->HardwareLock, NULL);
        CoreWrite32(context, SWXG_TRPIF, 0);
        context->Initialized = FALSE;
        WdfWaitLockRelease(context->HardwareLock);
        MmUnmapIoSpace(context->Registers, context->RegisterLength);
        context->Registers = NULL;
        context->RegisterLength = 0;
    }
    return STATUS_SUCCESS;
}
