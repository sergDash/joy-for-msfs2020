/*
 * joy-for-msfs2020 — виртуальный джойстик для MSFS-2020.
 *
 * Поток данных:
 *   /dev/input/event* (EV_KEY, только нажатие value == 1)
 *     → таблица bindings: клавиша → внутренняя команда
 *     → состояние осей axis_value[] (хранится, пока работает программа)
 *     → /dev/uinput: EV_ABS + SYN_REPORT → виртуальный джойстик → игра
 *
 * Захват клавиатуры (EVIOCGRAB) не используется — события продолжают
 * поступать остальным приложениям (требование AGENTS.md).
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

#include <linux/input.h>
#include <linux/uinput.h>

/* Границы каждой оси и шаг за одно нажатие (5% от 65535 — полного диапазона). */
#define AXIS_MIN   (-32768)
#define AXIS_MAX   32767
#define AXIS_STEP  3276

#define MAX_DEVICES 16

/* Оси виртуального джойстика: X, Y, Z — поверхности самолёта, RZ — тяга. */
enum axis {
	AX_X,
	AX_Y,
	AX_Z,
	AX_THROTTLE,
	AX_COUNT
};

static const __u16 axis_code[AX_COUNT] = { ABS_X, ABS_Y, ABS_Z, ABS_RZ };

/* Текущее положение осей: стартовое — 0 (центр), дальше хранится до выхода. */
static int axis_value[AX_COUNT];

/* fd виртуального устройства, куда пишутся события осей. */
static int uinput_fd = -1;

/* Внутренние команды таблицы «клавиша → команда» (см. AGENTS.md). */
enum command {
	CMD_X_UP,
	CMD_X_DOWN,
	CMD_X_CENTER,
	CMD_Y_UP,
	CMD_Y_DOWN,
	CMD_Y_CENTER,
	CMD_Z_UP,
	CMD_Z_DOWN,
	CMD_Z_CENTER,
	CMD_THROTTLE_UP,
	CMD_THROTTLE_DOWN,
	CMD_THROTTLE_CENTER
};

/*
 * Раскладка, вшитая в код. Одна клавиша может встречаться несколько раз:
 * Num7 выдаёт две команды — сброс Y и сброс Z.
 */
static const struct binding {
	int key;
	enum command cmd;
} bindings[] = {
	{ KEY_KP4,    CMD_X_DOWN },
	{ KEY_KP5,    CMD_X_CENTER },
	{ KEY_KP6,    CMD_X_UP },
	{ KEY_KP8,    CMD_Y_DOWN },
	{ KEY_KP7,    CMD_Y_CENTER },
	{ KEY_KP2,    CMD_Y_UP },
	{ KEY_KP1,    CMD_Z_DOWN },
	{ KEY_KP7,    CMD_Z_CENTER },
	{ KEY_KP3,    CMD_Z_UP },
	{ KEY_KP0,    CMD_THROTTLE_DOWN },
	{ KEY_KPPLUS, CMD_THROTTLE_CENTER },
	{ KEY_KPDOT,  CMD_THROTTLE_UP }
};

#define BINDINGS_COUNT ((int)(sizeof bindings / sizeof bindings[0]))

/* Операции над битовой картой кодов клавиш (0..KEY_MAX). */
#define LONG_BITS   (8 * sizeof(unsigned long))
#define LONG_COUNT(n) (((n) + LONG_BITS) / LONG_BITS)
#define LONG_INDEX(k) ((k) / LONG_BITS)
#define LONG_MASK(k)  (1UL << ((k) % LONG_BITS))

/*
 * Битовая карта зажатых клавиш: защита от двойного срабатывания.
 * Одну и ту же клавишу могут доставлять два устройства сразу —
 * физическая клавиатура и виртуальная, созданная keyd.
 */
static unsigned long pressed[LONG_COUNT(KEY_MAX + 1)];

static int clamp_axis(int value)
{
	if (value < AXIS_MIN)
		return AXIS_MIN;
	if (value > AXIS_MAX)
		return AXIS_MAX;
	return value;
}

/* Запись текущего положения одной оси в uinput: EV_ABS + SYN_REPORT. */
static void emit_axis(enum axis axis)
{
	struct input_event events[2];

	memset(&events[0], 0, sizeof events[0]);
	gettimeofday(&events[0].time, NULL);
	events[0].type = EV_ABS;
	events[0].code = axis_code[axis];
	events[0].value = axis_value[axis];

	events[1] = events[0];
	events[1].type = EV_SYN;
	events[1].code = SYN_REPORT;
	events[1].value = 0;

	if (write(uinput_fd, events, sizeof events) != (ssize_t)sizeof events)
		perror("write /dev/uinput");
}

