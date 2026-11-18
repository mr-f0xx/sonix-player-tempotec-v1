#include "src/system/device/axpcharge.h"
#include "src/system/device/batteryprofile.h"
#include "src/system/device/sysinfo.h"

#include <assert.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void fixture(const char *root, const char *node, const char *text) {
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", root, node);
	if (!text) {
		assert(unlink(path) == 0);
	} else {
		FILE *f = fopen(path, "w");
		assert(f && fputs(text, f) >= 0 && fclose(f) == 0);
	}
}

static void test_sysfs(void) {
	char root[] = "/tmp/sonix-batteryprofile-XXXXXX";
	assert(mkdtemp(root));
	char pmic[256], gauge[256], device[512], driver[512];
	snprintf(pmic, sizeof(pmic), "%s/axp_battery", root);
	snprintf(gauge, sizeof(gauge), "%s/battery", root);
	snprintf(device, sizeof(device), "%s/device", gauge);
	snprintf(driver, sizeof(driver), "%s/device/driver", gauge);
	assert(mkdir(pmic, 0700) == 0 && mkdir(gauge, 0700) == 0 && mkdir(device, 0700) == 0);
	assert(symlink("../../../bus/i2c/drivers/cw201x", driver) == 0);
	fixture(pmic, "present", "1\n");
	fixture(pmic, "voltage_now", "4012378\n");
	fixture(pmic, "current_now", "200000\n"); // stock placeholder, never a load reading
	fixture(pmic, "time_to_empty_now", "24000\n");
	fixture(gauge, "capacity", "60\n");
	fixture(gauge, "time_to_empty_now", "360\n");  // raw minutes, not Linux ABI seconds
	batteryprofile_reading_t out = batteryprofile_read(root, 25, true);
	assert(out.pmic_mv == 4012 && out.percent == 60 && out.cw2015_percent && out.gauge_minutes == 360);
	assert(batteryprofile_read(root, 25, false).pmic_mv == -1); // dark poll

	fixture(pmic, "present", "0\n");
	fixture(gauge, "time_to_empty_now", "8191\n");
	out = batteryprofile_read(root, 25, true);
	assert(out.pmic_mv == -1 && out.percent == 60 && out.gauge_minutes == -1);
	fixture(pmic, "present", "1\n");
	fixture(pmic, "voltage_now", "4012378mV\n");
	fixture(gauge, "capacity", "101\n");
	fixture(gauge, "time_to_empty_now", "360\n");
	out = batteryprofile_read(root, 25, true);
	assert(out.pmic_mv == -1 && out.percent == 25 && !out.cw2015_percent && out.gauge_minutes == -1);
	fixture(gauge, "capacity", "0\n");
	assert(batteryprofile_read(root, 25, true).percent == 0);
	fixture(gauge, "capacity", " 60 \n");
	fixture(gauge, "time_to_empty_now", "123456789012345678901234567890123456789\n");
	assert(batteryprofile_read(root, -1, true).gauge_minutes == -1);

	assert(unlink(driver) == 0);
	assert(symlink("../../../bus/i2c/drivers/cw2015", driver) == 0); // upstream uses seconds
	fixture(gauge, "time_to_empty_now", "7200\n");
	out = batteryprofile_read(root, 75, true);
	assert(out.percent == 75 && out.gauge_minutes == -1 && !out.cw2015_percent);
	assert(unlink(driver) == 0 && rmdir(device) == 0);

	out = batteryprofile_read(NULL, -1, true);
	assert(out.percent == -1 && out.pmic_mv == -1 && out.gauge_minutes == -1 && !out.cw2015_percent);
	fixture(gauge, "capacity", NULL);
	fixture(gauge, "time_to_empty_now", NULL);
	fixture(pmic, "present", NULL);
	fixture(pmic, "voltage_now", NULL);
	fixture(pmic, "current_now", NULL);
	fixture(pmic, "time_to_empty_now", NULL);
	assert(rmdir(pmic) == 0 && rmdir(gauge) == 0 && rmdir(root) == 0);
}

