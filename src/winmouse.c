// winmouse: Windows 11 pointer ballistics for Linux evdev mice.
//
// The arithmetic below is a transcription of win32kbase.sys (PDB GUID
// 7BD4193FFA935A0483172A97C84B455D1):
//   CMouseAcceleration::MOUSE_SENSITIVITY_INFO::UpdateMouseSensitivity
//   GetNormalizedMouseSensitivityFactor
//   CMouseProcessor::ApplyAccelerationToDelta          (EPP off)
//   CDeviceAcceleration::_BuildAccelerationCurve
//   CDeviceAcceleration::Accelerate                     (EPP on)
// See docs/REVERSE-ENGINEERING.md for the disassembly walkthrough.
//
// Physical USB/Bluetooth mice are grabbed, every evdev frame (one HID report,
// the same unit Windows accelerates) is transformed, and the result is
// re-emitted through a uinput device that Hyprland must treat as flat 1:1.

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <unistd.h>

#define VIRTUAL_NAME "WinMouse Virtual Pointer"
#define MAX_DEVICES 16
#define MAX_FRAME 64
#define NPOINTS 5

// HKCU\Control Panel\Mouse defaults (16.16 fixed point), identical to the
// values read from this machine's Windows registry.
static const int64_t default_curve_x[NPOINTS] = {0, 0x6e15, 0x14000, 0x3dc29, 0x280000};
static const int64_t default_curve_y[NPOINTS] = {0, 0x111fd, 0x42400, 0x12fc00, 0x1bbc000};

struct settings {
	unsigned sensitivity; // MouseSensitivity, 1..20 (10 = default)
	bool epp;             // MouseSpeed != 0 ("Enhance pointer precision")
	unsigned dpi;         // monitor DPI Windows would use (96 = 100%)
	int64_t curve_x[NPOINTS], curve_y[NPOINTS];
};

// CDeviceAcceleration::Ballistics as laid out by _BuildAccelerationCurve.
struct ballistics {
	int64_t x[NPOINTS], y[NPOINTS];
	int64_t slope[NPOINTS - 1], intercept[NPOINTS - 1];
};

// Per-session subpixel state (thread info +0x4f98 in Accelerate,
// CMouseProcessor +0x24/+0x28 in ApplyAccelerationToDelta).
struct accel_state {
	int64_t rem_x, rem_y;
	uint32_t last_segment;
	int32_t linear_rem_x, linear_rem_y;
};

// UpdateMouseSensitivity: slider position -> gain in 1/256 units.
static int32_t linear_factor(unsigned s)
{
	if (s <= 2)
		return (int32_t)((s << 8) >> 5);
	if (s <= 10)
		return (int32_t)(((s << 8) - 0x200) >> 3);
	return (int32_t)(((s << 8) - 0x600) >> 2);
}

// GetNormalizedMouseSensitivityFactor: scaled up on monitors above 96 DPI.
static int32_t normalized_factor(unsigned s, unsigned dpi)
{
	int32_t f = linear_factor(s);
	if (dpi <= 96)
		return f;
	int64_t v = ((int64_t)(f < 0 ? -f : f) * dpi + 48) / 96;
	if (v > INT32_MAX)
		return f < 0 ? INT32_MIN : INT32_MAX;
	return (int32_t)(f < 0 ? -v : v);
}

static void build_curve(struct ballistics *b, const struct settings *cfg)
{
	int64_t sens = (int64_t)(((uint64_t)cfg->sensitivity << 16) / 10);
	unsigned dpi = (uint16_t)cfg->dpi < 96 ? 96 : (uint16_t)cfg->dpi;
	int64_t dpi_scale = (int64_t)(((uint64_t)dpi << 16) / 120);

	for (int i = 0; i < NPOINTS; i++) {
		b->y[i] = (((dpi_scale * cfg->curve_y[i]) >> 16) * sens) >> 16;
		b->x[i] = (cfg->curve_x[i] * 0x38000) >> 16;
	}
	for (int i = 1; i < NPOINTS; i++) {
		int64_t run = b->x[i] - b->x[i - 1];
		int64_t slope = 0, intercept = 0;
		if (run != 0) {
			slope = (b->y[i] - b->y[i - 1]) * 65536 / run;
			intercept = b->y[i - 1] - ((slope * b->x[i - 1]) >> 16);
		}
		b->slope[i - 1] = slope;
		b->intercept[i - 1] = intercept;
	}
}

