# PCIe FireWire cards

Which PCIe FireWire cards ASFireWire has been tested with, how to identify a card before you buy
or install it, and what to check on a new one. These are results from one test machine (Intel
hackintosh, macOS 12 Monterey, MOTU 828mkII audio interfaces), not a general compatibility
list. Device results are in the
[compatibility wiki](https://github.com/mrmidi/ASFireWire/wiki/Device-Compatibility).

## Tested cards

| card | PCI IDs (OHCI function) | layout | result |
|---|---|---|---|
| no-name PCIe FireWire 800, TI XIO2213 "Cheetah Express" | `104c:823f`, subsystem `3412:7856`; bridge `104c:823e` | single chip: the XIO2213's own PCIe-to-PCI bridge with the OHCI function behind it | **works**: playback and capture, three 828mkII on one bus at 48 kHz and two at 96 kHz, Ableton Live at 96 kHz |
| StarTech PCIe FireWire 800 (three cards) | `104c:8025` (TSB82AA2), subsystem `0f2e:8025` | TSB82AA2 1394b link behind an ASMedia ASM1080/1083 PCIe-to-PCI bridge | **does not work reliably**: "cycle too long" and bus resets with the 828mkII, also on bare-metal Linux |
| *(second XIO2213 card, another make, 2026-11)* | *to be filled in* | | *to be tested* |

In short, the XIO2213 single-chip design works. Cards that put an older PCI FireWire chip behind
a separate PCIe-to-PCI bridge chip failed on this machine with every driver tried.

## Identifying a card

On macOS:

```
system_profiler SPPCIDataType          # vendor, device and subsystem ID per function
ioreg -rc IOPCIDevice -l | grep -i -E 'pci104c|pci11c1|pci1106'
```

On Linux: `lspci -nn | grep -i -E '1394|firewire'`, and `lspci -tv` for the bridge layout.

Look for:
- **The OHCI function's vendor:device.** TI is `104c`, LSI/Agere `11c1`, VIA `1106`.
- **Whether the OHCI function sits behind a bridge chip of another make** (an extra `PCI bridge`
  line from ASMedia, PLX, Pericom and the like) or behind the FireWire chip's own bridge
  (`104c:823e` for the XIO2213).
- **The subsystem ID.** `3412:7856` is a placeholder: the card's EEPROM was never given an
  identity of its own (see GUID below).

## Things to check on a new card

### GUID (EUI-64)

Every FireWire controller has a 64-bit GUID, loaded from the card's serial EEPROM at power-up.
Cheap cards often leave the EEPROM at a placeholder. The XIO2213 card above reports
`0x7856341278563412`.

- One such card on its own works.
- **Two nodes with the same GUID on one bus collide.** Do not connect two placeholder cards to
  the same bus.
- On separate buses the standard tolerates it, and ASFireWire does too: the driver puts the
  controller's GUID into the configuration ROM it shows the bus and uses it for nothing else.
  Cards are told apart by their PCI path (OpenCore arming, the app's card list, the agent),
  devices by their own GUIDs (settings, Core Audio device IDs), and each card runs its own
  driver instance with its own streams and bus management. Other software that tells
  controllers apart by GUID may still confuse two such cards.
- Read it:
  - from the driver's start steps: `[StartStep] … config ROM staged, controller GUID 0x…`
    (b229 on; the login agent keeps them in `~/Library/Logs/ASFW/start-<boot time>.log`);
  - in the ASFW app, on the diagnostics page with the OHCI registers ("GUID (64-bit Hex)");
  - on Linux with `cat /sys/bus/firewire/devices/fw*/guid`.
- The OHCI GUID registers are read-only once loaded from the EEPROM. Changing the GUID means rewriting
  the EEPROM through the XIO2213's bridge function (config offsets B0h–B3h, data manual
  SCPS210F §3.5.3–3.5.4), from Linux on bare metal. Back up all EEPROM bytes first.

### Interrupts

The XIO2213's OHCI function has no MSI. Interrupts are INTA, forwarded by the chip's bridge as
PCIe Assert_INTA messages, often on a line shared with other devices. Some older TI parts
behave the same way. A driver must work on INTx; ASFireWire does.

### ASPM

XIO2213 erratum #2: the system hangs or the device drops off the bus when ASPM L1 is enabled.
`asfw-opencore-arm.sh` sets `pci-aspm-default = <00000000>` on the card's bridge when it arms
the controller, and `--report` shows the value in the config. See
[DEBUGGING.md](DEBUGGING.md) §9.

### Warm reboots

The XIO2213 has no VAUX, and its sticky state survives a warm reboot on boards that do not
reset the slot. If a machine will not boot with the card in after a crash, power it off at the
wall for 30 s.

### Memory above 4 GiB

OHCI addresses 32 bits. On Intel with more than 4 GiB of RAM, VT-d must be on (see
[INSTALL.md](INSTALL.md), "Before the first install", step 4).

### Risers and extension cables

A card behind a PCIe riser or extension cable, for example when a wide GPU covers the slot, is
usually fine. The XIO2213 links at x1, 2.5 GT/s (PCIe 1.x), the most forgiving speed. Check
after installing:

```
ioreg -l -w0 | grep -A40 '"pci104c,823e"' | grep IOPCIExpressLinkStatus
```

`4113` = `0x1011`: x1 at 2.5 GT/s. If the link does not come up, or the card disappears after
warm boots, suspect the riser first.

## Several cards

One S400 bus carries a limited amount of isochronous data. Three 828mkII fit on one bus at
44.1 and 48 kHz; at 88.2 and 96 kHz two do and a third does not. For three or four units at
96 kHz use two cards, with at most two units on each bus.

The XIO2213 has four isochronous contexts in each direction, and ASFireWire uses one of each
per streaming unit, so at most four units stream on one card. The bus fills first.

Each card needs its own OpenCore arming entry: `asfw-opencore-arm.sh --report` lists every
controller's path, and the script arms all of them. A card added later changes the PCI paths of
the others; see "The PCI path moves" in [README-Hackintosh.md](README-Hackintosh.md).

## FireWire cables

The PHYs negotiate the highest speed both ends support; the cable gets no say. Old or cheap
cables that cannot carry S400 cleanly then cause damaged packets, "cycle too long" and bus
resets, not a slower link. Cheap new cables failed on this rig under Windows too.

What helped:
- Bilingual 9-pin to 6-pin cables made for FireWire 800.
- Short runs.
- Brand cables from the FireWire era.

To test a cable (b229 on): play and record for a minute, then run
`/usr/local/libexec/asfw-config-agent --link-stats`. It prints the errors the controller saw:
packets with a bad CRC, missing data blocks and cycles per unit, lost cycles and bus resets
per controller, failed requests per node, and the state of the card's own ports. A good cable
leaves every error counter at zero. See [INSTALL.md](INSTALL.md), "Link and cable check".
