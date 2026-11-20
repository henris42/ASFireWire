# Installing the ASFireWire backport

These builds are for macOS 12 Monterey up to Tahoe, on Macs where you control the
security settings, typically OpenCore. They are unsigned and experimental. **Know how
to recover (see the end) before you install.**

Each build comes as `ASFW-<version>-<macos>-b<build>.zip` and contains:

| File | What it is |
|---|---|
| `ASFW.app` | The app, with the driver (`net.asfw.driver.dext`) inside it. On Monterey/Ventura it is the **lite** ASFW app: configuration and diagnostics (install, bus inspector, controller status, AV/C and DICE reports, logs, logging settings, Core Audio, Duet) without the Metal audio analyzer, meters, DV capture and MCP; on Sonoma and later it is the full app |
| `install.sh` | Installs, updates, checks and removes it |
| `asfw-opencore-arm.sh` | Arms or disarms the FireWire controller in OpenCore's `config.plist` (see below) |
| `Entitlements/` | Used by `install.sh` to sign the app and driver |
| `BUILDINFO.txt` | Which source, macOS target and options this build was made from |

## Before the first install

1. **SIP must report "disabled".** Run `csrutil status`. A custom OpenCore
   `csr-active-config` that prints the breakdown with "disabled" lines is enough.
2. **Log in to the desktop.** Driver activation needs the logged-in user's session.
   Use Terminal on the Mac itself, not a bare ssh session (see "Over ssh" below).
