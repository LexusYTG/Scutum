/*
 * sc_shm.h — bus de comandos por memoria compartida (shim <-> daemon).
 * Header-only. Lo incluyen sc_core.c (glibc, container) y scutumd.c (bionic).
 *
 * ============================================================
 * MODELO
 * ============================================================
 * El socket AF_UNIX queda SOLO para:
 *   - el handshake (HELLO / HELLO_ACK) donde el daemon entrega el memfd,
 *   - pasar fds sueltos (SC_OP_FD, SCM_RIGHTS) cuando una op los necesita,
 *   - detectar que el otro lado murió.
 * Todo el trafico de comandos y respuestas viaja por dos rings SPSC
 * (un productor, un consumidor) dentro de una unica region compartida:
 *
 *   [ ctrl 4 KiB ][ ring req (shim -> daemon) ][ ring rsp (daemon -> shim) ]
 *
 * Con SCUTUM_SHM_MB=16 (default) son 2 rings de 8 MiB = 16 MiB de datos.
 * Se crea una region por conexion (= por pthread del shim). La memoria es
 * esparsa (memfd + ftruncate): solo se commitean las paginas que se tocan.
 *
 * ============================================================
 * RINGS
 * ============================================================
 * Cada ring es un stream de bytes. Los indices head/tail son contadores de
 * 64 bits que solo crecen; la posicion fisica es (pos & mask). Ocupacion =
 * head - tail. Frame = [struct sc_msg 24 B][payload len B][pad a multiplo de 8].
 * El flag SC_MSGF_FD en sc_msg.pad indica que, antes de publicar el frame, el
 * productor mando un SC_OP_FD con SCM_RIGHTS por el socket.
 *
 * Un frame puede ser mas grande que el ring: el productor publica por tramos
 * y el consumidor lo va drenando (camino "streaming"). Si el frame ya esta
 * completo y el payload es contiguo, el consumidor lo parsea EN SITIO, sin
 * copiar, y libera el espacio despues (sc_ring_advance).
 *
 * ============================================================
 * ESPERA (spin + futex)
 * ============================================================
 * El que espera hace spin con 'yield' sin ninguna syscall (20 us; luego
 * sched_yield entre chequeos). Pasado spin_ns marca ring->sleeping=1 y duerme en futex. El productor, tras
 * publicar head, mira sleeping y solo si esta en 1 hace FUTEX_WAKE. En el
 * camino rapido no hay syscalls. spin_ns == 0 => nunca duerme.
 * Mientras espera (spin o futex) revisa el socket cada ~1 ms (spin) o 50 ms
 * (futex) con recv(MSG_PEEK|MSG_DONTWAIT): EOF => el peer murio.
 */
#ifndef SC_SHM_H
#define SC_SHM_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <sched.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <linux/futex.h>

#include "scutum.h"

#define SC_SHM_MAGIC     0x314D4853u          /* "SHM1" */
#define SC_SHM_CTRL_SZ   4096u
#define SC_SHM_DEF_MB    16u

/* Los primeros 20 us de espera son spin puro (cero syscalls). Pasado eso, cada
 * vuelta de chequeo cede la CPU con sched_yield(): con pocos nucleos, o si los
 * dos procesos caen en el mismo, evita que el que espera ocupe el timeslice
 * del que tiene que producir. */
#define SC_SPIN_PURE_DEF_NS 20000ull

/* ------------------------------------------------------------ layout */

struct sc_ring_ctl {
    _Alignas(64) _Atomic uint64_t head;       /* lo escribe el productor */
    _Alignas(64) _Atomic uint64_t tail;       /* lo escribe el consumidor */
    _Alignas(64) _Atomic uint32_t sleeping;   /* 1 = consumidor dormido en futex */
    uint8_t _pad[60];
};

struct sc_shm_ctrl {
    uint32_t magic;
    uint32_t version;
    uint32_t ring_size;                       /* bytes por ring, potencia de 2 */
    uint32_t reserved;
    uint8_t  _pad[48];
    struct sc_ring_ctl req;                   /* shim   -> daemon */
    struct sc_ring_ctl rsp;                   /* daemon -> shim   */
};

_Static_assert(sizeof(struct sc_shm_ctrl) <= SC_SHM_CTRL_SZ, "ctrl no entra en una pagina");

static inline size_t sc_shm_total(uint32_t ring_size) {
    return (size_t)SC_SHM_CTRL_SZ + 2u * (size_t)ring_size;
}

