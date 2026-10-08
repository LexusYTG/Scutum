# Scutum — shim glibc (container, reemplaza libEGL/libGLESv2 de Mesa)
CC      ?= gcc
CFLAGS  := -O2 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -fPIC -shared -pthread
LDFLAGS := -ldl -lpthread -lxcb -lxcb-shm
SRC     := sc_egl.c sc_gles.c sc_core.c
HDR     := scutum.h sc_core.h sc_shm.h

.PHONY: all tests clean
all: libEGL.so libGLESv2.so

libEGL.so: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)
	cp -f $@ libGLESv2.so

tests: test_egl test_gl test_shm
test_egl: test_egl.c libEGL.so libGLESv2.so
	$(CC) -O2 -Wall -o $@ $< -L. -Wl,--allow-shlib-undefined -lEGL -lGLESv2
test_gl:  test_gl.c  libEGL.so libGLESv2.so
	$(CC) -O2 -Wall -o $@ $< -L. -Wl,--allow-shlib-undefined -lEGL -lGLESv2

# Prueba del bus por memoria compartida (rings, wrap, futex, fd por socket). Correr: ./test_shm
test_shm: test_shm.c sc_shm.h sc_core.c
	$(CC) -O2 -Wall -Wextra -Wno-unused-result -Wno-unused-function -pthread -o $@ test_shm.c sc_core.c -ldl

clean:
	rm -f libEGL.so libGLESv2.so test_egl test_gl test_shm *.o
