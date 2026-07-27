/**
 * ultrafine — set the brightness of LG UltraFine displays per physical Thunderbolt port.
 *
 * Works around a macOS pairing bug: the UltraFine's brightness HID device carries no
 * USB serial number, so with two identical monitors macOS pairs display <-> control
 * device by enumeration order. The pairing can come out correct, swapped, collided
 * (both sliders driving one panel) or missing (slider grayed out) on every replug.
 *
 * This tool ignores that pairing and addresses each panel by its USB locationID,
 * which is tied to the Thunderbolt port the monitor is plugged into.
 *
 * Usage:
 *   ultrafine                       show each monitor and its current brightness
 *   ultrafine both                  set every connected monitor to 100%
 *   ultrafine left                  set the monitor labeled "left" to 100%
 *   ultrafine both 60               set every connected monitor to 60%
 *   ultrafine 0x00200000 60         set by locationID
 *   ultrafine left 250nits          set an absolute nits value
 *   ultrafine identify              dim each monitor in turn and (re)assign labels
 *   ultrafine help                  print usage
 *
 * Omitting the value means 100%.
 *
 * Labels live in ~/.config/ultrafine/map.conf as "<locationID> <label>" lines.
 * Re-run identify after plugging a monitor into a different port.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <IOKit/hid/IOHIDManager.h>

#define LG_VENDOR_ID     1086
#define MONITOR_PAGE     0x80
#define NITS_MIN         400
#define NITS_MAX         54000
#define PERCENT_GAMMA    1.8
#define REPORT_SIZE      6
#define MAX_DISPLAYS     8

typedef struct {
	IOHIDDeviceRef device;
	int32_t location;
	int32_t nits;
	char label[64];
} Monitor;

static void config_path(char *out, size_t size)
{
	const char *home = getenv("HOME");
	snprintf(out, size, "%s/.config/ultrafine/map.conf", home ? home : ".");
}

static int32_t number_prop(IOHIDDeviceRef d, CFStringRef key)
{
	int32_t v = -1;
	CFTypeRef t = IOHIDDeviceGetProperty(d, key);
	if (t && CFGetTypeID(t) == CFNumberGetTypeID())
		CFNumberGetValue((CFNumberRef)t, kCFNumberSInt32Type, &v);

	return v;
}

/**
 * Read the brightness feature report. Returns nits*100, or -1 on failure.
 */
static int32_t read_nits(IOHIDDeviceRef d)
{
	uint8_t buf[REPORT_SIZE];
	CFIndex len = REPORT_SIZE;
	memset(buf, 0, sizeof buf);
	if (kIOReturnSuccess != IOHIDDeviceGetReport(d, kIOHIDReportTypeFeature, 0, buf, &len) || 4 > len)
		return -1;

	return (int32_t)(buf[0] | buf[1] << 8 | buf[2] << 16 | buf[3] << 24);
}

static int write_nits(IOHIDDeviceRef d, int32_t nits)
{
	if (NITS_MIN > nits) nits = NITS_MIN;
	if (NITS_MAX < nits) nits = NITS_MAX;

	uint8_t buf[REPORT_SIZE];
	CFIndex len = REPORT_SIZE;
	memset(buf, 0, sizeof buf);
	IOHIDDeviceGetReport(d, kIOHIDReportTypeFeature, 0, buf, &len);

	buf[0] = nits & 0xff;
	buf[1] = (nits >> 8) & 0xff;
	buf[2] = (nits >> 16) & 0xff;
	buf[3] = (nits >> 24) & 0xff;

	return kIOReturnSuccess == IOHIDDeviceSetReport(d, kIOHIDReportTypeFeature, 0, buf, REPORT_SIZE) ? 0 : -1;
}

static int32_t percent_to_nits(double percent)
{
	if (0 > percent) percent = 0;
	if (100 < percent) percent = 100;

	return (int32_t)lround(NITS_MIN + (NITS_MAX - NITS_MIN) * pow(percent / 100.0, PERCENT_GAMMA));
}

static double nits_to_percent(int32_t nits)
{
	double frac = (double)(nits - NITS_MIN) / (NITS_MAX - NITS_MIN);
	if (0 > frac) frac = 0;

	return 100.0 * pow(frac, 1.0 / PERCENT_GAMMA);
}

/**
 * Fill labels from the config file. Unlabeled monitors keep their port as label.
 */
