# Installing the ASFireWire backport

These builds are for macOS 12 Monterey up to Tahoe, on Macs where you control the
security settings, typically OpenCore. They are unsigned and experimental. **Know how
to recover (see the end) before you install.**

Each build comes as `ASFW-<version>-<macos>-b<build>.zip` and contains:

| File | What it is |
|---|---|
| `ASFW.app` | The app, with the driver (`net.asfw.driver.dext`) inside it. On Monterey/Ventura it is the **lite** ASFW app: configuration and diagnostics (install, bus inspector, controller status, AV/C and DICE reports, logs, logging settings, Core Audio, Duet) without the Metal audio analyzer, meters, DV capture and MCP; on Sonoma and later it is the full app |
| `install.sh` | Installs, updates, checks and removes it |
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
   - Find the PCI path of the FireWire controller, for example with
     [gfxutil](https://github.com/acidanthera/gfxutil): `gfxutil | grep -i 104c:8025`
     for the TI XIO2213B. It sits behind the card's bridge, so expect something like
     `PciRoot(0x0)/Pci(0x1C,0x4)/Pci(0x0,0x0)/Pci(0x0,0x0)`.
   - In OpenCore `config.plist`, under `DeviceProperties` > `Add` > *that path*, add the
     key `asfw-armed`, type **Data**, value exactly `01` (one byte; `0001` or a
     string does not arm). Reboot.
   - Check: `ioreg -rc IOPCIDevice | grep -i asfw-armed`
   - **To disarm, delete that key and reboot.** The EFI partition can be edited from
     another OS if this one no longer boots.

4. **Intel with more than 4 GiB of RAM: VT-d must be active.** FireWire OHCI controllers
   address 32 bits of physical memory, whatever the OS. Apple's own drivers for such hardware
   rely on the IOMMU (VT-d, `AppleVTD`) to remap DMA below 4 GiB; without it the kernel
   silently routes a DriverKit driver's DMA through a copy buffer and the controller's data
   never reaches the driver. ASFW detects this at start, logs
   `DMA … is bounced through a kernel copy` with the remedy, and does not start, leaving the
   card to Apple's driver. To make it work:
   - BIOS: VT-d (Intel Virtualization Technology for Directed I/O) **enabled**.
   - OpenCore `config.plist`: `Kernel > Quirks > DisableIoMapper = false` (and no
     `DisableIoMapperMapping`), and keep the `DMAR` ACPI table. After boot,
     `ioreg -l | grep -c AppleVTD` must be non-zero.
   - In a VM: give the guest a virtual IOMMU (ESXi: *Enable IOMMU in this virtual machine*), or
     keep the VM's memory at **≤ 3 GiB** so that no guest page lies above 4 GiB.

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

The app's **Device Settings** page sets a MOTU device's clock source and what its optical
ports carry (ADAT or TOSLINK S/PDIF). Changing an optical mode changes the channel
count, so the audio device is republished with the new layout. Every change is remembered
per device GUID, in the app's preferences.

On a MOTU whose front panel says **"Use Computer"** for the clock source, this page is the
only way to set it: the device defers to the host once a computer is connected. The sample
rate itself follows Core Audio (Audio MIDI Setup).

`install.sh` also installs a login agent, `/Library/LaunchAgents/net.asfw.apply-config.plist`,
which runs `/usr/local/libexec/asfw-config-agent` (a small command-line tool from the package,
signed like the app) as the logged-in user. It watches every controller ASFW drives and,
whenever a device with remembered settings appears (login, re-plug, power cycle, bus reset
with a new generation), reads the device and writes back the remembered values that differ.
Its log is `/tmp/asfw-apply-config.log`. It is deliberately not the app itself: launching the
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

## Check

```sh
./install.sh --status                             # developer mode, installed build, driver state
launchctl print gui/$(id -u)/net.asfw.apply-config | head -5   # device-settings agent loaded?
systemextensionsctl list                          # net.asfw.driver … [activated enabled]
ioreg -rc IOUserService | grep -i asfw            # driver attached to the controller?
log show --last 10m --predicate 'process == "net.asfw.driver"' | tail -50
```

If the driver starts and dies immediately, look for `Symbol not found` in that log.
It names exactly what the build is missing for this macOS.

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
