# Old kernel, modern Android

**What breaks when you put Android 10 on a Linux 3.4 kernel with 2013-era vendor
blobs — and how each one was fixed.**

Ten root causes, every one of them found by measurement rather than guesswork.
Most are not device-specific: they are about modern Android meeting a kernel
that predates the syscalls it assumes, and vendor libraries that predate the
bionic that now hosts them. If you are pushing a new Android onto old hardware,
you will meet several of these.

Reference port: **LineageOS 17.1 on ST-Ericsson u8500** (Samsung Galaxy S3 mini
GT-I8190, `golden`, board `montblanc`).

This repository carries the patches that make **Android 10 actually boot** on a
2013 ST-Ericsson NovaThor u8500 handset with a **Linux 3.4** kernel, plus the
tooling used to iterate on a device that has no working power button and cannot
be flashed by hand a hundred times.

The patches apply directly to the rest of the u8500 family (`codina`, `janice`,
`kyle`, …). Beyond that family, take the diagnosis rather than the diff — the
symptom-to-cause mapping is the part that transfers.

**There is no ROM image here, and there will not be one.** The images built from
this tree contain a personal WiFi configuration. Build your own.

---

## Why this exists

Every one of the problems below cost hours, and none of them announces itself.
The failure is always something generic — `system_server` dies, the boot loops,
WiFi "is enabled" but never associates — while the cause is three layers down.
If you are porting a modern Android to old hardware, you will meet most of
these. This is the list I wish I had found.

---

## The blockers, symptom first

| # | What you see | Root cause | Fix |
|---|---|---|---|
| 1 | `system_server` SEGV in `je_free`, every time BatteryStats updates rails | Passthrough HIDL HALs `delete` the pointer from `hw_get_module()`. That pointer is `HAL_MODULE_INFO_SYM` inside the dlopen'd library's `.data` — it was never on the heap. In passthrough mode the last strong ref is dropped **inside `system_server`**, so that is where it crashes | `mOpened` flag: only call `common.close()` on a device the module's own `open()` produced. `power` and `memtrack` both. |
| 2 | `audioserver` SIGSEGV loop | `ALOG_ASSERT` compiles to **nothing** when `LOG_NDEBUG` is set — which is every user/userdebug build. The dereference it "guards" in `EngineBase::loadVolumeConfig` is unguarded | Real null check + the device was missing `audio_policy_configuration.xml` entirely |
| 3 | `Jit thread pool` SIGSEGV `SEGV_ACCERR` | `memfd_create` arrived in Linux 3.17; ART's `JitCodeCache` needs it for its dual-view mapping and silently falls back to writing to an executable page | `dalvik.vm.usejit=false` — the image is fully dexpreopted anyway. (LineageOS also has `TARGET_HAS_MEMFD_BACKPORT`.) |
| 4 | `audioserver` SIGABRT in `pthread_join` | A 2013 `libsecril-client.so` joins an already-exited thread. Pre-O bionic returned `ESRCH`; modern bionic calls `async_safe_fatal` and kills the process | Correct fix is `TARGET_PROCESS_SDK_VERSION_OVERRIDE` per binary. **Careful:** `linker.cpp` matches those entries against `executable_path` only — a `.so` path never matches |
| 5 | `kernel does not support timerfd_create() with alarm timers` | `CLOCK_REALTIME_ALARM` / `CLOCK_BOOTTIME_ALARM` timerfd support landed in Linux 3.11 | Upstream backport in `fs/timerfd.c`. Note `kernel/time/Makefile` had `alarmtimer.o` commented out because its symbols collide with this tree's `drivers/rtc/alarm.c`; the mainline API is renamed rather than dropped |
| 6 | `Check failed: getHalDeviceVersion() >= 1.3` | The Samsung sensors blob reports `common.version = 0`. On a pre-1.1 HAL only the `v0` half of `sensors_poll_device_1_t` is allocated — reading `batch`/`flush`/`inject_sensor_data` **overruns the struct** | Shim: `batch()` maps to `setDelay()`, `flush()` returns `BAD_VALUE` |
| 7 | USB shows **zero interfaces** (`config 1 has no interfaces?`) the moment boot succeeds | The framework sets `sys.usb.config` itself, and the device `.rc` had no handler for the value it chose. `adbd` cannot serve USB ADB on this kernel at all (`CONFIG_USB_FUNCTIONFS` unset) | Handlers for every combination, all keeping `rndis,acm,adb` |
| 8 | `system_server`, `SystemUI` and `media.codec` all SEGV at `0x0` | No `CONFIG_ION` in the kernel, so `C2AllocatorIon`'s ctor returns before setting `mTraits`, and `setUsageMapper` then copies a `std::string` from null | Guard both call sites; `debug.stagefright.ccodec=0` |
| 9 | `SystemUI` SEGV loop at `0x30` | `EnhancedEstimatesImpl.getEstimate()` returns null and `PowerUI` dereferences it without checking | Fall back to the plain `BatteryStateSnapshot` |
| 10 | WiFi enabled, `wlan0` DOWN, `wpa_supplicant` never starts, `/data/misc/wifi/` empty | **Two** independent causes, see below | see below |
| 11 | Handset powers itself off ~50 s into **every** boot; `last_kmsg` shows `init: Received sys.powerctl='shutdown,thermal,battery' from pid: N (system_server)` while the battery sysfs reads 27 °C | `/sys/class/power_supply` has **three** nodes whose `type` is `Battery`. `BatteryMonitor::init()` walks them in readdir order and takes the first that answers per field, so which one wins is undefined. Here it took `sec-fuelgauge`, whose `temp` is not a temperature: `-1066830336` is `0xC0640000`, the raw IEEE-754 bits of `-3.5625`. When that garbage carried a positive sign the framework read tens of thousands of degrees and `BatteryService.shutdownIfOverTempLocked()` did its job | Board-specific `libhealthd.<device>` that names the bad nodes in `ignorePowerSupplyNames` **and** pins every `battery*Path` explicitly. Raising `config_shutdownBatteryTemperature` does **not** work - no threshold survives a float's bit pattern |
| 12 | Handset still powers itself off ~50 s into every boot **after** the health HAL is fixed and `dumpsys battery` agrees with sysfs to the digit | `BatteryService` was still starting `ShutdownActivity`. Neither of its two conditions could be true (level 49, temperature 351 against a 3000 threshold verified with `aapt2 dump resources`), and `last_kmsg` could not say otherwise because SELinux-permissive audit spam had overwritten everything before the 50 s mark - and Android 10's `init` logs `Received sys.powerctl` to logd, not kmsg, once logd is up | Write logcat to a file under `/data`: init unmounts it *cleanly* during shutdown, so the file survives. The trigger was in it: `ActivityTaskManager: START u0 {act=…REQUEST_SHUTDOWN cmp=…ShutdownActivity} from uid 1000` followed by `ShutdownActivity: onCreate(): confirm=false` - `confirm=false` is `BatteryService.startShutdownActivity()`'s signature and nothing else in the tree sends that intent |

