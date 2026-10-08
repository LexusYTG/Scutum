/*
 * sc_core.c — núcleo del shim Scutum (glibc, lado container).
 *
 * Responsabilidades:
 *   - conexión lazy al daemon (una por hilo, vía pthread_key)
 *   - handshake HELLO/HELLO_ACK por socket: versión de protocolo y recepción
 *     del memfd con la región compartida (ver sc_shm.h)
 *   - framing: el socket solo se usa en el handshake y para pasar fds
 *     (SC_OP_FD); todo comando/respuesta viaja por los rings en memoria
 *     compartida, sin syscalls en el camino rápido
 *   - batching: acumula instrucciones GL void en un buffer thread-local
 *   - emisión: helpers para agregar argumentos tipados al buffer activo
 *   - sync: ejecuta una instrucción con respuesta
 *
 * Notas de diseño:
 *   - Cada pthread tiene su propio socket, estado y buffer. No hay mutex.
 *   - El buffer es uno solo por hilo. Alterna entre modo BATCH y modo SYNC.
 *     sc_batch_begin flushea si había un batch pendiente.
 *   - El flush real (sendmsg) ocurre en sc_batch_end si el buffer superó
 *     SC_BATCH_MAX, o en sc_sync_begin siempre (para no mezclar batch y sync
 *     en el mismo mensaje), o en sc_batch_flush explícito.
 *   - Los errores se propagan como -errno. La app no los ve: cada función
 *     GL del shim devuelve void (o el valor pedido) y acumula el error en el
 *     slot thread-local t->last_gl_error, que se entrega en glGetError.
 */
#define _GNU_SOURCE
#include "scutum.h"
#include "sc_core.h"
#include "sc_shm.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

/* ============================================================ estado */

enum sc_mode { SC_MODE_IDLE = 0, SC_MODE_BATCH, SC_MODE_SYNC };

struct sc_thread {
    int      fd;               /* -1 = sin conexión */
    uint32_t next_req;
    int      hello_done;

    /* buffer de emisión (batch o sync, según mode) */
    uint8_t *tx;
    size_t   tx_len, tx_cap;

    /* estado del batch en curso */
    int      mode;
    uint32_t n_inst;
    size_t   inst_start;       /* offset del opcode de la instrucción abierta */
    int      in_inst;

    /* buffer de recepción para sync */
    uint8_t *rx;
    size_t   rx_len, rx_cap;
    uint32_t rx_msg_len;

    /* fd a anexar (SCM_RIGHTS) al proximo sync */
    int      has_tx_fd, tx_fd;

    /* región compartida (un par de rings por conexión) */
    uint8_t *shm;
    size_t   shm_len;
    struct sc_ring txr;        /* req: shim -> daemon (productor) */
    struct sc_ring rxr;        /* rsp: daemon -> shim (consumidor) */
    struct sc_wait wtx, wrx;

    /* error GL acumulado */
    uint32_t last_gl_error;
};

static pthread_key_t  g_key;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

/* Cierra socket y desmapea la región. El próximo uso reconecta. */
static void conn_drop(struct sc_thread *t) {
    if (t->shm) { munmap(t->shm, t->shm_len); t->shm = NULL; t->shm_len = 0; }
    if (t->fd >= 0) { close(t->fd); t->fd = -1; }
    t->hello_done = 0;
}

static void thread_free(void *p) {
    struct sc_thread *t = p;
    if (!t) return;
    conn_drop(t);
    free(t->tx);
    free(t->rx);
    free(t);
}

static void make_key(void) { pthread_key_create(&g_key, thread_free); }

static struct sc_thread *self(void) {
    pthread_once(&g_once, make_key);
    struct sc_thread *t = pthread_getspecific(g_key);
    if (!t) {
        t = calloc(1, sizeof *t);
        if (!t) return NULL;
        t->fd = -1;
        t->next_req = 1;
        t->mode = SC_MODE_IDLE;
        pthread_setspecific(g_key, t);
    }
    return t;
}

/* ============================================================ socket */

const char *sc_sock_path(void) {
    const char *p = getenv("SCUTUM_SOCK");
    return (p && *p) ? p : SC_SOCK_PATH;
}