// Integer part truncated toward zero; the 16-bit fraction stays as remainder.
static int64_t split_fixed(int64_t v, int64_t *rem)
{
	int64_t whole = v >= 0 ? (v & ~(int64_t)0xffff) : -((-v) & ~(int64_t)0xffff);
	*rem = v - whole;
	return whole / 65536;
}

static void accelerate_epp(const struct ballistics *b, struct accel_state *s, int *dx, int *dy)
{
	int64_t mx = (int64_t)*dx * 65536, my = (int64_t)*dy * 65536;
	int64_t ax = llabs(mx), ay = llabs(my);
	int64_t minor = ax >= ay ? my : mx;
	int64_t major = ax <= ay ? my : mx;
	int64_t speed = llabs(minor) / 2 + llabs(major);
	if (speed == 0)
		return;

	uint32_t seg = 0;
	while (speed > b->x[seg]) {
		if (++seg >= NPOINTS - 1)
			break;
	}
	seg--;

	int64_t gain = b->intercept[seg] * 65536 / speed + b->slope[seg];
	if (s->last_segment < seg)
		gain = (gain + b->intercept[s->last_segment] * 65536 / speed + b->slope[s->last_segment]) >> 1;
	s->last_segment = seg;

	*dx = (int)split_fixed(((mx * gain) >> 16) + s->rem_x, &s->rem_x);
	*dy = (int)split_fixed(((my * gain) >> 16) + s->rem_y, &s->rem_y);
}

static void accelerate_linear(int32_t factor, struct accel_state *s, int *dx, int *dy)
{
	if (factor == 256)
		return;
	if (*dx) {
		int32_t n = factor * *dx + s->linear_rem_x;
		*dx = n / 256;
		s->linear_rem_x = n % 256;
	}
	if (*dy) {
		int32_t n = factor * *dy + s->linear_rem_y;
		*dy = n / 256;
		s->linear_rem_y = n % 256;
	}
}

struct engine {
	struct settings cfg;
	struct ballistics curve;
	int32_t factor;
	struct accel_state state;
};

static void engine_apply(struct engine *e, int *dx, int *dy)
{
	if (e->cfg.epp)
		accelerate_epp(&e->curve, &e->state, dx, dy);
	else
		accelerate_linear(e->factor, &e->state, dx, dy);
}

static void engine_configure(struct engine *e, const struct settings *cfg)
{
	e->cfg = *cfg;
	build_curve(&e->curve, cfg);
	e->factor = normalized_factor(cfg->sensitivity, cfg->dpi);
}

// ---------------------------------------------------------------- config --

static void default_settings(struct settings *c)
{
	c->sensitivity = 10;
	c->epp = true;
	c->dpi = 96;
	memcpy(c->curve_x, default_curve_x, sizeof c->curve_x);
	memcpy(c->curve_y, default_curve_y, sizeof c->curve_y);
}

static bool parse_curve(const char *v, int64_t out[NPOINTS])
{
	int64_t tmp[NPOINTS];
	char *end;
	for (int i = 0; i < NPOINTS; i++) {
		errno = 0;
		tmp[i] = strtoll(v, &end, 0);
		if (errno || end == v || tmp[i] < 0 || tmp[i] > 0x7fffffff)
			return false;
		v = end + strspn(end, ", \t");
	}
	if (*v && *v != '\n')
		return false;
	memcpy(out, tmp, sizeof tmp);
	return true;
}

static void config_path(char *buf, size_t len)
{
	const char *xdg = getenv("XDG_CONFIG_HOME");
	if (xdg && *xdg)
		snprintf(buf, len, "%s/winmouse/config", xdg);
	else
		snprintf(buf, len, "%s/.config/winmouse/config", getenv("HOME") ? getenv("HOME") : ".");
}

static void load_settings(const char *path, struct settings *c)
{
	default_settings(c);
	FILE *f = fopen(path, "r");
	if (!f) {
		fprintf(stderr, "winmouse: %s not readable, using Windows defaults\n", path);
		return;
	}
	char line[512];
	while (fgets(line, sizeof line, f)) {
		char *eq = strchr(line, '=');
		if (line[0] == '#' || !eq)
			continue;
		*eq = '\0';
		char *key = line, *val = eq + 1;
		key[strcspn(key, " \t")] = '\0';
		val += strspn(val, " \t");
		long n = strtol(val, NULL, 0);
		if (!strcmp(key, "sensitivity") && n >= 1 && n <= 20)
			c->sensitivity = (unsigned)n;
		else if (!strcmp(key, "enhance_pointer_precision"))
			c->epp = n != 0;
		else if (!strcmp(key, "dpi") && n >= 96 && n <= 960)
			c->dpi = (unsigned)n;
		else if (!strcmp(key, "smooth_mouse_x_curve") && !parse_curve(val, c->curve_x))
			fprintf(stderr, "winmouse: ignoring invalid smooth_mouse_x_curve\n");
		else if (!strcmp(key, "smooth_mouse_y_curve") && !parse_curve(val, c->curve_y))
			fprintf(stderr, "winmouse: ignoring invalid smooth_mouse_y_curve\n");
	}
	fclose(f);
}

