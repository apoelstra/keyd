/*
 * keyd - A key remapping daemon.
 *
 * © 2019 Raheman Vaiya (see also: LICENSE).
 */

#include "keyd.h"

#include <stdio.h>
#include <pthread.h>
#include <string.h>
#include <sys/types.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/inotify.h>

/*
 * Abstract away evdev and inotify.
 *
 * We could make this cleaner by creating a single file descriptor via epoll
 * but this would break FreeBSD compatibility without a dedicated kqueue
 * implementation. A thread based approach was also considered, but
 * inter-thread communication adds too much overhead (~100us).
 *
 * Overview:
 *
 * A 'devmon' is a file descriptor which can be created with devmon_create()
 * and subsequently monitored for new devices read with devmon_read_device().
 *
 * A 'device' always corresponds to a keyboard or mouse from which activity can
 * be monitored with device->fd and events subsequently read using
 * device_read_event().
 *
 * If the event returned by device_read_event() is of type DEV_REMOVED then the
 * corresponding device should be considered invalid by the caller.
 */

static int has_key(uint8_t *keymask, uint8_t keymask_len, uint8_t key)
{
	return (keymask[key / 8] >> (key % 8)) & 0x01;
}

static uint8_t resolve_device_capabilities(int fd, uint32_t *num_keys, uint8_t *relmask, uint8_t *absmask)
{
	size_t num_media_keys = 0;
	size_t num_keyboard_keys = 0;
	size_t i;
	uint8_t keymask[(KEY_MAX+7)/8];

	uint8_t capabilities = 0;
	int has_media_keys = 0;

	const uint32_t media_keys[] = {
		KEY_BRIGHTNESSUP,
		KEY_VOLUMEUP,
		KEY_TOUCHPAD_TOGGLE,
		KEY_TOUCHPAD_OFF,
		KEY_MICMUTE,
		KEY_POWER,
	};
	const uint32_t keyboard_keys[] = {
		KEY_1, KEY_2, KEY_3, KEY_4,
		KEY_5, KEY_6, KEY_7, KEY_8,
		KEY_9, KEY_0, KEY_Q, KEY_W,
		KEY_E, KEY_R, KEY_T, KEY_Y,
	};

	if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keymask), keymask) < 0) {
		perror("ioctl");
		return 0;
	}

	if (ioctl(fd, EVIOCGBIT(EV_ABS, 1), absmask) < 0) {
		perror("ioctl");
		return 0;
	}

	if (ioctl(fd, EVIOCGBIT(EV_REL, 1), relmask) < 0) {
		perror("ioctl");
		return 0;
	}

	*num_keys = 0;
	for (i = 0; i < ARRAY_SIZE(keymask); i++)
		*num_keys += __builtin_popcount(keymask[i]);

	if (*relmask || *absmask)
		capabilities |= CAP_MOUSE;

	if (*absmask)
		capabilities |= CAP_MOUSE_ABS;

	/*
	 * If the device can certain media keys, we treat it as a keyboard.
	 *
	 * This is mainly to accommodate laptops with brightness/volume buttons which create
	 * a different device node from the main keyboard for some hotkeys.
	 *
	 * NOTE: This will subsume anything that can emit these keys and may produce
	 * false positives which need to be explcitly excluded by the user if they use
	 * the wildcard id.
	 */
	for (i = 0; i < ARRAY_SIZE(media_keys); i++) {
		if (has_key(keymask, sizeof keymask, media_keys[i]))
			num_media_keys++;
	}

	for (i = 0; i < ARRAY_SIZE(keyboard_keys); i++) {
		if (has_key(keymask, sizeof keymask, keyboard_keys[i]))
			num_keyboard_keys++;
	}

	if (*num_keys)
		capabilities |= CAP_KEY;

	if (num_keyboard_keys == ARRAY_SIZE(keyboard_keys) || num_media_keys != 0)
		capabilities |= CAP_KEYBOARD;

	return capabilities;
}

