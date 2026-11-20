// ioload — a CoreAudio client that takes a set time in every IO cycle.
//
// CoreAudio gives a client one buffer period to render in, less what the HAL
// and the device's driver take of it. ioload finds that share by experiment: it
// opens a device, sets the IO buffer size, and its IO procedure does nothing
// but spin for --burn-us microseconds and write silence. The largest burn that
// runs without overloads is what the device leaves a client; the period minus
// it is what the HAL's cycle and the driver cost together. A real client's
// render that needs more than that overloads on any driver.
//
//   ioload list
//   ioload run <device> [--buf FRAMES] [--secs S] [--burn-us US]
//                       [--sweep FROM:TO:STEP]          a run of --secs per step
//                       [--spike-us US --spike-every N] every Nth cycle burns US instead
//
// <device> is a device ID from `ioload list`, or part of its name ("828", an
// aggregate's name). Each step prints the cycles run, the overloads CoreAudio
// reported (kAudioDeviceProcessorOverload), the frames it skipped, and when the
// IO procedure was entered and left against CoreAudio's times for the cycle.
//
// --spike-us makes a client that is heavy once in N cycles, as one that works
// in blocks of N buffers is: with --buf 64 --spike-every 128 the ASFW driver's
// [IoTime] render line shows it at one position in 8192 frames.
//
// Build: clang -arch x86_64 -arch arm64 -mmacosx-version-min=12.0 -O2 -o ioload ioload.c \
//            -framework CoreAudio -framework CoreFoundation
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static AudioObjectPropertyAddress A(AudioObjectPropertySelector s, AudioObjectPropertyScope sc) {
    AudioObjectPropertyAddress a = { s, sc, kAudioObjectPropertyElementMain }; return a;
}
static void cfstr(CFStringRef s, char *buf, size_t n) { buf[0] = 0; if (s) CFStringGetCString(s, buf, n, kCFStringEncodingUTF8); }
static void device_name(AudioDeviceID d, char *buf, size_t n) {
    CFStringRef s = NULL; UInt32 sz = sizeof s;
    AudioObjectPropertyAddress a = A(kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal);
    AudioObjectGetPropertyData(d, &a, 0, NULL, &sz, &s); cfstr(s, buf, n); if (s) CFRelease(s);
}
static int chans(AudioDeviceID d, AudioObjectPropertyScope sc) {
    AudioObjectPropertyAddress a = A(kAudioDevicePropertyStreamConfiguration, sc);
    UInt32 sz = 0; if (AudioObjectGetPropertyDataSize(d, &a, 0, NULL, &sz)) return 0;
    AudioBufferList *bl = malloc(sz); int n = 0;
    if (!AudioObjectGetPropertyData(d, &a, 0, NULL, &sz, bl)) for (UInt32 i = 0; i < bl->mNumberBuffers; i++) n += bl->mBuffers[i].mNumberChannels;
    free(bl); return n;
}
static double nominal(AudioDeviceID d) {
    AudioObjectPropertyAddress a = A(kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal);
    Float64 r = 0; UInt32 sz = sizeof r; AudioObjectGetPropertyData(d, &a, 0, NULL, &sz, &r); return r;
}
static UInt32 u32prop(AudioDeviceID d, AudioObjectPropertySelector s, AudioObjectPropertyScope sc) {
    AudioObjectPropertyAddress a = A(s, sc); UInt32 v = 0, sz = sizeof v;
    AudioObjectGetPropertyData(d, &a, 0, NULL, &sz, &v); return v;
}
static AudioDeviceID *devices(UInt32 *count) {
    AudioObjectPropertyAddress a = A(kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal);
    UInt32 sz = 0; AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &sz);
    AudioDeviceID *ids = malloc(sz ? sz : 1); AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &sz, ids);
    *count = sz / sizeof *ids; return ids;
}

static int do_list(void) {
    UInt32 n = 0; AudioDeviceID *ids = devices(&n);
    for (UInt32 i = 0; i < n; i++) {
        char name[256]; device_name(ids[i], name, sizeof name);
        printf("%u\t%s\trate=%.0f\tin=%d\tout=%d\tbuf=%u\n", ids[i], name, nominal(ids[i]),
               chans(ids[i], kAudioObjectPropertyScopeInput), chans(ids[i], kAudioObjectPropertyScopeOutput),
               u32prop(ids[i], kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal));
    }
    free(ids); return 0;
}

