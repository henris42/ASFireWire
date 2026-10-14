# Installing the ASFireWire backport

These builds are for macOS 12 Monterey up to Tahoe, on Macs where you control the
security settings, typically OpenCore. They are unsigned and experimental. **Know how
to recover (see the end) before you install.**

Each build comes as `ASFW-<version>-<macos>-b<build>.zip` and contains:

| File | What it is |
|---|---|
| `ASFW.app` | The app, with the driver (`net.asfw.driver.dext`) inside it. On Monterey/Ventura it is a small installer app; on Sonoma and later it is the full ASFW app |
| `install.sh` | Installs, updates, checks and removes it |
| `Entitlements/` | Used by `install.sh` to sign the app and driver |
| `BUILDINFO.txt` | Which source, macOS target and options this build was made from |

## Before the first install

1. **SIP must report "disabled".** Run `csrutil status`. A custom OpenCore
   `csr-active-config` that prints the breakdown with "disabled" lines is enough.
2. **Log in to the desktop.** Driver activation needs the logged-in user's session.
   Use Terminal on the Mac itself, not a bare ssh session (see "Over ssh" below).
3. **Arm the controller.** These builds will not touch the FireWire controller unless
   OpenCore gives it an `asfw-armed` property. Without it the driver installs, logs
   `no asfw-armed property on the controller; not starting`, and leaves the card to
   Apple's driver. That is a good first test.
   - Find the PCI path of the FireWire controller, for example with
     [gfxutil](https://github.com/acidanthera/gfxutil): `gfxutil | grep -i 104c:8025`
     for the TI XIO2213B. It sits behind the card's bridge, so expect something like
     `PciRoot(0x0)/Pci(0x1C,0x4)/Pci(0x0,0x0)/Pci(0x0,0x0)`.
   - In OpenCore `config.plist`, under `DeviceProperties` > `Add` > *that path*, add the
     key `asfw-armed`, type **Data**, value `01`. Reboot.
   - Check: `ioreg -rc IOPCIDevice | grep -i asfw-armed`
   - **To disarm, delete that key and reboot.** The EFI partition can be edited from
     another OS if this one no longer boots.

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
   - **Monterey/Ventura:** headless. The script waits up to 2 minutes, long enough to
     approve it.
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

## Check

```sh
./install.sh --status                             # developer mode, installed build, driver state
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