// ---------------------------------------------------------------- devices --

#define BIT(arr, n) ((arr)[(n) / (8 * sizeof(long))] >> ((n) % (8 * sizeof(long))) & 1)
#define NLONGS(n) (((n) + 8 * sizeof(long) - 1) / (8 * sizeof(long)))

struct device {
	int fd;
	dev_t rdev;
	bool hires_wheel, hires_hwheel, dropping;
	int dx, dy, nframe;
	struct input_event frame[MAX_FRAME];
	unsigned long keys[NLONGS(KEY_CNT)];
	char name[128];
};

static struct device devices[MAX_DEVICES];
static int ndevices;
static int uinput_fd = -1;

static bool tracked(dev_t rdev)
{
	for (int i = 0; i < ndevices; i++)
		if (devices[i].rdev == rdev)
			return true;
	return false;
}

static void try_open(const char *path)
{
	struct stat st;
	if (stat(path, &st) < 0 || !S_ISCHR(st.st_mode) || tracked(st.st_rdev) || ndevices == MAX_DEVICES)
		return;
	int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return;

	struct input_id id;
	char name[128] = "";
	unsigned long rel[NLONGS(REL_CNT)] = {0}, key[NLONGS(KEY_CNT)] = {0};
	if (ioctl(fd, EVIOCGID, &id) < 0 || ioctl(fd, EVIOCGNAME(sizeof name), name) < 0 ||
	    ioctl(fd, EVIOCGBIT(EV_REL, sizeof rel), rel) < 0 || ioctl(fd, EVIOCGBIT(EV_KEY, sizeof key), key) < 0)
		goto skip;
	if (!strcmp(name, VIRTUAL_NAME) || (id.bustype != BUS_USB && id.bustype != BUS_BLUETOOTH))
		goto skip;
	if (!BIT(rel, REL_X) || !BIT(rel, REL_Y) || !BIT(key, BTN_LEFT))
		goto skip;
	if (ioctl(fd, EVIOCGRAB, 1) < 0) {
		fprintf(stderr, "winmouse: cannot grab %s (%s): %s\n", path, name, strerror(errno));
		goto skip;
	}

	struct device *d = &devices[ndevices++];
	memset(d, 0, sizeof *d);
	d->fd = fd;
	d->rdev = st.st_rdev;
	d->hires_wheel = BIT(rel, REL_WHEEL_HI_RES);
	d->hires_hwheel = BIT(rel, REL_HWHEEL_HI_RES);
	ioctl(fd, EVIOCGKEY(sizeof d->keys), d->keys);
	snprintf(d->name, sizeof d->name, "%s", name);
	fprintf(stderr, "winmouse: grabbed %s (%s)\n", path, name);
	return;
skip:
	close(fd);
}

static void scan_devices(void)
{
	DIR *dir = opendir("/dev/input");
	if (!dir)
		return;
	struct dirent *de;
	char path[300];
	while ((de = readdir(dir))) {
		if (strncmp(de->d_name, "event", 5))
			continue;
		snprintf(path, sizeof path, "/dev/input/%s", de->d_name);
		try_open(path);
	}
	closedir(dir);
}

static void remove_device(int i)
{
	fprintf(stderr, "winmouse: released %s\n", devices[i].name);
	close(devices[i].fd);
	devices[i] = devices[--ndevices];
}

static void release_all(void)
{
	while (ndevices)
		remove_device(ndevices - 1);
}

// ---------------------------------------------------------------- uinput --

