/*
 * scutum.h — protocolo Scutum completo.
 *
 * ============================================================
 * TRANSPORTE
 * ============================================================
 * Dos canales:
 *   - Socket AF_UNIX, SOCK_STREAM, SOCK_CLOEXEC. Path: $SCUTUM_SOCK o
 *     /tmp/scutum.sock. SOLO handshake (HELLO / HELLO_ACK), paso de fds
 *     sueltos (SC_OP_FD + SCM_RIGHTS) y deteccion de desconexion.
 *   - Memoria compartida (memfd entregado por el daemon en el HELLO_ACK):
 *     dos rings SPSC, uno por sentido. TODO el trafico de comandos y
 *     respuestas va por ahi. Ver sc_shm.h.
 * Framing (ambos canales): [sc_msg header 24B] + payload[header.len].
 * Los mensajes son little-endian, alineados a 8 bytes.
 *
 * Thread ID: el header lleva thread_id. EGL es per-thread (contexto actual,
 * surface actual, display actual). El daemon mantiene una tabla
 * thread_id -> { current_display, current_draw_surface, current_read_surface,
 *                current_context } y cada llamada EGL/GL se ejecuta con ese
 * contexto activo.
 *
 * ============================================================
 * VERSIONADO
 * ============================================================
 * Al conectar, el cliente envía SC_OP_HELLO con { u32 proto_version }.
 * El daemon responde SC_OP_HELLO_ACK con { u32 proto_version, u32 caps, u32 shm_total, u32 ring_size } +
 * 1 fd (SCM_RIGHTS) con el memfd de la region compartida, o
 * SC_OP_ERROR{E_PROTO_VERSION}. Si las versiones no son iguales, el cliente
 * cierra. SC_PROTO_VERSION=2.
 *
 * ============================================================
 * FRAGMENTACION
 * ============================================================
 * Payload máximo por mensaje: SC_MAX_PAYLOAD (64 MiB). Si una operación
 * necesita más (ej: upload de textura >64 MiB), se fragmenta a nivel de
 * protocolo con SC_OP_FRAGMENT:
 *   first: [u32 frag_id][u32 total_size][u32 this_off][u32 this_len][bytes]
 * El daemon acumula hasta total_size y luego procesa. frag_id es único por
 * conexión.
 * Si un solo batch GL supera SC_BATCH_MAX (1 MiB), el shim lo flushea
 * proactivamente antes de agregar más comandos. No hay fragmentación de
 * batches: cada SC_OP_GL_BATCH es autocontenido.
 *
 * ============================================================
 * FILE DESCRIPTORS
 * ============================================================
 * Algunas ops (EGL_IMAGE, NATIVE_CLIENT_BUFFER, ANDROID_SURFACE) requieren
 * pasar fds o handles nativos. Se usa SCM_RIGHTS anexado al mensaje con
 * sendmsg(). El payload lleva [u32 n_fds] y luego [u32 fd_idx]*n_fds, donde
 * cada fd_idx apunta al fd anexado. El daemon recibe con recvmsg() y duplica
 * con dup() antes de usarlo. Si n_fds > 0 y el mensaje no trajo SCM_RIGHTS,
 * el daemon cierra la conexión por protocolo inválido.
 *
 * ============================================================
 * HANDLES
 * ============================================================
 * - GLuint (buffers, texturas, shaders, programs, VAOs, samplers, queries,
 *   sync objects, transform feedbacks): passthrough. El daemon ejecuta
 *   glGen* real y devuelve el nombre real; el shim lo entrega tal cual a la
 *   app. Mismo número en ambos lados. No hay traducción.
 * - EGLDisplay, EGLConfig, EGLContext, EGLSurface, EGLImage, EGLSync,
 *   EGLNativeDisplay: opacos de 64 bits. El daemon los genera y los devuelve
 *   tal cual. El shim los pasa como u64 sin interpretar. Si un buffer de la
 *   app tiene que recibir N handles (ej: eglChooseConfig), el daemon manda
 *   los N u64 en la respuesta y el shim los escribe en el buffer del usuario.
 * - GLsync (fence): passthrough también, es GLuint-like (u64 en el ABI).
 *
 * ============================================================
 * ERRORES
 * ============================================================
 * SC_OP_ERROR: payload = [u32 class][u32 code][u32 msg_len][bytes msg].
 *   class: E_PROTO (framing/versión), E_EGL (EGL_BAD_*), E_GL (GL_INVALID_*),
 *          E_INTERNAL (bug o OOM del daemon).
 *   code: si class==E_EGL, es un EGLint de eglGetError(); si class==E_GL,
 *         es un GLenum; si class==E_PROTO, es un código de la tabla de abajo.
 *   msg:  opcional, UTF-8 sin NUL.
 * Cuando el daemon detecta E_PROTO, cierra la conexión tras enviar el error.
 * Cuando es E_EGL/E_GL, la conexión sigue y el error se reporta también vía
 * glGetError/eglGetError en la próxima llamada sync del shim.
 *
 * Códigos E_PROTO:
 *   1 = framing inválido (magic/len)
 *   2 = versión incompatible
 *   3 = payload truncado o con longitud inconsistente
 *   4 = fd anexado esperado y no presente
 *   5 = fragmentación inconsistente
 *   6 = opcode desconocido
 *   7 = tipo de argumento desconocido en un batch
 */