3. **Arm the controller.** These builds will not touch the FireWire controller unless
   OpenCore gives it an `asfw-armed` property with the one-byte value `01`. Without
   it the driver's personality does not even match the controller: the driver
   installs and activates, but is never launched, and Apple's driver takes the card
   as if ASFW were not there (`ioreg -rc IOUserService | grep -i asfw` prints
   nothing). That is a good first test. The driver also checks the property again
   in `Start()`, so a build that somehow matched anyway still refuses to run.
   - **With the script** (in the ZIP; `install.sh` offers it at the end on an OpenCore
     machine): `sudo ./asfw-opencore-arm.sh --report` lists every FireWire controller,
     its OpenCore path and the config state; `sudo ./asfw-opencore-arm.sh` arms them in
     the boot disk's `EFI/OC/config.plist`, after a dated backup next to it. `--disarm`
     undoes it, `--dry-run` shows the change, `--esp diskXsY` picks another EFI partition.
   - **By hand:** find the PCI path of the FireWire controller, for example with
     [gfxutil](https://github.com/acidanthera/gfxutil): `gfxutil | grep -i 104c:8025`
     for the TI XIO2213B. It sits behind the card's bridge, so expect something like
     `PciRoot(0x0)/Pci(0x1C,0x4)/Pci(0x0,0x0)/Pci(0x0,0x0)`. In OpenCore `config.plist`,
     under `DeviceProperties` > `Add` > *that path*, add the key `asfw-armed`, type
     **Data**, value exactly `01` (one byte; `0001` or a string does not arm). Reboot.
   - Check: `ioreg -rc IOPCIDevice | grep -i asfw-armed`
   - **To disarm, delete that key (or run `--disarm`) and reboot.** The EFI partition can
     be edited from another OS if this one no longer boots.

4. **Intel with more than 4 GiB of RAM.** FireWire OHCI controllers address 32 bits of
   physical memory, whatever the OS. Without an IOMMU the kernel routes a DriverKit driver's
   DMA to memory above 4 GiB through a copy buffer that the controller and the driver never
   share. The driver tests for this at start and then refuses to start with
   `DMA to … is bounced: RAM above 4 GiB and no IOMMU …`; Apple's driver keeps the card.
   - **Turn VT-d on:** enable VT-d in the BIOS and set `Kernel > Quirks > DisableIoMapper =
     false` (`sudo ./asfw-opencore-arm.sh --vtd` sets the quirk). With AppleVTD active the
     controller's DMA is remapped below 4 GiB. Tested with 16 GiB on a Z390 board (b175).
   - The only alternative is to limit RAM with the boot-arg `maxmem=2048`.
   - In a VM: keep the VM's memory at **≤ 3 GiB**, or give the guest a virtual IOMMU. With PCI
     passthrough and no virtual IOMMU, DMA into the host's memory window for the card's bridge
     fails (ESXi, build 170), so a VM is no place for audio work.

## Install or update

Unzip, open Terminal in the extracted folder, and run:

```sh
./install.sh
```

Updating is the same: unzip the new build and run it again. Each run does only
what is missing:

1. checks that SIP is disabled
2. turns system-extension developer mode on, if it is off
3. signs the app and copies it to `/Applications`, unless that build is already
   installed
4. activates the driver:
   - **Monterey/Ventura:** headless (`ASFW --activate`). The script waits up to 2 minutes,
     long enough to approve it. The app window also has Install/Delete buttons.
   - **Sonoma and later:** the app opens; press **Install** in it.
5. prints the driver state and **the one thing left to do**

The last lines tell you what comes next:

| It says | Do this |
|---|---|
| `The driver is activated and enabled.` / `Installation complete` | Nothing. Done. |
| `NEXT: allow the extension in System Preferences > Security & Privacy > General` | Click **Allow** there, then run `./install.sh` again |
| `NEXT: reboot, then run ./install.sh again` | An older build is removed at reboot. Reboot, then run it again |
| `NEXT: unusual state …` | Reboot and run it again. If it persists, `./install.sh --uninstall` and start over |

You can run `./install.sh` again at any point: after an approval, after a reboot,
or when unsure. It continues from where things stand.

Every build has a higher build number (`b37`, `b38`, …). macOS only replaces an
installed driver with a newer one, so always install the newest ZIP. Older ZIPs
are deliberately not kept.

## Device settings and the login agent

The app's **Device Settings** page sets a MOTU device's clock source, what its optical
ports carry (ADAT or TOSLINK S/PDIF) and its analog input levels, and under **Diagnostics**
the driver's log level (see "Log level and start steps"). Changing an optical mode changes the channel
count, so the audio device is republished with the new layout. Every change is remembered
per device GUID, in the app's preferences.

Each unit is published to Core Audio under its own name, **"828mkII #128791 — ASFW"**, and
its own device UID, both derived from the unit's GUID (the EUI-64 in its config ROM). So the
Sound pane, Audio MIDI Setup and DAW selections stay with the physical unit whichever card or
port it is on, whatever order the units were found in, and two units of one model can be told
apart. The number after `#` is the unit-specific part of the GUID, shown in decimal for MOTU,
which numbers its units there; compare it with the serial label on the bottom of the unit.

For scripted use over ssh (open the `ASFWDriver` user client, call a selector with scalars):

- **Register reads**: selector 8 (`destinationID, addressHi, addressLo, length` → handle), then
  selector 12 with the handle for the data. MOTU registers sit at `0xFFFF F000 0000` plus the
  register offset, so `0x0b00` is `addressHi=0xFFFF addressLo=0xF0000B00`; the destination is
  the device's node id with the local bus bits, `0xFFC0 | node`. Every MOTU register write the
  driver makes is already logged as `MotuProtocol: reg 0x… <read> -> <written>` followed by a
  `readback` line.

On a MOTU whose front panel says **"Use Computer"** for the clock source, this page is the
only way to set it: the device defers to the host once a computer is connected. The sample
rate itself follows Core Audio (Audio MIDI Setup).

Core Audio's **volume sliders are inactive** for a MOTU: the device publishes no
host-controllable level, because gain and mixing live in the interface itself (CueMix, the
front panel), as with MOTU's own driver. Only AV/C devices with level controls get sliders.
A software volume in the driver would be an addition, not a repair.

`install.sh` also installs a login agent, `/Library/LaunchAgents/net.asfw.apply-config.plist`,
which runs `/usr/local/libexec/asfw-config-agent` (a small command-line tool from the package,
signed like the app) as the logged-in user. It watches every controller ASFW drives and,
whenever a device with remembered settings appears (login, re-plug, power cycle, bus reset
with a new generation), reads the device and writes back the remembered values that differ.
It also sets the remembered driver log level on each new driver instance, and saves the
driver's start steps to `~/Library/Logs/ASFW`. Its log is `/tmp/asfw-apply-config.log`. It is deliberately not the app itself: launching the
app bundle makes sysextd re-check the embedded driver and, on macOS 12, restart it, and a
launchd-started app also shows a second Dock icon. With several FireWire cards each card is a
separate entry in the app's sidebar; settings belong to the device, whichever card it is
plugged into.

The packaged app is signed **without the App Sandbox**, and the packaged driver allows any
user client: ad-hoc signatures carry no Team ID, and the sandbox refuses a DriverKit user
client whose app and driver do not share one (`sandboxd: deny iokit-open-user-client`), which
left every page except Core Audio "disconnected" in builds up to b68. If that ever shows again,
the **Overview** page's Connection Log names the reason, as does
`log show --last 10m --predicate 'subsystem == "net.asfw.app"'`.

## Log level and start steps

The driver logs at one level, **warn** by default: errors, refusals (for example a sample
rate a device cannot run at) and the numbered start steps. Change it without reinstalling or
rebooting:

- in the app: **Device Settings → Diagnostics → Log level**;
- from Terminal: `/usr/local/libexec/asfw-config-agent --log-level debug` (without a level it
  shows the one in force; `default` goes back to warn), or
  `defaults write net.asfw.app logLevel -string debug`, which the login agent sets within 30 s.

The levels are `off`, `error`, `warn`, `info` (adds one line per stream start and stop),
`debug` (heartbeats and step-by-step detail) and `trace` (every transaction, packet dumps).
A subsystem can log at its own level: `info irm=debug isoch=trace` (the subsystems are the
`[Tag]`s in the log: Controller, Hardware, Async, IRM, Isoch, Audio, DirectAudio, AVC, …). One
level holds for every controller, and the login agent sets it again after each driver start,
so it survives reboots. Each change logs one line, `[Log] level warn -> debug (from app)`.
debug and trace cost CPU while audio streams: set the level back when done. A line below the
level costs the driver one comparison.

For a problem during start-up, set the level at boot: `sudo ./asfw-opencore-arm.sh --log-level
debug` adds the controller property `asfw-log-level` (a String), which the driver reads before
its first start step; `--no-log-level` removes it. The login agent leaves a level set that way
alone until the app or the command line sets another.

## CPU idle limit while audio runs

While a device runs IO the driver holds a limit on how long the CPUs may take to leave an idle
state (5 µs by default), so the system stays out of the deep idle states and the threads of an
audio cycle start without their exit delay. Audio at small buffers needs it; the price is idle
power while audio runs, and none when it does not. To see or change it:

- `/usr/local/libexec/asfw-config-agent --bus-stall` shows the limit and how many devices hold
  one now;
- `/usr/local/libexec/asfw-config-agent --bus-stall off` (or `5`, `10`, `20`, `25`, `30`, `40`,
  in µs; `default` goes back to 5) sets it, and the login agent sets it again after each driver
  start. A new limit holds from the next time a device starts IO: stop and start the audio
  client.

`ioreg -l -w0 | grep IOPlatformMaxBusDelay` shows the value the kernel has in force, in
nanoseconds, and the registry ID of its holder (`(4294967295,0)`: no limit). At log level
`info` each IO start logs `[BusStall] device … CPU idle limit 5 us held while IO runs`.

Every start logs numbered steps, `[StartStep] #01 start …` to `bring-up ends: bus up`, at every
level from warn up. At login the agent saves the boot's steps to
`~/Library/Logs/ASFW/start-<boot time>.log`, and appends any later driver start; the newest ten
files are kept. To read them live:

```sh
/usr/bin/log show --last boot --style compact --predicate 'eventMessage CONTAINS "[StartStep]"'
```

## Check

```sh
./install.sh --status                             # developer mode, installed build, driver state
launchctl print gui/$(id -u)/net.asfw.apply-config | head -5   # device-settings agent loaded?
systemextensionsctl list                          # net.asfw.driver … [activated enabled]
ioreg -rc IOUserService | grep -i asfw            # driver attached to the controller?
/usr/bin/log show --last 10m --predicate 'process == "net.asfw.driver"' | tail -50   # dyld errors
/usr/bin/log show --last 10m --style compact --predicate 'eventMessage CONTAINS "[StartStep]"'
```

If the driver starts and dies immediately, look for `Symbol not found` in the first log.
It names exactly what the build is missing for this macOS. The driver's own lines come from
the kernel process, tagged `[Controller]`, `[Audio]` and so on; filter on the tag, not on
the process.

## Uninstall

```sh
./install.sh --uninstall
```

This deactivates the driver and removes `/Applications/ASFW.app`. If it reports state
left over, reboot.

## Over ssh

Copying and `--status` work over ssh. Activation needs the desktop user's session, so
run that part as the logged-in user:

```sh
sudo launchctl asuser "$(id -u <user>)" /Applications/ASFW.app/Contents/MacOS/ASFW --activate
```

(Monterey/Ventura builds only; also `--deactivate`, `--status`.)

## If the Mac hangs or will not boot

A boot that hangs takes the unified log with it. If it does finish, even after minutes, the
login agent saves that boot's start steps to `~/Library/Logs/ASFW` (see "Log level and start
steps"): the last `[StartStep]` line and the gap after it show where the start waited. Arm with
`--log-level debug` for more detail. Do not trace the driver on a serial console with `dk=0x3`
and `drain_uart_sync=1`: on the test machine that alone stalled the boot for minutes (b175).

Use the first of these that you can:

1. **Disarm:** delete `asfw-armed` from OpenCore `DeviceProperties` (from another OS
   if needed) and reboot. The driver then does not touch the controller.
2. **Disable all DriverKit drivers for one boot:** add the boot-arg `dk=0x1001` in the
   OpenCore picker or `config.plist`. The kernel then logs `dext launches are
   disabled` and launches no dext.
   - USB keyboards are dexts too, so the console keyboard stops working; use ssh.
   - Over ssh, run `systemextensionsctl uninstall - net.asfw.driver`, remove the
     boot-arg, and reboot.
3. **Single-user root shell:** move the whole system-extension state aside and
   reboot:

   ```sh
   mount -t apfs /dev/<data volume> /System/Volumes/Data
   mv /System/Volumes/Data/Library/SystemExtensions /System/Volumes/Data/Library/SystemExtensions.off
   reboot
   ```

   `db.plist` in that folder is what makes activation survive a reboot, so move the
   folder, not just the dext.
