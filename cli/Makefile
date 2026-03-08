CC     = cc
CFLAGS = -Wall -O2

limen: limen.c
	$(CC) $(CFLAGS) -o limen limen.c

clean:
	rm -f limen

install: limen
	install -Dm755 limen $(HOME)/.local/bin/limen
