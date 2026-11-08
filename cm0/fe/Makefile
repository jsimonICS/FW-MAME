CFLAGS ?= -O2 -Wall -Wextra

fwmame: fwmame.c fwlibretro.h ../fb/fwfb.c ../fb/fwfb.h
	$(CC) $(CFLAGS) -o $@ fwmame.c ../fb/fwfb.c -ldl -lpthread -lm

clean:
	rm -f fwmame