static batteryprofile_estimate_t feed(batteryprofile_t *p, uint32_t *now, unsigned seconds, int percent, int minutes) {
	batteryprofile_estimate_t e = {0};
	assert(seconds > 0 && seconds % 5 == 0);
	for (unsigned i = 0; i < seconds; i += 5) {
		*now += 5000;
		e = batteryprofile_step(p, *now, (batteryprofile_reading_t){
			.percent = percent, .gauge_minutes = minutes, .cw2015_percent = true,
		}, false);
	}
	return e;
}

static void test_gauge_and_comparison(void) {
	batteryprofile_t p = {0};
	uint32_t t = UINT32_MAX - 10000; // settling spans the clock wrap
	assert(feed(&p, &t, 10, 50, 300).source == BATTERYPROFILE_NONE);
	batteryprofile_estimate_t base = feed(&p, &t, 5, 50, 300);
	assert(base.source == BATTERYPROFILE_GAUGE && base.draw_ma == 140 && base.minutes_left == 294);

	p = (batteryprofile_t){0}; // changing the load invalidates the old sample
	assert(feed(&p, &t, 10, 50, 210).source == BATTERYPROFILE_NONE);
	batteryprofile_estimate_t high = feed(&p, &t, 5, 50, 210);
	assert(high.source == BATTERYPROFILE_GAUGE && high.draw_ma == 200 && high.minutes_left == 206);
	int ma, percent;
	assert(batteryprofile_delta(high, base, &ma, &percent) && ma == 60 && percent == 43);
	assert(batteryprofile_delta(base, high, &ma, &percent) && ma == -60 && percent == -30);
	assert(batteryprofile_delta(base, base, &ma, &percent) && ma == 0 && percent == 0);
	high.source = BATTERYPROFILE_TREND;
	assert(!batteryprofile_delta(high, base, &ma, &percent));
	assert(!batteryprofile_delta((batteryprofile_estimate_t){0}, base, &ma, &percent));

	// VBUS suppresses runtime even when the charger is held at its target.
	batteryprofile_reading_t r = {.percent = 50, .gauge_minutes = 210, .cw2015_percent = true};
	assert(batteryprofile_step(&p, t + 1, r, true).source == BATTERYPROFILE_NONE);
	assert(!p.active && feed(&p, &t, 10, 50, 210).source == BATTERYPROFILE_NONE);
	assert(feed(&p, &t, 5, 50, 210).source == BATTERYPROFILE_GAUGE);
	assert(feed(&p, &t, 5, 50, 0).source == BATTERYPROFILE_NONE);
	assert(feed(&p, &t, 5, 50, 8191).source == BATTERYPROFILE_NONE);
	assert(feed(&p, &t, 5, 50, 1).source == BATTERYPROFILE_NONE); // implausible inferred load
	assert(feed(&p, &t, 5, 1, 1).source == BATTERYPROFILE_NONE);
	assert(feed(&p, &t, 5, -1, 210).source == BATTERYPROFILE_NONE);

	assert(feed(&p, &t, 15, 50, 210).source == BATTERYPROFILE_GAUGE);
	assert(feed(&p, &t, 5, 51, 210).source == BATTERYPROFILE_NONE); // upward gauge correction
	assert(feed(&p, &t, 10, 51, 210).source == BATTERYPROFILE_GAUGE);
	assert(feed(&p, &t, 5, 48, 210).source == BATTERYPROFILE_NONE); // several missed points
	assert(feed(&p, &t, 10, 48, 210).source == BATTERYPROFILE_GAUGE);
	t += 25000; // a suspend gap, not another active-load sample
	assert(feed(&p, &t, 5, 48, 210).source == BATTERYPROFILE_NONE);
	assert(feed(&p, &t, 10, 48, 210).source == BATTERYPROFILE_GAUGE);
	r.cw2015_percent = false;
	r.percent = 48;
	assert(batteryprofile_step(&p, t + 5000, r, false).source == BATTERYPROFILE_NONE);
	assert(!p.cw2015_percent);
}