### 10, in detail — because this one is worth its own section

The obvious cause is real but not sufficient:

```
android.hardware.wifi@1.0-service: finit_module return: -1: Function not implemented
android.hardware.wifi@1.0-service: Failed to load WiFi driver
HalDevMgr: Cannot start IWifi: 9
```

`libwifi-hal` insists on inserting the driver itself and does it with
`finit_module(2)` — a syscall from Linux 3.8. On 3.4 it can never succeed, and
the driver is already loaded anyway, which the HAL never checks.

AOSP's own documentation says the vendor HAL "is optional (not required) for
infrastructure Station (STA) and Soft AP (SAP) modes to function", so the fix is
to stop declaring it: remove the `android.hardware.wifi` `<hal>` block from
`manifest.xml` and drop the service from the product. `HalDeviceManager` then
logs *"Vendor Hal not supported, ignoring start"* and returns true.

**But that alone still does not bring WiFi up**, and this is the part that is
not written down anywhere. The device manifest declared:

```xml
<name>android.hardware.wifi.supplicant</name>
<version>1.0</version>
```

The supplicant in `external/wpa_supplicant_8` actually implements **1.2**.
`SupplicantStaIfaceHal.startDaemon()` branches on `isV1_1()`, and when true it
takes the **lazy-HAL** path — it simply asks for `@1.1::ISupplicant` and lets
`hwservicemanager` tell init to start the service. `hwservicemanager` can only
do that for an FQName **init declared**, and the `.rc` listed 1.0 only. So the
daemon was never started, by anyone, and `lshal` still showed it "registered".

Fix: declare 1.2 in the manifest and list all three interfaces in the `.rc`:

```
    interface android.hardware.wifi.supplicant@1.0::ISupplicant default
    interface android.hardware.wifi.supplicant@1.1::ISupplicant default
    interface android.hardware.wifi.supplicant@1.2::ISupplicant default
```

Two traps while you are in there:

* **Keep the service named `wpa_supplicant`.** AOSP's own rc calls it
  `vendor.wpa_supplicant`; copying that breaks the non-lazy fallback, because
  `SupplicantStaIfaceHal.INIT_SERVICE_NAME` is that literal and the fallback
  writes it to `ctl.start`.
