# Scutum

> *"OpenGL ES del driver, sin Mesa en el medio."*

Puente GLES/EGL **sin root** entre Android bionic y containers glibc.

## ADVERTENCIA

**Proyecto experimental.** Solo probado en **Mali-G52 MC2** con driver ARM propietario (OpenGL ES 3.2 v1.r49p1). Otros GPUs (Adreno, PowerVR, otros Mali, Xclipse, RDNA) no estan probados. Probalo bajo tu propio riesgo.

## Que es

Dos componentes comunicados por socket Unix AF_UNIX:

- **scutumd** (bionic, host Android) carga libEGL.so y libGLESv2.so reales de /system/lib64 (driver Mali) y ejecuta cada llamada que llega por el socket.
- **libEGL.so + libGLESv2.so** (glibc, container) son el shim. Reemplazan las libs de Mesa. Cada entry point EGL/GLES se serializa al socket, el daemon lo ejecuta contra el driver real y devuelve el resultado.

El container ve la GPU nativa como si fuera suya, con el driver oficial, sin emulacion, sin Zink, sin traduccion a Vulkan.

## Por que no es Zink

Zink traduce OpenGL a Vulkan. Scutum no traduce: expone el driver OpenGL ES directo. La app llama glDrawArrays y termina llamando glDrawArrays en el driver Mali. Lo unico que cambia es que la llamada cruza un socket.

Tampoco es freedreno: freedreno reimplementa el driver para Adreno. Scutum reenvia al driver oficial.

## Que logra

- EGL 1.5 completo.
- GLES 2.0 + 3.0 + 3.1 + 3.2 forwardeados completos.
- Presentacion por X11 (readback + flip + swap R/B + xcb_put_image).
- Presentacion por AHardwareBuffer (blit en GPU + lectura cacheada).

Verificado con SuperTuxKart via gl4es y con test_gl.

## Estructura

    glibc/    shim (container, gcc)
    bionic/   daemon (host Android, clang)

## Compilar

glibc (dentro del container):

    cd glibc && make

bionic (en Termux):

    cd bionic && make

## Instalacion

Binarios en el release v1.0:

- scutum-glibc-v1.0.tar.gz — libEGL.so, libGLESv2.so
- scutum-bionic-v1.0.tar.gz — scutumd, libcrashisolate.so, supervisor

glibc: copiar los .so a /usr/lib/aarch64-linux-gnu/. **libGLESv2.so debe ser symlink a libEGL.so** (un solo handle en memoria = un solo contexto EGL).

bionic: copiar scutumd y libcrashisolate.so al $PREFIX/bin/. Arrancar con SCUTUM_LIBDIR=/system/lib64 SCUTUM_SOCK=$PREFIX/tmp/scutum.sock scutumd.

## Variables

- SCUTUM_SOCK — path del socket. Default /tmp/scutum.sock.
- SCUTUM_LIBDIR — donde buscar el driver. Default /system/lib64.
- SCUTUM_VERBOSE=1 — trazas.
- SCUTUM_GLERR=1 — chequea glGetError tras cada op.

## Estado

Funcional. Bugs visuales conocidos en superficies complejas. Performance pendiente.

## Licencia

MIT.