uint32_t generate_uid(uint32_t num_keys, uint8_t absmask, uint8_t relmask, const char *name)
{
	uint32_t hash = 5183;

	//djb2 hash
	hash = hash*33 + (uint8_t)(num_keys >> 24);
	hash = hash*33 + (uint8_t)(num_keys >> 16);
	hash = hash*33 + (uint8_t)(num_keys >> 8);
	hash = hash*33 + (uint8_t)(num_keys);
	hash = hash*33 + absmask;
	hash = hash*33 + relmask;

	while (*name) {
		hash = hash*33 + *name;
		name++;
	}

	return hash;
}

static int device_init(const char *path, struct device *dev)
{
	int fd;
	int capabilities;
	uint32_t num_keys;
	uint8_t relmask;
	uint8_t absmask;
	struct input_absinfo absinfo;

	memset(dev, 0, sizeof *dev);

	if ((fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC, 0600)) < 0) {
		keyd_log("failed to open %s\n", path);
		return -1;
	}

	dbg_print_evdev_details(path);

	capabilities = resolve_device_capabilities(fd, &num_keys, &relmask, &absmask);

	if (ioctl(fd, EVIOCGNAME(sizeof(dev->name)), dev->name) == -1) {
		keyd_log("ERROR: could not fetch device name of %s\n", dev->path);
		return -1;
	}

	if (capabilities & CAP_MOUSE_ABS) {
		if (ioctl(fd, EVIOCGABS(ABS_X), &absinfo) < 0) {
			perror("ioctl");
			return -1;
		}

		dev->_minx = absinfo.minimum;
		dev->_maxx = absinfo.maximum;

		if (ioctl(fd, EVIOCGABS(ABS_Y), &absinfo) < 0) {
			perror("ioctl");
			return -1;
		}

		dev->_miny = absinfo.minimum;
		dev->_maxy = absinfo.maximum;
	}

	dbg("capabilities of %s (%s): %x", path, dev->name, capabilities);

	if (capabilities) {
		struct input_id info;

		if (ioctl(fd, EVIOCGID, &info) == -1) {
			perror("ioctl EVIOCGID");
			return -1;
		}

		strncpy(dev->path, path, sizeof(dev->path)-1);
		dev->path[sizeof(dev->path)-1] = 0;

		/*
		 * Attempt to generate a reproducible unique identifier for each device.
		 * The product and vendor ids are insufficient to identify some devices since
		 * they can create multiple device nodes with different capabilities. Thus
		 * we factor in the device name and capabilities of the resultant evdev node
		 * to further distinguish between input devices. These should be regarded as
		 * opaque identifiers by the user.
		 */
		snprintf(dev->id, sizeof dev->id, "%04x:%04x:%08x", info.vendor, info.product, generate_uid(num_keys, absmask, relmask, dev->name));

		dev->fd = fd;
		dev->capabilities = capabilities;
		dev->data = NULL;
		dev->grabbed = 0;

		dev->is_virtual = info.vendor == 0x0FAC;
		return 0;
	} else {
		close(fd);
		return -1;
	}

	return -1;
}

struct device_worker {
	pthread_t tid;
	char path[1024];
	struct device dev;
};

static void *device_scan_worker(void *arg)
{
	struct device_worker *w = (struct device_worker *)arg;
	if (device_init(w->path, &w->dev) < 0)
		return NULL;

	return &w->dev;
}

int device_scan(struct device devices[MAX_DEVICES])
{
	int i;
	struct device_worker workers[MAX_DEVICES];
	struct dirent *ent;
	DIR *dh = opendir("/dev/input/");
	int n = 0, ndevs;

	if (!dh) {
		perror("opendir /dev/input");
		exit(-1);
	}

	while((ent = readdir(dh))) {
		if (ent->d_type != DT_DIR && !strncmp(ent->d_name, "event", 5)) {
			assert(n < MAX_DEVICES);
			struct device_worker *w = &workers[n++];

			snprintf(w->path, sizeof(w->path), "/dev/input/%s", ent->d_name);
			pthread_create(&w->tid, NULL, device_scan_worker, w);
		}
	}

	ndevs = 0;
	for(i = 0; i < n; i++) {
		struct device *d;
		pthread_join(workers[i].tid, (void**)&d);

		if (d)
			devices[ndevs++] = workers[i].dev;
	}

	closedir(dh);
	return ndevs;
}

