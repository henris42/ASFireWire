# Debugging a boot hang on a hackintosh

What macOS gives a kernel or driver developer when the machine stops with a black
screen, what it does not, and how to set up the pieces that work. Written for the
ASFW bring-up hang on the ASRock B760M-HDV/M.2 (b193), but nothing here is specific
to that board. Every kernel fact below is taken from the xnu source for macOS 12
(tag `xnu-8020.140.41`), not from web guides; the file names are given so it can be
checked again.

## 1. What a black screen does not tell you

"Black screen, no ping, no ssh for five minutes" is consistent with three states:

- the kernel is stopped (a hang in the PCI or IOMMU path, or a panic nobody saw);
- the kernel runs but the display never came back from the firmware framebuffer to
  the GPU driver, and networking never got far enough to answer (the network comes
  up around loginwindow, so a failed ping at the GPU handover proves nothing);
- the boot is alive and waiting in user space.

A dext cannot stop the machine by itself: it is a user process. A hang means kernel
code did not return from something the dext asked for. The question is always
*which request*, and that is what every tool below tries to answer.

## 2. Logs that do not survive

| Mechanism | Why it fails here |
|---|---|
| Unified log (`log show --last boot`) | logd writes to disk lazily; a hang followed by a power cycle loses the tail, which is the part that matters. b193: "not one `[StartStep]`" from a boot where the driver certainly ran. |
| Verbose boot (`-v`) | Kernel text on the screen until the GPU driver takes the console, which is exactly where the black screen happens. |
| Serial console (`serial=3`, `debug=0x8`) | Needs a real UART at I/O port 0x3F8; a USB adapter cannot be driven by the kernel console. The B760M-HDV/M.2 has no COM header, and on boards that have one the console dies early once a kext claims the UART. Dead end. |
| Panic log | Written only if the kernel *panicked* and could store the log. A hang never panics by itself. See §5 for making sure the path works at all. |

What survives: a file written synchronously by a user-space daemon (the ASFW boot
trace, §8), a kernel core dump sent to another machine (§6), and a hardware debugger
(§7).

## 3. The `debug=` boot-arg

Authoritative list: `osfmk/kern/debug.h` in xnu, section "Debug boot-args".
macOS 12: <https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-8020.140.41/osfmk/kern/debug.h>

| Flag | Value | Meaning |
|---|---|---|
| `DB_HALT` | `0x1` | Halt early in boot and wait for a debugger. |
| `DB_NMI` | `0x4` | Enter the debugger on an NMI (see §4 for what that means on a PC). |
| `DB_KPRT` | `0x8` | `kprintf` output to the serial port. |
| `DB_KDB` | `0x10` | Use the in-kernel debugger instead of KDP. |
| `DB_ARP` | `0x40` | Answer ARP while in the debugger, so a routed host can attach. |
| `DB_KDP_BP_DIS` | `0x80` | Disable KDP breakpoints. |
| `DB_KDP_GETC_ENA` | `0x200` | Allow `getc` in the debugger. |
| `DB_KERN_DUMP_ON_PANIC` | `0x400` | Write a kernel core on panic. |
| `DB_KERN_DUMP_ON_NMI` | `0x800` | Write a kernel core when the debugger is entered from an NMI. |
| `DB_DBG_POST_CORE` | `0x1000` | Stay in the debugger after an NMI core. |
| `DB_PANICLOG_DUMP` | `0x2000` | Send the panic log to the dump server instead of a core. |
| `DB_REBOOT_POST_CORE` | `0x4000` | Reboot after the post-panic dump. |
| `DB_NMI_BTN_ENA` | `0x8000` | Let the (Mac) button raise an NMI directly. |
| `DB_DISABLE_LOCAL_CORE` | `0x20000` | Never write a core to the local disk. |
| `DB_DISABLE_GZIP_CORE` | `0x40000` | Do not compress cores. |
| `DB_DISABLE_CROSS_PANIC` | `0x80000` | x86 with a T2 only. |
| `DB_REBOOT_ALWAYS` | `0x100000` | Never wait for a debugger connection. |
| `DB_DISABLE_STACKSHOT_TO_DISK` | `0x200000` | No stackshot to the local disk. |

Obsolete in macOS 12 and ignored: `0x2` (`DB_PRT`) and `0x100` (`DB_LOG_PI_SCRN`,
"panic info to screen"). Web guides still quote `debug=0x100` as "halt at boot" or
"keep the panic on screen"; on Monterey it does nothing. `0x10000` was removed.

