# Scutum — daemon bionic (host, dlopen-ea /system/lib64/libEGL.so y libGLESv2.so)
CC      ?= clang
CFLAGS  := -O2 -Wall -Wextra -pthread -I../glibc
LDFLAGS := -ldl

.PHONY: all clean
all: scutumd libcrashisolate.so

scutumd: scutumd.c ../glibc/scutum.h ../glibc/sc_shm.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

libcrashisolate.so: crashisolate.c
	$(CC) -O2 -Wall -fPIC -shared -o $@ $<

clean:
	rm -f scutumd libcrashisolate.so *.o