/* Vista local de un ring (una por lado). */
struct sc_ring {
    struct sc_ring_ctl *c;
    uint8_t  *data;
    uint64_t  size, mask;
    uint64_t  lpos;     /* mi indice: head si soy productor, tail si soy consumidor */
    uint64_t  ppub;     /* productor: ultimo head publicado */
    uint64_t  peer;     /* productor: cache del tail del consumidor */
};

/* Parametros de espera. spin_ns==0: nunca duerme. timeout_ns==0: sin timeout. */
struct sc_wait {
    int      alive_fd;
    uint64_t spin_ns;
    uint64_t timeout_ns;
};

static inline void sc_ring_init(struct sc_ring *r, struct sc_ring_ctl *c,
                                uint8_t *data, uint64_t size)
{
    memset(r, 0, sizeof *r);
    r->c = c; r->data = data; r->size = size; r->mask = size - 1;
}

/* ------------------------------------------------------------ utilidades */

/* Duracion del spin puro. Si este proceso solo puede correr en UNA cpu, el spin
 * es contraproducente (el productor no puede correr mientras yo giro): 0 =>
 * se cede la CPU desde el primer chequeo. */
static inline uint64_t sc_spin_pure_ns(void) {
    static int cached = 0;
    static uint64_t v = SC_SPIN_PURE_DEF_NS;
    if (!cached) {
        cpu_set_t cs;
        if (sched_getaffinity(0, sizeof cs, &cs) == 0 && CPU_COUNT(&cs) <= 1) v = 0;
        cached = 1;
    }
    return v;
}
#define SC_SPIN_PURE_NS  sc_spin_pure_ns()

static inline void sc_cpu_relax(void) {
#if defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("pause" ::: "memory");
#else
    atomic_signal_fence(memory_order_seq_cst);
#endif
}

static inline uint64_t sc_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* futex entre procesos: SIN FUTEX_PRIVATE_FLAG (la memoria es MAP_SHARED). */
static inline long sc_futex(void *addr, int op, uint32_t val, const struct timespec *to) {
    return syscall(SYS_futex, addr, op, val, to, NULL, 0);
}

/* Lee una variable de entorno en microsegundos y la devuelve en ns.
 * Si no existe o es invalida devuelve def_us. Un 0 explicito vale 0. */
static inline uint64_t sc_env_us_to_ns(const char *name, uint64_t def_us) {
    const char *e = getenv(name);
    if (!e || !*e) return def_us * 1000ull;
    char *end = NULL;
    unsigned long long v = strtoull(e, &end, 10);
    if (end == e) return def_us * 1000ull;
    return (uint64_t)v * 1000ull;
}

/* 1 si el otro extremo del socket sigue vivo (o fd < 0). */
static inline int sc_peer_alive(int fd) {
    if (fd < 0) return 1;
    char b; ssize_t r;
    do { r = recv(fd, &b, 1, MSG_PEEK | MSG_DONTWAIT); } while (r < 0 && errno == EINTR);
    if (r == 0) return 0;
    if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return 0;
    return 1;
}

/* ------------------------------------------------------------ copia con wrap */

static inline void sc_ring_copy_in(const struct sc_ring *r, uint64_t pos,
                                   const void *src, uint64_t n)
{
    uint64_t off = pos & r->mask, first = r->size - off;
    if (first > n) first = n;
    memcpy(r->data + off, src, first);
    if (n > first) memcpy(r->data, (const uint8_t *)src + first, n - first);
}

static inline void sc_ring_copy_out(const struct sc_ring *r, uint64_t pos,
                                    void *dst, uint64_t n)
{
    uint64_t off = pos & r->mask, first = r->size - off;
    if (first > n) first = n;
    memcpy(dst, r->data + off, first);
    if (n > first) memcpy((uint8_t *)dst + first, r->data, n - first);
}

/* ------------------------------------------------------------ productor */

/* Publica todo lo escrito hasta ahora y despierta al consumidor si duerme.
 * seq_cst en ambos lados del par (store head / load sleeping) para el patron
 * Dekker: el consumidor hace store sleeping / load head. */
static inline void sc_ring_publish(struct sc_ring *r) {
    r->ppub = r->lpos;
    atomic_store_explicit(&r->c->head, r->lpos, memory_order_seq_cst);
    if (atomic_load_explicit(&r->c->sleeping, memory_order_seq_cst)) {
        atomic_store_explicit(&r->c->sleeping, 0, memory_order_seq_cst);
        sc_futex(&r->c->sleeping, FUTEX_WAKE, 1, NULL);
    }
}

