# 🛡️ Scutum

**Hardware-accelerated OpenGL ES for glibc Linux apps running inside a container on Android — by bridging them to the phone's real GPU driver.**

![License](https://img.shields.io/badge/license-MIT-blue)
![Arch](https://img.shields.io/badge/arch-aarch64-informational)
![Protocol](https://img.shields.io/badge/protocol-v2-success)
![Language](https://img.shields.io/badge/language-C-lightgrey)

---

## Table of contents

- [What is Scutum?](#what-is-scutum)
- [Why does it exist?](#why-does-it-exist)
- [How it works](#how-it-works)
  - [Architecture](#architecture)
  - [The wire protocol](#the-wire-protocol)
  - [Shared-memory command bus](#shared-memory-command-bus)
  - [Presenting frames to X11](#presenting-frames-to-x11)
  - [GPU-quirk workarounds](#gpu-quirk-workarounds)
- [Repository layout](#repository-layout)
- [Building](#building)
- [Running](#running)
- [Configuration](#configuration)
- [Debugging](#debugging)
- [Known limitations](#known-limitations)
- [License](#license)

---

## What is Scutum?

Scutum is a **GPU forwarding layer** made of two cooperating pieces:

| Component | Runs in | Linked against | Role |
|---|---|---|---|
| **Shim** (`libEGL.so`, `libGLESv2.so`) | The Linux container (glibc) | glibc | Drop-in replacement for Mesa's EGL/GLES libraries. Instead of talking to a GPU, it serializes every EGL/GLES call and sends it to the daemon. |
| **Daemon** (`scutumd`) | The Android host (e.g. Termux) | bionic | Loads Android's *real* `libEGL.so` / `libGLESv2.so`, executes the calls on the physical GPU, and returns results and rendered frames. |

An application inside the container believes it is using a normal EGL/GLES 3.2 driver. In reality, every call is transparently executed on the phone's own GPU driver (Mali, in the development target).

```
 ┌──────────────── Linux container (glibc) ───────────────┐      ┌────── Android host (bionic) ──────┐
 │                                                         │      │                                    │
 │   App / gl4es  ──►  Scutum shim  ══ shared memory ═══════════►  scutumd  ──►  Android libEGL/GLES   │
 │  (OpenGL / GLES)   libEGL.so        (lock-free rings)   │      │ (1 thread per client)    │         │
 │                    libGLESv2.so  ◄══════════════════════════════                          ▼         │
 │                         │                               │      │                       Real GPU      │
 │                         ▼                               │      │                                    │
 │                  X11 window (xcb + MIT-SHM)             │      └────────────────────────────────────┘
 └─────────────────────────────────────────────────────────┘
```

---

## Why does it exist?

Running a desktop Linux distro on a phone (via `chroot`/`proot` inside Termux, for example) creates a graphics gap:

1. **The container is glibc; Android is bionic.** The GPU driver on Android is a bionic library. A glibc program cannot simply `dlopen` it — the two C libraries and their dynamic linkers are incompatible.
2. **Mesa has no hardware path here.** Without access to the real driver, the container falls back to software rendering (e.g. llvmpipe), which is far too slow for 3D applications and games.
3. **Android's EGL doesn't understand X11.** Even if the driver were reachable, Android EGL expects Android native windows, not X11 windows or X11 displays.

Scutum solves this by **splitting the problem across the process boundary** instead of trying to load the driver in the wrong environment:

- the *bionic* side does the one thing only it can do — talk to the real driver;
- the *glibc* side does the one thing only it can do — look like a standard EGL/GLES library to Linux software;
- a purpose-built, low-overhead protocol connects them.

The result is that unmodified Linux OpenGL ES (and, through translation layers such as **gl4es**, even desktop OpenGL 1.x/2.x) applications — SuperTuxKart is the reference workload — can render on the phone's GPU.

---

## How it works

### Architecture

**Shim (`glibc/`)**

- `sc_egl.c` — implements the EGL API. Handles are treated as opaque 64-bit values: the daemon generates them, the shim hands them to the app untouched.
- `sc_gles.c` — implements GLES 2.0 / 3.0 / 3.1 / 3.2 plus extensions. Void calls are **batched**; calls that return data are **synchronous**.
- `sc_core.c` — connection management, handshake, batching, sync round-trips, and the shared-memory transport.
- `sc_clientarr.h` — client-side vertex/index array emulation (see [workarounds](#gpu-quirk-workarounds)).
- `scutum.h`, `sc_shm.h`, `sc_gl_enum.h` — the protocol definition, ring-buffer implementation, and generated GL opcode table, shared with the daemon.

**Daemon (`bionic/`)**

- `scutumd.c` — listens on a Unix socket and spawns **one pthread per client connection**. EGL/GLES contexts are per-thread, so serving each client on its own thread preserves correct context semantics even for apps with multiple contexts.
- `crashisolate.c` — a preloadable crash handler (`libcrashisolate.so`): if a *worker* thread segfaults inside the vendor driver, only that thread exits; the daemon survives. A crash on the main thread prints a symbolized backtrace and exits.
- `scutumd-supervisor.sh` — a restart loop that relaunches the daemon if it dies and cleans up the stale socket.

### The wire protocol

Defined in `glibc/scutum.h` (protocol version **2**).

- **Framing:** every message is a 24-byte header (`magic "SCUT"`, `op`, `len`, `req_id`, `thread_id`, flags) followed by `len` payload bytes. Little-endian, 8-byte aligned.
- **Handshake:** the client sends `HELLO{proto_version}`; the daemon replies `HELLO_ACK{version, caps, shm_total, ring_size}` plus a file descriptor. A version mismatch yields `E_PROTO_VERSION` and the client disconnects.
- **GL batches:** void GL calls are packed into a single `SC_OP_GL_BATCH` (`n_inst` × `{fn, argbytes, args}`), flushed automatically at 1 MiB or before any sync call. No reply is expected; GL errors accumulate per-thread and are delivered on the next `glGetError`.
- **Sync calls:** anything that returns a value or fills a user buffer (`glGetString`, `glReadPixels`, `eglChooseConfig`, …) is sent as a single `SC_OP_GL_SYNC` and blocks until the reply arrives.
- **Object names pass through unchanged.** `GLuint` names (textures, buffers, shaders, programs…) are the *real* driver names, so no translation table is needed. EGL objects are opaque `u64`s.
- **File descriptors:** when an operation needs to pass an fd (e.g. a shared frame buffer), it travels over the Unix socket via `SCM_RIGHTS`.
- **Large payloads:** up to 64 MiB per message, with `SC_OP_FRAGMENT` for anything bigger.
- **Errors:** a typed `SC_OP_ERROR` carries a class (protocol / EGL / GL / internal), a code, and an optional message.

### Shared-memory command bus

The Unix socket is used **only** for the handshake, fd passing, and detecting that the other side died. All commands and replies flow through a **pair of lock-free single-producer/single-consumer ring buffers** in a `memfd` shared region:

```
[ control page 4 KiB ][ request ring: shim → daemon ][ response ring: daemon → shim ]
```

- One region per connection (i.e. per shim thread). Default size is 16 MiB (two 8 MiB rings, tunable with `SCUTUM_SHM_MB`); memory is sparse, so only touched pages are committed.
- Head/tail are monotonically increasing 64-bit counters, so wrap-around needs no special case. Frames larger than the ring are streamed in segments.
- Where possible the consumer parses a frame **in place, with zero copies**.
- **Waiting is adaptive:** ~20 µs of pure spinning (no syscalls), then `sched_yield`, then a cross-process `futex` sleep. The producer only issues `FUTEX_WAKE` if the consumer flagged itself as sleeping — so the fast path involves **no syscalls at all**. On single-CPU affinities the spin phase is skipped automatically.
- While waiting, each side periodically peeks the socket to detect peer death (EOF) instead of hanging forever.

### Presenting frames to X11

Android's EGL cannot render to an X11 window, so Scutum presents frames itself:

1. `eglCreateWindowSurface` creates an **off-screen pbuffer** of the X11 window's size on the daemon side and registers it.
2. `eglGetConfigAttrib(EGL_NATIVE_VISUAL_ID)` is intercepted and answered with the X root window's visual (via `xcb`), so toolkits like gl4es can match configs to visuals.
3. On `eglSwapBuffers`, the daemon reads the rendered frame back to the CPU. By default it uses an **`AHardwareBuffer`** path (GPU blit + cached CPU read); `sync` and `pbo` readback modes are available as alternatives.
4. The frame is written straight into the shared region and the shim blits it to the window with **MIT-SHM** (`xcb_shm_put_image`), double-buffered, falling back to plain `xcb_put_image` if SHM is unavailable.

### GPU-quirk workarounds

Real mobile drivers and real-world apps disagree in ways that need active mediation. The shim contains targeted fixes for these:

| Problem | What Scutum does |
|---|---|
| **Client-side vertex/index arrays.** gl4es passes pointers into *container* memory to `glVertexAttribPointer` / `glDrawElements`. Forwarded as-is, they are invalid in the daemon's address space and crash the driver. | `sc_clientarr.h` tracks bound VBOs, enabled attributes, pointers and divisors. At draw time it uploads the client data into a rotating pool of scratch buffers on the daemon, repoints the attributes, then restores bindings. |
| **`mediump` precision.** gl4es emits `precision mediump float`; on Mali Bifrost/Valhall that is true fp16, so vertex positions visibly break. | Shaders are rewritten to force `highp` (disable with `SC_HIGHP=0`). |
| **EGL config ordering.** EGL sorts configs by smallest depth first, so apps asking for a 16-bit (or no) depth buffer get poor depth precision and 3D models render wrongly. | A minimum depth size is enforced (default 24, via `SC_DEPTH_BITS`). |
| **Display types.** Mali in a JVM-less environment (Termux) cannot accept X11 displays. | The shim substitutes the platform's default display. |
| **Round-trip latency.** `glGenTextures`/`glGenBuffers` cost ~1 ms per sync call under proot; apps call `glGetError` dozens of times per frame. | Object names are pre-generated in pools of 64; `glGetError` has a fast path; expensive validation (`SCUTUM_CHECK_PROGRAMS`, `SC_CHECK_ERR`) is opt-in. |
| **Driver crashes.** A segfault in a vendor driver thread shouldn't kill every client. | `libcrashisolate.so` isolates worker-thread crashes and supervises restarts. |

---

## Repository layout

```
Scutum/
├── Makefile                 # top-level build driver
├── LICENSE                  # MIT
├── glibc/                   # ── container side ──
│   ├── sc_egl.c             #   EGL shim + X11/xcb presentation
│   ├── sc_gles.c            #   GLES 2.0–3.2 shim
│   ├── sc_core.c            #   connection, batching, sync, shm transport
│   ├── sc_core.h            #   internal shim API
│   ├── sc_clientarr.h       #   client-side array emulation
│   ├── scutum.h             #   wire protocol (shared with daemon)
│   ├── sc_shm.h             #   shared-memory ring buffers (shared)
│   ├── sc_gl_enum.h         #   GL/EGL opcode table (shared)
│   ├── test_egl.c           #   smoke test: EGL init
│   ├── test_gl.c            #   smoke test: context + clear
│   └── test_shm.c           #   unit test: ring wrap / futex / fd passing
├── bionic/                  # ── Android host side ──
│   ├── scutumd.c            #   the daemon
│   ├── crashisolate.c       #   crash-isolation preload library
│   └── scutumd-supervisor.sh#   auto-restart wrapper (Termux)
└── build/                   # prebuilt aarch64 binaries
    ├── bionic/scutumd
    └── glibc/{libEGL.so, libGLESv2.so}
```

> `libEGL.so` and `libGLESv2.so` are the **same binary** — the build copies one to the other, since the shim implements both APIs in a single object.

---

## Building

Scutum targets **aarch64**. Build each side in its own environment.

### Shim — inside the glibc container

Requirements: `gcc`, `libxcb` and `libxcb-shm` development headers, plus the Khronos EGL/GLES headers.

```sh
cd glibc
make            # produces libEGL.so and libGLESv2.so
make tests      # builds test_egl, test_gl, test_shm
```

### Daemon — on the Android host (Termux)

Requirements: `clang` and the Android EGL/GLES headers.

```sh
cd bionic
make            # produces scutumd and libcrashisolate.so
```

Prebuilt binaries are also included under `build/`.

---

## Running

**1. Start the daemon on the Android host** (ideally under the supervisor):

```sh
cd bionic
SCUTUM_LIBDIR=/system/lib64 \
SCUTUM_SOCK="$PREFIX/tmp/scutum.sock" \
LD_PRELOAD=./libcrashisolate.so \
  ./scutumd
```

Or use `scutumd-supervisor.sh`, which also logs to `~/scutumd.log` and restarts the daemon after a crash. *(Edit the `LD_PRELOAD` path inside the script to match where you built `libcrashisolate.so`.)*

**2. Make the socket visible inside the container** — bind-mount or otherwise expose the socket path, then point the shim at it:

```sh
export SCUTUM_SOCK=/path/to/scutum.sock
```

**3. Put the shim first on the library path in the container** and run your app:

```sh
export LD_LIBRARY_PATH=/path/to/Scutum/glibc:$LD_LIBRARY_PATH
./glibc/test_egl     # prints EGL version and vendor strings
./glibc/test_gl      # creates a pbuffer context, prints GL strings, clears
./glibc/test_shm     # exercises the shared-memory transport
```

If `test_gl` prints the GL vendor/renderer of your phone's GPU rather than `llvmpipe`, Scutum is working.

---

## Configuration

All configuration is through environment variables.

### Daemon (`scutumd`)

| Variable | Purpose |
|---|---|
| `SCUTUM_SOCK` | Socket path (default `/tmp/scutum.sock`). |
| `SCUTUM_LIBDIR` | Directory containing Android's `libEGL.so` / `libGLESv2.so` (default search: `/system/lib64`, `/system/lib`). |
| `SCUTUM_SHM_MB` | Total shared-memory size per connection (default 16). |
| `SCUTUM_SPIN_US` | Spin duration before sleeping on the futex. |
| `SCUTUM_PRESENT` | Frame readback mode: default `AHardwareBuffer`, or `sync` / `pbo`. |
| `SCUTUM_BGRA` | `1` = read `GL_BGRA_EXT` directly (avoids R/B swizzle). |
| `SCUTUM_CHECK_PROGRAMS` | `1` = verify shader compile status synchronously (slow). |
| `SCUTUM_GLERR` | `1` = report GL errors in the daemon. |
| `SCUTUM_VERBOSE` | Verbose logging. |
| `SCUTUM_STATS` / `SCUTUM_PROBE` | Per-frame timing statistics / fixed-vs-per-pixel cost probe. |

### Shim (inside the container)

| Variable | Purpose |
|---|---|
| `SCUTUM_SOCK` | Socket path to connect to. |
| `SC_NO_SHM` | `1` = disable MIT-SHM presentation (use `xcb_put_image`). |
| `SC_DEPTH_BITS` | Minimum depth buffer size (default 24). |
| `SC_HIGHP` | `0` = don't force `highp` in shaders. |
| `SC_QUERY_VISIBLE` | `1` = expose occlusion query results. |
| `SC_STATS` | `1` = print transport statistics. |

### Tracing (shim)

`SC_TRACE_EGL`, `SC_TRACE_SWAP`, `SC_TRACE_SHADERS`, `SC_TRACE_CA`, `SC_TRACE_MAP`, `SC_CHECK_ERR`, `SC_GLERR_SYNC` — set to `1` for diagnostics.

---

## Debugging

- **Crash reports.** With `libcrashisolate.so` preloaded, a `SIGSEGV`/`SIGBUS` prints `module+offset (symbol)` for the faulting PC, link register, and a 32-frame backtrace. The offsets can be fed directly to `addr2line`.
- **Seeing what an app sends.** `SC_TRACE_SHADERS=1` dumps the shaders (useful with gl4es); `SC_TRACE_CA=1` logs each client-array upload.
- **Finding rejected GL calls.** `SC_CHECK_ERR=1` (requires building with `SC_DEBUG_CHK`) and `SCUTUM_GLERR=1` surface driver-side errors. These add synchronous round-trips, so use them only for diagnosis.
- **Performance.** `SCUTUM_STATS=1` and `SC_STATS=1` print timing breakdowns for presentation and transport.

---

## Known limitations

These are documented in the source and not yet resolved:

- `glMapBufferRange` / `glUnmapBuffer`: no write-back of mapped memory yet.
- `glGetShaderPrecisionFormat`: the daemon does not report the `precision` field.
- `glDebugMessageCallback` is a no-op.
- `eglGetPlatformDisplay` and its `EXT` variant share an opcode but use different attribute layouts.
- Client-array upload copies `[0..maxvertex]` rather than `[min..max]` — correct, but can send more data than needed. `glDrawElementsBaseVertex` and multi-draw calls bypass this path.
- Client-array tracking applies to VAO 0 only (GLES2 semantics); with a non-zero VAO, state is forwarded unchanged.
- Presentation is a **GPU → CPU readback → X11** copy. It is heavily optimized, but it is not zero-copy and will cost more at high resolutions.
- Developed and tuned against a **Mali** GPU on Termux; other GPUs may need additional workarounds.

---

## License

Released under the [MIT License](LICENSE) — © 2026 LexusYTG.
