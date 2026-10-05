/*
 * sc_clientarr.h — arrays de vertices / indices del lado CLIENTE para Scutum.
 *
 * PROBLEMA: gl4es (backend GLES2) llama glVertexAttribPointer / glDrawElements
 * con punteros a memoria del proceso del container (sin VBO). El shim los
 * reenviaba tal cual al daemon, que los pasaba al Mali: direccion invalida en
 * otro proceso -> SIGSEGV en glDrawArrays (crashisolate: worker aislado).
 *
 * SOLUCION: el shim rastrea el estado (VBO enlazados, atributos habilitados,
 * punteros cliente, divisores). Al dibujar, copia los datos cliente a buffers
 * scratch del daemon (un glBufferData), apunta los atributos al scratch con
 * offsets y restaura los bindings. Solo rastrea VAO 0 (en ES2 no hay VAOs; con
 * un VAO != 0 se reenvia todo tal cual).
 *
 * Debug: SC_TRACE_CA=1 imprime cada subida.
 * Limitacion conocida: sube [0..maxvertex] (no [min..max]); correcto pero puede
 * enviar de mas. glDrawElementsBaseVertex/MultiDraw no pasan por aca.
 */
#ifndef SC_CLIENTARR_H
#define SC_CLIENTARR_H

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SC_CA_MAXATTR 16
#define SC_CA_MAXCTX  16
#define SC_CA_MAXBYTES (256u * 1024u * 1024u)
#define SC_CA_RING 16   /* buffers scratch rotativos: evita reusar el que la GPU aun lee */
#define SC_CA_POOL 64   /* nombres pre-generados por pedido (glGenTextures/Buffers) */

typedef struct {
    uint8_t  en, integer, client;
    GLint    size;
    GLenum   type;
    GLboolean norm;
    GLsizei  stride;
    const void *ptr;
    GLuint   div;
} sc_ca_attr;

typedef struct {
    const void *key;
    int      used;
    GLuint   arr, elem, vao;
    GLuint   scratch_a[SC_CA_RING], scratch_e[SC_CA_RING];
    unsigned ring;
    GLuint   pool[2][SC_CA_POOL];   /* 0 = texturas, 1 = buffers */
    int      pool_n[2];
    sc_ca_attr a[SC_CA_MAXATTR];
} sc_ca_ctx;

static sc_ca_ctx g_ca_tab[SC_CA_MAXCTX];
static pthread_mutex_t g_ca_mu = PTHREAD_MUTEX_INITIALIZER;
static __thread sc_ca_ctx *t_ca;

static int ca_trace(void) {
    static int on = -1;
    if (on < 0) { const char *t = getenv("SC_TRACE_CA"); on = (t && *t == '1'); }
    return on;
}

/* ---- contexto actual (lo llama sc_egl.c) ---- */
void sc_ca_make_current(const void *ctx) {
    if (!ctx) { t_ca = NULL; return; }
    pthread_mutex_lock(&g_ca_mu);
    sc_ca_ctx *f = NULL, *fr = NULL;
    for (int i = 0; i < SC_CA_MAXCTX; i++) {
        if (g_ca_tab[i].used && g_ca_tab[i].key == ctx) { f = &g_ca_tab[i]; break; }
        if (!g_ca_tab[i].used && !fr) fr = &g_ca_tab[i];
    }
    if (!f && fr) { memset(fr, 0, sizeof *fr); fr->used = 1; fr->key = ctx; f = fr; }
    pthread_mutex_unlock(&g_ca_mu);
    t_ca = f;   /* NULL si la tabla esta llena: todo pasa sin emulacion */
}
void sc_ca_destroy(const void *ctx) {
    if (!ctx) return;
    pthread_mutex_lock(&g_ca_mu);
    for (int i = 0; i < SC_CA_MAXCTX; i++)
        if (g_ca_tab[i].used && g_ca_tab[i].key == ctx) g_ca_tab[i].used = 0;
    pthread_mutex_unlock(&g_ca_mu);
    if (t_ca && t_ca->key == ctx) t_ca = NULL;
}

static inline sc_ca_ctx *ca_cur(void) { return t_ca; }

/* ---- rastreo de estado ---- */
static void ca_on_bind_buffer(GLenum target, GLuint buf) {
    sc_ca_ctx *s = t_ca; if (!s) return;
    if (target == 0x8892) s->arr = buf;                      /* GL_ARRAY_BUFFER */
    else if (target == 0x8893 && s->vao == 0) s->elem = buf; /* GL_ELEMENT_ARRAY_BUFFER */
}

/* ---- copia sombra de buffers GL_ELEMENT_ARRAY_BUFFER (para calcular el max indice
 *      cuando hay atributos cliente + indices en VBO) ---- */