#ifndef SCUTUM_H
#define SCUTUM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================ version */

#define SC_PROTO_VERSION   2u
#define SC_MAGIC           0x53435554u  /* "SCUT" */
#define SC_SOCK_PATH       "/tmp/scutum.sock"
#define SC_MAX_PAYLOAD     (64u * 1024u * 1024u)
#define SC_BATCH_MAX       (1u * 1024u * 1024u)

#define SC_MAX_FDS         8
#define SC_MAX_THREADS     64
#define SC_MAX_FRAGMENTS   64

/* capabilities (handshake) */
#define SC_CAP_EGL_15       (1u << 0)
#define SC_CAP_GLES_30      (1u << 1)
#define SC_CAP_GLES_31      (1u << 2)
#define SC_CAP_GLES_32      (1u << 3)
#define SC_CAP_FD_PASSING   (1u << 4)
#define SC_CAP_SHM          (1u << 5)   /* bus de comandos por memoria compartida */

/* ============================================================ errores */

enum sc_err_class {
    SC_E_PROTO    = 1,
    SC_E_EGL      = 2,
    SC_E_GL       = 3,
    SC_E_INTERNAL = 4,
};

enum sc_err_proto {
    SC_PERR_FRAMING      = 1,
    SC_PERR_VERSION      = 2,
    SC_PERR_TRUNCATED    = 3,
    SC_PERR_FD_MISSING   = 4,
    SC_PERR_FRAG         = 5,
    SC_PERR_OPCODE       = 6,
    SC_PERR_ARG_TYPE     = 7,
};

/* ============================================================ ops de mensaje */

enum sc_op {
    SC_OP_QUIT          = 0x00,
    SC_OP_PING          = 0x01,   /* payload vacío; resp PONG */
    SC_OP_PONG          = 0x02,   /* payload vacío */
    SC_OP_ERROR         = 0x03,   /* sc_error */

    SC_OP_HELLO         = 0x04,   /* u32 proto_version */
    SC_OP_HELLO_ACK     = 0x05,   /* u32 proto_version, u32 caps, u32 shm_total, u32 ring_size
                               * + 1 fd (memfd de la region compartida) */

    SC_OP_FRAGMENT      = 0x06,   /* u32 frag_id, u32 total, u32 off, u32 len, bytes */

    SC_OP_EGL           = 0x10,   /* u32 egl_op, args... ; resp i32 result + datos */
    SC_OP_GL_BATCH      = 0x11,   /* u32 n_inst, inst[n_inst] ; sin resp */
    SC_OP_GL_SYNC       = 0x12,   /* inst única ; resp i32 result + bytes */
    SC_OP_SYNC_ACK      = 0x13,   /* ack del batch (opcional, ver flush ack) */
    SC_OP_FD            = 0x14,   /* SOLO por socket: header sin payload + 1 fd (SCM_RIGHTS).
                               * Se manda ANTES de publicar en el ring el frame que
                               * lleva SC_MSGF_FD; el daemon lo recibe al leer ese frame. */
};

