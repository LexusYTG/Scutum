#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
/*
 * sc_egl.c — shim de EGL (glibc, lado container).
 *
 * Reemplaza libEGL.so. Cada función serializa su llamada al daemon y espera
 * respuesta. Los handles EGL (Display, Config, Context, Surface, Image,
 * Sync) son u64 opacos de extremo a extremo: el daemon los genera con el
 * driver real y los devuelve tal cual; el shim los devuelve a la app sin
 * interpretarlos.
 *
 * Convenciones de emisión (ver scutum.h, enum sc_egl_op):
 *   - Los EGLint son i32, EGLBoolean i32, EGLenum u32.
 *   - Los EGLAttrib son i64 (intptr_t en el ABI EGL 1.5).
 *   - Las listas de atributos van [u32 n][items...] terminadas en EGL_NONE.
 *     NULL => [u32 0].
 *   - Los handles opacos van como u64.
 *   - Las respuestas llegan por SC_OP_GL_SYNC y empiezan con [i32 result]
 *     (ver sc_core.c: sc_sync_result). sc_sync_recv_bytes copia SIEMPRE
 *     desde rx+4 (después del result), así que cada respuesta se parsea
 *     con una única llamada a sc_sync_recv_bytes y se decodifica local.
 */
#define EGL_EGLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <xcb/xcb.h>
#include <xcb/shm.h>
#include <sys/mman.h>
#include <fcntl.h>

#include "scutum.h"
#include "sc_core.h"

/* ------------------------------------------------------------- helpers */

#define EH(x)   ((uint64_t)(uintptr_t)(x))
#define HP(x)   ((void *)(uintptr_t)(x))

/* ============================================================
 * X11 window presentation.
 *
 * Android EGL no entiende X11. Este bloque hace el present del
 * lado del shim:
 *   - eglGetConfigAttrib(EGL_NATIVE_VISUAL_ID) se intercepta y
 *     devuelve el visual del root window (vía xcb).
 *   - eglCreateWindowSurface crea un pbuffer del tamaño de la
 *     ventana X11 y lo registra.
 *   - eglSwapBuffers lee el pbuffer con glReadPixels y blitea a
 *     X11 con xcb_put_image.
 * ============================================================ */

static pthread_once_t g_x_once = PTHREAD_ONCE_INIT;
static xcb_connection_t *g_x_conn;
static xcb_screen_t     *g_x_screen;
static xcb_visualid_t    g_x_root_visual;

static void x_init_once(void) {
    int scr = 0;
    xcb_connection_t *c = xcb_connect(NULL, &scr);
    if (!c || xcb_connection_has_error(c)) {
        if (c) xcb_disconnect(c);
        return;
    }
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(c));
    for (int i = 0; i < scr && it.rem; i++) xcb_screen_next(&it);
    if (!it.data) { xcb_disconnect(c); return; }
    g_x_conn = c;
    g_x_screen = it.data;
    g_x_root_visual = g_x_screen->root_visual;
}

static inline int x_ensure(void) {
    pthread_once(&g_x_once, x_init_once);
    return g_x_conn ? 0 : -1;
}

struct win_surf {
    uint64_t       key;   /* EGLSurface (opaco) */
    xcb_window_t   win;
    uint32_t       w, h;
    xcb_gcontext_t gc;
};
#define MAX_WIN_SURF 64
static struct win_surf g_ws[MAX_WIN_SURF];
static int g_n_ws;
static pthread_mutex_t g_ws_lock = PTHREAD_MUTEX_INITIALIZER;

static int ws_register(uint64_t key, xcb_window_t win, uint32_t w, uint32_t h) {
    if (x_ensure() != 0) return -1;
    pthread_mutex_lock(&g_ws_lock);
    if (g_n_ws >= MAX_WIN_SURF) { pthread_mutex_unlock(&g_ws_lock); return -1; }
    struct win_surf *s = &g_ws[g_n_ws];
    s->key = key; s->win = win; s->w = w; s->h = h;
    s->gc = xcb_generate_id(g_x_conn);
    xcb_create_gc(g_x_conn, s->gc, win, 0, NULL);
    xcb_flush(g_x_conn);
    g_n_ws++;
    pthread_mutex_unlock(&g_ws_lock);
    return 0;
}

static int ws_lookup(uint64_t key, struct win_surf *out) {
    pthread_mutex_lock(&g_ws_lock);
    int found = 0;
    for (int i = 0; i < g_n_ws; i++) {
        if (g_ws[i].key == key) { *out = g_ws[i]; found = 1; break; }
    }
    pthread_mutex_unlock(&g_ws_lock);
    return found;
}

static void ws_unregister(uint64_t key) {
    uint32_t gc = 0;
    pthread_mutex_lock(&g_ws_lock);
    for (int i = 0; i < g_n_ws; i++) {
        if (g_ws[i].key == key) {
            gc = g_ws[i].gc;
            g_ws[i] = g_ws[g_n_ws - 1];
            g_n_ws--;
            break;
        }
    }
    pthread_mutex_unlock(&g_ws_lock);
    if (gc && g_x_conn) {
        xcb_free_gc(g_x_conn, gc);
        xcb_flush(g_x_conn);
    }
}


/* ============================================================ MIT-SHM (frame presentado sin copias por socket/X)
 * Dos buffers memfd compartidos con (a) el daemon (SCM_RIGHTS, el daemon escribe el frame ya
 * convertido directo ahi) y (b) el servidor X (xcb_shm_attach_fd, MIT-SHM 1.2+). Cada frame:
 * PRESENT(slot) -> el daemon escribe en la memoria compartida -> xcb_shm_put_image (sin datos
 * por socket). Doble buffer + evento de completion para no pisar un frame que X aun lee.
 * Si algo falla (sin MIT-SHM 1.2, memfd, attach), se usa el camino por bytes de siempre. */
