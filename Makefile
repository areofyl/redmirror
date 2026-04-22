CC = cc
CFLAGS = -Wall -Wextra -std=c99 -D_POSIX_C_SOURCE=200809L -O2
LDFLAGS = -lcurl -lpthread

redmirror: redmirror.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f redmirror

.PHONY: clean
