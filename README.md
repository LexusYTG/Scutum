# Scutum

> *"OpenGL ES del driver, sin Mesa en el medio."*

Puente GLES/EGL **sin root** entre Android bionic y containers glibc.

## Qué es

Dos componentes comunicados por socket Unix AF_UNIX:

- **scutumd** (bionic, host Android) carga `libEGL.so` y `libGLESv2.so` reales del driver (`/system/lib64/`) y ejecuta cada llamada que llega por el socket.
- **libEGL.so + libGLESv2.so** (glibc, container) son el shim. Reemplazan las libs de Mesa. Cada entry point EGL/GLES se serializa al socket, el daemon lo ejecuta contra el driver real y devuelve el resultado.

El container ve la GPU nativa como si fuera suya, con el driver oficial, sin emulación, sin Zink, sin traducción a Vulkan.

## Por qué no es Zink, ni freedreno

- **Zink** traduce OpenGL a Vulkan. Scutum no traduce: expone el driver OpenGL ES **directo**. La app llama `glDrawArrays` y termina llamando `glDrawArrays` en el driver Mali. Lo único que cambia es que la llamada cruza un socket.
- **freedreno** reimplementa el driver para Adreno. Scutum reenvía al driver oficial de ARM.

El resultado es que el rendimiento depende solo de dos factores: el costo del round-trip por socket y lo que el driver pueda hacer. Ninguna traducción de por medio.

## Rendimiento medido

Todo sobre **Mali-G52 MC2** con driver ARM propietario **OpenGL ES 3.2 v1.r49p1** a **1600×720**.

| Juego | Scutum + gl4es | Nativo Android | % del nativo |
|---|---:|---:|---:|
| **Red Eclipse** | 60 fps estables (gráficos default) | — | — |
| **SuperTuxKart** | 30 fps estables (cap del juego) | — | — |
| **Luanti / Minetest** | 30–40 fps | 35–55 fps | **~65–85%** |
| **Extreme Tux Racer** | 30–50 fps | — | — |
| **SuperTux 2** | 15–20 fps | — | — |

Luanti es el caso más duro del set: geometría voxel densa, miles de draw calls por frame. Aun así entrega dos tercios a cinco sextos del nativo **con dos capas de traducción encima** (gl4es traduciendo GL 2.1 → GLES, Scutum cruzando un socket). Los otros juegos, con geometría moderada, van al tope de lo que da el driver sin degradación perceptible.

## Qué logra

- **EGL 1.5** completo.
- **GLES 2.0 + 3.0 + 3.1 + 3.2** forwardeados completos.
- **Presentación por X11**: readback + flip + swap R/B + `xcb_put_image`.
- **Presentación por AHardwareBuffer**: blit en GPU + lectura cacheada, evita el readback directo desde el pbuffer.
- **Client arrays**: interceptados y subidos como scratch buffers antes del draw (evita SIGSEGV en `glDrawArrays`/`glDrawElements` con punteros cliente).
- **Pool de nombres** en `glGenTextures`/`glGenBuffers`: batching de 64 nombres por round-trip.
- **Oclusión queries** (`GL_SAMPLES_PASSED`) con resultado forzado cuando `SC_QUERY_VISIBLE=1` (para mapas donde el frustum culling por query queda roto).

## Estado

**Funcional y estable.** Verificado end-to-end en instalación limpia con:

- Red Eclipse (GLX puro, motor Cube 2)
- SuperTuxKart (SDL2 + EGL, con gl4es)
- SuperTux 2 (SDL2 + EGL)
- Luanti / Minetest (SDL2 + EGL)
- Extreme Tux Racer (SDL 1.2 + GLX)
- vkcube / vkcubepp (por Spatha)

Sin bugs abiertos que impidan el uso. Los pendientes son de rendimiento (batching de más comandos, reducir round-trips) y cosméticos.

## Estructura

    glibc/    shim (container, gcc)
    bionic/   daemon (host Android, clang)
    build/    binarios compilados (.so en glibc/, scutumd en bionic/)

## Compilar

glibc (dentro del container):

    cd glibc && make

bionic (en Termux):

    cd bionic && make

Los binarios salen en `build/glibc/` y `build/bionic/`.

## Instalación

Binarios del release v1.2:

- `libEGL.so`, `libGLESv2.so` → `/usr/lib/aarch64-linux-gnu/` del container
- `scutumd`, `libcrashisolate.so` → `$PREFIX/bin/` del host

**`libGLESv2.so` debe ser symlink a `libEGL.so`** — un solo handle en memoria = un solo contexto EGL. Si son archivos separados, gl4es detecta "Android driver" y devuelve Max vertex attrib: 0.

Arrancar el daemon:

    SCUTUM_LIBDIR=/system/lib64 \
    SCUTUM_SOCK=$PREFIX/tmp/scutum.sock \
    scutumd

## Variables

- `SCUTUM_SOCK` — path del socket. Default `/tmp/scutum.sock`.
- `SCUTUM_LIBDIR` — dónde buscar el driver. Default `/system/lib64`.
- `SCUTUM_VERBOSE=1` — trazas.
- `SCUTUM_GLERR=1` — chequea `glGetError` tras cada op.
- `SCUTUM_CHECK_PROGRAMS=1` — loguea COMPILE_FAIL y LINK_FAIL con el log real del driver.
- `SCUTUM_PRESENT=sync|pbo|ahb` — modo de presentación. Default `ahb`.
- `SCUTUM_QUERY_VISIBLE=1` — fuerza oclusión queries a "visible".
- `SC_DEBUG_BANNER=1` — imprime el banner `[sc-build]` al cargar el shim.

## Licencia

MIT.