struct shm_buf { int fd; uint8_t *p; xcb_shm_seg_t seg; int pending, daemon_has; };
static struct {
    uint64_t key; int inited, disabled;
    uint32_t w, h; size_t sz;
    uint8_t first_event;
    struct shm_buf b[2];
    unsigned cur;
} g_shm;
static pthread_mutex_t g_shm_lock = PTHREAD_MUTEX_INITIALIZER;

static void shm_release(void) {
    for (int i = 0; i < 2; i++) {
        struct shm_buf *b = &g_shm.b[i];
        if (b->seg && g_x_conn) xcb_shm_detach(g_x_conn, b->seg);
        if (b->p) munmap(b->p, g_shm.sz);
        if (b->fd > 0) close(b->fd);
        memset(b, 0, sizeof *b);
    }
    if (g_x_conn) xcb_flush(g_x_conn);
    g_shm.w = g_shm.h = 0; g_shm.sz = 0;
}

static int shm_probe(void) {
    const xcb_query_extension_reply_t *ext = xcb_get_extension_data(g_x_conn, &xcb_shm_id);
    if (!ext || !ext->present) return -1;
    xcb_shm_query_version_reply_t *v = xcb_shm_query_version_reply(
        g_x_conn, xcb_shm_query_version(g_x_conn), NULL);
    if (!v) return -1;
    int ok = (v->major_version > 1) || (v->major_version == 1 && v->minor_version >= 2);
    free(v);
    if (!ok) return -1;
    g_shm.first_event = ext->first_event;
    return 0;
}

static int shm_setup(uint32_t w, uint32_t h) {
    shm_release();
    size_t sz = (size_t)w * h * 4;
    for (int i = 0; i < 2; i++) {
        struct shm_buf *b = &g_shm.b[i];
        b->fd = memfd_create("scutum-fb", MFD_CLOEXEC);
        if (b->fd < 0 || ftruncate(b->fd, (off_t)sz) != 0) goto fail;
        b->p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, b->fd, 0);
        if (b->p == MAP_FAILED) { b->p = NULL; goto fail; }
        int dupfd = dup(b->fd);
        if (dupfd < 0) goto fail;
        b->seg = xcb_generate_id(g_x_conn);
        xcb_void_cookie_t ck = xcb_shm_attach_fd_checked(g_x_conn, b->seg, dupfd, 0);  /* xcb cierra dupfd */
        xcb_generic_error_t *err = xcb_request_check(g_x_conn, ck);
        if (err) { free(err); b->seg = 0; goto fail; }
    }
    g_shm.w = w; g_shm.h = h; g_shm.sz = sz; g_shm.cur = 0;
    return 0;
fail:
    shm_release();
    return -1;
}

static void shm_drain_events(void) {
    xcb_generic_event_t *ev;
    while ((ev = xcb_poll_for_event(g_x_conn)) != NULL) {
        if ((ev->response_type & 0x7f) == (uint8_t)(g_shm.first_event + XCB_SHM_COMPLETION)) {
            xcb_shm_seg_t s = ((xcb_shm_completion_event_t *)ev)->shmseg;
            for (int i = 0; i < 2; i++) if (g_shm.b[i].seg == s) g_shm.b[i].pending = 0;
        }
        free(ev);
    }
}

/* 1 = presentado por MIT-SHM; 0 = usar el camino por bytes. */
static int present_shm(const struct win_surf *ws) {
    pthread_mutex_lock(&g_shm_lock);
    int rv = 0;
    if (g_shm.disabled) goto out;
    if (!g_shm.inited) {
        g_shm.inited = 1;
        const char *e = getenv("SC_NO_SHM");
        if ((e && *e == '1') || shm_probe() != 0) { g_shm.disabled = 1; goto out; }
    }
    if (g_shm.key && g_shm.key != ws->key) goto out;       /* otra superficie: camino por bytes */
    if (g_shm.w != ws->w || g_shm.h != ws->h || !g_shm.b[0].p) {
        if (shm_setup(ws->w, ws->h) != 0) { g_shm.disabled = 1; goto out; }
        g_shm.key = ws->key;
    }
    struct shm_buf *b = &g_shm.b[g_shm.cur];

    /* esperar (con tope) a que X termine de leer este buffer del frame anterior */
    for (int i = 0; i < 400 && b->pending; i++) {
        shm_drain_events();
        if (b->pending) usleep(250);
    }
    b->pending = 0;

    sc_sync_begin(SC_EGL_PRESENT);
    sc_emit_u32(ws->w);
    sc_emit_u32(ws->h);
    sc_emit_u32(g_shm.cur);
    sc_emit_u32(b->daemon_has ? 0 : 1);
    if (!b->daemon_has) sc_sync_attach_fd(b->fd);
    if (sc_sync_send() != 0) goto out;
    int32_t res = sc_sync_result();
    if (res == 2) {                       /* el daemon no pudo mapear: bytes siguen -> usar el camino viejo */
        fprintf(stderr, "[sc-egl] daemon sin memoria compartida, uso frames por socket\n");
        sc_sync_recv_bytes(b->p, g_shm.sz);   /* aprovechamos estos bytes para este frame */
        g_shm.disabled = 1;
    } else if (res != 1) {
        goto out;
    }
    b->daemon_has = 1;

    xcb_shm_put_image(g_x_conn, ws->win, ws->gc,
                      (uint16_t)ws->w, (uint16_t)ws->h, 0, 0,
                      (uint16_t)ws->w, (uint16_t)ws->h, 0, 0,
                      24, XCB_IMAGE_FORMAT_Z_PIXMAP, 1 /*send_event*/, b->seg, 0);
    b->pending = 1;
    xcb_flush(g_x_conn);
    g_shm.cur ^= 1;
    rv = 1;
out:
    pthread_mutex_unlock(&g_shm_lock);
    return rv;
}

/* Emite [u32 n][i32 item]*n con terminador EGL_NONE incluido.
 * NULL => [u32 0]. */
