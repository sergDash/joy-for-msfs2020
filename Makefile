CC      ?= cc
CFLAGS  ?= -Wall -Wextra -O2

joy-for-msfs2020: joy-for-msfs2020.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f joy-for-msfs2020

.PHONY: clean