* **Delete the old command-line flags.** An Android 8-era `.rc` passes
  `-c/data/misc/wifi/...`, `-O/data/misc/wifi/sockets`, `-e…/entropy.bin`.
  Every one of those paths is a **compile-time constant** in Android 10:
  `Android.mk` bakes `CONFIG_CTRL_IFACE_DIR="/data/vendor/wifi/wpa/sockets"` in,
  and `hidl/1.2/supplicant.cpp` hard-codes
  `/data/vendor/wifi/wpa/wpa_supplicant.conf`, seeding it from
  `/vendor/etc/wifi/`. The flags were being ignored, silently.

---

## The one that is not a bug report but a lesson

`golden_defconfig` contained, 363 lines apart:

```
1520: CONFIG_VIDEO_DEV=y
1883: # CONFIG_VIDEO_DEV is not set
```

Kconfig takes the **last** assignment and mentions it only as
`warning: override: reassigning to symbol …`, which nobody reads in a build log.
So `/dev/video*` did not exist, and with it nothing of the standard video stack:
no `ffmpeg -f v4l2`, no `gstreamer`, no USB webcam.

Turning it on does not build. 70 errors across 7 files:

```
v4l2-fh.c:32: error: conflicting types for 'v4l2_fh_init'
v4l2-fh.c:46: error: 'struct v4l2_fh' has no member named 'events'
v4l2-ioctl.c:1480: error: too few arguments to function 'v4l2_s_ctrl'
```

The reason: `include/media/*.h` in this tree is **byte-identical to upstream
v3.4** (check it), while `drivers/media/video/v4l2-*.c` is still the 2.6.37
version. Someone moved the tree to 3.4, brought the headers, left the sources,
and closed the switch instead of finishing the port. Replacing those 9 files
with upstream v3.4 takes the error count to zero.

**The general lesson: a disabled config symbol is not always a hardware
decision. Sometimes it is a lid over code that does not compile.** The same
pattern was true here for `CONFIG_ZRAM` (its zsmalloc is x86-only in this tree)
and `CONFIG_CRYPTO_DEV_UX500`. Scan your defconfig for symbols assigned twice
with different values before you trust any of it.

---


## The battery that lies

Worth its own section, because the symptom points away from the cause and
because the same shape shows up on a lot of Samsung boards of this era.

The handset would boot, reach the launcher, and power off about fifty seconds
later. Every time. Nothing in `logcat` said why - the framework shuts down
*cleanly*, so there is no crash to find. The evidence only survives in
`last_kmsg`:

```
init: Received sys.powerctl='shutdown,thermal,battery' from pid: 2296 (system_server)
```

`"thermal,battery"` is `PowerManager.SHUTDOWN_BATTERY_THERMAL_STATE`, which in
Android 10 has exactly one producer worth checking:
`BatteryService.shutdownIfOverTempLocked()`, firing when
`mHealthInfo.batteryTemperature > config_shutdownBatteryTemperature` (default
`680`, i.e. 68.0 °C).

But the device's own sysfs said 27 °C. So the framework was not wrong - it was
being lied to. Reading every power-supply node showed why:

```
/sys/class/power_supply/battery        type=Battery  capacity=45  voltage_now=3830000  temp=287
/sys/class/power_supply/sec-charger    type=Battery  (no readable fields)
/sys/class/power_supply/sec-fuelgauge  type=Battery  capacity=45  voltage_now=3835    temp=-1066830336
```

Three nodes claim to be the battery. `sec-fuelgauge` reports voltage in
**millivolts** where the kernel ABI says microvolts, and its `temp` is a float
printed through an integer formatter - `-1066830336` is `0xC0640000`, which as
an IEEE-754 single is `-3.5625`. Whenever the underlying float was positive,
the same bug produced a huge *positive* integer, and the framework read it as a
temperature.

`BatteryMonitor::init()` does not choose between them in any defined way:

```cpp
case ANDROID_POWER_SUPPLY_TYPE_BATTERY:
    if (mHealthdConfig->batteryTemperaturePath.isEmpty()) {
        path.appendFormat("%s/%s/temp", POWER_SUPPLY_SYSFS_PATH, name);
        if (access(path, R_OK) == 0)
            mHealthdConfig->batteryTemperaturePath = path;
    }
```

First node that answers wins, and the walk order is whatever `readdir` returns.
Per *field*, not per node - so the capacity can come from one and the
temperature from another.