static void emit_attr_iv(const EGLint *a) {
    if (!a) { sc_emit_u32(0); return; }
    int n = 0;
    while (a[n] != EGL_NONE) n++;
    sc_emit_u32((uint32_t)(n + 1));
    for (int i = 0; i <= n; i++) sc_emit_i32(a[i]);
}

/* Igual pero para EGLAttrib (i64). Terminador EGL_NONE (== 0) incluido.
 * NULL => [u32 0]. */
static void emit_attr_i64(const EGLAttrib *a) {
    if (!a) { sc_emit_u32(0); return; }
    int n = 0;
    while (a[n] != (EGLAttrib)EGL_NONE) n++;
    sc_emit_u32((uint32_t)(n + 1));
    for (int i = 0; i <= n; i++) sc_emit_u64((uint64_t)a[i]);
}

/* ------------------------------------------------------------- display */

EGLDisplay eglGetDisplay(EGLNativeDisplayType display_id) {
    /* mali_default_display_shim: Mali no entiende displays X11.
     * Forzamos EGL_DEFAULT_DISPLAY (0) hacia el daemon. */
    (void)display_id;
    sc_sync_begin(SC_EGL_GET_DISPLAY);
    sc_emit_u64(0);
    if (sc_sync_send() != 0) return EGL_NO_DISPLAY;
    return (EGLDisplay)HP(sc_sync_recv_u64());
}

EGLDisplay eglGetPlatformDisplay(EGLenum platform, void *native,
                                 const EGLAttrib *attribs) {
    /* platform_as_default: Mali en Termux (sin JVM/Activity) no puede
     * crear un display de plataforma Android. Ignoramos la plataforma
     * y pedimos el default display, igual que eglGetDisplay(0). */
    (void)platform; (void)native; (void)attribs;
    sc_sync_begin(SC_EGL_GET_DISPLAY);
    sc_emit_u64(0);
    if (sc_sync_send() != 0) return EGL_NO_DISPLAY;
    return (EGLDisplay)HP(sc_sync_recv_u64());
}

EGLDisplay eglGetPlatformDisplayEXT(EGLenum platform, void *native,
                                    const EGLint *attribs) {
    /* platform_as_default */
    (void)platform; (void)native; (void)attribs;
    sc_sync_begin(SC_EGL_GET_DISPLAY);
    sc_emit_u64(0);
    if (sc_sync_send() != 0) return EGL_NO_DISPLAY;
    return (EGLDisplay)HP(sc_sync_recv_u64());
}

EGLBoolean eglInitialize(EGLDisplay dpy, EGLint *major, EGLint *minor) {
    sc_sync_begin(SC_EGL_INITIALIZE);
    sc_emit_u64(EH(dpy));
    if (sc_sync_send() != 0) return EGL_FALSE;
    if (sc_sync_result() == 0) return EGL_FALSE;
    /* respuesta: [i32 result][i32 major][i32 minor] */
    int32_t v[2] = { 0, 0 };
    if (sc_sync_recv_bytes(v, sizeof v) >= sizeof v) {
        if (major) *major = v[0];
        if (minor) *minor = v[1];
    }
    return EGL_TRUE;
}

EGLBoolean eglTerminate(EGLDisplay dpy) {
    sc_sync_begin(SC_EGL_TERMINATE);
    sc_emit_u64(EH(dpy));
    if (sc_sync_send() != 0) return EGL_FALSE;
    return sc_sync_result() ? EGL_TRUE : EGL_FALSE;
}

const char *eglQueryString(EGLDisplay dpy, EGLint name) {
    sc_sync_begin(SC_EGL_QUERY_STRING);
    sc_emit_u64(EH(dpy));
    sc_emit_i32(name);
    if (sc_sync_send() != 0) return NULL;
    /* respuesta: [i32 result][u32 len][bytes...] */
    uint8_t buf[4096];
    size_t got = sc_sync_recv_bytes(buf, sizeof buf);
    if (got < 4) return sc_string_intern_slot("", 0, (uint32_t)name, 0);
    uint32_t len;
    memcpy(&len, buf, 4);
    if (len > got - 4) len = (uint32_t)(got - 4);
    return sc_string_intern_slot(buf + 4, len, (uint32_t)name, 0);
}

EGLBoolean eglQueryContext(EGLDisplay dpy, EGLContext ctx,
                            EGLint attribute, EGLint *value)
{
    sc_sync_begin(SC_EGL_QUERY_CONTEXT);
    sc_emit_u64(EH(dpy));
    sc_emit_u64(EH(ctx));
    sc_emit_i32(attribute);
    if (sc_sync_send() != 0) return EGL_FALSE;
    int32_t ok = sc_sync_result();
    int32_t v  = 0;
    sc_sync_recv_bytes(&v, 4);
    if (value) *value = v;
    return ok ? EGL_TRUE : EGL_FALSE;
}

EGLint eglGetError(void) {
    sc_sync_begin(SC_EGL_GET_ERROR);
    if (sc_sync_send() != 0) return EGL_NOT_INITIALIZED;
    return sc_sync_result();
}

/* ------------------------------------------------------------- config */

EGLBoolean eglGetConfigs(EGLDisplay dpy, EGLConfig *configs,
                         EGLint config_size, EGLint *num_config)
{
    sc_sync_begin(SC_EGL_GET_CONFIGS);
    sc_emit_u64(EH(dpy));
    sc_emit_u32((uint32_t)(config_size > 0 ? config_size : 0));
    if (sc_sync_send() != 0) return EGL_FALSE;
    if (sc_sync_result() == 0) return EGL_FALSE;

    /* respuesta: [i32 result][u32 n_out][u64 cfg[n_out]] */
    uint32_t n = 0;
    if (sc_sync_recv_bytes(&n, 4) < 4) n = 0;
    if (num_config) *num_config = (EGLint)n;

    if (configs && n > 0 && config_size > 0) {
        uint32_t k = (uint32_t)config_size < n ? (uint32_t)config_size : n;
        size_t   sz = 4 + (size_t)k * 8;
        uint8_t *buf = malloc(sz);
        if (buf) {
            sc_sync_recv_bytes(buf, sz);
            for (uint32_t i = 0; i < k; i++) {
                uint64_t c;
                memcpy(&c, buf + 4 + i * 8, 8);
                configs[i] = (EGLConfig)HP(c);
            }
            free(buf);
        }
    }
    return EGL_TRUE;
}