static AudioDeviceID find_device(const char *what) {
    char *end = NULL; unsigned long id = strtoul(what, &end, 10);
    UInt32 n = 0; AudioDeviceID *ids = devices(&n); AudioDeviceID found = 0; int matches = 0;
    for (UInt32 i = 0; i < n; i++) {
        char name[256]; device_name(ids[i], name, sizeof name);
        if (*what && *end == 0 ? ids[i] == id : strstr(name, what) != NULL) { found = ids[i]; matches++; }
    }
    free(ids);
    if (matches > 1) { fprintf(stderr, "\"%s\" matches %d devices; use the ID from `ioload list`\n", what, matches); exit(1); }
    return found;
}

// ---- the IO procedure and what it counts
static mach_timebase_info_data_t g_tb;
static uint64_t us_to_ticks(double us) { return (uint64_t)(us * 1000.0 * g_tb.denom / g_tb.numer); }
static double ticks_to_us(double ticks) { return ticks * g_tb.numer / g_tb.denom / 1000.0; }

static _Atomic uint64_t g_burn_ticks, g_spike_ticks; static _Atomic uint32_t g_spike_every;
static _Atomic uint64_t g_cycles, g_overloads, g_skipped;
static _Atomic int64_t g_enter_sum, g_enter_max, g_left_sum, g_left_min;
static _Atomic uint64_t g_cycle_index; static double g_next_out = -1; static UInt32 g_frames;

static void reset_counts(void) {
    g_cycles = 0; g_overloads = 0; g_skipped = 0;
    g_enter_sum = 0; g_enter_max = INT64_MIN; g_left_sum = 0; g_left_min = INT64_MAX;
}

static OSStatus io_cb(AudioObjectID d, const AudioTimeStamp *now, const AudioBufferList *in, const AudioTimeStamp *it,
                      AudioBufferList *out, const AudioTimeStamp *ot, void *ctx) {
    (void)d; (void)in; (void)it; (void)ctx;
    const uint64_t entered = mach_absolute_time();
    const uint64_t index = atomic_fetch_add_explicit(&g_cycle_index, 1, memory_order_relaxed);
    const uint32_t every = atomic_load_explicit(&g_spike_every, memory_order_relaxed);
    const uint64_t burn = every != 0 && index % every == every - 1
                              ? atomic_load_explicit(&g_spike_ticks, memory_order_relaxed)
                              : atomic_load_explicit(&g_burn_ticks, memory_order_relaxed);
    UInt32 frames = 0;
    for (UInt32 b = 0; b < out->mNumberBuffers; b++) {
        memset(out->mBuffers[b].mData, 0, out->mBuffers[b].mDataByteSize);
        if (b == 0 && out->mBuffers[b].mNumberChannels) frames = out->mBuffers[b].mDataByteSize / (4 * out->mBuffers[b].mNumberChannels);
    }
    while (mach_absolute_time() - entered < burn) { }
    const uint64_t left = mach_absolute_time();

    atomic_fetch_add_explicit(&g_cycles, 1, memory_order_relaxed);
    if (ot->mFlags & kAudioTimeStampSampleTimeValid) {
        if (g_next_out >= 0 && ot->mSampleTime > g_next_out + 0.5)
            atomic_fetch_add_explicit(&g_skipped, (uint64_t)(ot->mSampleTime - g_next_out + 0.5), memory_order_relaxed);
        g_next_out = ot->mSampleTime + frames; g_frames = frames;
    }
    if (now->mFlags & kAudioTimeStampHostTimeValid) {
        const int64_t late = (int64_t)(entered - now->mHostTime);
        atomic_fetch_add_explicit(&g_enter_sum, late, memory_order_relaxed);
        if (late > atomic_load_explicit(&g_enter_max, memory_order_relaxed)) atomic_store_explicit(&g_enter_max, late, memory_order_relaxed);
    }
    if (ot->mFlags & kAudioTimeStampHostTimeValid) {
        const int64_t ahead = (int64_t)(ot->mHostTime - left);
        atomic_fetch_add_explicit(&g_left_sum, ahead, memory_order_relaxed);
        if (ahead < atomic_load_explicit(&g_left_min, memory_order_relaxed)) atomic_store_explicit(&g_left_min, ahead, memory_order_relaxed);
    }
    return 0;
}

static OSStatus overload_cb(AudioObjectID d, UInt32 n, const AudioObjectPropertyAddress *a, void *ctx) {
    (void)d; (void)n; (void)a; (void)ctx;
    atomic_fetch_add_explicit(&g_overloads, 1, memory_order_relaxed); return 0;
}

