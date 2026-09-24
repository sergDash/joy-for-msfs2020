/*
 * Автономный тест логики joy-for-msfs2020: шаг, границы, dedup, команды.
 * Подключает основной .c напрямую; вместо uinput — /dev/null.
 * Клавиатура/uinput в тесте не используются — права не нужны.
 */
#define main joy_program_main
#include "joy-for-msfs2020.c"
#undef main

#include <assert.h>

static struct input_event key_event(int code, int value)
{
	struct input_event ie;

	memset(&ie, 0, sizeof ie);
	ie.type = EV_KEY;
	ie.code = code;
	ie.value = value;
	return ie;
}

static void center_all(void)
{
	axis_value[AX_X] = 0;
	axis_value[AX_Y] = 0;
	axis_value[AX_Z] = 0;
	/* Тягу не трогаем — она хранит своё стартовое значение. */
}

int main(void)
{
	struct input_event ie;

	uinput_fd = open("/dev/null", O_WRONLY);
	assert(uinput_fd >= 0);

	/* Стартовое состояние: X, Y, Z в центре, тяга — на минимуме. */
	assert(axis_value[AX_X] == 0);
	assert(axis_value[AX_Y] == 0);
	assert(axis_value[AX_Z] == 0);
	assert(axis_value[AX_THROTTLE] == AXIS_MIN);

	/* Шаг ±3276. */
	apply_command(CMD_X_DOWN);
	assert(axis_value[AX_X] == -3276);
	apply_command(CMD_X_UP);
	assert(axis_value[AX_X] == 0);
	apply_command(CMD_X_UP);
	assert(axis_value[AX_X] == 3276);
	apply_command(CMD_X_CENTER);
	assert(axis_value[AX_X] == 0);

	/* Ограничение нижней границей: 11 шагов вниз из 0 → -32768. */
	{
		int i;

		for (i = 0; i < 11; i++)
			apply_command(CMD_X_DOWN);
		assert(axis_value[AX_X] == -32768);
		apply_command(CMD_X_DOWN);	/* упёрлась — без изменений */
		assert(axis_value[AX_X] == -32768);
		apply_command(CMD_X_CENTER);
		assert(axis_value[AX_X] == 0);
	}

	/* Ограничение верхней границей. */
	{
		int i;

		for (i = 0; i < 11; i++)
			apply_command(CMD_X_UP);
		assert(axis_value[AX_X] == 32767);
		apply_command(CMD_X_UP);
		assert(axis_value[AX_X] == 32767);
	}
	center_all();

	/* Обработка событий: только нажатие (value == 1). */
	ie = key_event(KEY_KP4, 1);
	handle_event(&ie);
	assert(axis_value[AX_X] == -3276);

	ie = key_event(KEY_KP4, 2);	/* автоповтор — игнор */
	handle_event(&ie);
	assert(axis_value[AX_X] == -3276);

	ie = key_event(KEY_KP4, 0);	/* отпускание — только сброс бита */
	handle_event(&ie);
	assert(axis_value[AX_X] == -3276);

	/* Двойной down без release (два устройства об одной клавише) — один шаг. */
	ie = key_event(KEY_KP6, 1);
	handle_event(&ie);
	assert(axis_value[AX_X] == 0);		/* -3276 + 3276 */
	ie = key_event(KEY_KP6, 1);
	handle_event(&ie);
	assert(axis_value[AX_X] == 0);		/* дубль проигнорирован */

	/* После release следующий down снова двигает ось. */
	ie = key_event(KEY_KP6, 0);
	handle_event(&ie);
	ie = key_event(KEY_KP6, 1);
	handle_event(&ie);
	assert(axis_value[AX_X] == 3276);
	ie = key_event(KEY_KP6, 0);
	handle_event(&ie);
	apply_command(CMD_X_CENTER);
	assert(axis_value[AX_X] == 0);

	/* Num7 выдаёт две команды: сброс Y и сброс Z. */
	ie = key_event(KEY_KP8, 1);	/* Y уменьшить */
	handle_event(&ie);
	ie = key_event(KEY_KP8, 0);
	handle_event(&ie);
	ie = key_event(KEY_KP1, 1);	/* Z уменьшить */
	handle_event(&ie);
	ie = key_event(KEY_KP1, 0);
	handle_event(&ie);
	assert(axis_value[AX_Y] == -3276);
	assert(axis_value[AX_Z] == -1310);	/* шаг Z — 2% */
	ie = key_event(KEY_KP7, 1);
	handle_event(&ie);
	assert(axis_value[AX_Y] == 0);
	assert(axis_value[AX_Z] == 0);
	ie = key_event(KEY_KP7, 0);
	handle_event(&ie);

	/* Тяга: старт на минимуме, шаг 2600, сброса нет. */
	assert(axis_value[AX_THROTTLE] == AXIS_MIN);
	ie = key_event(KEY_KPENTER, 1);	/* уменьшить — от минимума не двигается */
	handle_event(&ie);
	ie = key_event(KEY_KPENTER, 0);
	handle_event(&ie);
	assert(axis_value[AX_THROTTLE] == AXIS_MIN);
	ie = key_event(KEY_KPPLUS, 1);	/* увеличить */
	handle_event(&ie);
	ie = key_event(KEY_KPPLUS, 0);
	handle_event(&ie);
	assert(axis_value[AX_THROTTLE] == AXIS_MIN + 2600);
	ie = key_event(KEY_KPPLUS, 1);	/* ещё увеличить */
	handle_event(&ie);
	ie = key_event(KEY_KPPLUS, 0);
	handle_event(&ie);
	assert(axis_value[AX_THROTTLE] == AXIS_MIN + 5200);
	ie = key_event(KEY_KPENTER, 1);	/* уменьшить */
	handle_event(&ie);
	ie = key_event(KEY_KPENTER, 0);
	handle_event(&ie);
	assert(axis_value[AX_THROTTLE] == AXIS_MIN + 2600);

	/* Клавиша вне раскладки не двигает оси. */
	ie = key_event(KEY_A, 1);
	handle_event(&ie);
	assert(axis_value[AX_X] == 0);
	assert(axis_value[AX_Y] == 0);
	assert(axis_value[AX_Z] == 0);
	assert(axis_value[AX_THROTTLE] == AXIS_MIN + 2600);

	close(uinput_fd);
	printf("все проверки пройдены\n");
	return 0;
}