/* ---- SC_TRACE_EGL=1: traza de las llamadas EGL que deciden si una app puede arrancar ---- */
static int sc_trace_egl(void) {
    static int on = -1;
    if (on < 0) { const char *t = getenv("SC_TRACE_EGL"); on = (t && *t == '1'); }
    return on;
}
static void sc_trace_attrs(const char *who, const EGLint *a) {
    fprintf(stderr, "[sc-egl-trace] %s attrs:", who);
    if (!a) { fprintf(stderr, " (NULL)\n"); return; }
    for (int i = 0; a[i] != EGL_NONE && i < 120; i += 2) fprintf(stderr, " 0x%x=0x%x", (unsigned)a[i], (unsigned)a[i + 1]);
    fprintf(stderr, "\n");
}

/* Nuestras window surfaces son pbuffers en el daemon (ver eglCreateWindowSurface): el config
 * elegido tiene que soportar EGL_PBUFFER_BIT ademas de lo que pida la app. Si SURFACE_TYPE no
 * esta, el default de EGL es WINDOW_BIT. */
/* Profundidad minima. GL4ES pide EGL_DEPTH_SIZE 16 (o nada); EGL ordena las configs con el
 * menor depth primero, asi que Mali entrega D16 (o sin depth) y los modelos 3D salen como
 * un amasijo de poligonos por z-fighting. SC_DEPTH_BITS=0 desactiva el ajuste. */
static int sc_min_depth(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("SC_DEPTH_BITS"); v = e ? atoi(e) : 24; }
    return v;
}
static void emit_attr_iv_pbuf(const EGLint *a, int bump) {
    EGLint tmp[128]; int n = 0, found = 0, fdepth = 0;
    int want_depth = bump ? sc_min_depth() : 0;
    if (a) {
        for (; a[n] != EGL_NONE && n < 118; n += 2) {
            tmp[n] = a[n]; tmp[n + 1] = a[n + 1];
            if (a[n] == EGL_SURFACE_TYPE) { tmp[n + 1] |= EGL_PBUFFER_BIT; found = 1; }
            if (a[n] == EGL_DEPTH_SIZE) {
                fdepth = 1;
                if (want_depth > 0 && tmp[n + 1] < want_depth) tmp[n + 1] = want_depth;
            }
        }
    }
    if (want_depth > 0 && !fdepth) { tmp[n++] = EGL_DEPTH_SIZE; tmp[n++] = want_depth; }
    if (!found) { tmp[n++] = EGL_SURFACE_TYPE; tmp[n++] = EGL_WINDOW_BIT | EGL_PBUFFER_BIT; }
    tmp[n] = EGL_NONE;
    emit_attr_iv(tmp);
}

static EGLBoolean choose_config_impl(EGLDisplay dpy, const EGLint *attrib_list,
                           EGLConfig *configs, EGLint config_size,
                           EGLint *num_config, int bump)
{
    if (sc_trace_egl()) sc_trace_attrs("eglChooseConfig pide", attrib_list);
    sc_sync_begin(SC_EGL_CHOOSE_CONFIG);
    sc_emit_u64(EH(dpy));
    emit_attr_iv_pbuf(attrib_list, bump);
    sc_emit_u32((uint32_t)(config_size > 0 ? config_size : 0));
    if (sc_sync_send() != 0) return EGL_FALSE;
    if (sc_sync_result() == 0) return EGL_FALSE;

    uint32_t n = 0;
    if (sc_sync_recv_bytes(&n, 4) < 4) n = 0;
    if (num_config) *num_config = (EGLint)n;
    if (sc_trace_egl()) fprintf(stderr, "[sc-egl-trace] eglChooseConfig -> %u configs (config_size=%d)\n", n, (int)config_size);

    if (configs && n > 0 && config_size > 0) {
        uint32_t k = (uint32_t)config_size < n ? (uint32_t)config_size : n;
        size_t   sz = 4 + (size_t)k * 8;
        uint8_t *buf = malloc(sz);
        if (buf) {
            sc_sync_recv_bytes(buf, sz);
            for (uint32_t i = 0; i < k; i++) {
                uint64_t c;
                memcpy(&c, buf + 4 + i * 8, 8);
                configs[i] = (EGLConfig)HP(c);
            }
            free(buf);
        }
    }
    return EGL_TRUE;
}

EGLBoolean eglChooseConfig(EGLDisplay dpy, const EGLint *attrib_list,
                           EGLConfig *configs, EGLint config_size,
                           EGLint *num_config)
{
    EGLint n = 0;
    EGLBoolean ok = choose_config_impl(dpy, attrib_list, configs, config_size, &n, 1);
    if ((!ok || n == 0) && sc_min_depth() > 0) {   /* el driver no tiene D24: pedido original */
        n = 0;
        ok = choose_config_impl(dpy, attrib_list, configs, config_size, &n, 0);
    }
    if (num_config) *num_config = n;
    return ok;
}