/*
 * NOTE: Only a single devmon fd may exist. Implementing this properly
 * would involve bookkeeping state for each fd, but this is
 * unnecessary for our use.
 */
int devmon_create()
{
	static int init = 0;
	assert(!init);
	init = 1;

	int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (fd < 0) {
		perror("inotify");
		exit(-1);
	}

	int wd = inotify_add_watch(fd, "/dev/input/", IN_CREATE);
	if (wd < 0) {
		perror("inotify");
		exit(-1);
	}

	return fd;
}

/*
 * A non blocking call which returns any devices available on the provided
 * monitor descriptor. Returns 0 on success.
 */
int devmon_read_device(int fd, struct device *dev)
{
	static char buf[4096];
	static int buf_sz = 0;
	static char *ptr = buf;

	while (1) {
		char path[1024];
		struct inotify_event *ev;

		if (ptr >= (buf+buf_sz)) {
			ptr = buf;
			buf_sz = read(fd, buf, sizeof(buf));
			if (buf_sz == -1) {
				buf_sz = 0;
				return -1;
			}
		}

		ev = (struct inotify_event*)ptr;
		ptr += sizeof(struct inotify_event) + ev->len;

		if (strncmp(ev->name, "event", 5))
			continue;

		snprintf(path, sizeof path, "/dev/input/%s", ev->name);

		if (!device_init(path, dev))
			return 0;
	}
}

int device_grab(struct device *dev)
{
	size_t i;
	struct input_event ev;
	uint8_t state[KEY_MAX / 8 + 1];
	int pending_release = 0;

	if (dev->grabbed)
		return 0;

	/*
	 * await neutral key state to ensure any residual
	 * key up events propagate.
	 */

	while (1) {
		int n = 0;
		memset(state, 0, sizeof(state));

		if (ioctl(dev->fd, EVIOCGKEY(sizeof state), state) < 0) {
			perror("ioctl EVIOCGKEY");
			return -1;
		}

		for (i = 0; i < KEY_MAX; i++) {
			if ((state[i / 8] >> (i % 8)) & 0x1)
				n++;
		}

		if (n == 0)
			break;
		else
			pending_release = 1;
	}

	if (pending_release) {
		//Allow the key up events to propagate before
		//grabbing the device.
		usleep(100);
	}

	if (ioctl(dev->fd, EVIOCGRAB, (void *) 1) < 0) {
		perror("EVIOCGRAB");
		return -1;
	}

	/* drain any input events before the grab (assumes NONBLOCK is set on the fd) */
	while (read(dev->fd, &ev, sizeof(ev)) > 0) {
	}

	dev->grabbed = 1;
	return 0;
}

int device_ungrab(struct device *dev)
{
	if (!dev->grabbed)
		return 0;

	if (!ioctl(dev->fd, EVIOCGRAB, (void *) 0)) {
		dev->grabbed = 0;
		return 0;
	} else {
		return -1;
	}
}

/*
 * Read a device event from the given device or return
 * NULL if none are available (may happen in the
 * case of a spurious wakeup).
 */
struct device_event *device_read_event(struct device *dev)
{
	struct input_event ev;
	static struct device_event devev;

	assert(dev->fd != -1);

	if (read(dev->fd, &ev, sizeof(ev)) < 0) {
		if (errno == EAGAIN) {
			return NULL;
		} else {
			dev->fd = -1;
			devev.type = DEV_REMOVED;
			return &devev;
		}
	}

