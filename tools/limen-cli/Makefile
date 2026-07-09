CC     = cc
CFLAGS = -Wall -O2

# Cross-compile for Windows with:
#   make CC=x86_64-w64-mingw32-gcc EXE=.exe LDLIBS=-lws2_32
EXE    =
LDLIBS =

limen-cli$(EXE): limen-cli.c
	$(CC) $(CFLAGS) -o $@ limen-cli.c $(LDLIBS)

clean:
	rm -f limen-cli limen-cli.exe

install: limen-cli
	install -Dm755 limen-cli $(HOME)/.local/bin/limen-cli