/* Исполнение команды: изменение состояния оси и отправка нового значения. */
static void apply_command(enum command cmd)
{
	enum axis axis = AX_X;
	int value = 0;

	switch (cmd) {
	case CMD_X_UP:
		axis = AX_X;
		value = axis_value[AX_X] + AXIS_STEP;
		break;
	case CMD_X_DOWN:
		axis = AX_X;
		value = axis_value[AX_X] - AXIS_STEP;
		break;
	case CMD_X_CENTER:
		axis = AX_X;
		value = 0;
		break;
	case CMD_Y_UP:
		axis = AX_Y;
		value = axis_value[AX_Y] + AXIS_STEP;
		break;
	case CMD_Y_DOWN:
		axis = AX_Y;
		value = axis_value[AX_Y] - AXIS_STEP;
		break;
	case CMD_Y_CENTER:
		axis = AX_Y;
		value = 0;
		break;
	case CMD_Z_UP:
		axis = AX_Z;
		value = axis_value[AX_Z] + AXIS_STEP;
		break;
	case CMD_Z_DOWN:
		axis = AX_Z;
		value = axis_value[AX_Z] - AXIS_STEP;
		break;
	case CMD_Z_CENTER:
		axis = AX_Z;
		value = 0;
		break;
	case CMD_THROTTLE_UP:
		axis = AX_THROTTLE;
		value = axis_value[AX_THROTTLE] + AXIS_STEP;
		break;
	case CMD_THROTTLE_DOWN:
		axis = AX_THROTTLE;
		value = axis_value[AX_THROTTLE] - AXIS_STEP;
		break;
	case CMD_THROTTLE_CENTER:
		axis = AX_THROTTLE;
		value = 0;
		break;
	}

	value = clamp_axis(value);

	/* Ось уже упёрлась в границу — новое событие не нужно. */
	if (value == axis_value[axis])
		return;

	axis_value[axis] = value;
	emit_axis(axis);
}

/* Обработка события одного устройства ввода. */
static void handle_event(const struct input_event *ie)
{
	int i;

	if (ie->type != EV_KEY)
		return;

	/* Отпускание: снимаем бит нажатия, оси не трогаем. */
	if (ie->value == 0) {
		pressed[LONG_INDEX(ie->code)] &= ~LONG_MASK(ie->code);
		return;
	}

	/* value == 2 — автоповтор удержания, игнорируем. */
	if (ie->value != 1)
		return;

	/* Клавиша уже зажата — это повторный down с другого устройства, пропуск. */
	if (pressed[LONG_INDEX(ie->code)] & LONG_MASK(ie->code))
		return;
	pressed[LONG_INDEX(ie->code)] |= LONG_MASK(ie->code);

	/* Поиск команд в раскладке: у одной клавиши может быть до двух команд. */
	for (i = 0; i < BINDINGS_COUNT; i++) {
		if (bindings[i].key == ie->code)
			apply_command(bindings[i].cmd);
	}
}

/* Проверка: объявляет ли устройство ВСЕ клавиши из таблицы раскладки. */
static int device_has_all_keys(int fd)
{
	unsigned long evbits[LONG_COUNT(EV_MAX + 1)];
	unsigned long keybits[LONG_COUNT(KEY_MAX + 1)];
	int i;

	memset(evbits, 0, sizeof evbits);
	if (ioctl(fd, EVIOCGBIT(0, sizeof evbits), evbits) < 0)
		return 0;
	if (!(evbits[LONG_INDEX(EV_KEY)] & LONG_MASK(EV_KEY)))
		return 0;

	memset(keybits, 0, sizeof keybits);
	if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keybits), keybits) < 0)
		return 0;

	for (i = 0; i < BINDINGS_COUNT; i++) {
		int key = bindings[i].key;
		if (!(keybits[LONG_INDEX(key)] & LONG_MASK(key)))
			return 0;
	}
	return 1;
}

/*
 * Обход /dev/input/event*: открываются все устройства, которые содержат
 * клавиши раскладки. Так находятся и физические клавиатуры, и виртуальная
 * keyd — события читаются с того устройства, которое их реально шлёт.
 */
