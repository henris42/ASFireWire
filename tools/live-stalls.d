#!/usr/sbin/dtrace -s
/*
 * live-stalls.d - what the threads of one process were waiting on whenever
 * one of them stayed off the CPU for longer than a threshold.
 *
 *   sudo dtrace -p $(pgrep -x Live) -s live-stalls.d 1500 > stalls.txt
 *
 * The argument is the threshold in microseconds; Ctrl-C ends it. Needs SIP
 * off (as the driver does). Each event prints the time, the thread, how long
 * it was off the CPU, the kernel stack and the user stack it came back to:
 * the user stack is where it waited. Core Audio's IO thread is the one with
 * HALC_ProxyIOContext::IOWorkLoop in its user stack; it normally waits less
 * than one buffer (1333 us at 96 kHz / 128 frames, 667 us at 64), so with a
 * threshold above the buffer every event of that thread is a stall.
 *
 * A render that is long with no event of the IO thread in it was computing,
 * not waiting.
 */
#pragma D option quiet
#pragma D option ustackframes=30
#pragma D option stackframes=12
#pragma D option bufsize=16m

sched:::off-cpu
/pid == $target/
{
    self->off = timestamp;
}

sched:::on-cpu
/self->off && (timestamp - self->off) >= $1 * 1000/
{
    printf("\n==== %Y.%03d thread %d off the CPU for %d us\n", walltimestamp,
           (walltimestamp % 1000000000) / 1000000, tid, (timestamp - self->off) / 1000);
    stack();
    ustack();
}

sched:::on-cpu
/self->off/
{
    self->off = 0;
}
