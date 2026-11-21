# Using the driver in a Hackintosh

The backport builds of ASFireWire run on OpenCore machines ("Hackintoshes") as well as on real Macs. A Hackintosh has no
built-in FireWire, so the controller is a PCIe card, and the firmware, PCI layout and CPU differ from anything Apple ships. This
chapter covers what is specific to that setup. It is based on one tested machine: an Intel B760 board, i7-14700KF, macOS 12
Monterey, TI XIO2213 card, MOTU 828mkII interfaces. Read [INSTALL.md](INSTALL.md) first, and
[README-PCIe-Cards.md](README-PCIe-Cards.md) for the card.

## Requirements

| item | needed | why |
|---|---|---|
| OpenCore | yes | The driver is armed through OpenCore `DeviceProperties` (below) |
| SIP | `csrutil status` reports "disabled" (set with `csr-active-config`) | The release is ad-hoc signed on the machine by `install.sh`; macOS only loads such a driver with SIP off |
| PCIe FireWire card | a single-chip TI XIO2213 is known to work | See [README-PCIe-Cards.md](README-PCIe-Cards.md) |
| VT-d (Intel) | on, if the machine has more than 4 GiB RAM | OHCI DMA is 32-bit; see below |

## Arming the controller

The driver only claims a FireWire controller that carries the property `asfw-armed = <01>`. On a Hackintosh that property comes
from OpenCore's `config.plist` → `DeviceProperties` → `Add` → *the PCI path of the OHCI function*. This is a safety switch:
without the property the driver is not even launched, so removing it, or booting another OpenCore without it, leaves the
controller to Apple's driver (on the macOS versions that still have one) and the machine as if ASFireWire were not installed.

```
sudo ./asfw-opencore-arm.sh --report   # controllers, their OpenCore paths, what the config has
sudo ./asfw-opencore-arm.sh            # arm every controller found (backs up config.plist first)
sudo ./asfw-opencore-arm.sh --disarm
```

Arming also sets `pci-aspm-default = 0` on the card's PCIe bridge: with XIO2213 erratum #2 the system hangs when the link
leaves ASPM L1. A change takes effect at the next boot.

### The PCI path moves

The path looks like `PciRoot(0x0)/Pci(0x1C,0x5)/Pci(0x0,0x0)/Pci(0x0,0x0)`. Its root-port part changes whenever the PCIe layout
changes: adding an NVMe drive or a second card, moving a card, or some BIOS changes. On the test machine it moved three times in
three weeks (`1C,4` → `1C,0` → `1C,5`).

**When the path moves, the driver doesn't start** and the interfaces disappear. Nothing is broken; the new path just isn't armed.
- `--report` shows the controller as `unarmed`, the old path as `no controller at …`, and says that the path has probably moved.
- `sudo ./asfw-opencore-arm.sh --follow` arms the new path; reboot. It acts only in exactly that case (a controller that is
  unarmed, and an armed path with no controller at it), so it never arms a machine that was not armed before or was disarmed.
- The old path stays armed. That is harmless, and useful if the numbering moves back.
- To have it done at boot: `sudo ./asfw-opencore-arm.sh --install-boot-check` installs a LaunchDaemon (`net.asfw.rearm`) that
  runs `--follow` at every boot, logs to `/var/log/asfw/rearm.log`, and tells the logged-in user with a notification when one
  more reboot is needed. `--remove-boot-check` removes it, and so does `install.sh --uninstall`. `--disarm` removes the old
  paths too, so a disarmed machine stays disarmed.

## Memory above 4 GiB: VT-d

OHCI controllers address 32 bits. On an Intel machine with more than 4 GiB RAM, macOS bounces a DriverKit driver's DMA through a
copy buffer unless an IOMMU remaps it. The driver tests for that at start and refuses to start
(`DMA to … is bounced: RAM above 4 GiB and no IOMMU …` in the log).
- Turn **VT-d on** in the BIOS and set `Kernel → Quirks → DisableIoMapper = false` (`asfw-opencore-arm.sh --vtd`). The config
  must not delete the ACPI `DMAR` table.
- `--report` confirms "VT-d active (AppleVTD)".
- The only alternative is limiting RAM with the boot-arg `maxmem=2048`.
- macOS has no AMD IOMMU driver, so on AMD machines only `maxmem` remains. Untested.