/* flags en sc_msg.pad */
#define SC_MSGF_FD         (1u << 0)   /* este frame tiene un SC_OP_FD esperando en el socket */

struct sc_msg {
    uint32_t magic;
    uint32_t op;
    uint32_t len;         /* bytes de payload */
    uint32_t req_id;
    uint32_t thread_id;   /* 0 = hilo principal */
    uint32_t pad;         /* reservado, 0 */
};
/* sizeof(sc_msg) = 24, alineado a 8 */

struct sc_error {
    uint32_t class_;
    uint32_t code;
    uint32_t msg_len;
    /* bytes msg[msg_len] siguen */
};

/* ============================================================ EGL ops */

/*
 * Cada op de EGL tiene un layout fijo de request y de respuesta.
 * Formato general:
 *   req  = [u32 egl_op][args...]
 *   resp = [i32 result][datos opcionales]
 * Los "handle" son u64 opacos; los EGLint/EGLenum son u32/i32.
 * Los punteros a arrays (Vk-style) se mandan como: [u32 count][items...].
 * Los punteros a datos variables (strings) se mandan con longitud explícita.
 */
enum sc_egl_op {
    /* display */
    SC_EGL_GET_DISPLAY              = 0x0100,  /* req: u64 native_dpy (0 = DEFAULT)
                                                * resp: u64 display */
    SC_EGL_GET_PLATFORM_DISPLAY     = 0x0101,  /* req: u32 plat_enum, u64 native, u32 nattr, attr[]
                                                * resp: u64 display */
    SC_EGL_INITIALIZE               = 0x0102,  /* req: u64 display
                                                * resp: i32 ok, i32 major, i32 minor */
    SC_EGL_TERMINATE                = 0x0103,  /* req: u64 display; resp: i32 ok */
    SC_EGL_QUERY_STRING             = 0x0104,  /* req: u64 display, i32 name
                                                * resp: u32 len, char[len] (sin NUL) */
    SC_EGL_GET_ERROR                = 0x0105,  /* req: ; resp: i32 egl_error */

    /* config */
    SC_EGL_CHOOSE_CONFIG            = 0x0106,  /* req: u64 display, u32 nattr, i32 attr[], u32 max
                                                * resp: i32 ok, u32 n_out, u64 cfg[n_out] */
    SC_EGL_GET_CONFIGS              = 0x0107,  /* req: u64 display, u32 max
                                                * resp: i32 ok, u32 n_out, u64 cfg[n_out] */
    SC_EGL_GET_CONFIG_ATTRIB        = 0x0108,  /* req: u64 display, u64 cfg, i32 attrib
                                                * resp: i32 ok, i32 value */
    SC_EGL_GET_CONFIGS_ATTRIB       = 0x0128,  /* req: u64 display, u32 n, (u64 cfg,i32 attr)[n]
                                                * resp: i32 ok, (i32 val)[n] */

    /* context */
    SC_EGL_CREATE_CONTEXT           = 0x0109,  /* req: u64 display, u64 cfg, u64 share, u32 nattr, i32 attr[]
                                                * resp: u64 context */
    SC_EGL_DESTROY_CONTEXT          = 0x010A,  /* req: u64 display, u64 context; resp: i32 ok */
    SC_EGL_MAKE_CURRENT             = 0x010B,  /* req: u64 display, u64 draw, u64 read, u64 ctx
                                                * resp: i32 ok */
    SC_EGL_GET_CURRENT_CONTEXT      = 0x0116,  /* req: ; resp: u64 context */
    SC_EGL_GET_CURRENT_SURFACE      = 0x0117,  /* req: i32 readdraw; resp: u64 surface */
    SC_EGL_GET_CURRENT_DISPLAY      = 0x0118,  /* req: ; resp: u64 display */
    SC_EGL_RELEASE_THREAD           = 0x011A,  /* req: ; resp: i32 ok */

