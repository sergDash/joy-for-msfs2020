CC      ?= cc
CFLAGS  ?= -Wall -Wextra -O2

all: joy-for-msfs2020

joy-for-msfs2020: joy-for-msfs2020.c
	$(CC) $(CFLAGS) -o $@ $<

# Тест логики: подключает joy-for-msfs2020.c и проверяет оси/раскладку
# на синтетических событиях. Права на /dev/input и /dev/uinput не нужны.
test_joy: test_joy.c joy-for-msfs2020.c
	$(CC) $(CFLAGS) -o $@ test_joy.c

test: test_joy
	./test_joy

clean:
	rm -f joy-for-msfs2020 test_joy

.PHONY: all test clean