static void test_charge_trend(void) {
	batteryprofile_t p = {0};
	uint32_t t = 0;
	assert(feed(&p, &t, 20, 80, 640).source == BATTERYPROFILE_GAUGE);
	assert(feed(&p, &t, 5, 79, -1).source == BATTERYPROFILE_NONE); // first edge, not a whole point
	assert(feed(&p, &t, 295, 79, -1).source == BATTERYPROFILE_NONE);
	assert(feed(&p, &t, 5, 78, -1).source == BATTERYPROFILE_NONE); // only one whole point
	assert(feed(&p, &t, 415, 78, -1).source == BATTERYPROFILE_NONE);
	batteryprofile_estimate_t e = feed(&p, &t, 5, 77, 616); // favour the observed rate, even with valid RRT
	assert(e.source == BATTERYPROFILE_TREND && e.draw_ma == 140 && e.minutes_left == 456);
	assert(feed(&p, &t, 355, 77, -1).source == BATTERYPROFILE_TREND);
	e = feed(&p, &t, 5, 76, -1); // roll the oldest interval out
	assert(e.source == BATTERYPROFILE_TREND && e.draw_ma == 129);
	assert(feed(&p, &t, 1085, 76, -1).source == BATTERYPROFILE_NONE); // gauge stopped falling
	assert(feed(&p, &t, 5, 75, -1).source == BATTERYPROFILE_NONE);
	assert(feed(&p, &t, 5, 74, -1).source == BATTERYPROFILE_NONE); // too quick to count
	assert(p.interval_ms == 0);
}

// axpcharge's real read-only path, with a fake I2C adapter. In particular the
// profiler must never write REG62 or treat a failed transfer as 0 mA.
static unsigned reg62;
static bool missing_adapter, failed_read;
static unsigned reads, closes;

const sysinfo_model_t *sysinfo_model(void) {
	static const sysinfo_model_t model = {.pmic_charger = true};
	return &model;
}

int __wrap_open(const char *path, int flags, ...) {
	(void)flags;
	assert(strcmp(path, "/dev/i2c-0") == 0);
	return missing_adapter ? -1 : 7;
}

int __wrap_open64(const char *path, int flags, ...) { return __wrap_open(path, flags); }

int __wrap_ioctl(int fd, unsigned long op, ...) {
	assert(fd == 7);
	va_list args;
	va_start(args, op);
	if (op == I2C_SLAVE_FORCE) {
		assert(va_arg(args, int) == 0x34);
		va_end(args);
		return 0;
	}
	assert(op == I2C_SMBUS);
	struct i2c_smbus_ioctl_data *transfer = va_arg(args, struct i2c_smbus_ioctl_data *);
	va_end(args);
	assert(transfer->command == 0x62 && transfer->read_write == I2C_SMBUS_READ &&
		   transfer->size == I2C_SMBUS_BYTE_DATA);
	reads++;
	transfer->data->byte = (uint8_t)reg62;
	return failed_read ? -1 : 0;
}

int __wrap_close(int fd) {
	assert(fd == 7);
	closes++;
	return 0;
}

static void test_charge_setting(void) {
	reg62 = 0xab; // mask unrelated high bits
	assert(axpcharge_current_setting_ma() == 500);
	reg62 = 8;
	assert(axpcharge_current_setting_ma() == 200);
	reg62 = 0;
	assert(axpcharge_current_setting_ma() == 0);
	reg62 = 4;
	assert(axpcharge_current_setting_ma() == 100);
	reg62 = 16;
	assert(axpcharge_current_setting_ma() == 1000);
	reg62 = 3;
	assert(axpcharge_current_setting_ma() == -1);
	reg62 = 17;
	assert(axpcharge_current_setting_ma() == -1);
	failed_read = true;
	reg62 = 8;
	assert(axpcharge_current_setting_ma() == -1);
	missing_adapter = true;
	assert(axpcharge_current_setting_ma() == -1);
	assert(reads == 8 && closes == 8);
}

int main(void) {
	test_sysfs();
	test_gauge_and_comparison();
	test_charge_trend();
	test_charge_setting();
	puts("PASS battery profiler (stock sysfs, rate and runtime estimates, baseline, read-only charger setting)");
	return 0;
}