Two side effects of any non-zero `debug=` on x86 (`osfmk/kern/debug.c`,
`panic_init`): `panicDebugging` is set, so a panic waits in the debugger instead of
rebooting (add `0x100000` to reboot anyway), and local cores become possible (§6).

Other boot-args used below: `keepsyms=1` keeps kernel symbols for the panic
backtrace; `kdp_match_name=enX` picks the KDP interface; `_panicd_ip`, `_router_ip`,
`panicd_port`, `_panicd_corename` configure the network core dump; `wdt=-1` disables
the watchdog on Macs that have one; `interrupt_watchdog=0` disables the x86
interrupt watchdog (leave it on, §5).

## 4. NMI on a PC: there is no way in

The hope "an NMI drops the kernel into the debugger, then we take a core" does not
hold on a hackintosh, for two independent reasons.

**No source.** A Mac raises the debugger NMI from the power button through its
platform driver (SMC). A PC has no button, no SMC, and no IPMI on a desktop board.
The chipset's TCO watchdog can be programmed to raise an NMI on timeout, but that
takes ring-0 register writes before the hang, and the firmware usually locks the
TCO registers at the end of POST.

**No handler.** Even with a source, xnu on bare-metal x86 does not enter the
debugger on an external NMI. `NMIInterruptHandler` in `osfmk/i386/mp.c` treats an
NMI as a cross-CPU signal: it handles the kernel's own reasons (spinlock timeout, TLB
flush timeout, cross-call timeout, interrupt watchdog), and drops into the debugger
on a plain NMI only when `virtualized && (debug_boot_arg & DB_NMI)`, that is, under
a hypervisor. On real hardware a stray NMI goes through `mp_kdp_wait(FALSE, FALSE)`
and returns. `DB_KERN_DUMP_ON_NMI` therefore has nothing to fire from here.

So on this platform "core dump on NMI" has to become "core dump on panic", and the
kernel's own hang detectors are the way to get the panic.

## 5. Make the kernel panic instead of hang, and make the panic log survive

**The detectors that exist.** When one core is stuck (for example on a PCI read that
never completes) and another core needs it, xnu panics from the other core: TLB
flush timeout, cross-call timeout, spinlock acquisition timeout, and the interrupt
watchdog (a CPU too long in one interrupt vector). Each prints a backtrace of the
stuck core through `panic_i386_backtrace`. A deadlock on mutexes does not trigger
them; a stalled core usually does within a minute.

**Check that a panic leaves a trace at all.** b193 reported "no panic report". On a
hackintosh that means nothing until the panic path is proven: macOS keeps the panic
log in NVRAM (`aapl,panic-info`) and writes `/Library/Logs/DiagnosticReports/
Kernel-<date>.panic` on the next boot. Emulated or unreliable NVRAM under OpenCore
loses it. Test once, on a boot with the card out:

```
sudo dtrace -w -n 'BEGIN { panic(); }'
```

(needs DTrace allowed by the SIP configuration; `csrutil status` shows it). After
the reboot a `Kernel-*.panic` file must exist. If it does not, fix NVRAM first
(OpenCore `LegacyOverwrite`/`WriteFlash`, or native NVRAM); until then every "no
panic report" is uninformative, and the b193 hang may well have been a panic.

Add `keepsyms=1` so the backtrace is symbolized.

## 6. Kernel core dumps

A core gives every core's registers and stacks, which the panic log does not.

**Local core** (`osfmk/kern/debug.c`, `on_device_corefile_enabled`): on macOS the
kernel writes a local core when `debug=` has `0x400` or `0x800` and not `0x20000`,
provided its polled corefile could be opened before the panic. If it could not, the
panic log says `skipping local kernel core because core file could not be opened
prior to panic`. Writing the core leaves the disk driver unusable, so the machine
has to be reset afterwards; that is expected.

**Network core** (`osfmk/kdp/kdp_udp.c`): with `_panicd_ip=<host IP>` (and
`_router_ip=` when the host is on another subnet) the kernel sends the core to a
`kdumpd` on the host, UDP port 1069. On the host Mac:

```
sudo mkdir -p /PanicDumps && sudo chown nobody /PanicDumps
sudo launchctl load -w /System/Library/LaunchDaemons/com.apple.kdumpd.plist
```

(`/usr/libexec/kdumpd` ships with macOS.) This path needs a NIC whose driver has the
kernel debugger hooks, see §7; the onboard Realtek does not.

