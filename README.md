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
