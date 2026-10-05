/*
 * sc_core.h — API interna del shim Scutum.
 * Solo la usan sc_egl.c y sc_gles.c. No la incluye la app.
 *
 * Modelo:
 *   - Cada pthread tiene su propio estado (socket, buffer de batch, buffer de
 *     sync) y su propio thread_id (0 = hilo principal, se envía siempre 0 por
 *     ahora; el daemon distingue por hilo del socket, que es único por pthread
 *     porque cada pthread abre su propio socket).
 *   - Las funciones GL void usan sc_batch_begin / sc_emit_* / sc_batch_end.
 *   - Las funciones GL y EGL que devuelven algo usan
 *       sc_sync_begin(op) / sc_emit_* / sc_sync_send()
 *     y luego sc_sync_result / sc_sync_recv_*.
 *   - Los tipos de emit que usan las llamadas generadas son I32/U32/F32/U64.
 *     U64 también se usa para "puntero a datos" que el shim puede empaquetar
 *     en un solo sc_emit_bytes, o para handles opacos.
 */
#ifndef SC_CORE_H
#define SC_CORE_H

#include <stddef.h>
#include <stdint.h>
#include "scutum.h"

/* ---- batching ---- */

/* Inicia una instrucción (op es un SC_GL_*). Flushea el batch si excedió
 * SC_BATCH_MAX. Devuelve 0 o -errno. */
int sc_batch_begin(uint32_t op);
/* Cierra la instrucción actual y arranca el contador de n_inst. */
int sc_batch_end(void);
/* Fuerza el flush del batch pendiente. */
int sc_batch_flush(void);

/* ---- emit (a batch o a sync, según el modo activo) ---- */

int sc_emit_u32(uint32_t v);
int sc_emit_i32(int32_t  v);
int sc_emit_u64(uint64_t v);
int sc_emit_f32(float    v);

/* Empaqueta [u32 len][bytes]. len=0 => sin bytes (equivale a NULL). */
int sc_emit_bytes(const void *p, size_t n);
/* Igual pero con NUL opcional como terminador (no se envía). NULL => len=0. */
int sc_emit_string(const char *s);

/* Alias que usa el generador (sc_gles.c): sc_emit_I32/U32/F32/U64.
 * No se usan identificadores en mayúscula como nombres de macro para no
 * chocar con tipos; son inline wrappers. */
static inline int sc_emit_I32(int32_t  v) { return sc_emit_i32(v); }
static inline int sc_emit_U32(uint32_t v) { return sc_emit_u32(v); }
static inline int sc_emit_U64(uint64_t v) { return sc_emit_u64(v); }
static inline int sc_emit_F32(float    v) { return sc_emit_f32(v); }

/* ---- sync ---- */

/* Inicia una instrucción con respuesta. Flushea batch si había uno. */
int sc_sync_begin(uint32_t op);
/* Envía el request y trae la respuesta. Los datos quedan accesibles con
 * sc_sync_result / sc_sync_recv_u64 / sc_sync_recv_bytes. */
int sc_sync_send(void);

void     sc_sync_attach_fd(int fd);   /* anexa un fd al proximo sync_send */
int32_t  sc_sync_result(void);
uint64_t sc_sync_recv_u64(void);
void     sc_sync_recv_void(void);
size_t   sc_sync_recv_bytes(void *out, size_t max);

/* ---- error GL acumulado ---- */

uint32_t sc_last_gl_error(void);
void     sc_set_gl_error(uint32_t e);
void     sc_clear_gl_error(void);

/* ---- helpers de stringify de nombre para glGetString/glGetStringi ---- */
/* El shim no reenvía el puntero devuelto por el daemon: recibe los bytes y
 * los guarda en un buffer estático por thread para devolver el char* al
 * usuario. La longitud es del string; el NUL se agrega del lado del shim. */
const char *sc_string_intern(const void *bytes, size_t len);

/* variante con slot explícito (name/index = GL_VENDOR, GL_RENDERER, ...) */
const char *sc_string_intern_slot(const void *bytes, size_t len,
                                  uint32_t name, uint32_t index);

/* ---- arrays cliente (sc_clientarr.h) ---- */
void sc_ca_make_current(const void *ctx);
void sc_ca_destroy(const void *ctx);

#endif /* SC_CORE_H */