    /* surface */
    SC_EGL_CREATE_WINDOW_SURFACE    = 0x010C,  /* req: u64 display, u64 cfg, u64 native_win, u32 nattr, i32 attr[]
                                                *      n_fds, (fd_ref)...
                                                * resp: u64 surface */
    SC_EGL_CREATE_PBUFFER_SURFACE   = 0x010D,  /* req: u64 display, u64 cfg, u32 nattr, i32 attr[]
                                                * resp: u64 surface */
    SC_EGL_CREATE_PIXMAP_SURFACE    = 0x010E,  /* req: u64 display, u64 cfg, u64 native_pixmap, u32 nattr, i32 attr[]
                                                * resp: u64 surface */
    SC_EGL_DESTROY_SURFACE          = 0x010F,  /* req: u64 display, u64 surface; resp: i32 ok */
    SC_EGL_QUERY_SURFACE            = 0x0110,  /* req: u64 display, u64 surface, i32 attrib
                                                * resp: i32 ok, i32 value */
    SC_EGL_SURFACE_ATTRIB           = 0x0111,  /* req: u64 display, u64 surface, i32 attrib, i32 value
                                                * resp: i32 ok */
    SC_EGL_SWAP_BUFFERS             = 0x0112,  /* req: u64 display, u64 surface
                                                *      n_fds, (fd_ref)...
                                                * resp: i32 ok */
    SC_EGL_SWAP_BUFFERS_WITH_DAMAGE = 0x0129,  /* req: display, surface, u32 nrect, rect[]
                                                * resp: i32 ok */
    SC_EGL_PRESENT                  = 0x012B,  /* req: u32 w, u32 h
                                                * resp: i32 ok, bytes[w*h*4] BGRX, filas arriba->abajo
                                                * (el daemon hace readback + flip + swap R/B) */
    SC_EGL_SWAP_INTERVAL            = 0x0113,  /* req: u64 display, i32 interval; resp: i32 ok */
    SC_EGL_COPY_BUFFERS             = 0x0120,  /* req: u64 display, u64 surface, u64 native_pixmap
                                                *      n_fds, (fd_ref)...
                                                * resp: i32 ok */
    SC_EGL_BIND_TEX_IMAGE           = 0x011E,  /* req: u64 display, u64 surface, i32 buffer
                                                * resp: i32 ok */
    SC_EGL_RELEASE_TEX_IMAGE        = 0x011F,  /* req: u64 display, u64 surface, i32 buffer
                                                * resp: i32 ok */

    /* api */
    SC_EGL_BIND_API                 = 0x0114,  /* req: u32 api; resp: i32 ok */
    SC_EGL_QUERY_API                = 0x0115,  /* req: ; resp: u32 api */

    /* proc */
    SC_EGL_GET_PROC_ADDRESS         = 0x0119,  /* req: u32 name_len, char name[]
                                                * resp: u32 known, u32 kind, u32 opcode
                                                * (kind: 0=desconocida, 1=EGL, 2=GL) */

    /* waits */
    SC_EGL_WAIT_CLIENT              = 0x011B,  /* req: ; resp: i32 ok */
    SC_EGL_WAIT_GL                  = 0x011C,  /* req: ; resp: i32 ok */
    SC_EGL_WAIT_NATIVE              = 0x011D,  /* req: u32 engine; resp: i32 ok */

    /* sync (EGL 1.5) */
    SC_EGL_CREATE_SYNC              = 0x0121,  /* req: u64 display, u32 type, u32 nattr, i64 attr[]
                                                * resp: u64 sync */
    SC_EGL_DESTROY_SYNC             = 0x0122,  /* req: u64 display, u64 sync; resp: i32 ok */
    SC_EGL_CLIENT_WAIT_SYNC         = 0x0123,  /* req: u64 display, u64 sync, i32 flags, u64 timeout
                                                * resp: i32 result */
    SC_EGL_WAIT_SYNC                = 0x0124,  /* req: u64 display, u64 sync, i32 flags, u64 timeout
                                                * resp: i32 ok */
    SC_EGL_GET_SYNC_ATTRIB          = 0x0125,  /* req: u64 display, u64 sync, i32 attrib
                                                * resp: i32 ok, i64 value */

    /* native */
    SC_EGL_DUP_NATIVE_DISPLAY       = 0x0126,  /* req: u32 display_id; resp: u32 ok, n_fds, fds */
    SC_EGL_GET_NATIVE_CLIENT_BUFFER = 0x0127,  /* req: u64 display, u64 surface
                                                * resp: u32 ok, n_fds, fds, u64 native, u32 nattr, i32 attr[] */