static void load_labels(Monitor *mons, int count)
{
	char path[512];
	config_path(path, sizeof path);
	FILE *f = fopen(path, "r");
	if (!f)
		return;

	char line[256];
	while (fgets(line, sizeof line, f)) {
		int32_t loc = 0;
		char label[64] = "";
		if (2 != sscanf(line, "%i %63s", (int *)&loc, label))
			continue;

		for (int i = 0; i < count; i++)
			if (mons[i].location == loc)
				snprintf(mons[i].label, sizeof mons[i].label, "%s", label);
	}

	fclose(f);
}

static void save_labels(Monitor *mons, int count)
{
	char path[512], dir[512];
	config_path(path, sizeof path);
	snprintf(dir, sizeof dir, "%s", path);
	char *slash = strrchr(dir, '/');
	if (slash) {
		*slash = 0;
		char cmd[600];
		snprintf(cmd, sizeof cmd, "mkdir -p '%s'", dir);
		system(cmd);
	}

	FILE *f = fopen(path, "w");
	if (!f) {
		fprintf(stderr, "ultrafine: cannot write %s\n", path);
		return;
	}

	fprintf(f, "# ultrafine labels — <locationID> <label>. Re-run 'ultrafine identify' after changing ports.\n");
	for (int i = 0; i < count; i++)
		fprintf(f, "0x%08x %s\n", mons[i].location, mons[i].label);

	fclose(f);
	printf("Saved %s\n", path);
}

static int find_monitors(Monitor *mons, int max)
{
	IOHIDManagerRef mgr = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
	CFMutableDictionaryRef match = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	int vid = LG_VENDOR_ID, page = MONITOR_PAGE, usage = 1;
	CFNumberRef n_vid = CFNumberCreate(NULL, kCFNumberIntType, &vid);
	CFNumberRef n_page = CFNumberCreate(NULL, kCFNumberIntType, &page);
	CFNumberRef n_usage = CFNumberCreate(NULL, kCFNumberIntType, &usage);
	CFDictionarySetValue(match, CFSTR(kIOHIDVendorIDKey), n_vid);
	CFDictionarySetValue(match, CFSTR(kIOHIDPrimaryUsagePageKey), n_page);
	CFDictionarySetValue(match, CFSTR(kIOHIDPrimaryUsageKey), n_usage);
	IOHIDManagerSetDeviceMatching(mgr, match);
	IOHIDManagerOpen(mgr, kIOHIDOptionsTypeNone);

	CFSetRef set = IOHIDManagerCopyDevices(mgr);
	if (!set)
		return 0;

	CFIndex found = CFSetGetCount(set);
	if (found > max) found = max;

	IOHIDDeviceRef devs[MAX_DISPLAYS];
	CFSetGetValues(set, (const void **)devs);

	int count = 0;
	for (CFIndex i = 0; i < found; i++) {
		IOHIDDeviceRef d = devs[i];
		if (kIOReturnSuccess != IOHIDDeviceOpen(d, kIOHIDOptionsTypeNone))
			continue;

		mons[count].device = d;
		mons[count].location = number_prop(d, CFSTR(kIOHIDLocationIDKey));
		mons[count].nits = read_nits(d);
		snprintf(mons[count].label, sizeof mons[count].label, "0x%08x", mons[count].location);
		count++;
	}

	// Stable output order: lowest port first.
	for (int i = 0; i < count; i++)
		for (int j = i + 1; j < count; j++)
			if (mons[j].location < mons[i].location) {
				Monitor t = mons[i];
				mons[i] = mons[j];
				mons[j] = t;
			}

	load_labels(mons, count);

	return count;
}

static void print_status(FILE *out, Monitor *mons, int count)
{
	for (int i = 0; i < count; i++) {
		if (0 > mons[i].nits) {
			fprintf(out, "%-10s port 0x%08x  unreadable\n", mons[i].label, mons[i].location);
			continue;
		}

		fprintf(out, "%-10s port 0x%08x  %3.0f%%  (%.0f nits)\n", mons[i].label, mons[i].location,
			nits_to_percent(mons[i].nits), mons[i].nits / 100.0);
	}
}

