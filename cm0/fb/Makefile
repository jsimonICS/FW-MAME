CFLAGS ?= -O2 -Wall -Wextra

fbsend: fbsend.c fwfb.c fwfb.h
	$(CC) $(CFLAGS) -o $@ fbsend.c fwfb.c

clean:
	rm -f fbsend