static void run_step(double burn_us, double secs, int spiking, double spike_us, unsigned spike_every) {
    atomic_store(&g_burn_ticks, us_to_ticks(burn_us));
    usleep(300000);  // the change settles before the count starts
    reset_counts();
    usleep((useconds_t)(secs * 1e6));
    const uint64_t cycles = g_cycles, overloads = g_overloads, skipped = g_skipped;
    const double enter_avg = cycles ? ticks_to_us((double)g_enter_sum / cycles) : 0;
    const double left_avg = cycles ? ticks_to_us((double)g_left_sum / cycles) : 0;
    printf("burn %4.0f us", burn_us);
    if (spiking) printf(" (%.0f us every %u)", spike_us, spike_every);
    printf(": %llu cycles of %u frames, %llu overloads, %llu frames skipped; IO procedure entered avg %.0f us "
           "(at most %.0f) after the cycle's time, left avg %.0f us (least %.0f) before the output's presentation\n",
           (unsigned long long)cycles, g_frames, (unsigned long long)overloads, (unsigned long long)skipped, enter_avg,
           cycles ? ticks_to_us((double)g_enter_max) : 0, left_avg, cycles ? ticks_to_us((double)g_left_min) : 0);
    fflush(stdout);
}

static int do_run(int argc, char **argv) {
    const char *what = argv[0]; UInt32 buf = 0; double secs = 20, burn = 0, spike = 0;
    double from = -1, to = 0, step = 0; unsigned spike_every = 0;
    for (int i = 1; i < argc; i++) {
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(argv[i], "--buf") && v) { buf = (UInt32)atoi(v); i++; }
        else if (!strcmp(argv[i], "--secs") && v) { secs = atof(v); i++; }
        else if (!strcmp(argv[i], "--burn-us") && v) { burn = atof(v); i++; }
        else if (!strcmp(argv[i], "--spike-us") && v) { spike = atof(v); i++; }
        else if (!strcmp(argv[i], "--spike-every") && v) { spike_every = (unsigned)atoi(v); i++; }
        else if (!strcmp(argv[i], "--sweep") && v && sscanf(v, "%lf:%lf:%lf", &from, &to, &step) == 3 && step > 0) { i++; }
        else { fprintf(stderr, "unknown or incomplete option: %s\n", argv[i]); return 2; }
    }
    const int spiking = spike_every != 0 && spike > 0;
    AudioDeviceID d = find_device(what);
    if (!d) { fprintf(stderr, "no device \"%s\" (see `ioload list`)\n", what); return 1; }
    mach_timebase_info(&g_tb);

    if (buf) {
        AudioObjectPropertyAddress a = A(kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal);
        OSStatus e = AudioObjectSetPropertyData(d, &a, 0, NULL, sizeof buf, &buf);
        if (e) fprintf(stderr, "setting the buffer size to %u: status %d\n", buf, (int)e);
    }
    char name[256]; device_name(d, name, sizeof name);
    const UInt32 frames = u32prop(d, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal);
    const double rate = nominal(d);
    printf("device %u \"%s\": %.0f Hz, %u-frame buffers (%.0f us per cycle), in=%d out=%d; safety offset in %u out %u, "
           "latency in %u out %u frames\n",
           d, name, rate, frames, rate > 0 ? frames * 1e6 / rate : 0, chans(d, kAudioObjectPropertyScopeInput),
           chans(d, kAudioObjectPropertyScopeOutput),
           u32prop(d, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeInput),
           u32prop(d, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput),
           u32prop(d, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeInput),
           u32prop(d, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput));

    AudioObjectPropertyAddress ov = A(kAudioDeviceProcessorOverload, kAudioObjectPropertyScopeGlobal);
    AudioObjectAddPropertyListener(d, &ov, overload_cb, NULL);
    atomic_store(&g_spike_ticks, us_to_ticks(spike)); atomic_store(&g_spike_every, spiking ? spike_every : 0);
    atomic_store(&g_burn_ticks, us_to_ticks(from >= 0 ? from : burn));
    reset_counts();
    AudioDeviceIOProcID proc = NULL;
    OSStatus e = AudioDeviceCreateIOProcID(d, io_cb, NULL, &proc);
    if (!e) e = AudioDeviceStart(d, proc);
    if (e) { fprintf(stderr, "starting IO: status %d\n", (int)e); return 1; }
    sleep(2);  // the start's own overloads and skips are not the measurement

    if (from >= 0) for (double b = from; b <= to + 1e-9; b += step) run_step(b, secs, spiking, spike, spike_every);
    else run_step(burn, secs, spiking, spike, spike_every);

    AudioDeviceStop(d, proc); AudioDeviceDestroyIOProcID(d, proc);
    AudioObjectRemovePropertyListener(d, &ov, overload_cb, NULL);
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "list")) return do_list();
    if (argc >= 3 && !strcmp(argv[1], "run")) return do_run(argc - 2, argv + 2);
    fprintf(stderr, "usage: ioload list\n"
                    "       ioload run <device> [--buf FRAMES] [--secs S] [--burn-us US] [--sweep FROM:TO:STEP]\n"
                    "                           [--spike-us US --spike-every N]\n");
    return 2;
}