Suggested boot-args for a diagnostic boot with a KDP-capable NIC:

```
debug=0x444 kdp_match_name=enX _panicd_ip=192.168.x.y keepsyms=1 -v
```

`0x444` = core on panic, ARP in the debugger, NMI handling (harmless on a PC).
Without a KDP NIC use `debug=0x400 keepsyms=1` and rely on the local core.

## 7. Two-machine kernel debugging (KDP over Ethernet)

Apple's Kernel Debug Kit for the exact build (12.7.6 is 21H1320) supplies the
symbols for `lldb` on the host; the release kernel on the target already contains
KDP, no kernel swap is needed.

**The NIC.** KDP polls the Ethernet controller through hooks the driver must
implement (`IONetworkController::attachDebuggerClient`, a polled send/receive
pair; `IONetworkingFamily/IOKernelDebugger.cpp`). Apple's built-in drivers have
them; third-party kexts (the B760's Realtek RTL8125 with LucyRTL8125Ethernet) do
not, and USB and Wi-Fi never work. An Intel I210-AT (PCI ID 8086:1533, PCIe card or
M.2 E-key) runs on Apple's own `AppleIntelI210Ethernet` without patches and is the
usual choice; an I211 (8086:1539) does not match Apple's driver.

**Target boot-args:** `debug=0x44 kdp_match_name=enX wdt=-1`, `enX` from
`ifconfig` on a normal boot. `debug=0x45` additionally halts early in boot and waits
for the debugger, which is the only way in on a PC (no NMI, §4).

**Host:**

```
lldb /Library/Developer/KDKs/KDK_12.7.6_21H1320.kdk/System/Library/Kernels/kernel
(lldb) kdp-remote <target IP>
```

**What it can and cannot do here.** With `0x45` you attach before the ASFW bring-up
runs, set a breakpoint on the kernel routine the trace's last step names (the
IOPCIFamily entry behind the dext's config or memory access, or `IOUserServer`'s
RPC dispatch), continue, and single-step across the access. If the core stalls on
the access the connection dies at that instruction, which is itself the answer. If
another thread holds a lock, the debugger shows every core's stack. What it cannot
do is break into a running kernel later: once continued, there is no way back in
without a panic. A core that is stalled on an MMIO read is also invisible to a
debugger that runs on that core.

## 8. What ASFW adds on its own side

- **Boot trace to disk** (`asfw-boot-trace` property, `asfw-opencore-arm.sh --boot-trace`):
  the driver publishes every start step, register write with read-back, kernel call
  and poll into its I/O Registry entry and pauses after each; the LaunchDaemon
  `net.asfw.boot-trace` appends them to `/var/log/asfw/boot-trace-<boot>.log` with an
  fsync per line. After a hang, the last line is the last step that completed; the
  next one in the code is the request the kernel did not return from.
- **Manual start** (`asfw-start = manual`, `--manual-start`): the driver claims the
  controller at boot and does nothing; `asfw-config-agent --bring-up` runs the
  bring-up later, with the machine fully up (ssh, log, trace all working).
- **Start watchdog**: a `Start` that does not return in 30 s (600 s in lockstep)
  ends the dext process so IOKit treats the start as failed. It cannot end a thread
  that is stuck inside a kernel RPC; that case is exactly the one the tools above
  are for.
- **Early checks**: the first MMIO read fails the start when the controller reads
  all ones; the function's power state is read and set to D0 before anything else.

## 9. Hardware debugger

Intel DCI (Direct Connect Interface) is JTAG over a USB 3 debug cable and the only
tool that stops the cores from outside, independent of what the kernel is doing: a
core stalled on a PCIe read is visible to it and to nothing else. It needs the
hidden BIOS setting enabled, the cable, and Intel System Debugger with a licence for
the platform.

## 10. Order of attack

1. Prove the panic path (§5, the forced panic). Cheap, and it decides whether past
   "no panic report" results meant anything.
2. Run the bring-up under the trace with the manual start (§8). The last line names
   the request.
3. With an I210 in the machine: `debug=0x444 kdp_match_name=enX _panicd_ip=…
   keepsyms=1`, `kdumpd` on the host, repeat. A stalled core becomes a panic and a
   core dump within a minute.
4. If the trace points at one access and the core does not explain it, KDP with
   `0x45` and a breakpoint on that access (§7).
5. DCI when the licence arrives (§9).
