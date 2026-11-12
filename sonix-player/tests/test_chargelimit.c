#include <assert.h>
#include <stdio.h>

#include "src/system/device/chargelimit.h"

int main(void) {
	// While charging, stop at the configured threshold.
	assert(!charge_limit_should_allow(80, 80, false));
	assert(!charge_limit_should_allow(80, 81, false));
	assert(charge_limit_should_allow(80, 79, false));

	// Once stopped, use three points of hysteresis to avoid chatter.
	assert(!charge_limit_should_allow(80, 78, true));
	assert(charge_limit_should_allow(80, 77, true));

	// Full charge and the higher slider values use the same rule.
	assert(charge_limit_should_allow(100, 100, true));
	assert(!charge_limit_should_allow(90, 90, false));
	assert(!charge_limit_should_allow(90, 88, true));
	assert(charge_limit_should_allow(90, 87, true));

	puts("PASS charge limit threshold and hysteresis");
	return 0;
}