static int run_identify(Monitor *mons, int count)
{
	printf("Dimming each monitor in turn. Type a label for the one that dims (e.g. left, right).\n\n");
	for (int i = 0; i < count; i++) {
		int32_t restore = 0 < mons[i].nits ? mons[i].nits : NITS_MAX;
		write_nits(mons[i].device, percent_to_nits(10));
		printf("Monitor on port 0x%08x is dim now — label: ", mons[i].location);
		fflush(stdout);

		char in[64] = "";
		if (fgets(in, sizeof in, stdin)) {
			in[strcspn(in, "\r\n")] = 0;
			if (*in)
				snprintf(mons[i].label, sizeof mons[i].label, "%s", in);
		}

		write_nits(mons[i].device, restore);
	}

	printf("\n");
	save_labels(mons, count);

	return 0;
}

static void print_usage(FILE *out)
{
	fprintf(out,
		"ultrafine — brightness control for LG UltraFine displays, addressed by Thunderbolt port.\n"
		"\n"
		"Usage:\n"
		"  ultrafine                     show every monitor and its current brightness\n"
		"  ultrafine <target> [value]    set brightness; the value defaults to 100%%\n"
		"  ultrafine identify            dim each monitor in turn and (re)assign labels\n"
		"  ultrafine help                show this text\n"
		"\n"
		"Target:\n"
		"  both | all                    every connected monitor\n"
		"  <label>                       a monitor you named with 'identify' (e.g. left, right)\n"
		"  0x...                         a monitor's location ID, as shown by 'ultrafine'\n"
		"\n"
		"Value:\n"
		"  0-100                         percent, on a perceptual curve like the macOS slider\n"
		"  <n>nits                       absolute brightness, 4 to 540 nits\n"
		"\n"
		"Examples:\n"
		"  ultrafine both                both monitors to full brightness\n"
		"  ultrafine left 30             the monitor labeled 'left' to 30%%\n"
		"  ultrafine right 250nits       the monitor labeled 'right' to 250 nits\n"
		"\n"
		"Labels live in ~/.config/ultrafine/map.conf and follow the port, not the panel.\n"
		"Run 'identify' again after plugging a monitor into a different port.\n");
}

int main(int argc, char **argv)
{
	if (1 < argc && (0 == strcmp(argv[1], "help") || 0 == strcmp(argv[1], "-h") || 0 == strcmp(argv[1], "--help"))) {
		print_usage(stdout);
		return 0;
	}

	Monitor mons[MAX_DISPLAYS];
	int count = find_monitors(mons, MAX_DISPLAYS);
	if (0 == count) {
		fprintf(stderr, "ultrafine: no LG UltraFine brightness devices found\n");
		return 1;
	}

	if (1 == argc) {
		print_status(stdout, mons, count);
		return 0;
	}

	if (0 == strcmp(argv[1], "identify"))
		return run_identify(mons, count);

	if ('-' == argv[1][0]) {
		fprintf(stderr, "ultrafine: unknown option '%s'\n\n", argv[1]);
		print_usage(stderr);
		return 2;
	}

	const char *target = argv[1];
	// A bare target means full brightness — the case this tool exists for.
	const char *value = 3 <= argc ? argv[2] : "100";
	int all = 0 == strcmp(target, "both") || 0 == strcmp(target, "all");
	int32_t target_loc = '0' == target[0] && 'x' == target[1] ? (int32_t)strtoul(target, NULL, 16) : 0;

	int32_t nits;
	if (strstr(value, "nits"))
		nits = (int32_t)lround(strtod(value, NULL) * 100);
	else
		nits = percent_to_nits(strtod(value, NULL));

	int hits = 0;
	for (int i = 0; i < count; i++) {
		if (!all && mons[i].location != target_loc && 0 != strcmp(mons[i].label, target))
			continue;

		if (0 != write_nits(mons[i].device, nits))
			fprintf(stderr, "ultrafine: write failed on port 0x%08x\n", mons[i].location);

		mons[i].nits = nits;
		hits++;
	}

	if (0 == hits) {
		fprintf(stderr, "ultrafine: no monitor matches '%s'. Connected:\n", target);
		print_status(stderr, mons, count);
		fprintf(stderr, "Name them with 'ultrafine identify', or see 'ultrafine help'.\n");
		return 1;
	}

	usleep(200000);
	for (int i = 0; i < count; i++)
		mons[i].nits = read_nits(mons[i].device);

	print_status(stdout, mons, count);

	return 0;
}
