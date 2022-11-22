all: shswitch

shswitch: main.c
	gcc -Wall -Wextra -pedantic -o $@ $< $(shell pkg-config --libs libuv)

.PHONY: clean
clean:
	rm -f shswitch