	switch (ev.type) {
	case EV_REL:
		switch (ev.code) {
		case REL_WHEEL:
			devev.type = DEV_MOUSE_SCROLL;
			devev.y = ev.value;
			devev.x = 0;

			break;
		case REL_HWHEEL:
			devev.type = DEV_MOUSE_SCROLL;
			devev.y = 0;
			devev.x = ev.value;

			break;
		case REL_X:
			/*
			 * Queue and emit a single event on SYN to account for
			 * programs which are particular about input grouping.
			 */
			dev->_pending_rel_x += ev.value;

			return NULL;
			break;
		case REL_Y:
			dev->_pending_rel_y += ev.value;

			return NULL;
			break;
//		case REL_WHEEL_HI_RES:
//			/* TODO: implement me */
//			return NULL;
//		case REL_HWHEEL_HI_RES:
//			/* TODO: implement me */
//			return NULL;
		default:
			dbg("Unrecognized EV_REL code: %d\n", ev.code);
			return NULL;
		}

		break;
	case EV_SYN:
		if (dev->_pending_rel_x || dev->_pending_rel_y) {
			devev.type = DEV_MOUSE_MOVE;
			devev.y = dev->_pending_rel_y;
			devev.x = dev->_pending_rel_x;

			dev->_pending_rel_y = 0;
			dev->_pending_rel_x = 0;
		} else {
			return NULL;
		}
		break;
	case EV_ABS:
		switch (ev.code) {
		case ABS_X:
			devev.type = DEV_MOUSE_MOVE_ABS;
			devev.x = (ev.value * 1024) / (dev->_maxx - dev->_minx);
			devev.y = 0;

			break;
		case ABS_Y:
			devev.type = DEV_MOUSE_MOVE_ABS;
			devev.y = (ev.value * 1024) / (dev->_maxy - dev->_miny);
			devev.x = 0;

			break;
		default:
			dbg("Unrecognized EV_ABS code: %x", ev.code);
			return NULL;
		}

		break;
	case EV_KEY:
		/*
		 * KEYD_* codes <256 correspond to their evdev
		 * counterparts.
		 */

		/* Ignore repeat events. */
		if (ev.value == 2)
			return NULL;

		if (ev.code >= 256) {
			switch (ev.code) {
				/*
                                 * Shifted fn keys on laptops which support it.
                                 *
                                 * NOTE:
	                         *
                                 * Shifted function keys, some laptops (e.g thinkpads) will map
                                 * these to exotic media keys instead.
                                 */
				case KEY_FN_F1:  ev.code = KEYD_F13; break;
				case KEY_FN_F2:  ev.code = KEYD_F14; break;
				case KEY_FN_F3:  ev.code = KEYD_F15; break;
				case KEY_FN_F4:  ev.code = KEYD_F16; break;
				case KEY_FN_F5:  ev.code = KEYD_F17; break;
				case KEY_FN_F6:  ev.code = KEYD_F18; break;
				case KEY_FN_F7:  ev.code = KEYD_F19; break;
				case KEY_FN_F8:  ev.code = KEYD_F20; break;
				case KEY_FN_F9:  ev.code = KEYD_F21; break;
				case KEY_FN_F10: ev.code = KEYD_F22; break;
				case KEY_FN_F11: ev.code = KEYD_F23; break;
				case KEY_FN_F12: ev.code = KEYD_F24; break;

				case KEY_PROG1: ev.code = KEYD_F21; break;
				case KEY_PROG2: ev.code = KEYD_F22; break;
				case KEY_PROG3: ev.code = KEYD_F23; break;
				case KEY_PROG4: ev.code = KEYD_F24; break;

				case KEY_TOUCHPAD_TOGGLE: ev.code = KEYD_F21; break;
				case KEY_FAVORITES: ev.code = KEYD_BOOKMARKS; break;

				/* Thinkpad fn shifted f9-f11 */
				case KEY_NOTIFICATION_CENTER:  ev.code = KEYD_F21; break;
				case KEY_PICKUP_PHONE:         ev.code = KEYD_F22; break;
				case KEY_HANGUP_PHONE:         ev.code = KEYD_F23; break;

				/* Misc (think/idea)pad fn keys */
				case KEY_FN_RIGHT_SHIFT:       ev.code = KEYD_F13; break;
				case KEY_KEYBOARD:             ev.code = KEYD_F14; break;
				case KEY_REFRESH_RATE_TOGGLE:  ev.code = KEYD_F15; break;
				case KEY_SELECTIVE_SCREENSHOT: ev.code = KEYD_F16; break;
				case KEY_TOUCHPAD_OFF:         ev.code = KEYD_F17; break;
				case KEY_TOUCHPAD_ON:          ev.code = KEYD_F18; break;
				case KEY_VENDOR:               ev.code = KEYD_F19; break;


				/* Menu keys found below LCD screens on some devices (i.e additional function keys) */
				case KEY_KBD_LCD_MENU1:	       ev.code = KEYD_F20; break;
				case KEY_KBD_LCD_MENU2:	       ev.code = KEYD_F21; break;
				case KEY_KBD_LCD_MENU3:	       ev.code = KEYD_F22; break;
				case KEY_KBD_LCD_MENU4:	       ev.code = KEYD_F23; break;
				case KEY_KBD_LCD_MENU5:	       ev.code = KEYD_F24; break;

				/* Misc keys found on various laptops */
				case KEY_EDITOR:         ev.code = KEYD_F13; break;
				case KEY_SPREADSHEET:    ev.code = KEYD_F14; break;
				case KEY_GRAPHICSEDITOR: ev.code = KEYD_F15; break;
				case KEY_PRESENTATION:   ev.code = KEYD_F16; break;
				case KEY_DATABASE:       ev.code = KEYD_F17; break;
				case KEY_NEWS:           ev.code = KEYD_F18; break;
				case KEY_VOICEMAIL:      ev.code = KEYD_F19; break;
				case KEY_ADDRESSBOOK:    ev.code = KEYD_F20; break;
				case KEY_MESSENGER:      ev.code = KEYD_F21; break;

				case KEY_FN:             ev.code = KEYD_FN; break;
				case KEY_ZOOM:           ev.code = KEYD_ZOOM; break;
				case KEY_VOICECOMMAND:   ev.code = KEYD_VOICECOMMAND; break;

				/* Copilot key on newer kernels */
				case KEY_ACCESSIBILITY: ev.code = KEYD_F23; break;

				/* Mouse buttons */
				case BTN_LEFT:    ev.code = KEYD_LEFT_MOUSE; break;
				case BTN_MIDDLE:  ev.code = KEYD_MIDDLE_MOUSE; break;
				case BTN_RIGHT:   ev.code = KEYD_RIGHT_MOUSE; break;
				case BTN_SIDE:    ev.code = KEYD_MOUSE_1; break;
				case BTN_EXTRA:   ev.code = KEYD_MOUSE_2; break;
				case BTN_BACK:    ev.code = KEYD_MOUSE_BACK; break;
				case BTN_FORWARD: ev.code = KEYD_MOUSE_FORWARD; break;
				case BTN_TASK:    ev.code = KEYD_F18; break;

				case BTN_0:       ev.code = KEYD_F13; break;
				case BTN_1:       ev.code = KEYD_F14; break;
				case BTN_2:       ev.code = KEYD_F15; break;
				case BTN_3:       ev.code = KEYD_F16; break;
				case BTN_4:       ev.code = KEYD_F17; break;
				case BTN_5:       ev.code = KEYD_F18; break;
				case BTN_6:       ev.code = KEYD_F19; break;
				case BTN_7:       ev.code = KEYD_F20; break;
				case BTN_8:       ev.code = KEYD_F21; break;
				case BTN_9:       ev.code = KEYD_F22; break;

				default:
					dbg("unsupported evdev code: 0x%x\n", ev.code);
					return NULL;
			}
		}

		devev.type = DEV_KEY;
		devev.code = ev.code;
		devev.pressed = ev.value;

		dbg2("key %s %s", KEY_NAME(devev.code), devev.pressed ? "down" : "up");

		break;
	case EV_LED:
		devev.type = DEV_LED;
		devev.code = ev.code;
		devev.pressed = ev.value;

		break;
	default:
		if (ev.type)
			dbg2("unrecognized evdev event type: %d %d %d", ev.type, ev.code, ev.value);
		return NULL;
	}

	return &devev;
}

void device_set_led(const struct device *dev, int led, int state)
{
	struct input_event ev = {
		.type = EV_LED,
		.code = led,
		.value = state
	};

	xwrite(dev->fd, &ev, sizeof ev);
}