/* Espera hasta que haya al menos 1 byte libre. 0 o -errno. */
static inline int sc_ring_wait_space(struct sc_ring *r, const struct sc_wait *w) {
    uint64_t t0 = sc_now_ns(), last = t0;
    unsigned it = 0;
    for (;;) {
        r->peer = atomic_load_explicit(&r->c->tail, memory_order_acquire);
        if (r->size - (r->lpos - r->peer) > 0) return 0;
        sc_cpu_relax();
        if ((++it & 127u) == 0) {
            uint64_t now = sc_now_ns();
            if (w->timeout_ns && now - t0 >= w->timeout_ns) return -ETIMEDOUT;
            if (now - last >= 1000000ull) {
                last = now;
                if (!sc_peer_alive(w->alive_fd)) return -ECONNRESET;
            }
            if (w->spin_ns && now - t0 >= w->spin_ns) {
                struct timespec ts = { 0, 50000 };       /* 50 us */
                nanosleep(&ts, NULL);
            } else if (now - t0 >= SC_SPIN_PURE_NS) {
                sched_yield();
            }
        }
    }
}

/* Envia un frame [hdr][p1 n1][p2 n2][pad]. n1+n2 debe ser h->len. */
static inline int sc_ring_send2(struct sc_ring *r, const struct sc_msg *h,
                                const void *p1, uint32_t n1,
                                const void *p2, uint32_t n2,
                                const struct sc_wait *w)
{
    uint64_t len = (uint64_t)n1 + n2;
    uint64_t pad = (((len + 7) & ~7ull) - len);
    const uint8_t *sp[4] = { (const uint8_t *)h, (const uint8_t *)p1, (const uint8_t *)p2, NULL };
    uint64_t       sn[4] = { sizeof *h, n1, n2, pad };

    for (int s = 0; s < 4; s++) {
        const uint8_t *p = sp[s];
        uint64_t n = sn[s];
        while (n) {
            uint64_t freeb = r->size - (r->lpos - r->peer);
            if (freeb == 0) {
                r->peer = atomic_load_explicit(&r->c->tail, memory_order_acquire);
                freeb = r->size - (r->lpos - r->peer);
                if (freeb == 0) {
                    if (r->ppub != r->lpos) sc_ring_publish(r);   /* que el consumidor drene */
                    int rc = sc_ring_wait_space(r, w);
                    if (rc) return rc;
                    freeb = r->size - (r->lpos - r->peer);
                }
            }
            uint64_t k = n < freeb ? n : freeb;
            if (p) { sc_ring_copy_in(r, r->lpos, p, k); p += k; }
            r->lpos += k;
            n -= k;
        }
    }
    sc_ring_publish(r);
    return 0;
}

static inline int sc_ring_send(struct sc_ring *r, const struct sc_msg *h,
                               const void *payload, uint32_t len,
                               const struct sc_wait *w)
{
    return sc_ring_send2(r, h, payload, len, NULL, 0, w);
}

/* ------------------------------------------------------------ consumidor */

/* Libera n bytes (ya consumidos) hacia el productor. */
static inline void sc_ring_advance(struct sc_ring *r, uint64_t n) {
    r->lpos += n;
    atomic_store_explicit(&r->c->tail, r->lpos, memory_order_release);
}