static int open_keyboards(int *fds)
{
	struct dirent *ent;
	DIR *dir;
	int n = 0;
	char path[PATH_MAX];

	dir = opendir("/dev/input");
	if (dir == NULL) {
		perror("opendir /dev/input");
		return -1;
	}

	while ((ent = readdir(dir)) != NULL && n < MAX_DEVICES) {
		int fd;

		if (strncmp(ent->d_name, "event", 5) != 0)
			continue;

		snprintf(path, sizeof path, "/dev/input/%s", ent->d_name);
		fd = open(path, O_RDONLY);
		if (fd < 0)
			continue;	/* нет прав на устройство — пропускаем */

		if (!device_has_all_keys(fd)) {
			close(fd);
			continue;
		}
		fds[n++] = fd;
	}
	closedir(dir);

	if (n == 0) {
		fprintf(stderr, "не найдено устройство с клавишами NumPad из таблицы\n");
		fprintf(stderr, "проверьте доступ к /dev/input/event* (группа input или запуск от root)\n");
		return -1;
	}
	return n;
}

/* Создание виртуального джойстика: 4 оси ABS в диапазоне AXIS_MIN..AXIS_MAX. */
static int setup_uinput(void)
{
	struct uinput_setup setup;
	struct uinput_abs_setup abs;
	int fd;
	int i;

	fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	if (fd < 0) {
		perror("open /dev/uinput");
		return -1;
	}

	if (ioctl(fd, UI_SET_EVBIT, EV_ABS) < 0) {
		perror("ioctl UI_SET_EVBIT");
		close(fd);
		return -1;
	}
	for (i = 0; i < AX_COUNT; i++) {
		if (ioctl(fd, UI_SET_ABSBIT, axis_code[i]) < 0) {
			perror("ioctl UI_SET_ABSBIT");
			close(fd);
			return -1;
		}
	}

	memset(&setup, 0, sizeof setup);
	setup.id.bustype = BUS_USB;
	setup.id.vendor = 0x1234;
	setup.id.product = 0x0001;
	snprintf(setup.name, sizeof setup.name, "joy-for-msfs2020");
	if (ioctl(fd, UI_DEV_SETUP, &setup) < 0) {
		perror("ioctl UI_DEV_SETUP");
		close(fd);
		return -1;
	}

	for (i = 0; i < AX_COUNT; i++) {
		memset(&abs, 0, sizeof abs);
		abs.code = axis_code[i];
		abs.absinfo.minimum = AXIS_MIN;
		abs.absinfo.maximum = AXIS_MAX;
		abs.absinfo.value = axis_value[i];
		if (ioctl(fd, UI_ABS_SETUP, &abs) < 0) {
			perror("ioctl UI_ABS_SETUP");
			close(fd);
			return -1;
		}
	}

	if (ioctl(fd, UI_DEV_CREATE) < 0) {
		perror("ioctl UI_DEV_CREATE");
		close(fd);
		return -1;
	}
	return fd;
}

static volatile sig_atomic_t running = 1;

static void on_signal(int signo)
{
	(void)signo;
	running = 0;
}

int main(void)
{
	struct pollfd pfds[MAX_DEVICES];
	int fds[MAX_DEVICES];
	int nkbd;
	int i;

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	uinput_fd = setup_uinput();
	if (uinput_fd < 0)
		return 1;

	nkbd = open_keyboards(fds);
	if (nkbd < 0) {
		ioctl(uinput_fd, UI_DEV_DESTROY);
		close(uinput_fd);
		return 1;
	}

	for (i = 0; i < nkbd; i++) {
		pfds[i].fd = fds[i];
		pfds[i].events = POLLIN;
	}

	while (running) {
		int ready = poll(pfds, nkbd, -1);

		if (ready < 0) {
			if (errno == EINTR)
				continue;	/* сигнал: running == 0, выход по условию цикла */
			perror("poll");
			break;
		}

		for (i = 0; i < nkbd; i++) {
			struct input_event events[64];
			ssize_t size;
			int j;

			if (pfds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
				fprintf(stderr, "устройство ввода потеряно (fd %d)\n", pfds[i].fd);
				running = 0;
				break;
			}
			if (!(pfds[i].revents & POLLIN))
				continue;

			size = read(pfds[i].fd, events, sizeof events);
			if (size < 0) {
				perror("read");
				running = 0;
				break;
			}
			for (j = 0; j < size / (ssize_t)sizeof(struct input_event); j++)
				handle_event(&events[j]);
		}
	}

	for (i = 0; i < nkbd; i++)
		close(fds[i]);
	ioctl(uinput_fd, UI_DEV_DESTROY);
	close(uinput_fd);
	return 0;
}