EGLBoolean eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config,
                              EGLint attribute, EGLint *value)
{
    /* Android EGL no tiene X11 visual ID. Devolvemos el visual del
     * root window para que GL4ES arme su tabla config<->visual. */
    if (attribute == EGL_NATIVE_VISUAL_ID) {
        if (x_ensure() != 0) return EGL_FALSE;
        if (value) *value = (EGLint)g_x_root_visual;
        return EGL_TRUE;
    }
    sc_sync_begin(SC_EGL_GET_CONFIG_ATTRIB);
    sc_emit_u64(EH(dpy));
    sc_emit_u64(EH(config));
    sc_emit_i32(attribute);
    if (sc_sync_send() != 0) return EGL_FALSE;
    int32_t ok = sc_sync_result();
    int32_t v  = 0;
    sc_sync_recv_bytes(&v, 4);
    if (value) *value = v;
    return ok ? EGL_TRUE : EGL_FALSE;
}

/* ------------------------------------------------------------- context */

EGLContext eglCreateContext(EGLDisplay dpy, EGLConfig config,
                            EGLContext share_context,
                            const EGLint *attrib_list)
{
    sc_sync_begin(SC_EGL_CREATE_CONTEXT);
    sc_emit_u64(EH(dpy));
    sc_emit_u64(EH(config));
    sc_emit_u64(EH(share_context));
    emit_attr_iv(attrib_list);
    if (sc_trace_egl()) sc_trace_attrs("eglCreateContext pide", attrib_list);
    if (sc_sync_send() != 0) return EGL_NO_CONTEXT;
    EGLContext cx = (EGLContext)HP(sc_sync_recv_u64());
    if (sc_trace_egl()) fprintf(stderr, "[sc-egl-trace] eglCreateContext -> %p\n", (void *)cx);
    return cx;
}

EGLBoolean eglDestroyContext(EGLDisplay dpy, EGLContext ctx) {
    sc_sync_begin(SC_EGL_DESTROY_CONTEXT);
    sc_emit_u64(EH(dpy));
    sc_emit_u64(EH(ctx));
    if (sc_sync_send() != 0) return EGL_FALSE;
    if (sc_sync_result()) { sc_ca_destroy((const void *)ctx); return EGL_TRUE; }
    return EGL_FALSE;
}

EGLBoolean eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read,
                          EGLContext ctx)
{
    sc_sync_begin(SC_EGL_MAKE_CURRENT);
    sc_emit_u64(EH(dpy));
    sc_emit_u64(EH(draw));
    sc_emit_u64(EH(read));
    sc_emit_u64(EH(ctx));
    if (sc_sync_send() != 0) return EGL_FALSE;
    {
        EGLBoolean okmc = sc_sync_result() ? EGL_TRUE : EGL_FALSE;
        const char *e = getenv("SC_TRACE_SWAP");
        if (e && *e == '1')
            fprintf(stderr, "[sc-mc] tid=%ld draw=%p read=%p ctx=%p -> %d\n",
                    (long)syscall(SYS_gettid), (void*)draw, (void*)read, (void*)ctx, (int)okmc);
        if (okmc) sc_ca_make_current(ctx == EGL_NO_CONTEXT ? NULL : (const void *)ctx);
        return okmc;
    }
}

EGLContext eglGetCurrentContext(void) {
    sc_sync_begin(SC_EGL_GET_CURRENT_CONTEXT);
    if (sc_sync_send() != 0) return EGL_NO_CONTEXT;
    return (EGLContext)HP(sc_sync_recv_u64());
}

EGLSurface eglGetCurrentSurface(EGLint readdraw) {
    sc_sync_begin(SC_EGL_GET_CURRENT_SURFACE);
    sc_emit_i32(readdraw);
    if (sc_sync_send() != 0) return EGL_NO_SURFACE;
    return (EGLSurface)HP(sc_sync_recv_u64());
}

EGLDisplay eglGetCurrentDisplay(void) {
    sc_sync_begin(SC_EGL_GET_CURRENT_DISPLAY);
    if (sc_sync_send() != 0) return EGL_NO_DISPLAY;
    return (EGLDisplay)HP(sc_sync_recv_u64());
}

EGLBoolean eglReleaseThread(void) {
    sc_sync_begin(SC_EGL_RELEASE_THREAD);
    if (sc_sync_send() != 0) return EGL_FALSE;
    return sc_sync_result() ? EGL_TRUE : EGL_FALSE;
}

/* ------------------------------------------------------------- surface */

EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config,
                                  EGLNativeWindowType win,
                                  const EGLint *attrib_list)
{
    (void)attrib_list;
    fprintf(stderr, "[sc-egl] CWS dpy=%p cfg=%p win=0x%lx\n",
            (void*)dpy, (void*)config, (unsigned long)(uintptr_t)win);
    fflush(stderr);

    if (!win) { fprintf(stderr, "[sc-egl] win NULL\n"); return EGL_NO_SURFACE; }
    if (x_ensure() != 0) { fprintf(stderr, "[sc-egl] sin conexion X\n"); fflush(stderr); return EGL_NO_SURFACE; }

    uint32_t w = 640, h = 480;
    xcb_get_geometry_cookie_t gck = xcb_get_geometry(g_x_conn, (xcb_window_t)(uintptr_t)win);
    xcb_get_geometry_reply_t *geo = xcb_get_geometry_reply(g_x_conn, gck, NULL);
    if (geo) {
        if (geo->width && geo->height) { w = geo->width; h = geo->height; }
        else fprintf(stderr, "[sc-egl] geo %ux%u -> fallback %ux%u\n",
                     geo->width, geo->height, w, h);
        free(geo);
    } else {
        fprintf(stderr, "[sc-egl] xcb_get_geometry fallo -> fallback %ux%u\n", w, h);
    }

    EGLint pba[8];
    int n = 0;
    pba[n++] = EGL_WIDTH;  pba[n++] = (EGLint)w;
    pba[n++] = EGL_HEIGHT; pba[n++] = (EGLint)h;
    pba[n++] = EGL_NONE;

    EGLSurface surf = eglCreatePbufferSurface(dpy, config, pba);
    if (surf == EGL_NO_SURFACE) {
        EGLint e1 = eglGetError();
        fprintf(stderr, "[sc-egl] pbuffer con config de la app fallo err=0x%x\n", (unsigned)e1);
        EGLint ca[] = {
            EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
            EGL_DEPTH_SIZE, 16,
            EGL_NONE
        };
        EGLConfig cfg = 0; EGLint nn = 0;
        if (eglChooseConfig(dpy, ca, &cfg, 1, &nn) && nn && cfg) {
            surf = eglCreatePbufferSurface(dpy, cfg, pba);
            fprintf(stderr, "[sc-egl] retry -> surf=%p err=0x%x\n",
                    (void*)surf, (unsigned)eglGetError());
        }
        if (surf == EGL_NO_SURFACE) { fflush(stderr); return EGL_NO_SURFACE; }
    }

    if (ws_register((uint64_t)(uintptr_t)surf, (xcb_window_t)(uintptr_t)win, w, h) != 0) {
        eglDestroySurface(dpy, surf);
        fprintf(stderr, "[sc-egl] ws_register fallo\n");
        fflush(stderr);
        return EGL_NO_SURFACE;
    }
    fprintf(stderr, "[sc-egl] OK surf=%p %ux%u\n", (void*)surf, w, h);
    fflush(stderr);
    return surf;
}