The mechanism Android provides for this is `healthd_config::ignorePowerSupplyNames`,
set from a board-specific `libhealthd.<device>`, selected by one line in
`BoardConfig.mk`:

```make
BOARD_HAL_STATIC_LIBRARIES := libhealthd.golden
``` `device/<vendor>/<device>/health/` in this repo has
the whole file; the shape is:

```cpp
void healthd_board_init(struct healthd_config* config) {
    config->ignorePowerSupplyNames.push_back(String8("sec-fuelgauge"));
    config->ignorePowerSupplyNames.push_back(String8("sec-charger"));
    config->batteryTemperaturePath = String8("/sys/class/power_supply/battery/temp");
    /* ...and every other battery*Path, pinned */
}
```

Both halves matter. `init()` only fills a path that is still empty, so pinning
them makes the directory walk unable to override you even if the ignore list is
ever bypassed.

There is a third line of defence in `healthd_board_battery_update()`, which sees
every poll before the framework does. It refuses values that are not physically
possible - temperature outside -30…90 °C, cell voltage outside 2.5…4.6 V - and,
because this gauge was separately observed jumping 46% → 28% → 29% inside one
session, it refuses to believe a reported 0% while the cell voltage says
otherwise. A gauge that can invent an 18-point drop can invent a zero, and
`BatteryService.shouldShutdownLocked()` turns a zero into a shutdown.

**Two things that do not work, so you can skip them:**

- Raising `config_shutdownBatteryTemperature`. We tried `3000` (300 °C). The
  bogus value was around 1.1 billion. No threshold survives a bit pattern.
- Assuming the node named `battery` wins because it sorts first. `readdir` is
  not sorted, and the code does not sort it.

**How to check your own device in one line:**

```sh
for d in /sys/class/power_supply/*; do \
  echo "$(basename $d) type=$(cat $d/type 2>/dev/null) temp=$(cat $d/temp 2>/dev/null)"; done
```

More than one `type=Battery`, or a `temp` that is not roughly ten times a
plausible °C, and you have this bug.


## A debug mode must not disable the network

Not a kernel bug, a design mistake, and the one that cost the most hours.

This ROM has a "shield mode": no SurfaceFlinger, no zygote, no framework -
just a serial console, USB ethernet and adbd over TCP. It exists so that a
handset whose framework is crash-looping is still a machine you can work on.
Good idea. But the script that entered it also stopped the service that brings
up WiFi without the framework, on the reasoning that shield mode "owns the
handset".

That reasoning is wrong, and the cost was measured rather than argued. When the
handset later wedged in shield mode, exactly one channel was left: rndis at the
end of the USB cable. Had the cable been out, or had it been plugged into a
wall charger instead of the host, the device would have been completely
unreachable with no way back except the power button.

Shield mode stops the *framework*. It has no business stopping the *network*. A
node's network is more fundamental than anything running on top of it: you can
always kill the software over a working link, and you can never fix a link over
dead software.

The rule that came out of it: **whatever your degraded/rescue mode turns off,
it must leave at least two independent ways in, and it must never reduce that
number to one.** Serial plus USB-ethernet is one physical cable - that is one
channel wearing two hats.


## When the log you need is the log that gets erased

Three separate instruments lied about the same failure, and each lie looked
like an answer.

**`last_kmsg` looked empty of causes.** It was not empty - it started at the
50 second mark, because SELinux in permissive mode logs *every* denial and this
ROM produces thousands of them during boot. The ring buffer had wrapped and
taken the interesting part with it. A log that begins in the middle looks like
a log that has nothing in it.

**`bootstat` named a reason that was already fixed.** `Canonical boot reason:
shutdown,thermal,battery` kept appearing after the temperature path had been
corrected and verified. On a device whose bootloader forwards no
`androidboot.bootreason`, that string can be a persisted leftover rather than a
statement about this boot.

**`grep | head -20` hid the answer.** The matches were dominated by `Watchdog`
and thermal-HAL noise near the top of a 6000-line file; every `ShutdownThread`
line sat past line 4600. The command returned quickly, printed twenty plausible
lines, and pointed the wrong way. `head` on a grep of an unfamiliar log is a
way to be confidently wrong.

What worked was writing `logcat` to a file on `/data` and reading it after the
next boot. `init` unmounts `/data` cleanly on its way down - the unmount is
right there in `last_kmsg` - so a file written there survives the very event
you are trying to explain. It is the one buffer on this device that neither
wraps nor is overwritten by the reboot.

```sh
# before the failure
logcat -b main -b system -b crash -v time > /data/sirsch/son.log &
# after the next boot
grep -nE 'ShutdownThread|REQUEST_SHUTDOWN|Shutting down' /data/sirsch/son.log
```


