/* test_shm.c — prueba de los rings de sc_shm.h entre dos procesos.
 * Uso: make test_shm && ./test_shm */
#define _GNU_SOURCE
#include <stdio.h>
#include <assert.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include "sc_shm.h"

#define RING 65536u   /* chico a proposito: fuerza wrap y streaming */

static uint64_t rng = 88172645463325252ull;
static uint8_t pat(uint64_t i) { return (uint8_t)((i * 2654435761u) >> 13); }

struct side { void *m; struct sc_ring tx, rx; struct sc_wait w; };

static void side_init(struct side *s, void *m, int is_shim, int sock, uint64_t spin_ns) {
    struct sc_shm_ctrl *c = m; s->m = m;
    uint8_t *d0 = (uint8_t *)m + SC_SHM_CTRL_SZ, *d1 = d0 + RING;
    if (is_shim) { sc_ring_init(&s->tx, &c->req, d0, RING); sc_ring_init(&s->rx, &c->rsp, d1, RING); }
    else         { sc_ring_init(&s->tx, &c->rsp, d1, RING); sc_ring_init(&s->rx, &c->req, d0, RING); }
    s->w.alive_fd = sock; s->w.spin_ns = spin_ns; s->w.timeout_ns = 20ull * 1000000000ull;
}

static void fill(uint8_t *b, uint32_t n, uint32_t seed) { for (uint32_t i = 0; i < n; i++) b[i] = pat(i + seed); }
static int  check(const uint8_t *b, uint32_t n, uint32_t seed) { for (uint32_t i = 0; i < n; i++) if (b[i] != pat(i + seed)) return 0; return 1; }

/* "daemon": hace eco de cada frame (op+1, mismo payload). Un frame con op==99 termina. */
static int daemon_main(void *m, int sock, uint64_t spin_ns) {
    struct side s; side_init(&s, m, 0, sock, spin_ns);
    uint8_t *buf = NULL; size_t cap = 0; long n = 0;
    for (;;) {
        struct sc_msg h; const uint8_t *p; uint64_t rel;
        int rc = sc_ring_recv(&s.rx, &h, &p, &rel, &buf, &cap, 1, SC_MAX_PAYLOAD, &s.w);
        if (rc) { fprintf(stderr, "daemon recv rc=%d\n", rc); return 1; }
        if (h.pad & SC_MSGF_FD) {                       /* token de fd por el socket */
            struct sc_msg th; int fds[2]; uint32_t nf = 2;
            if (sc_recv_msg_fds(sock, &th, NULL, 0, fds, &nf) || th.op != SC_OP_FD || nf != 1) return 2;
            char c; if (pread(fds[0], &c, 1, 0) != 1 || c != 'F') return 3;   /* leer el fd recibido */
            close(fds[0]);
        }
        if (h.op == 99) { if (rel) sc_ring_advance(&s.rx, rel); return 0; }
        if (!check(p ? p : (const uint8_t *)"", h.len, h.req_id)) { fprintf(stderr, "daemon: payload corrupto len=%u\n", h.len); return 4; }
        struct sc_msg r = { SC_MAGIC, h.op + 1, h.len, h.req_id, 0, 0 };
        rc = sc_ring_send(&s.tx, &r, p, h.len, &s.w);
        if (rel) sc_ring_advance(&s.rx, rel);
        if (rc) return 5;
        n++;
    }
}