int sc_connect(const char *path, int timeout_ms) {
    if (!path || strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path))
        return -ENAMETOOLONG;

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -errno;

    if (timeout_ms > 0) {
        struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, path);

    int rc;
    do { rc = connect(fd, (struct sockaddr *)&addr, sizeof addr); }
    while (rc < 0 && errno == EINTR);
    if (rc < 0) { int e = errno; close(fd); return -e; }
    return fd;
}

int sc_write_all(int fd, const void *buf, size_t n) {
    const uint8_t *p = buf;
    while (n) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return -ETIMEDOUT;
            return -errno;
        }
        p += w; n -= (size_t)w;
    }
    return 0;
}

int sc_read_all(int fd, void *buf, size_t n) {
    uint8_t *p = buf;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return -ETIMEDOUT;
            return -errno;
        }
        if (r == 0) return -ECONNRESET;
        p += r; n -= (size_t)r;
    }
    return 0;
}

int sc_send_msg_fds(int fd, uint32_t op, uint32_t req, uint32_t thread_id,
                    const void *payload, uint32_t len,
                    const int *fds, uint32_t n_fds)
{
    if (len > SC_MAX_PAYLOAD) return -EMSGSIZE;
    if (len && !payload) return -EINVAL;
    if (n_fds > SC_MAX_FDS) return -EINVAL;

    struct sc_msg h = { SC_MAGIC, op, len, req, thread_id, 0 };

    if (n_fds == 0) {
        int rc = sc_write_all(fd, &h, sizeof h);
        if (rc == 0 && len) rc = sc_write_all(fd, payload, len);
        return rc;
    }

    struct iovec iov[2];
    iov[0].iov_base = &h;                     iov[0].iov_len = sizeof h;
    iov[1].iov_base = (void *)payload;        iov[1].iov_len = len;

    char cmsgbuf[CMSG_SPACE(SC_MAX_FDS * sizeof(int))];
    struct msghdr mh;
    memset(&mh, 0, sizeof mh);
    mh.msg_iov = iov;
    mh.msg_iovlen = len ? 2 : 1;
    mh.msg_control = cmsgbuf;
    mh.msg_controllen = CMSG_SPACE(n_fds * sizeof(int));

    struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type  = SCM_RIGHTS;
    cm->cmsg_len   = CMSG_LEN(n_fds * sizeof(int));
    memcpy(CMSG_DATA(cm), fds, n_fds * sizeof(int));

    /* Con SCM_RIGHTS no podemos aceptar envío parcial: un solo sendmsg o error. */
    ssize_t w;
    do { w = sendmsg(fd, &mh, MSG_NOSIGNAL); } while (w < 0 && errno == EINTR);
    if (w < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? -ETIMEDOUT : -errno;
    if ((size_t)w != sizeof h + len) return -EIO;
    return 0;
}

int sc_recv_msg_fds(int fd, struct sc_msg *h, void *payload, uint32_t cap,
                    int *fds_out, uint32_t *n_fds_io)
{
    /* Fase 1: header. MSG_WAITALL garantiza los 24 bytes exactos.
     * Los fds, si vienen, se anexan a este primer recvmsg. */
    struct iovec iov = { h, sizeof *h };
    char cmsgbuf[CMSG_SPACE(SC_MAX_FDS * sizeof(int))];
    struct msghdr mh;
    memset(&mh, 0, sizeof mh);
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    if (fds_out) {
        mh.msg_control = cmsgbuf;
        mh.msg_controllen = sizeof cmsgbuf;
    }

    ssize_t r;
    do { r = recvmsg(fd, &mh, MSG_WAITALL); } while (r < 0 && errno == EINTR);
    if (r == 0) return -ECONNRESET;
    if (r < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? -ETIMEDOUT : -errno;
    if ((size_t)r != sizeof *h) return -EPROTO;

    if (fds_out && n_fds_io) {
        uint32_t want = *n_fds_io, got = 0;
        for (struct cmsghdr *cm = CMSG_FIRSTHDR(&mh); cm; cm = CMSG_NXTHDR(&mh, cm)) {
            if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS) {
                size_t n = (cm->cmsg_len - CMSG_LEN(0)) / sizeof(int);
                if (n > want) n = want;
                memcpy(fds_out, CMSG_DATA(cm), n * sizeof(int));
                got = (uint32_t)n;
                /* fds extra que no caben: cerrar para no filtrarlos */
                for (size_t k = n; k < (cm->cmsg_len - CMSG_LEN(0)) / sizeof(int); k++)
                    close(((const int *)CMSG_DATA(cm))[k]);
            }
        }
        *n_fds_io = got;
    }

    if (h->magic != SC_MAGIC) return -EPROTO;
    if (h->len > SC_MAX_PAYLOAD) return -EPROTO;
    if (h->len > cap) return -EMSGSIZE;
    if (h->len) {
        int rc = sc_read_all(fd, payload, h->len);
        if (rc) return rc;
    }
    return 0;
}

/* ============================================================ conexión */

/* Lazy: la primera vez que se necesita, conecta y hace HELLO. */
static int ensure_conn(struct sc_thread *t) {
    int shm_fd = -1;
    if (t->fd >= 0 && t->hello_done) return 0;

    if (t->fd < 0) {
        int fd = sc_connect(sc_sock_path(), 2000);
        if (fd < 0) return fd;
        t->fd = fd;
        t->hello_done = 0;
    }

    uint32_t ver = SC_PROTO_VERSION;
    uint32_t req = t->next_req++;
    int rc = sc_send_msg_fds(t->fd, SC_OP_HELLO, req, 0, &ver, sizeof ver, NULL, 0);
    if (rc) goto fail;

    struct sc_msg h;
    uint8_t buf[32];
    {
        int fds[1]; uint32_t nf = 1;
        rc = sc_recv_msg_fds(t->fd, &h, buf, sizeof buf, fds, &nf);
        if (rc) goto fail;
        if (nf == 1) shm_fd = fds[0];
    }
    if (h.op == SC_OP_ERROR) { rc = -EPROTO; goto fail; }
    if (h.op != SC_OP_HELLO_ACK || h.len < 16) { rc = -EPROTO; goto fail; }
    uint32_t srv_ver, srv_caps, shm_total, ring_size;
    memcpy(&srv_ver,   buf,      4);
    memcpy(&srv_caps,  buf + 4,  4);
    memcpy(&shm_total, buf + 8,  4);
    memcpy(&ring_size, buf + 12, 4);
    if (srv_ver != SC_PROTO_VERSION) { rc = -EPROTO; goto fail; }
    if (!(srv_caps & SC_CAP_SHM) || shm_fd < 0) { rc = -EPROTO; goto fail; }
    if (ring_size < 4096 || (ring_size & (ring_size - 1)) ||
        shm_total != sc_shm_total(ring_size)) { rc = -EPROTO; goto fail; }

    {
        void *m = mmap(NULL, shm_total, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
        close(shm_fd); shm_fd = -1;
        if (m == MAP_FAILED) { rc = -errno; goto fail; }
        struct sc_shm_ctrl *ctl = m;
        if (ctl->magic != SC_SHM_MAGIC || ctl->ring_size != ring_size) {
            munmap(m, shm_total); rc = -EPROTO; goto fail;
        }
        t->shm = m; t->shm_len = shm_total;
        sc_ring_init(&t->txr, &ctl->req, (uint8_t *)m + SC_SHM_CTRL_SZ, ring_size);
        sc_ring_init(&t->rxr, &ctl->rsp, (uint8_t *)m + SC_SHM_CTRL_SZ + ring_size, ring_size);
        /* espera de respuesta: spin corto y luego futex. SCUTUM_SPIN_US=0 => nunca duerme. */
        uint64_t spin = sc_env_us_to_ns("SCUTUM_SPIN_US", 300);
        t->wtx.alive_fd = t->fd; t->wtx.spin_ns = spin; t->wtx.timeout_ns = 60ull * 1000000000ull;
        t->wrx = t->wtx;
    }
    t->hello_done = 1;
    {   /* El timeout de 2 s era para connect/HELLO. Operaciones GL reales (link, compile,
         * uploads grandes en Mali) pueden tardar mas; un timeout cierra la conexion y el
         * daemon pierde el contexto EGL actual de este hilo. */
        struct timeval tv = { 60, 0 };
        setsockopt(t->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(t->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    }
    return 0;

fail:
    if (shm_fd >= 0) close(shm_fd);
    conn_drop(t);
    return rc;
}

/* Publica un frame en el ring req. Si hay fd, primero lo manda por el socket
 * (SC_OP_FD) para que el daemon lo tenga esperando cuando lea el frame. */
static int shm_send(struct sc_thread *t, uint32_t op, const void *payload,
                    uint32_t len, int fd)
{
    if (len > SC_MAX_PAYLOAD) return -EMSGSIZE;
    uint32_t req = t->next_req++;
    struct sc_msg h = { SC_MAGIC, op, len, req, 0, fd >= 0 ? SC_MSGF_FD : 0u };
    if (fd >= 0) {
        int rc = sc_send_msg_fds(t->fd, SC_OP_FD, req, 0, NULL, 0, &fd, 1);
        if (rc) return rc;
    }
    return sc_ring_send(&t->txr, &h, payload, len, &t->wtx);
}

/* ============================================================ batching */

static int tx_reserve(struct sc_thread *t, size_t extra) {
    if (t->tx_cap >= t->tx_len + extra) return 0;
    size_t nc = t->tx_cap ? t->tx_cap : 4096;
    while (nc < t->tx_len + extra) nc *= 2;
    uint8_t *np = realloc(t->tx, nc);
    if (!np) return -ENOMEM;
    t->tx = np;
    t->tx_cap = nc;
    return 0;
}

static int tx_append(struct sc_thread *t, const void *d, size_t n) {
    if (tx_reserve(t, n)) return -ENOMEM;
    memcpy(t->tx + t->tx_len, d, n);
    t->tx_len += n;
    return 0;
}

static int tx_append_u32(struct sc_thread *t, uint32_t v) { return tx_append(t, &v, 4); }
static int tx_append_u64(struct sc_thread *t, uint64_t v) { return tx_append(t, &v, 8); }
static int tx_append_f32(struct sc_thread *t, float v)    { return tx_append(t, &v, 4); }

static void tx_patch_u32(struct sc_thread *t, size_t off, uint32_t v) {
    memcpy(t->tx + off, &v, 4);
}


/* ============================================================ SC_STATS (perfilador) */
#include <time.h>
struct st_ent { uint32_t op; uint64_t n, ns, bytes; };
static struct st_ent g_st[128];
static int g_st_n;
static uint64_t g_st_batches, g_st_batch_bytes, g_st_batch_ns;
static pthread_mutex_t g_st_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_st_on = -1;
static inline int st_on(void) {
    if (g_st_on < 0) { const char *e = getenv("SC_STATS"); g_st_on = (e && *e == '1'); }
    return g_st_on;
}
static inline uint64_t st_now(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}
static void st_add(uint32_t op, uint64_t ns, uint64_t bytes) {
    pthread_mutex_lock(&g_st_mu);
    int i; for (i = 0; i < g_st_n; i++) if (g_st[i].op == op) break;
    if (i == g_st_n && g_st_n < 128) { g_st[g_st_n].op = op; g_st[g_st_n].n = g_st[g_st_n].ns = g_st[g_st_n].bytes = 0; g_st_n++; }
    if (i < g_st_n) { g_st[i].n++; g_st[i].ns += ns; g_st[i].bytes += bytes; }
    pthread_mutex_unlock(&g_st_mu);
}
/* Llamar una vez por frame (eglSwapBuffers). swap_ns = tiempo total del swap. */
void sc_stats_frame(uint64_t swap_ns) {
    if (!st_on()) return;
    static unsigned frames; static uint64_t acc_swap, t_start;
    if (!t_start) t_start = st_now();
    frames++; acc_swap += swap_ns;
    if (frames < 30) return;
    uint64_t wall = st_now() - t_start;
    pthread_mutex_lock(&g_st_mu);
    fprintf(stderr, "[sc-stats] %u frames en %.0f ms (%.1f fps) | swap medio %.1f ms | batches/frame %.1f (%.0f KB/frame, envio %.1f ms/frame)\n",
            frames, wall/1e6, frames * 1e9 / (double)wall, acc_swap/1e6/frames,
            (double)g_st_batches/frames, g_st_batch_bytes/1024.0/frames, g_st_batch_ns/1e6/frames);
    /* top 12 por tiempo */
    for (int k = 0; k < 12; k++) {
        int b = -1;
        for (int i = 0; i < g_st_n; i++) if (g_st[i].n && (b < 0 || g_st[i].ns > g_st[b].ns)) b = i;
        if (b < 0) break;
        fprintf(stderr, "[sc-stats]   op=0x%08x  %6.1f llamadas/frame  %7.2f ms/frame  %7.1f KB/frame\n",
                g_st[b].op, (double)g_st[b].n/frames, g_st[b].ns/1e6/frames, g_st[b].bytes/1024.0/frames);
        g_st[b].n = 0; g_st[b].ns = 0; g_st[b].bytes = 0;
    }
    for (int i = 0; i < g_st_n; i++) g_st[i].n = g_st[i].ns = g_st[i].bytes = 0;
    g_st_batches = g_st_batch_bytes = g_st_batch_ns = 0;
    pthread_mutex_unlock(&g_st_mu);
    frames = 0; acc_swap = 0; t_start = st_now();
}
uint64_t sc_stats_now(void) { return st_on() ? st_now() : 0; }

/* Envía el contenido actual del tx como SC_OP_GL_BATCH. */
static int flush_batch(struct sc_thread *t) {
    if (t->mode != SC_MODE_BATCH || t->tx_len == 0) return 0;
    /* El primer u32 del buffer ya es n_inst. */
    int rc = ensure_conn(t);
    uint64_t t0 = st_on() ? st_now() : 0;
    if (rc == 0)
        rc = shm_send(t, SC_OP_GL_BATCH, t->tx, (uint32_t)t->tx_len, -1);
    if (t0) { pthread_mutex_lock(&g_st_mu); g_st_batches++; g_st_batch_bytes += t->tx_len; g_st_batch_ns += st_now() - t0; pthread_mutex_unlock(&g_st_mu); }
    if (rc) {
        fprintf(stderr, "[sc-core] flush_batch FALLO rc=%d errno=%d -> conexion cerrada\n", rc, errno);
        conn_drop(t);
        t->last_gl_error = 0x0505;  /* GL_OUT_OF_MEMORY como proxy */
    }
    t->tx_len = 0;
    t->n_inst = 0;
    t->mode = SC_MODE_IDLE;
    t->in_inst = 0;
    return rc;
}

int sc_batch_flush(void) { return flush_batch(self()); }

int sc_batch_begin(uint32_t op) {
    struct sc_thread *t = self();
    if (!t) return -ENOMEM;
    if (t->mode == SC_MODE_SYNC) return -EINVAL;
    if (t->in_inst) return -EINVAL;

    if (t->mode != SC_MODE_BATCH) {
        t->tx_len = 0;
        t->n_inst = 0;
        if (tx_append_u32(t, 0)) return -ENOMEM;  /* placeholder n_inst */
        t->mode = SC_MODE_BATCH;
    }
    t->inst_start = t->tx_len;
    if (tx_append_u32(t, op)) return -ENOMEM;
    if (tx_append_u32(t, 0)) return -ENOMEM;      /* placeholder argbytes */
    t->in_inst = 1;
    return 0;
}

int sc_batch_end(void) {
    struct sc_thread *t = self();
    if (!t || !t->in_inst) return -EINVAL;
    uint32_t body = (uint32_t)(t->tx_len - t->inst_start - 8);
    tx_patch_u32(t, t->inst_start + 4, body);
    t->n_inst++;
    tx_patch_u32(t, 0, t->n_inst);
    t->in_inst = 0;

    if (t->tx_len >= SC_BATCH_MAX) {
        int rc = flush_batch(t);
        if (rc) return rc;
    }
    return 0;
}

/* ============================================================ emit */

/* Los emit escriben al buffer activo (batch o sync). En modo IDLE fallan. */
int sc_emit_u32(uint32_t v) { struct sc_thread *t = self(); return t ? tx_append_u32(t, v) : -ENOMEM; }
int sc_emit_i32(int32_t  v) { struct sc_thread *t = self(); return t ? tx_append_u32(t, (uint32_t)v) : -ENOMEM; }
int sc_emit_u64(uint64_t v) { struct sc_thread *t = self(); return t ? tx_append_u64(t, v) : -ENOMEM; }
int sc_emit_f32(float    v) { struct sc_thread *t = self(); return t ? tx_append_f32(t, v) : -ENOMEM; }

int sc_emit_bytes(const void *p, size_t n) {
    struct sc_thread *t = self();
    if (!t) return -ENOMEM;
    uint32_t n32 = (uint32_t)n;
    if (tx_append_u32(t, n32)) return -ENOMEM;
    if (n == 0) return 0;
    if (!p) return -EINVAL;
    return tx_append(t, p, n);
}

/* Emite [u32 len][bytes sin NUL]. NULL -> len=0. */
int sc_emit_string(const char *s) {
    if (!s) return sc_emit_bytes(NULL, 0);
    return sc_emit_bytes(s, strlen(s));
}

/* ============================================================ sync */

int sc_sync_begin(uint32_t op) {
    struct sc_thread *t = self();
    if (!t) return -ENOMEM;
    if (t->in_inst) return -EINVAL;

    if (t->mode == SC_MODE_BATCH) {
        int rc = flush_batch(t);
        if (rc) return rc;
    }
    t->tx_len = 0;
    t->mode = SC_MODE_SYNC;
    return tx_append_u32(t, op);
}

/* Envía el request y trae la respuesta. Al volver, t->rx contiene
 * [i32 result][datos...] y t->rx_msg_len = h.len. */
static int sc_sync_send_impl(struct sc_thread *t);
int sc_sync_send(void) {
    struct sc_thread *t = self();
    if (!t || t->mode != SC_MODE_SYNC) return -EINVAL;
    if (!st_on()) return sc_sync_send_impl(t);
    uint32_t op = 0; if (t->tx_len >= 4) memcpy(&op, t->tx, 4);
    uint64_t t0 = st_now();
    int rc = sc_sync_send_impl(t);
    st_add(op, st_now() - t0, (uint64_t)t->tx_len + t->rx_len);
    return rc;
}
static int sc_sync_send_impl(struct sc_thread *t) {

    int rc = ensure_conn(t);
    if (rc) goto fail;
    {
        int fdv = t->tx_fd, hf = t->has_tx_fd;
        t->has_tx_fd = 0;
        rc = shm_send(t, SC_OP_GL_SYNC, t->tx, (uint32_t)t->tx_len, hf ? fdv : -1);
    }
    if (rc) goto fail;

    /* respuesta por el ring rsp (spin y luego futex); siempre copia a t->rx */
    struct sc_msg h;
    const uint8_t *rp; uint64_t rel;
    rc = sc_ring_recv(&t->rxr, &h, &rp, &rel, &t->rx, &t->rx_cap, 0,
                      SC_MAX_PAYLOAD, &t->wrx);
    if (rc) goto fail;

    if (h.op == SC_OP_ERROR) {
        /* El payload ya quedó copiado en t->rx y el ring sigue sincronizado.
         * NO cerrar la conexion: el daemon perderia el contexto EGL actual del hilo. */
        uint8_t ebuf[256]; memset(ebuf, 0, sizeof ebuf);
        size_t take = h.len < sizeof ebuf ? h.len : sizeof ebuf;
        if (take && t->rx) memcpy(ebuf, t->rx, take);
        fprintf(stderr, "[sc-core] daemon respondio SC_OP_ERROR len=%u first8=%02x%02x%02x%02x%02x%02x%02x%02x (conexion se mantiene)\n",
                (unsigned)h.len, ebuf[0],ebuf[1],ebuf[2],ebuf[3],ebuf[4],ebuf[5],ebuf[6],ebuf[7]);
        t->last_gl_error = 0x0502;  /* GL_INVALID_OPERATION */
        t->rx_len = 0;
        t->mode = SC_MODE_IDLE;
        return -EIO;
    }
    if (h.op != SC_OP_GL_SYNC) { rc = -EPROTO; goto fail; }

    t->rx_len = h.len;
    t->rx_msg_len = h.len;
    t->mode = SC_MODE_IDLE;
    return 0;

fail:
    fprintf(stderr, "[sc-core] sync_send FALLO rc=%d errno=%d -> conexion cerrada, el proximo uso reconecta SIN contexto\n", rc, errno);
    conn_drop(t);
    t->mode = SC_MODE_IDLE;
    t->last_gl_error = 0x0505;
    return rc;
}

/* Anexa 'fd' (SCM_RIGHTS) al proximo sc_sync_send de este hilo. */
void sc_sync_attach_fd(int fd) {
    struct sc_thread *t = self();
    if (t) { t->tx_fd = fd; t->has_tx_fd = 1; }
}

int32_t sc_sync_result(void) {
    struct sc_thread *t = self();
    if (!t || t->rx_len < 4) return (int32_t)0x80000000;
    int32_t r; memcpy(&r, t->rx, 4);
    return r;
}

uint64_t sc_sync_recv_u64(void) {
    struct sc_thread *t = self();
    if (!t || t->rx_len < 12) return 0;
    uint64_t v; memcpy(&v, t->rx + 4, 8);
    return v;
}

void sc_sync_recv_void(void) { (void)self(); }

/* Copia hasta `max` bytes del payload de respuesta al buffer del usuario.
 * Devuelve la cantidad copiada. */
size_t sc_sync_recv_bytes(void *out, size_t max) {
    struct sc_thread *t = self();
    if (!t || !out || t->rx_len < 4) return 0;
    size_t avail = t->rx_len - 4;
    if (avail > max) avail = max;
    memcpy(out, t->rx + 4, avail);
    return avail;
}

uint32_t sc_last_gl_error(void) {
    struct sc_thread *t = self();
    return t ? t->last_gl_error : 0;
}

void sc_set_gl_error(uint32_t e) {
    struct sc_thread *t = self();
    if (t) t->last_gl_error = e;
}

void sc_clear_gl_error(void) {
    struct sc_thread *t = self();
    if (t) t->last_gl_error = 0;
}

/* ============================================================ strings */

/*
 * glGetString/glGetStringi devuelven un const GLubyte* que la app usa como
 * string de vida util indefinida (hasta el proximo cambio de contexto).
 * El daemon manda los bytes; el shim los guarda en un slot thread-local
 * para poder devolver el char* correcto. Guardamos por índice de nombre
 * (GL_VENDOR, GL_RENDERER, ...) porque la app suele llamar varias veces
 * con el mismo name y espera punteros estables.
 */
struct sc_string_slot {
    uint32_t name;      /* GL_VENDOR, GL_RENDERER, ... o 0 para glGetStringi */
    uint32_t index;     /* solo para glGetStringi */
    char    *buf;
};

#define SC_STR_SLOTS 2048

static pthread_key_t  g_str_key;
static pthread_once_t g_str_once = PTHREAD_ONCE_INIT;

static void str_free(void *p) {
    struct sc_string_slot *s = p;
    if (!s) return;
    for (int i = 0; i < SC_STR_SLOTS; i++) free(s[i].buf);
    free(s);
}

static void str_make_key(void) { pthread_key_create(&g_str_key, str_free); }

static struct sc_string_slot *str_slots(void) {
    pthread_once(&g_str_once, str_make_key);
    struct sc_string_slot *s = pthread_getspecific(g_str_key);
    if (!s) {
        s = calloc(SC_STR_SLOTS, sizeof *s);
        if (!s) return NULL;
        pthread_setspecific(g_str_key, s);
    }
    return s;
}

const char *sc_string_intern(const void *bytes, size_t len) {
    return sc_string_intern_slot(bytes, len, 0, 0);
}

/* Variante con slot explícito: name/index identifican la consulta.
 * Si el slot ya tenía una cadena para ese name/index, la reusa. */
const char *sc_string_intern_slot(const void *bytes, size_t len,
                                  uint32_t name, uint32_t index)
{
    struct sc_string_slot *s = str_slots();
    if (!s) return NULL;

    int slot = -1, free_slot = -1;
    for (int i = 0; i < SC_STR_SLOTS; i++) {
        if (s[i].buf && s[i].name == name && s[i].index == index) { slot = i; break; }
        if (!s[i].buf && free_slot < 0) free_slot = i;
    }
    if (slot < 0) slot = free_slot;
    if (slot < 0) {
        /* todos ocupados: pisamos el 0. El free está abajo, una sola vez:
         * antes había un free acá Y otro abajo, mismo puntero → double free. */
        slot = 0;
    }

    char *nb = malloc(len + 1);
    if (!nb) return NULL;
    if (len) memcpy(nb, bytes, len);
    nb[len] = 0;

    free(s[slot].buf);
    s[slot].buf  = nb;
    s[slot].name = name;
    s[slot].index = index;
    return nb;
}
