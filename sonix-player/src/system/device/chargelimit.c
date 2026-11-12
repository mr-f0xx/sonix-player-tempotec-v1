#include "chargelimit.h"

bool charge_limit_should_allow(int limit, int battery_percent, bool held) {
	if (limit >= 100) {
		return true;
	}
	return held ? battery_percent <= limit - 3 : battery_percent < limit;
}