EGLSurface eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig config,
                                   const EGLint *attrib_list)
{
    sc_sync_begin(SC_EGL_CREATE_PBUFFER_SURFACE);
    sc_emit_u64(EH(dpy));
    sc_emit_u64(EH(config));
    emit_attr_iv(attrib_list);
    if (sc_sync_send() != 0) return EGL_NO_SURFACE;
    return (EGLSurface)HP(sc_sync_recv_u64());
}

EGLSurface eglCreatePixmapSurface(EGLDisplay dpy, EGLConfig config,
                                  EGLNativePixmapType pixmap,
                                  const EGLint *attrib_list)
{
    sc_sync_begin(SC_EGL_CREATE_PIXMAP_SURFACE);
    sc_emit_u64(EH(dpy));
    sc_emit_u64(EH(config));
    sc_emit_u64((uint64_t)(uintptr_t)pixmap);
    emit_attr_iv(attrib_list);
    if (sc_sync_send() != 0) return EGL_NO_SURFACE;
    return (EGLSurface)HP(sc_sync_recv_u64());
}

EGLBoolean eglDestroySurface(EGLDisplay dpy, EGLSurface surface) {
    ws_unregister((uint64_t)(uintptr_t)surface);
    sc_sync_begin(SC_EGL_DESTROY_SURFACE);
    sc_emit_u64(EH(dpy));
    sc_emit_u64(EH(surface));
    if (sc_sync_send() != 0) return EGL_FALSE;
    return sc_sync_result() ? EGL_TRUE : EGL_FALSE;
}

EGLBoolean eglQuerySurface(EGLDisplay dpy, EGLSurface surface, EGLint attrib,
                           EGLint *value)
{
    /* ws_query_local: si es una window surface X11 que envolvimos,
     * responder los atributos que Mali no sabe (dimensiones y estado
     * X11), en vez de reenviar. Mismo patrón que Spatha con
     * vkGetPhysicalDeviceSurfaceCapabilitiesKHR.
     * Los atributos que sí conoce (EGL_CONFIG_ID, EGL_ALPHA_SIZE, ...)
     * se reenvían a Mali abajo. */
    struct win_surf ws;
    if (ws_lookup((uint64_t)(uintptr_t)surface, &ws)) {
        if (!value) return EGL_FALSE;
        switch (attrib) {
        case EGL_WIDTH:              *value = (EGLint)ws.w; return EGL_TRUE;
        case EGL_HEIGHT:             *value = (EGLint)ws.h; return EGL_TRUE;
        case EGL_SWAP_BEHAVIOR:      *value = EGL_BUFFER_PRESERVED; return EGL_TRUE;
        case EGL_RENDER_BUFFER:      *value = EGL_BACK_BUFFER;      return EGL_TRUE;
        case EGL_LARGEST_PBUFFER:    *value = EGL_FALSE;            return EGL_TRUE;
        case EGL_TEXTURE_FORMAT:     *value = EGL_NO_TEXTURE;       return EGL_TRUE;
        case EGL_TEXTURE_TARGET:     *value = EGL_NO_TEXTURE;       return EGL_TRUE;
        case EGL_MIPMAP_TEXTURE:     *value = EGL_FALSE;            return EGL_TRUE;
        case EGL_MIPMAP_LEVEL:       *value = 0;                    return EGL_TRUE;
        case EGL_MULTISAMPLE_RESOLVE:*value = EGL_MULTISAMPLE_RESOLVE_DEFAULT; return EGL_TRUE;
        case EGL_HORIZONTAL_RESOLUTION:
        case EGL_VERTICAL_RESOLUTION: *value = (EGLint)EGL_UNKNOWN; return EGL_TRUE;
        case EGL_PIXEL_ASPECT_RATIO: *value = 1;                    return EGL_TRUE;
        case EGL_CLIENT_APIS:        *value = (EGLint)EGL_OPENGL_ES_API; return EGL_TRUE;
        default: break;  /* EGL_CONFIG_ID, EGL_ALPHA_SIZE, etc: los sabe Mali */
        }
    }

    sc_sync_begin(SC_EGL_QUERY_SURFACE);
    sc_emit_u64(EH(dpy));
    sc_emit_u64(EH(surface));
    sc_emit_i32(attrib);
    if (sc_sync_send() != 0) return EGL_FALSE;
    int32_t ok = sc_sync_result();
    int32_t v  = 0;
    sc_sync_recv_bytes(&v, 4);
    if (value) *value = v;
    return ok ? EGL_TRUE : EGL_FALSE;
}

