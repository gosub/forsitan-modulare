CC     = cc
CFLAGS = -Wall -O2

limen-cli: limen-cli.c
	$(CC) $(CFLAGS) -o limen-cli limen-cli.c

clean:
	rm -f limen-cli

install: limen-cli
	install -Dm755 limen-cli $(HOME)/.local/bin/limen-cli
