/*
 * golden (Samsung GT-I8190, ST-Ericsson u8500) - board hook for healthd.
 *
 * WHY THIS FILE EXISTS
 *
 * This handset exposes THREE power-supply nodes whose `type` reads "Battery":
 *
 *   battery         capacity=45  voltage_now=3830000 (uV)  temp=287   status=Charging
 *   sec-charger     (type only, no readable fields)
 *   sec-fuelgauge   capacity=45  voltage_now=3835    (mV)  temp=-1066830336
 *
 * Only the first is sane.  sec-fuelgauge reports voltage in millivolts where
 * the kernel ABI says microvolts, and its `temp` is not a temperature at all -
 * -1066830336 is 0xC0640000, the raw IEEE-754 bit pattern of -3.5625.  The
 * driver is printing a float through an integer formatter.
 *
 * BatteryMonitor::init() walks /sys/class/power_supply in readdir order and
 * takes the first node that answers for each field, so which of the three wins
 * is not defined anywhere - it is whatever order the filesystem hands back.
 * When that garbage temperature happened to carry a positive sign bit the
 * framework read it as tens of thousands of degrees, BatteryService called
 * shutdownIfOverTempLocked(), and the handset powered itself off about fifty
 * seconds into every boot:
 *
 *   init: Received sys.powerctl='shutdown,thermal,battery' from pid: 2296 (system_server)
 *
 * That is the framework behaving correctly on fabricated data.  Raising
 * config_shutdownBatteryTemperature does not fix it - no threshold survives a
 * number that is really a float's bit pattern.  The fix is to stop reading
 * that node, and then to refuse to pass on any value that is impossible
 * anyway.
 */

#include <batteryservice/BatteryService.h>
#include <cutils/klog.h>
#include <healthd/healthd.h>

using android::String8;

#define LOG_TAG "healthd-golden"
#define PS "/sys/class/power_supply"

// Plausibility window for a lithium cell in a room, in tenths of a degree C.
// Anything outside this did not come from a thermistor.
static const int SICAKLIK_ALT = -300;   // -30.0 C
static const int SICAKLIK_UST = 900;    //  90.0 C
static const int SICAKLIK_GUVENLI = 250;

// Cell voltage in mV.  A single Li-ion cell cannot be outside this and still
// be a cell.
static const int GERILIM_ALT = 2500;
static const int GERILIM_UST = 4600;
static const int GERILIM_GUVENLI = 3800;

// Below this the pack really is empty and a level of 0 should be believed.
// Above it, a reported 0% is the gauge lying and must not power the phone off.
static const int BOSALMA_GERILIMI = 3400;

void healthd_board_init(struct healthd_config* config) {
    // Belt: never even look at the two bad nodes.
    config->ignorePowerSupplyNames.push_back(String8("sec-fuelgauge"));
    config->ignorePowerSupplyNames.push_back(String8("sec-charger"));

    // Braces: pin every field to the node that answers honestly.  init() only
    // fills a path that is still empty, so pinning them here makes the
    // directory scan unable to override us even if the ignore list is missed.
    config->batteryStatusPath      = String8(PS "/battery/status");
    config->batteryHealthPath      = String8(PS "/battery/health");
    config->batteryPresentPath     = String8(PS "/battery/present");
    config->batteryCapacityPath    = String8(PS "/battery/capacity");
    config->batteryVoltagePath     = String8(PS "/battery/voltage_now");
    config->batteryTemperaturePath = String8(PS "/battery/temp");
    config->batteryTechnologyPath  = String8(PS "/battery/technology");
    config->batteryCurrentNowPath  = String8(PS "/battery/current_now");

    // Left deliberately empty: this gauge has no trustworthy charge counter,
    // and a wrong one is worse than none - the framework treats a present
    // counter as authoritative for its remaining-time estimate.
    config->batteryCurrentAvgPath.clear();
    config->batteryChargeCounterPath.clear();
    config->batteryFullChargePath.clear();
}

int healthd_board_battery_update(struct android::BatteryProperties* props) {
    if (props == nullptr) return 0;

    // Third line of defence, and the only one that still works if a future
    // kernel renames these nodes.  Every value the framework can act on
    // destructively is checked against physics before it is passed up.
    //
    // Logged once per transition rather than once per poll: healthd polls
    // every 60 s and a permanently broken sensor would otherwise fill the
    // kernel ring buffer, which is the one place the evidence for the next
    // failure has to fit.
    static bool sicaklik_bildirildi = false;
    static bool gerilim_bildirildi = false;
    static bool seviye_bildirildi = false;

    if (props->batteryTemperature < SICAKLIK_ALT ||
        props->batteryTemperature > SICAKLIK_UST) {
        if (!sicaklik_bildirildi) {
            KLOG_WARNING(LOG_TAG, "implausible temperature %d, using %d\n",
                         props->batteryTemperature, SICAKLIK_GUVENLI);
            sicaklik_bildirildi = true;
        }
        props->batteryTemperature = SICAKLIK_GUVENLI;
    } else {
        sicaklik_bildirildi = false;
    }

    if (props->batteryVoltage < GERILIM_ALT || props->batteryVoltage > GERILIM_UST) {
        if (!gerilim_bildirildi) {
            KLOG_WARNING(LOG_TAG, "implausible voltage %d mV, using %d\n",
                         props->batteryVoltage, GERILIM_GUVENLI);
            gerilim_bildirildi = true;
        }
        props->batteryVoltage = GERILIM_GUVENLI;
    } else {
        gerilim_bildirildi = false;
    }

    // The fuel gauge on this handset jumps: 46% -> 28% -> 29% was measured
    // inside a single session while the cell sat steady at 3.79 V.  A gauge
    // that can invent an 18-point drop can invent a zero, and a zero is what
    // BatteryService.shouldShutdownLocked() turns into a shutdown.  So a
    // reported empty is only believed when the voltage agrees it is empty.
    if (props->batteryLevel <= 0 && props->batteryVoltage > BOSALMA_GERILIMI) {
        if (!seviye_bildirildi) {
            KLOG_WARNING(LOG_TAG, "level 0 at %d mV - gauge disbelieved\n",
                         props->batteryVoltage);
            seviye_bildirildi = true;
        }
        props->batteryLevel = 1;
    } else {
        seviye_bildirildi = false;
    }

    // 0 = let healthd log the periodic battery line to the kernel log.  On a
    // handset whose only surviving evidence across a reset is last_kmsg, that
    // line is worth its space.
    return 0;
}