EGLBoolean eglSurfaceAttrib(EGLDisplay dpy, EGLSurface surface, EGLint attrib,
                            EGLint value)
{
    sc_sync_begin(SC_EGL_SURFACE_ATTRIB);
    sc_emit_u64(EH(dpy));
    sc_emit_u64(EH(surface));
    sc_emit_i32(attrib);
    sc_emit_i32(value);
    if (sc_sync_send() != 0) return EGL_FALSE;
    return sc_sync_result() ? EGL_TRUE : EGL_FALSE;
}

extern void sc_stats_frame(uint64_t swap_ns);
extern uint64_t sc_stats_now(void);

EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
    struct win_surf ws;
    if (ws_lookup((uint64_t)(uintptr_t)surface, &ws) && g_x_conn) {
        uint64_t st0 = sc_stats_now();
        /* Window surface X11 interceptada: el daemon hace readback, flip y
         * swap R/B (SC_EGL_PRESENT) y devuelve el frame listo en BGRX. */
        sc_batch_flush();

        if (present_shm(&ws)) {
            if (st0) sc_stats_frame(sc_stats_now() - st0);
            return EGL_TRUE;
        }

        size_t stride = (size_t)ws.w * 4;
        size_t sz     = stride * ws.h;
        static __thread uint8_t *buf = NULL; static __thread size_t buf_cap = 0;
        if (buf_cap < sz) {
            free(buf); buf = malloc(sz); buf_cap = buf ? sz : 0;
            if (!buf) return EGL_FALSE;
        }
        sc_sync_begin(SC_EGL_PRESENT);
        sc_emit_u32(ws.w);
        sc_emit_u32(ws.h);
        sc_emit_u32(0xFFFFFFFFu);   /* sin slot: frame por socket */
        sc_emit_u32(0);
        if (sc_sync_send() != 0) return EGL_FALSE;
        if (sc_sync_result() != 1) return EGL_FALSE;
        sc_sync_recv_bytes(buf, sz);

        size_t maxb = (size_t)xcb_get_maximum_request_length(g_x_conn) * 4;
        size_t rows = (maxb > 64 + stride) ? (maxb - 64) / stride : 1;
        if (rows < 1) rows = 1;

        for (uint32_t y = 0; y < ws.h; y += (uint32_t)rows) {
            uint32_t nr = ws.h - y < rows ? ws.h - y : (uint32_t)rows;
            xcb_put_image(g_x_conn, XCB_IMAGE_FORMAT_Z_PIXMAP,
                          ws.win, ws.gc,
                          (uint16_t)ws.w, (uint16_t)nr,
                          0, (int16_t)y, 0, 24,
                          (uint32_t)(nr * stride),
                          buf + (size_t)y * stride);
        }
        xcb_flush(g_x_conn);
        if (st0) sc_stats_frame(sc_stats_now() - st0);
        return EGL_TRUE;
    }

    /* Fallback: pbuffer u otra surface que Mali entiende */
    sc_batch_flush();
    sc_sync_begin(SC_EGL_SWAP_BUFFERS);
    sc_emit_u64(EH(dpy));
    sc_emit_u64(EH(surface));
    sc_emit_u32(0);  /* n_fds */
    if (sc_sync_send() != 0) return EGL_FALSE;
    return sc_sync_result() ? EGL_TRUE : EGL_FALSE;
}

EGLBoolean eglSwapBuffersWithDamageKHR(EGLDisplay dpy, EGLSurface surface,
                                       const EGLint *rects, EGLint n_rects)
{
    (void)rects; (void)n_rects;
    return eglSwapBuffers(dpy, surface);
}

EGLBoolean eglSwapInterval(EGLDisplay dpy, EGLint interval) {
    sc_sync_begin(SC_EGL_SWAP_INTERVAL);
    sc_emit_u64(EH(dpy));
    sc_emit_i32(interval);
    if (sc_sync_send() != 0) return EGL_FALSE;
    return sc_sync_result() ? EGL_TRUE : EGL_FALSE;
}

EGLBoolean eglCopyBuffers(EGLDisplay dpy, EGLSurface surface,
                          EGLNativePixmapType target)
{
    sc_sync_begin(SC_EGL_COPY_BUFFERS);
    sc_emit_u64(EH(dpy));
    sc_emit_u64(EH(surface));
    sc_emit_u64((uint64_t)(uintptr_t)target);
    sc_emit_u32(0);  /* n_fds */
    if (sc_sync_send() != 0) return EGL_FALSE;
    return sc_sync_result() ? EGL_TRUE : EGL_FALSE;
}

EGLBoolean eglBindTexImage(EGLDisplay dpy, EGLSurface surface, EGLint buffer) {
    sc_sync_begin(SC_EGL_BIND_TEX_IMAGE);
    sc_emit_u64(EH(dpy));
    sc_emit_u64(EH(surface));
    sc_emit_i32(buffer);
    if (sc_sync_send() != 0) return EGL_FALSE;
    return sc_sync_result() ? EGL_TRUE : EGL_FALSE;
}

EGLBoolean eglReleaseTexImage(EGLDisplay dpy, EGLSurface surface, EGLint buffer) {
    sc_sync_begin(SC_EGL_RELEASE_TEX_IMAGE);
    sc_emit_u64(EH(dpy));
    sc_emit_u64(EH(surface));
    sc_emit_i32(buffer);
    if (sc_sync_send() != 0) return EGL_FALSE;
    return sc_sync_result() ? EGL_TRUE : EGL_FALSE;
}

/* ------------------------------------------------------------- api/proc */

EGLBoolean eglBindAPI(EGLenum api) {
    sc_sync_begin(SC_EGL_BIND_API);
    sc_emit_u32((uint32_t)api);
    if (sc_sync_send() != 0) return EGL_FALSE;
    EGLBoolean r = sc_sync_result() ? EGL_TRUE : EGL_FALSE;
    if (sc_trace_egl()) fprintf(stderr, "[sc-egl-trace] eglBindAPI(0x%x) -> %d%s\n", (unsigned)api, (int)r,
                                 api == 0x30A2 ? "  (EGL_OPENGL_API: Mali no lo soporta)" : "");
    return r;
}