#define SC_SH_N 8192u
#define SC_SH_TOMB 0xFFFFFFFFu
typedef struct { GLuint id; uint8_t *d; size_t n; } sc_sh;
static sc_sh g_sh[SC_SH_N];
static pthread_mutex_t g_sh_mu = PTHREAD_MUTEX_INITIALIZER;

static sc_sh *sh_find(GLuint id, int create) {   /* con g_sh_mu tomado */
    unsigned h = (unsigned)(((uint64_t)id * 2654435761u) % SC_SH_N), firstfree = SC_SH_N;
    for (unsigned k = 0; k < SC_SH_N; k++) {
        unsigned i = (h + k) % SC_SH_N;
        if (g_sh[i].id == id) return &g_sh[i];
        if (g_sh[i].id == SC_SH_TOMB && firstfree == SC_SH_N) firstfree = i;
        if (g_sh[i].id == 0) { if (firstfree == SC_SH_N) firstfree = i; break; }
    }
    if (!create || firstfree == SC_SH_N) return NULL;
    memset(&g_sh[firstfree], 0, sizeof g_sh[firstfree]);
    g_sh[firstfree].id = id;
    return &g_sh[firstfree];
}
static void ca_shadow_data(GLenum target, GLsizeiptr size, const void *data) {
    sc_ca_ctx *s = t_ca; if (!s || target != 0x8893 || !s->elem || size < 0) return;
    pthread_mutex_lock(&g_sh_mu);
    sc_sh *e = sh_find(s->elem, 1);
    if (e) {
        free(e->d); e->d = NULL; e->n = 0;
        if (size > 0 && (e->d = (uint8_t *)calloc(1, (size_t)size))) {
            e->n = (size_t)size;
            if (data) memcpy(e->d, data, (size_t)size);
        }
    }
    pthread_mutex_unlock(&g_sh_mu);
}
static void ca_shadow_sub(GLenum target, GLintptr off, GLsizeiptr size, const void *data) {
    sc_ca_ctx *s = t_ca; if (!s || target != 0x8893 || !s->elem || !data || off < 0 || size <= 0) return;
    pthread_mutex_lock(&g_sh_mu);
    sc_sh *e = sh_find(s->elem, 0);
    if (e && e->d && (size_t)off + (size_t)size <= e->n) memcpy(e->d + off, data, (size_t)size);
    pthread_mutex_unlock(&g_sh_mu);
}
static void ca_shadow_free(GLuint id) {
    pthread_mutex_lock(&g_sh_mu);
    sc_sh *e = sh_find(id, 0);
    if (e) { free(e->d); e->d = NULL; e->n = 0; e->id = SC_SH_TOMB; }
    pthread_mutex_unlock(&g_sh_mu);
}
/* 1 = ok (*mx = max indice), 0 = sin datos sombra */
static int ca_shadow_max(GLuint id, size_t off, GLsizei count, size_t isz, uint32_t *mx) {
    int ok = 0; uint32_t m = 0;
    pthread_mutex_lock(&g_sh_mu);
    sc_sh *e = sh_find(id, 0);
    if (e && e->d && off + (size_t)count * isz <= e->n) {
        const uint8_t *p = e->d + off;
        for (GLsizei i = 0; i < count; i++) {
            uint32_t v;
            if (isz == 1) v = p[i];
            else if (isz == 2) { uint16_t t; memcpy(&t, p + 2 * (size_t)i, 2); v = t; }
            else { memcpy(&v, p + 4 * (size_t)i, 4); }
            if (v > m) m = v;
        }
        ok = 1;
    }
    pthread_mutex_unlock(&g_sh_mu);
    *mx = m; return ok;
}
static void ca_on_delete_buffers(GLsizei n, const GLuint *b) {
    sc_ca_ctx *s = t_ca; if (!s || !b) return;
    for (GLsizei i = 0; i < n; i++) {
        if (!b[i]) continue;
        ca_shadow_free(b[i]);
        if (s->arr == b[i])  s->arr = 0;
        if (s->elem == b[i]) s->elem = 0;
    }
}
static void ca_on_bind_vao(GLuint v) { sc_ca_ctx *s = t_ca; if (s) s->vao = v; }
static void ca_on_enable(GLuint i, int en) {
    sc_ca_ctx *s = t_ca;
    if (s && s->vao == 0 && i < SC_CA_MAXATTR) s->a[i].en = (uint8_t)en;
}
static void ca_on_divisor(GLuint i, GLuint d) {
    sc_ca_ctx *s = t_ca;
    if (s && s->vao == 0 && i < SC_CA_MAXATTR) s->a[i].div = d;
}
/* Devuelve 1 si es puntero cliente (NO reenviar al daemon); 0 si es VBO/offset. */
static int ca_on_pointer(GLuint i, GLint size, GLenum type, GLboolean norm,
                         GLsizei stride, const void *ptr, int integer) {
    sc_ca_ctx *s = t_ca;
    if (!s || s->vao != 0 || i >= SC_CA_MAXATTR) return 0;
    sc_ca_attr *a = &s->a[i];
    a->size = size; a->type = type; a->norm = norm; a->stride = stride;
    a->ptr = ptr; a->integer = (uint8_t)integer;
    a->client = (s->arr == 0 && ptr != NULL);
    return a->client;
}