## Layout

```
patches/    one patch per AOSP/kernel repo, apply with `git apply` from that repo's root
device/     files that are new rather than modified
tools/      the host-side harness (see below)
docs/       longer write-ups
```

Patch `07-kernel_samsung_golden.patch` is large mostly because of the V4L2
replacement described above; the rest of it is the timerfd backport.

## Tools

The interesting one is `tools/nobetci.sh` ("the sentry"). This handset has no
working power button, cannot be charged reliably, and takes ~2 minutes to boot,
so hand-driven iteration was not possible. The sentry watches the USB product ID
(`04e8:685d` = recovery, `04e8:6860` = ROM), and on its own: pulls logs and
tombstones from `/cache`, syncs a staged `/system` tree with md5 verification,
writes `boot.img` back to the boot partition, sends the device round again, and
parks in recovery when the battery is too low to finish a boot.

Two things it learned the hard way, both worth copying:

* **`adb shell` eats stdin.** `while read f; do adb shell …; done < <(find …)`
  processes exactly one file and reports success. Read into an array first, and
  put `</dev/null` on every `adb` call.
* **Unreadable is not the same as different.** An early version treated a garbled
  console reply as a version mismatch and threw away a *working* boot. Only act
  on a positively identified answer.

`tools/nod.c` is a ~40 KB static HTTP server that reports the handset's own
state as JSON on port 8088 - uptime, CPU frequency and governor per core,
thermal zones, network interfaces, and a battery block that never gives a
single percentage. It gives what the gauge said, what the voltage implies, the
difference, and `olcer_guvenilir: false` when they disagree by more than 12
points. On a device whose fuel gauge fabricates values that distinction is the
whole point. No libraries, no interpreter, no framework - it answers while
`zygote` is crash-looping, which is when you need it.

`tools/stream.c` and `tools/gecikme.c` are the two memory measurements that
explain why this SoC gains only ~1.27x from its second core: one thread already
saturates the bus (STREAM Triad 574 -> 661 MB/s from one thread to two, and
Scale does not improve at all). `gecikme.c` refuses to print a latency smaller
than one cycle - the first version silently read 0.00 ns at every size because
the compiler kept the pointer chase in registers, and a physically impossible
number is not a small error, it is the measurement not happening.

`tools/goz.cpp` is the detector that makes the handset useful rather than
merely alive: it runs yolo-fastestv2 int8 through ncnn and prints JSON. It is a
file processor, not a video pipeline, and that is a measurement-driven choice -
one core on this SoC already saturates the memory bus, so continuous streaming
is the wrong shape of work, while ~5 fps detection on a device with its own
battery is exactly right for producing events.

It also carries a lesson worth more than the tool. The exported network's
output is **not raw logits**: measured on a blank frame, `[0:12]` (box) lands
in 0.299-0.691, `[12:15]` (objectness) is exactly 0.000, and `[15:95]` (class)
is 0.001-0.148 - the activations were folded into the graph at export. The
first version applied sigmoid and tanh on top of that. It *looked* like it
worked: on a real COCO frame of two bowls of broccoli it correctly said
"broccoli" and "dining table". But every score clustered around 0.61, because
`sigmoid(0) = 0.5` and `sqrt(0.5 x 0.75) = 0.61` - the objectness signal was
gone and the "confidence" was a constant. A detector that finds the right class
while inventing its confidence is more dangerous than one that is simply wrong,
because it silently defeats anyone trying to set a threshold.

The check that caught it was not looking at boxes but at the *distribution* of
the numbers per index range, on a blank input where the answer is known: an
objectness field that is exactly zero on an empty frame is telling you it has
already been through a sigmoid.

After the fix, fp32 and int8 agree closely on the same frames (0.693 vs 0.682
on the top box, same coordinates), which is both the decode's confirmation and
a useful result on its own: on this network int8 costs almost no accuracy and
roughly halves the time.

`tools/konsol-sor.py` drives the CDC-ACM serial console (`ttyGS0`↔`ttyACM0`),
which is the channel that survives when `adbd` cannot. Note it holds the port
open for the whole session — closing it hangs up the shell on the device side.

## Credits

Fixes 1 and 2 correspond to AOSP gerrit changes 1634024 and 1216758. The timerfd
backport is Todd Poynor's "timerfd: Add alarm timers". Everything else was found
the slow way.

## Licence

Patches inherit the licence of the file they modify: Apache-2.0 for AOSP
components, GPL-2.0 for the kernel. The host-side tooling in `tools/` is
Apache-2.0.