EGLenum eglQueryAPI(void) {
    sc_sync_begin(SC_EGL_QUERY_API);
    if (sc_sync_send() != 0) return 0;
    return (EGLenum)sc_sync_result();
}

__eglMustCastToProperFunctionPointerType
eglGetProcAddress(const char *procname) {
    if (!procname) return NULL;
    sc_sync_begin(SC_EGL_GET_PROC_ADDRESS);
    sc_emit_string(procname);
    if (sc_sync_send() != 0) return NULL;
    /* respuesta: [i32 result][u32 known][u32 kind][u32 opcode] */
    uint8_t buf[12];
    if (sc_sync_recv_bytes(buf, sizeof buf) < sizeof buf) return NULL;
    uint32_t known, kind;
    memcpy(&known, buf,     4);
    memcpy(&kind,  buf + 4, 4);
    if (!known) return NULL;
    extern void *sc_shim_lookup(const char *name, uint32_t kind);
    return (__eglMustCastToProperFunctionPointerType)sc_shim_lookup(procname, kind);
}

EGLBoolean eglWaitClient(void) {
    sc_batch_flush();
    sc_sync_begin(SC_EGL_WAIT_CLIENT);
    if (sc_sync_send() != 0) return EGL_FALSE;
    return sc_sync_result() ? EGL_TRUE : EGL_FALSE;
}

EGLBoolean eglWaitGL(void) {
    sc_batch_flush();
    sc_sync_begin(SC_EGL_WAIT_GL);
    if (sc_sync_send() != 0) return EGL_FALSE;
    return sc_sync_result() ? EGL_TRUE : EGL_FALSE;
}

EGLBoolean eglWaitNative(EGLint engine) {
    sc_batch_flush();
    sc_sync_begin(SC_EGL_WAIT_NATIVE);
    sc_emit_i32(engine);
    if (sc_sync_send() != 0) return EGL_FALSE;
    return sc_sync_result() ? EGL_TRUE : EGL_FALSE;
}

/* ------------------------------------------------------------- sync (1.5) */

EGLSync eglCreateSync(EGLDisplay dpy, EGLenum type, const EGLAttrib *attrib_list) {
    sc_sync_begin(SC_EGL_CREATE_SYNC);
    sc_emit_u64(EH(dpy));
    sc_emit_u32((uint32_t)type);
    emit_attr_i64(attrib_list);
    if (sc_sync_send() != 0) return EGL_NO_SYNC;
    return (EGLSync)HP(sc_sync_recv_u64());
}

EGLBoolean eglDestroySync(EGLDisplay dpy, EGLSync sync) {
    sc_sync_begin(SC_EGL_DESTROY_SYNC);
    sc_emit_u64(EH(dpy));
    sc_emit_u64(EH(sync));
    if (sc_sync_send() != 0) return EGL_FALSE;
    return sc_sync_result() ? EGL_TRUE : EGL_FALSE;
}

EGLint eglClientWaitSync(EGLDisplay dpy, EGLSync sync, EGLint flags,
                         EGLTime timeout)
{
    sc_batch_flush();
    sc_sync_begin(SC_EGL_CLIENT_WAIT_SYNC);
    sc_emit_u64(EH(dpy));
    sc_emit_u64(EH(sync));
    sc_emit_i32(flags);
    sc_emit_u64((uint64_t)timeout);
    if (sc_sync_send() != 0) return EGL_FALSE;
    return sc_sync_result();
}

EGLBoolean eglWaitSync(EGLDisplay dpy, EGLSync sync, EGLint flags) {
    sc_batch_flush();
    sc_sync_begin(SC_EGL_WAIT_SYNC);
    sc_emit_u64(EH(dpy));
    sc_emit_u64(EH(sync));
    sc_emit_i32(flags);
    if (sc_sync_send() != 0) return EGL_FALSE;
    return sc_sync_result() ? EGL_TRUE : EGL_FALSE;
}

EGLBoolean eglGetSyncAttrib(EGLDisplay dpy, EGLSync sync, EGLint attribute,
                            EGLAttrib *value)
{
    sc_sync_begin(SC_EGL_GET_SYNC_ATTRIB);
    sc_emit_u64(EH(dpy));
    sc_emit_u64(EH(sync));
    sc_emit_i32(attribute);
    if (sc_sync_send() != 0) return EGL_FALSE;
    int32_t ok = sc_sync_result();
    int64_t v  = 0;
    sc_sync_recv_bytes(&v, 8);
    if (value) *value = (EGLAttrib)v;
    return ok ? EGL_TRUE : EGL_FALSE;
}

/* ------------------------------------------------------------- native */

/*
 * Firma real (EGL_ANDROID_get_native_client_buffer):
 *   EGLClientBuffer eglGetNativeClientBufferANDROID(const struct AHardwareBuffer *);
 * Requiere SCM_RIGHTS para pasar el fd del AHardwareBuffer; se implementa
 * el camino sin fds: devolvemos NULL y la app hace su fallback.
 */
struct AHardwareBuffer;  /* solo forward; no incluye android/hardware_buffer.h */

EGLClientBuffer eglGetNativeClientBufferANDROID(const struct AHardwareBuffer *buffer)
{
    (void)buffer;
    return NULL;
}

/* ------------------------------------------------------------- dispatch */

/*
 * sc_shim_lookup lo implementa sc_gles.c (o sc_dispatch.c si se separa).
 * El shim de eglGetProcAddress lo llama con el nombre y el kind que
 * devolvió el daemon (1 = EGL, 2 = GL). Devuelve el puntero a la función
 * del propio shim, no la del driver.
 */
extern void *sc_shim_lookup(const char *name, uint32_t kind);
