# SW1000XG KMDF diagnostic initializer

This is the first Windows kernel wrapper around the platform-neutral hardware
core. It claims only `PCI\VEN_1073&DEV_1000`, requires a translated memory BAR
large enough to cover `0x3FF13`, and maps it non-cached in
`EvtDevicePrepareHardware`. Startup runs in `EvtDeviceD0Entry`, serialized with a
wait lock, so the card is reinitialized after sleep and hibernate as well as on
first start. `EvtDeviceD0Exit` writes `TRPIF=0` and `EvtDeviceReleaseHardware`
unmaps the BAR.

No interrupt is connected yet, so every interrupt source is kept disabled:
`TRPIF=0` is written before startup, again if startup fails part-way, and on
every D0 exit.

It deliberately exposes no device interface, IOCTL, audio endpoint, or MIDI
endpoint. A user-mode process cannot ask it to read or write arbitrary MMIO.

## Private asset generation

Do not redistribute Yamaha assets. Generate a private source unit from your own
extraction:

```text
python tools/generate_assets.py /path/to/extracted src/kmdf/sw1000xg_assets.generated.c
```

Compile that generated file and do not compile `sw1000xg_assets.placeholder.c`.
The placeholder is useful only for analysis builds and will make hardware
preparation fail safely with `STATUS_INVALID_PARAMETER`.

## Windows build requirements

Open the included `sw1000xg-modern-driver.sln` in Visual Studio with the current
Windows Driver Kit and select x64. The project automatically uses the generated
asset file when it exists, otherwise it uses the safe placeholder. The INF is
development-only and requires a generated catalog and test signature before
installation.

Do not install this on a primary machine. The startup sequence is statically
recovered but not yet validated against a physical card. Use a sacrificial test
host, kernel debugger, test signing, Driver Verifier, and a recoverable boot
configuration.