/* Espera >= need bytes publicados. Devuelve los bytes disponibles o -errno. */
static inline int64_t sc_ring_wait_avail(struct sc_ring *r, uint64_t need,
                                         const struct sc_wait *w)
{
    uint64_t head = atomic_load_explicit(&r->c->head, memory_order_acquire);
    if (head - r->lpos >= need) return (int64_t)(head - r->lpos);

    uint64_t t0 = sc_now_ns(), last = t0;
    unsigned it = 0;

    /* fase 1: spin puro, cero syscalls */
    for (;;) {
        head = atomic_load_explicit(&r->c->head, memory_order_acquire);
        if (head - r->lpos >= need) return (int64_t)(head - r->lpos);
        sc_cpu_relax();
        if ((++it & 127u) == 0) {
            uint64_t now = sc_now_ns();
            if (w->timeout_ns && now - t0 >= w->timeout_ns) return -ETIMEDOUT;
            if (now - last >= 1000000ull) {
                last = now;
                if (!sc_peer_alive(w->alive_fd)) return -ECONNRESET;
            }
            if (w->spin_ns && now - t0 >= w->spin_ns) break;
            if (now - t0 >= SC_SPIN_PURE_NS) sched_yield();
        }
    }

    /* fase 2: dormir en futex hasta que el productor publique */
    for (;;) {
        atomic_store_explicit(&r->c->sleeping, 1, memory_order_seq_cst);
        head = atomic_load_explicit(&r->c->head, memory_order_seq_cst);
        if (head - r->lpos >= need) {
            atomic_store_explicit(&r->c->sleeping, 0, memory_order_relaxed);
            return (int64_t)(head - r->lpos);
        }
        struct timespec ts = { 0, 50 * 1000 * 1000 };       /* 50 ms: chequeo de vida */
        sc_futex(&r->c->sleeping, FUTEX_WAIT, 1, &ts);
        atomic_store_explicit(&r->c->sleeping, 0, memory_order_relaxed);

        head = atomic_load_explicit(&r->c->head, memory_order_acquire);
        if (head - r->lpos >= need) return (int64_t)(head - r->lpos);
        uint64_t now = sc_now_ns();
        if (w->timeout_ns && now - t0 >= w->timeout_ns) return -ETIMEDOUT;
        if (!sc_peer_alive(w->alive_fd)) return -ECONNRESET;
    }
}

static inline int sc_rxbuf_reserve(uint8_t **buf, size_t *cap, uint64_t need) {
    if (*cap >= need) return 0;
    size_t nc = *cap ? *cap : 4096;
    while (nc < need) nc *= 2;
    uint8_t *np = (uint8_t *)realloc(*buf, nc);
    if (!np) return -ENOMEM;
    *buf = np; *cap = nc;
    return 0;
}

/*
 * Lee un frame. Deja el header en *h y el payload en *out (NULL si len==0).
 *   - Si allow_inplace y el frame esta completo y contiguo: *out apunta DENTRO
 *     del ring y *rel = bytes a liberar con sc_ring_advance() cuando se termino
 *     de usar el payload.
 *   - Si no: el payload se copia a *buf (se agranda con realloc) y *rel = 0
 *     (el espacio ya fue liberado).
 */
static inline int sc_ring_recv(struct sc_ring *r, struct sc_msg *h,
                               const uint8_t **out, uint64_t *rel,
                               uint8_t **buf, size_t *cap,
                               int allow_inplace, uint32_t max_payload,
                               const struct sc_wait *w)
{
    *out = NULL; *rel = 0;
    int64_t a = sc_ring_wait_avail(r, sizeof *h, w);
    if (a < 0) return (int)a;
    sc_ring_copy_out(r, r->lpos, h, sizeof *h);
    if (h->magic != SC_MAGIC || h->len > max_payload) return -EPROTO;

    uint64_t len = h->len;
    uint64_t padded = (len + 7) & ~7ull;
    uint64_t total  = sizeof *h + padded;

    if ((uint64_t)a >= total) {                 /* frame completo ya publicado */
        if (len == 0) { sc_ring_advance(r, total); return 0; }
        uint64_t off = (r->lpos + sizeof *h) & r->mask;
        if (allow_inplace && off + len <= r->size) {
            *out = r->data + off;
            *rel = total;
            return 0;
        }
        int rc = sc_rxbuf_reserve(buf, cap, len);
        if (rc) return rc;
        sc_ring_copy_out(r, r->lpos + sizeof *h, *buf, len);
        sc_ring_advance(r, total);
        *out = *buf;
        return 0;
    }

    /* frame parcial (o mayor que lo publicado): streaming */
    if (len) {
        int rc = sc_rxbuf_reserve(buf, cap, len);
        if (rc) return rc;
    }
    sc_ring_advance(r, sizeof *h);
    uint64_t got = 0;
    while (got < len) {
        int64_t av = sc_ring_wait_avail(r, 1, w);
        if (av < 0) return (int)av;
        uint64_t k = (uint64_t)av < len - got ? (uint64_t)av : len - got;
        sc_ring_copy_out(r, r->lpos, *buf + got, k);
        got += k;
        sc_ring_advance(r, k);
    }
    if (padded > len) {
        int64_t av = sc_ring_wait_avail(r, padded - len, w);
        if (av < 0) return (int)av;
        sc_ring_advance(r, padded - len);
    }
    *out = len ? *buf : NULL;
    return 0;
}

#endif /* SC_SHM_H */