static int rt(struct side *s, uint32_t op, uint32_t len, uint32_t seed, int fd) {
    uint8_t *tx = malloc(len ? len : 1); fill(tx, len, seed);
    struct sc_msg h = { SC_MAGIC, op, len, seed, 0, fd >= 0 ? SC_MSGF_FD : 0u };
    if (fd >= 0) assert(sc_send_msg_fds(s->w.alive_fd, SC_OP_FD, seed, 0, NULL, 0, &fd, 1) == 0);
    int rc = sc_ring_send(&s->tx, &h, tx, len, &s->w); free(tx);
    if (rc) return rc;
    struct sc_msg r; const uint8_t *p; uint64_t rel; static uint8_t *rb; static size_t rc2;
    rc = sc_ring_recv(&s->rx, &r, &p, &rel, &rb, &rc2, 0, SC_MAX_PAYLOAD, &s->w);
    if (rc) return rc;
    if (r.op != op + 1 || r.len != len || r.req_id != seed) return -1000;
    if (!check(p ? p : (const uint8_t *)"", len, seed)) return -1001;
    return 0;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    for (int mode = 0; mode < 2; mode++) {
        uint64_t spin = mode == 0 ? 200000ull /* 0.2 ms: fuerza futex */ : 0ull /* nunca duerme */;
        int mfd = (int)syscall(SYS_memfd_create, "t", 1u);
        assert(mfd >= 0 && ftruncate(mfd, sc_shm_total(RING)) == 0);
        void *m = mmap(NULL, sc_shm_total(RING), PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
        struct sc_shm_ctrl *c = m; c->magic = SC_SHM_MAGIC; c->version = SC_PROTO_VERSION; c->ring_size = RING;
        int sp[2]; assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sp) == 0);
        pid_t pid = fork();
        if (pid == 0) { close(sp[0]); _exit(daemon_main(m, sp[1], spin)); }
        close(sp[1]);
        struct side s; side_init(&s, m, 1, sp[0], mode == 0 ? 200000ull : 300000ull);
        printf("== modo %s\n", mode == 0 ? "spin 0.2ms + futex" : "daemon sin dormir (spin infinito)");

        /* 1. tamaños variados, incluyendo cruce de wrap y frames > ring (streaming) */
        uint32_t sizes[] = { 0, 1, 7, 8, 9, 100, 4096, 30000, 65000, 65536, 65537, 200000, 1u << 20, 3u << 20 };
        uint32_t seed = 1;
        for (unsigned i = 0; i < sizeof sizes / sizeof *sizes; i++) {
            int rc = rt(&s, 10, sizes[i], seed++, -1);
            printf("   len=%-8u %s\n", sizes[i], rc ? "FALLO" : "ok"); if (rc) { printf("rc=%d\n", rc); return 1; }
        }
        /* 2. muchos frames chicos con tamaños pseudo-aleatorios (wrap constante) */
        for (int i = 0; i < 20000; i++) {
            rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
            int rc = rt(&s, 20, (uint32_t)(rng % 5000), seed++, -1);
            if (rc) { printf("   aleatorio FALLO i=%d rc=%d\n", i, rc); return 1; }
        }
        printf("   20000 frames aleatorios ok\n");
        /* 3. dormir y despertar: pausa > spin para que el daemon duerma en futex */
        usleep(300000);
        assert(rt(&s, 30, 64, seed++, -1) == 0);
        usleep(300000);
        assert(rt(&s, 30, 5000, seed++, -1) == 0);
        printf("   despertar desde futex ok\n");
        /* 4. fd por socket */
        int ffd = (int)syscall(SYS_memfd_create, "f", 1u); assert(write(ffd, "F", 1) == 1);
        assert(rt(&s, 40, 48, seed++, ffd) == 0); close(ffd);
        printf("   fd por socket (SC_OP_FD) ok\n");

        /* 5. latencia round-trip de un frame de 64 B */
        int N = 200000; uint64_t t0 = sc_now_ns();
        for (int i = 0; i < N; i++) if (rt(&s, 50, 64, 7, -1)) { puts("lat FALLO"); return 1; }
        uint64_t dt = sc_now_ns() - t0;
        printf("   round-trip ring: %.0f ns (%.2f M rt/s)\n", (double)dt / N, N / (dt / 1e3));

        struct sc_msg q = { SC_MAGIC, 99, 0, 0, 0, 0 };
        assert(sc_ring_send(&s.tx, &q, NULL, 0, &s.w) == 0);
        int st; waitpid(pid, &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st)) { printf("daemon salio mal: %d\n", st); return 1; }

        /* 6. peer muerto: el que espera debe enterarse por el socket */
        if (mode == 0) {
            int sp2[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sp2) == 0);
            pid_t p2 = fork();
            if (p2 == 0) { close(sp2[0]); usleep(150000); _exit(0); }   /* "shim" muere sin escribir */
            close(sp2[1]);
            int mfd2 = (int)syscall(SYS_memfd_create, "t2", 1u); assert(mfd2 >= 0 && ftruncate(mfd2, sc_shm_total(RING)) == 0);
            void *m2 = mmap(NULL, sc_shm_total(RING), PROT_READ | PROT_WRITE, MAP_SHARED, mfd2, 0);
            struct side d; side_init(&d, m2, 0, sp2[0], 1000000ull);
            struct sc_msg h; const uint8_t *p; uint64_t rel; uint8_t *b = NULL; size_t cp = 0;
            uint64_t a = sc_now_ns();
            int rc = sc_ring_recv(&d.rx, &h, &p, &rel, &b, &cp, 1, SC_MAX_PAYLOAD, &d.w);
            printf("   peer muerto detectado: rc=%d (%s) en %.0f ms\n", rc, rc == -ECONNRESET ? "ECONNRESET ok" : "MAL", (sc_now_ns() - a) / 1e6);
            if (rc != -ECONNRESET) return 1;
            waitpid(p2, NULL, 0);
        }
        munmap(m, sc_shm_total(RING)); close(mfd); close(sp[0]);
    }

    /* baseline: round-trip por socketpair (lo que se usaba) */
    int sp[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0);
    if (fork() == 0) { close(sp[0]); char b[64]; for (;;) { ssize_t n = read(sp[1], b, 64); if (n <= 0) _exit(0); write(sp[1], b, n); } }
    char b[64] = {0}; int N = 100000; uint64_t t0 = sc_now_ns();
    for (int i = 0; i < N; i++) { write(sp[0], b, 64); read(sp[0], b, 64); }
    printf("== baseline socketpair round-trip: %.0f ns\n", (double)(sc_now_ns() - t0) / N);
    close(sp[0]); wait(NULL);
    puts("TODO OK");
    return 0;
}
