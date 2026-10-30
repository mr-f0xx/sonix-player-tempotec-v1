// Run from an isolated directory: never overwrite the simulator's resources.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "src/system/device/sysinfo.h"

const char *storage_sd_root(void) { return NULL; }

int main(void) {
	char dir[] = "/tmp/sonix-sysinfo-XXXXXX";
	assert(mkdtemp(dir));
	assert(chdir(dir) == 0);
	assert(system("mkdir -p usr/resource/sonix/components") == 0);
	const char *path = "usr/resource/sonix/components/system-info.json";
	sysinfo_load();
	assert(!sysinfo_base_build_version()[0]);

	FILE *f = fopen(path, "w");
	assert(f);
	fputs("{\"BASE_BUILD_VERSION\":\"2026-03-26 18:49:34\","
		  "\"build_version\":\"1234\",\"OS_version\":\"beta\"}", f);
	fclose(f);
	sysinfo_load();
	assert(strcmp(sysinfo_base_build_version(), "2026-03-26 18:49:34") == 0);
	assert(strcmp(sysinfo_build_version(), "1234") == 0);
	assert(strcmp(sysinfo_os_version(), "beta") == 0);

	// Loading an older image must clear the cached base build.
	f = fopen(path, "w");
	assert(f);
	fputs("{\"build_version\":\"old\"}", f);
	fclose(f);
	sysinfo_load();
	assert(!sysinfo_base_build_version()[0]);
	assert(unlink(path) == 0);
	sysinfo_load();
	assert(!sysinfo_base_build_version()[0]);
	assert(!sysinfo_build_version()[0]);
	assert(system("rmdir usr/resource/sonix/components usr/resource/sonix usr/resource usr") == 0);
	assert(chdir("/") == 0);
	assert(rmdir(dir) == 0);
	puts("PASS sysinfo base build and legacy images");
	return 0;
}