## Hybrid Intel CPUs (12th–14th gen)

Nothing here is specific to ASFireWire, but audio at small buffers depends on it. These are observations from the one test
machine.

- **SMBIOS.** On iMac20,1 the Apple frequency vectors use HWP (Speed Shift): with Speed Shift off in the BIOS, XCPM writes
  `IA32_PM_ENABLE` (MSR 0x770) and the kernel panics with a general-protection fault. **iMacPro1,1**'s vectors don't use HWP, and
  turbo worked fully with it (5.45 GHz on one thread) without CPUFriend. A CPUFriend data provider built for another CPU or
  board-id should be removed when the SMBIOS changes.
- **Topology.** macOS has no notion of P- and E-cores; without help it treats all threads as equal cores. CpuTopologyRebuild
  presents the E-cores as extra threads of the P-cores. With 0 E-cores the test machine didn't boot with that kext loaded.
- **Locking the clocks doesn't help.** A fixed all-core ratio with C1E off made overloads far more frequent at 64 frames, and
  raised idle power from 2.5 to 12 W. Disabling the CPU idle states in the BIOS did not reduce the overloads either.

## Audio performance

Ableton Live 12, an aggregate of two 828mkII, i7-14700KF:

| sample rate / buffer | result |
|---|---|
| 96 kHz / 128 | usable: a few overloads per minute, all from the client's own peaks |
| 96 kHz / 64 | glitchy: the client's render plus Core Audio's overhead exceeds the 667 µs cycle |
| 48 kHz / 64 | nearly clean |

- The driver's own work is 2–5 µs per IO call; with the way into the driver process and back, a call costs Core Audio's IO
  thread 4–15 µs. There are two calls per unit and cycle.
- `ioload` (`tools/ioload.c`, build line in its header) measures how much of each cycle a device leaves a client: on the test
  machine about 400 µs of 667 µs for the aggregate, 475 µs for one unit.
- Core Audio labels many overloads `DriverTookTooLong`. That means the client stayed just under its own budget, not that the
  driver was slow. The driver's `[IoTime]` lines (log level `info`) give the time of every call and of the client's render
  between them.

## Diagnostics

- **Log level:** `asfw-config-agent --log-level off|error|warn|info|debug|trace`. Use `warn` (the default) or `off` for normal
  use, `info` for timing figures (`[IoTime]`). `debug` and `trace` cost CPU while audio streams and change the timing.
- **The log:** the driver's lines are under the `kernel` process:
  `log show --last 15m --predicate 'process == "kernel"' | grep net.asfw.driver.dext`.
- **Link quality:** `asfw-config-agent --link-stats` shows CRC errors, missing data blocks and missing cycles per unit, the
  controller's cycle errors and bus resets, and the card's PHY ports with their negotiated speed. At `warn`, an error appears in
  the log as a `[Link]` line when it happens. Old or cheap FireWire cables often fail at S400 without lowering the speed; these
  counters show it. See "Link and cable check" in [INSTALL.md](INSTALL.md).
- **DTrace** works with SIP off. Two rules:
  - Never stop the driver process while it streams: no `sample`, `spindump` or debugger on it. On a card without MSI (the
    XIO2213) the controller's interrupt line is level-triggered and stays disabled while the driver's threads are suspended; if
    other devices share the line, the machine can freeze.
  - Aggregate in the trace rather than printing every event, and set thresholds above one buffer's duration: a heavy trace
    changes the timing it measures.

## Troubleshooting

| symptom | likely cause |
|---|---|
| Interfaces missing after adding or moving hardware | The PCI path moved; `--report`, then `--follow` (above) |
| The driver doesn't start, the log says `DMA to … is bounced` | More than 4 GiB RAM and no IOMMU: VT-d (above) |
| The board won't boot with the card in, after a crash or hard power-off | The XIO2213 has no standby power and keeps state across warm resets. Switch the PSU off at the wall for 30 s |
| Black screen after a verbose boot (`-v`) with an AMD RX 6000 GPU | The hand-over from the verbose console to the GPU driver, not this driver. Boot without `-v`, with `agdpmod=pikera` |
| The machine hangs at boot and the cause is unclear | [DEBUGGING.md](DEBUGGING.md); `--disarm` from another OpenCore or OS takes the driver out of the picture |