    /* android surface (para SuperTuxKart, apps que necesitan EGL Android) */
    SC_EGL_GET_ANDROID_SURFACE      = 0x012A,  /* req: u64 display, u64 surface
                                                * resp: u32 ok, u64 native_window (handle Android) */
};

/* ============================================================ GL batch */

/*
 * Un batch es una secuencia autocontenida de instrucciones:
 *   [u32 n_inst]
 *   n_inst × { [u32 fn][u32 argbytes][args[argbytes]] }
 * fn es un SC_GL_* (ver sc_gl_enum.h, generado aparte).
 * argbytes = longitud del payload de argumentos, alineada a 8.
 *
 * Los argumentos de cada función tienen layout fijo, definido por la firma
 * real de GLES (misma que en el header oficial). Tipos:
 *   - enteros y floats: u32/i32/f32 crudos, 4 bytes.
 *   - pointers a datos (const void*): [u32 nbytes][bytes]. Si nbytes == 0,
 *     el puntero era NULL.
 *   - arrays: [u32 count][items...] cuando la firma dice count.
 *   - strings: [u32 len][bytes sin NUL].
 *   - GLsync, EGLImage: u64.
 *
 * El shim conoce la firma real (la incluye de GLES headers), así que emite
 * directamente los bytes en el orden de la firma. El daemon también conoce
 * la firma (mismo header), así que decodifica igual. No hay ambigüedad.
 *
 * GL_BATCH no tiene respuesta: si algo falla en el daemon (glGetError != 0),
 * el error se acumula en un slot por thread y se entrega en el próximo
 * SC_OP_GL_SYNC con la op SC_GL_GET_ERROR.
 */

/* SC_OP_GL_SYNC: una sola instrucción, layout idéntico a una instrucción de
 * batch. El daemon ejecuta y responde [i32 result][bytes]. Para funciones que
 * devuelven void, result==0 y no hay bytes. Para funciones que devuelven un
 * valor escalar, result=0 y bytes = u32/u64. Para funciones que escriben en
 * un buffer del usuario (glGet*, glReadPixels), el shim envía el tamaño y el
 * daemon devuelve los bytes que el shim escribe en el buffer del usuario. */

/* ============================================================ FD passing */

/*
 * Mensajes con fds: sendmsg() con cmsg SCM_RIGHTS anexando hasta SC_MAX_FDS
 * descriptores. El payload incluye un array [u32 fd_ref[n_fds]] que indexa
 * los fds en el orden del cmsg. Si un mensaje no lleva cmsg pero el payload
 * declara n_fds>0, es SC_PERR_FD_MISSING.
 *
 * Al recibir, el daemon hace dup() de cada fd (o los usa directamente si
 * son de vida corta) y cierra los originales tras procesar.
 */

/* ============================================================ API */

const char *sc_sock_path(void);

int sc_connect(const char *path, int timeout_ms);
int sc_write_all(int fd, const void *buf, size_t n);
int sc_read_all(int fd, void *buf, size_t n);

/* send/recv con soporte de fds (0 = sin fds) */
int sc_send_msg_fds(int fd, uint32_t op, uint32_t req, uint32_t thread_id,
                    const void *payload, uint32_t len,
                    const int *fds, uint32_t n_fds);
int sc_recv_msg_fds(int fd, struct sc_msg *h, void *payload, uint32_t cap,
                    int *fds_out, uint32_t *n_fds_inout);

/* wrappers sin fds */
static inline int sc_send_msg(int fd, uint32_t op, uint32_t req,
                              uint32_t thread_id,
                              const void *payload, uint32_t len) {
    return sc_send_msg_fds(fd, op, req, thread_id, payload, len, NULL, 0);
}
static inline int sc_recv_msg(int fd, struct sc_msg *h, void *payload, uint32_t cap) {
    uint32_t n = 0;
    return sc_recv_msg_fds(fd, h, payload, cap, NULL, &n);
}

#ifdef __cplusplus
}
#endif

#endif /* SCUTUM_H */