/* ---- emision cruda (sin pasar por los wrappers que rastrean) ---- */
static void ca_raw_bind(GLenum target, GLuint id) {
    sc_batch_begin(SC_GL_glBindBuffer); sc_emit_u32(target); sc_emit_u32(id); sc_batch_end();
}
static void ca_raw_data(GLenum target, const void *d, size_t n) {
    sc_batch_begin(SC_GL_glBufferData);
    sc_emit_u32(target); sc_emit_u64((uint64_t)n); sc_emit_bytes(d, n);
    sc_emit_u32(0x88E0 /* GL_STREAM_DRAW */);
    sc_batch_end();
}
static void ca_raw_ptr(GLuint i, const sc_ca_attr *a, uint64_t off) {
    if (a->integer) {
        sc_batch_begin(SC_GL_glVertexAttribIPointer);
        sc_emit_u32(i); sc_emit_i32(a->size); sc_emit_u32(a->type);
        sc_emit_i32(a->stride); sc_emit_u64(off);
    } else {
        sc_batch_begin(SC_GL_glVertexAttribPointer);
        sc_emit_u32(i); sc_emit_i32(a->size); sc_emit_u32(a->type); sc_emit_u32(a->norm);
        sc_emit_i32(a->stride); sc_emit_u64(off);
    }
    sc_batch_end();
}

static size_t ca_tsize(GLenum t) {
    switch (t) {
    case 0x1400: case 0x1401: return 1;               /* BYTE, UBYTE */
    case 0x1402: case 0x1403: case 0x140B: case 0x8D61: return 2;  /* SHORT, USHORT, HALF_FLOAT */
    default: return 4;                                /* INT, UINT, FLOAT, FIXED */
    }
}
static size_t ca_elem_bytes(const sc_ca_attr *a) {
    if (a->type == 0x8D9F || a->type == 0x8368) return 4; /* 2_10_10_10_REV */
    return (size_t)(a->size > 0 ? a->size : 1) * ca_tsize(a->type);
}
static size_t ca_stride(const sc_ca_attr *a) { return a->stride > 0 ? (size_t)a->stride : ca_elem_bytes(a); }

static void ca_warn_once(const char *m) {
    static int w; if (!w) { w = 1; fprintf(stderr, "[sc-ca] %s\n", m); }
}

/*
 * Antes de un draw. Devuelve 0 = nada que hacer, 1 = se subieron datos (llamar
 * ca_end tras el draw), -1 = no se puede resolver: NO dibujar (mejor que SIGSEGV).
 * indexed: *ind es el puntero 'indices' (se pone a 0 si se subieron indices).
 */