static int create_uinput(void)
{
	int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return -1;
	ioctl(fd, UI_SET_EVBIT, EV_KEY);
	ioctl(fd, UI_SET_EVBIT, EV_REL);
	ioctl(fd, UI_SET_EVBIT, EV_MSC);
	for (int k = BTN_LEFT; k <= BTN_TASK; k++)
		ioctl(fd, UI_SET_KEYBIT, k);
	static const int rels[] = {REL_X, REL_Y, REL_WHEEL, REL_HWHEEL, REL_WHEEL_HI_RES, REL_HWHEEL_HI_RES};
	for (size_t i = 0; i < sizeof rels / sizeof *rels; i++)
		ioctl(fd, UI_SET_RELBIT, rels[i]);
	ioctl(fd, UI_SET_MSCBIT, MSC_SCAN);
	ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_POINTER);

	struct uinput_setup setup = {.id = {.bustype = BUS_VIRTUAL, .vendor = 0x1209, .product = 0x57ad, .version = 1}};
	snprintf(setup.name, UINPUT_MAX_NAME_SIZE, VIRTUAL_NAME);
	if (ioctl(fd, UI_DEV_SETUP, &setup) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static void emit_frame(const struct input_event *ev, int n)
{
	if (n == 0)
		return;
	ssize_t len = (ssize_t)(n * sizeof *ev);
	if (write(uinput_fd, ev, (size_t)len) != len)
		fprintf(stderr, "winmouse: uinput write failed: %s\n", strerror(errno));
}

static void push(struct input_event *out, int *n, int type, int code, int value)
{
	if (*n < MAX_FRAME + 4)
		out[(*n)++] = (struct input_event){.type = (uint16_t)type, .code = (uint16_t)code, .value = value};
}

// After SYN_DROPPED, re-read button state so nothing stays pressed.
static void resync_buttons(struct device *d)
{
	unsigned long now[NLONGS(KEY_CNT)] = {0};
	struct input_event out[MAX_FRAME + 4];
	int n = 0;
	if (ioctl(d->fd, EVIOCGKEY(sizeof now), now) < 0)
		return;
	for (int k = BTN_LEFT; k <= BTN_TASK; k++)
		if (BIT(now, k) != BIT(d->keys, k))
			push(out, &n, EV_KEY, k, (int)BIT(now, k));
	memcpy(d->keys, now, sizeof now);
	if (n) {
		push(out, &n, EV_SYN, SYN_REPORT, 0);
		emit_frame(out, n);
	}
}

static void flush_frame(struct engine *e, struct device *d)
{
	struct input_event out[MAX_FRAME + 4];
	int n = 0;
	for (int i = 0; i < d->nframe; i++)
		out[n++] = d->frame[i];
	if (d->dx || d->dy) {
		engine_apply(e, &d->dx, &d->dy);
		if (d->dx)
			push(out, &n, EV_REL, REL_X, d->dx);
		if (d->dy)
			push(out, &n, EV_REL, REL_Y, d->dy);
	}
	if (n) {
		push(out, &n, EV_SYN, SYN_REPORT, 0);
		emit_frame(out, n);
	}
	d->dx = d->dy = d->nframe = 0;
}

static void buffer_event(struct device *d, int type, int code, int value)
{
	if (d->nframe < MAX_FRAME)
		d->frame[d->nframe++] = (struct input_event){.type = (uint16_t)type, .code = (uint16_t)code, .value = value};
}

// Returns false when the device is gone.
static bool handle_device(struct engine *e, struct device *d)
{
	struct input_event ev[64];
	for (;;) {
		ssize_t r = read(d->fd, ev, sizeof ev);
		if (r < 0)
			return errno == EAGAIN || errno == EINTR;
		if (r == 0)
			return false;
		for (size_t i = 0; i < (size_t)r / sizeof *ev; i++) {
			struct input_event *x = &ev[i];
			if (x->type == EV_SYN && x->code == SYN_DROPPED) {
				d->dropping = true;
				d->dx = d->dy = d->nframe = 0;
			} else if (x->type == EV_SYN && x->code == SYN_REPORT) {
				if (d->dropping) {
					d->dropping = false;
					resync_buttons(d);
				} else {
					flush_frame(e, d);
				}
			} else if (d->dropping) {
				continue;
			} else if (x->type == EV_REL && x->code == REL_X) {
				d->dx += x->value;
			} else if (x->type == EV_REL && x->code == REL_Y) {
				d->dy += x->value;
			} else {
				if (x->type == EV_KEY && x->code < KEY_CNT) {
					unsigned long mask = 1UL << (x->code % (8 * sizeof(long)));
					if (x->value)
						d->keys[x->code / (8 * sizeof(long))] |= mask;
					else
						d->keys[x->code / (8 * sizeof(long))] &= ~mask;
				}
				buffer_event(d, x->type, x->code, x->value);
				if (x->type == EV_REL && x->code == REL_WHEEL && !d->hires_wheel)
					buffer_event(d, EV_REL, REL_WHEEL_HI_RES, x->value * 120);
				if (x->type == EV_REL && x->code == REL_HWHEEL && !d->hires_hwheel)
					buffer_event(d, EV_REL, REL_HWHEEL_HI_RES, x->value * 120);
			}
		}
	}
}

// ---------------------------------------------------------------- main --

static void describe(const struct engine *e)
{
	fprintf(stderr, "winmouse: sensitivity=%u epp=%d dpi=%u", e->cfg.sensitivity, e->cfg.epp, e->cfg.dpi);
	if (e->cfg.epp) {
		fprintf(stderr, " curve:");
		for (int i = 0; i < NPOINTS; i++)
			fprintf(stderr, " (%.4f,%.4f)", e->curve.x[i] / 65536.0, e->curve.y[i] / 65536.0);
	} else {
		fprintf(stderr, " factor=%d/256", e->factor);
	}
	fputc('\n', stderr);
}

// `winmouse --simulate SENS EPP DPI` reads "dx dy" packets from stdin and
// prints the Windows result, for verification against the original code.
static int simulate(char **argv)
{
	struct engine e = {0};
	struct settings cfg;
	default_settings(&cfg);
	cfg.sensitivity = (unsigned)atoi(argv[0]);
	cfg.epp = atoi(argv[1]) != 0;
	cfg.dpi = (unsigned)atoi(argv[2]);
	engine_configure(&e, &cfg);
	if (getenv("WINMOUSE_PRINT_CURVE")) {
		for (int i = 0; i < NPOINTS; i++)
			printf("point %lld %lld\n", (long long)e.curve.x[i], (long long)e.curve.y[i]);
		for (int i = 0; i < NPOINTS - 1; i++)
			printf("segment %lld %lld\n", (long long)e.curve.slope[i], (long long)e.curve.intercept[i]);
		printf("factor %d\n", e.factor);
	}
	int dx, dy;
	while (scanf("%d %d", &dx, &dy) == 2) {
		engine_apply(&e, &dx, &dy);
		printf("%d %d\n", dx, dy);
	}
	return 0;
}

int main(int argc, char **argv)
{
	if (argc == 5 && !strcmp(argv[1], "--simulate"))
		return simulate(argv + 2);

	char path[PATH_MAX];
	config_path(path, sizeof path);
	struct settings cfg;
	struct engine engine = {0};
	load_settings(path, &cfg);
	engine_configure(&engine, &cfg);
	describe(&engine);

	uinput_fd = create_uinput();
	if (uinput_fd < 0) {
		fprintf(stderr, "winmouse: cannot create uinput device: %s (run install.sh)\n", strerror(errno));
		return 1;
	}

	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGHUP);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGINT);
	sigprocmask(SIG_BLOCK, &mask, NULL);
	int sfd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
	int ifd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
	inotify_add_watch(ifd, "/dev/input", IN_CREATE | IN_ATTRIB);

	// Give udev a moment to finish the virtual device before grabbing mice.
	usleep(200000);
	scan_devices();
	if (ndevices == 0)
		fprintf(stderr, "winmouse: no mouse grabbed yet; waiting for hotplug\n");

	bool running = true;
	while (running) {
		struct pollfd pfd[MAX_DEVICES + 2];
		pfd[0] = (struct pollfd){.fd = sfd, .events = POLLIN};
		pfd[1] = (struct pollfd){.fd = ifd, .events = POLLIN};
		for (int i = 0; i < ndevices; i++)
			pfd[i + 2] = (struct pollfd){.fd = devices[i].fd, .events = POLLIN};
		int count = ndevices;
		if (poll(pfd, (nfds_t)count + 2, -1) < 0 && errno != EINTR)
			break;

		for (int i = count - 1; i >= 0; i--)
			if (pfd[i + 2].revents && !handle_device(&engine, &devices[i]))
				remove_device(i);

		if (pfd[1].revents & POLLIN) {
			char buf[4096];
			while (read(ifd, buf, sizeof buf) > 0)
				;
			usleep(50000);
			scan_devices();
		}
		if (pfd[0].revents & POLLIN) {
			struct signalfd_siginfo si;
			while (read(sfd, &si, sizeof si) == sizeof si) {
				if (si.ssi_signo == SIGHUP) {
					load_settings(path, &cfg);
					engine_configure(&engine, &cfg);
					describe(&engine);
				} else {
					running = false;
				}
			}
		}
	}

	release_all();
	ioctl(uinput_fd, UI_DEV_DESTROY);
	close(uinput_fd);
	return 0;
}