static int ca_begin(sc_ca_ctx *s, int indexed, GLint first, GLsizei count,
                    GLenum itype, const void **ind, GLsizei inst) {
    if (!s || s->vao != 0 || count <= 0) return 0;
    int idx[SC_CA_MAXATTR], n = 0;
    for (int i = 0; i < SC_CA_MAXATTR; i++)
        if (s->a[i].en && s->a[i].client && s->a[i].ptr) idx[n++] = i;
    int client_idx = indexed && s->elem == 0 && ind && *ind != NULL;
    if (!n && !client_idx) return 0;

    uint64_t nv = 0;
    size_t isz = 0;
    if (indexed) {
        isz = itype == 0x1401 ? 1 : itype == 0x1403 ? 2 : itype == 0x1405 ? 4 : 0;
        if (!isz) return -1;
        uint32_t mx = 0;
        if (!client_idx) {   /* indices en VBO + atributos cliente: rango desde la copia sombra */
            if (!s->elem || !ca_shadow_max(s->elem, ind ? (size_t)(uintptr_t)*ind : 0, count, isz, &mx)) {
                ca_warn_once("atributos cliente con indices en VBO sin copia sombra: draw omitido");
                return -1;
            }
        } else {
            const void *p = *ind;
            for (GLsizei i = 0; i < count; i++) {
                uint32_t v = isz == 1 ? ((const uint8_t *)p)[i]
                           : isz == 2 ? ((const uint16_t *)p)[i]
                                      : ((const uint32_t *)p)[i];
                if (v > mx) mx = v;
            }
        }
        nv = (uint64_t)mx + 1;
    } else {
        if (first < 0) return -1;
        nv = (uint64_t)first + (uint64_t)count;
    }

    if (!s->scratch_a[0]) {
        GLuint ids[2 * SC_CA_RING];
        memset(ids, 0, sizeof ids);
        gen_ids(SC_GL_glGenBuffers, 2 * SC_CA_RING, ids);
        for (int i = 0; i < 2 * SC_CA_RING; i++)
            if (!ids[i]) { ca_warn_once("no pude crear buffers scratch"); return -1; }
        for (int i = 0; i < SC_CA_RING; i++) { s->scratch_a[i] = ids[i]; s->scratch_e[i] = ids[SC_CA_RING + i]; }
    }
    unsigned rg = s->ring++ % SC_CA_RING;

    if (n) {
        size_t offs[SC_CA_MAXATTR], lens[SC_CA_MAXATTR], total = 0;
        GLsizei instn = inst > 0 ? inst : 1;
        for (int k = 0; k < n; k++) {
            const sc_ca_attr *a = &s->a[idx[k]];
            uint64_t v = a->div ? ((uint64_t)instn + a->div - 1) / a->div : nv;
            if (v == 0) v = 1;
            size_t st = ca_stride(a), eb = ca_elem_bytes(a);
            if (v > SC_CA_MAXBYTES / (st ? st : 1)) return -1;
            lens[k] = (size_t)(v - 1) * st + eb;
            offs[k] = total;
            total += (lens[k] + 15) & ~(size_t)15;
            if (total > SC_CA_MAXBYTES) return -1;
        }
        uint8_t *buf = (uint8_t *)calloc(1, total ? total : 1);
        if (!buf) return -1;
        for (int k = 0; k < n; k++)
            memcpy(buf + offs[k], s->a[idx[k]].ptr, lens[k]);
        ca_raw_bind(0x8892, s->scratch_a[rg]);
        ca_raw_data(0x8892, buf, total);
        free(buf);
        for (int k = 0; k < n; k++) ca_raw_ptr((GLuint)idx[k], &s->a[idx[k]], (uint64_t)offs[k]);
        if (ca_trace()) fprintf(stderr, "[sc-ca] subi %d atributos cliente, %zu bytes, %llu vertices\n",
                                n, total, (unsigned long long)nv);
    }
    if (client_idx) {
        ca_raw_bind(0x8893, s->scratch_e[rg]);
        ca_raw_data(0x8893, *ind, (size_t)count * isz);
        *ind = NULL;   /* offset 0 en el buffer scratch */
        if (ca_trace()) fprintf(stderr, "[sc-ca] subi %zu bytes de indices\n", (size_t)count * isz);
    }
    return 1;
}

static void ca_end(sc_ca_ctx *s, int ca) {
    if (!s || ca <= 0) return;
    ca_raw_bind(0x8892, s->arr);
    ca_raw_bind(0x8893, s->elem);
}

/*
 * glGenTextures/glGenBuffers eran un round-trip sincrono por llamada (~0.9 ms c/u en proot:
 * 136 llamadas/frame = 118 ms). Se piden SC_CA_POOL nombres de una vez y se reparten local.
 * kind: 0 = texturas, 1 = buffers. Devuelve 1 si atendio la llamada, 0 = usar el camino directo.
 */
static int ca_gen_pooled(int kind, uint32_t op, GLsizei n, GLuint *out) {
    sc_ca_ctx *s = t_ca;
    if (!s || !out || n <= 0 || n > 32) return 0;
    if (s->pool_n[kind] < n) {
        GLuint tmp[SC_CA_POOL];
        memset(tmp, 0, sizeof tmp);
        gen_ids(op, SC_CA_POOL, tmp);
        int c = 0;
        while (c < SC_CA_POOL && tmp[c]) c++;
        if (c == 0) return 0;                       /* fallo: camino directo */
        if (s->pool_n[kind] + c > SC_CA_POOL) c = SC_CA_POOL - s->pool_n[kind];
        memcpy(s->pool[kind] + s->pool_n[kind], tmp, (size_t)c * sizeof(GLuint));
        s->pool_n[kind] += c;
        if (s->pool_n[kind] < n) return 0;
    }
    memcpy(out, s->pool[kind], (size_t)n * sizeof(GLuint));
    s->pool_n[kind] -= n;
    memmove(s->pool[kind], s->pool[kind] + n, (size_t)s->pool_n[kind] * sizeof(GLuint));
    return 1;
}

#endif /* SC_CLIENTARR_H */
