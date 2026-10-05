/*
 * scutumd.c — daemon Scutum (bionic, Termux). Carga libEGL/libGLESv2 reales
 * de Android y ejecuta las llamadas que le llegan del shim por socket Unix.
 *
 * Un pthread por cliente: EGL/GLES tienen contexto per-thread; atender todo
 * en un solo hilo rompería apps con múltiples contextos.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <dlfcn.h>

#define GL_GLEXT_PROTOTYPES
#define EGL_EGLEXT_PROTOTYPES
#include <time.h>
#include <sys/mman.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <GLES3/gl3.h>
#include <GLES3/gl31.h>
#include <GLES3/gl32.h>
#include <GLES3/gl3ext.h>

#include "scutum.h"
#include "sc_gl_enum.h"

/* Constantes que faltan en algunos sysroots GLES; mismo valor OpenGL.
 * Nota: GL_UNSIGNED_SHORT_5_6_5_REV (0x8368) colisiona con
 * GL_UNSIGNED_INT_2_10_10_10_REV; GL_BGRA (0x80E1) colisiona con
 * GL_BGRA_EXT. El NDK los define, asi que no metemos fallback aca. */
#ifndef GL_UNSIGNED_INT_10_10_10_2
#define GL_UNSIGNED_INT_10_10_10_2 0x8DF6
#endif

static int g_verbose = 0;
#define LOG(...) do { if (g_verbose) fprintf(stderr, __VA_ARGS__); } while (0)
#define ERR(...) do { fprintf(stderr, __VA_ARGS__); } while (0)

/* ============================================================ carga dinámica */

static void *g_egl_handle, *g_gles_handle;

#define GL_FUNCS(X) \
    X(glActiveTexture) X(glAttachShader) X(glBindAttribLocation) \
    X(glBindBuffer) X(glBindFramebuffer) X(glBindRenderbuffer) \
    X(glBindTexture) X(glBlendColor) X(glBlendEquation) \
    X(glBlendEquationSeparate) X(glBlendFunc) X(glBlendFuncSeparate) \
    X(glBufferData) X(glBufferSubData) X(glCheckFramebufferStatus) \
    X(glClear) X(glClearColor) X(glClearDepthf) X(glClearStencil) \
    X(glColorMask) X(glCompileShader) X(glCompressedTexImage2D) \
    X(glCompressedTexSubImage2D) X(glCopyTexImage2D) X(glCopyTexSubImage2D) \
    X(glCreateProgram) X(glCreateShader) X(glCullFace) X(glDeleteBuffers) \
    X(glDeleteFramebuffers) X(glDeleteProgram) X(glDeleteRenderbuffers) \
    X(glDeleteShader) X(glDeleteTextures) X(glDepthFunc) X(glDepthMask) \
    X(glDepthRangef) X(glDetachShader) X(glDisable) \
    X(glDisableVertexAttribArray) X(glDrawArrays) X(glDrawElements) \
    X(glEnable) X(glEnableVertexAttribArray) X(glFinish) X(glFlush) \
    X(glFramebufferRenderbuffer) X(glFramebufferTexture2D) X(glFrontFace) \
    X(glGenBuffers) X(glGenerateMipmap) X(glGenFramebuffers) \
    X(glGenRenderbuffers) X(glGenTextures) X(glGetActiveAttrib) \
    X(glGetActiveUniform) X(glGetAttachedShaders) X(glGetAttribLocation) \
    X(glGetBooleanv) X(glGetBufferParameteriv) X(glGetError) X(glGetFloatv) \
    X(glGetFramebufferAttachmentParameteriv) X(glGetIntegerv) \
    X(glGetProgramInfoLog) X(glGetProgramiv) X(glGetRenderbufferParameteriv) \
    X(glGetShaderInfoLog) X(glGetShaderiv) X(glGetShaderPrecisionFormat) \
    X(glGetShaderSource) X(glGetString) X(glGetTexParameterfv) \
    X(glGetTexParameteriv) X(glGetUniformfv) X(glGetUniformiv) \
    X(glGetUniformLocation) X(glGetVertexAttribfv) X(glGetVertexAttribiv) \
    X(glGetVertexAttribPointerv) X(glHint) X(glIsBuffer) X(glIsEnabled) \
    X(glIsFramebuffer) X(glIsProgram) X(glIsRenderbuffer) X(glIsShader) \
    X(glIsTexture) X(glLineWidth) X(glLinkProgram) X(glPixelStorei) \
    X(glPolygonOffset) X(glReadPixels) X(glReleaseShaderCompiler) \
    X(glRenderbufferStorage) X(glSampleCoverage) X(glScissor) \
    X(glShaderBinary) X(glShaderSource) X(glStencilFunc) \
    X(glStencilFuncSeparate) X(glStencilMask) X(glStencilMaskSeparate) \
    X(glStencilOp) X(glStencilOpSeparate) X(glTexImage2D) X(glTexParameterf) \
    X(glTexParameterfv) X(glTexParameteri) X(glTexParameteriv) \
    X(glTexSubImage2D) X(glUniform1f) X(glUniform1fv) X(glUniform1i) \
    X(glUniform1iv) X(glUniform2f) X(glUniform2fv) X(glUniform2i) \
    X(glUniform2iv) X(glUniform3f) X(glUniform3fv) X(glUniform3i) \
    X(glUniform3iv) X(glUniform4f) X(glUniform4fv) X(glUniform4i) \
    X(glUniform4iv) X(glUniformMatrix2fv) X(glUniformMatrix3fv) \
    X(glUniformMatrix4fv) X(glUseProgram) X(glValidateProgram) \
    X(glVertexAttrib1f) X(glVertexAttrib1fv) X(glVertexAttrib2f) \
    X(glVertexAttrib2fv) X(glVertexAttrib3f) X(glVertexAttrib3fv) \
    X(glVertexAttrib4f) X(glVertexAttrib4fv) X(glVertexAttribPointer) \
    X(glViewport) \
    X(glReadBuffer) X(glDrawRangeElements) X(glTexImage3D) X(glTexSubImage3D) \
    X(glCopyTexSubImage3D) X(glCompressedTexImage3D) X(glCompressedTexSubImage3D) \
    X(glGenQueries) X(glDeleteQueries) X(glIsQuery) X(glBeginQuery) X(glEndQuery) \
    X(glGetQueryiv) X(glGetQueryObjectuiv) X(glUnmapBuffer) X(glGetBufferPointerv) \
    X(glDrawBuffers) X(glUniformMatrix2x3fv) X(glUniformMatrix3x2fv) \
    X(glUniformMatrix2x4fv) X(glUniformMatrix4x2fv) X(glUniformMatrix3x4fv) \
    X(glUniformMatrix4x3fv) X(glBlitFramebuffer) X(glRenderbufferStorageMultisample) \
    X(glFramebufferTextureLayer) X(glMapBufferRange) X(glFlushMappedBufferRange) \
    X(glBindVertexArray) X(glDeleteVertexArrays) X(glGenVertexArrays) \
    X(glIsVertexArray) X(glGetIntegeri_v) X(glBeginTransformFeedback) \
    X(glEndTransformFeedback) X(glBindBufferRange) X(glBindBufferBase) \
    X(glTransformFeedbackVaryings) X(glGetTransformFeedbackVarying) \
    X(glVertexAttribIPointer) X(glGetVertexAttribIiv) X(glGetVertexAttribIuiv) \
    X(glVertexAttribI4i) X(glVertexAttribI4ui) X(glVertexAttribI4iv) \
    X(glVertexAttribI4uiv) X(glGetUniformuiv) X(glGetFragDataLocation) \
    X(glUniform1ui) X(glUniform2ui) X(glUniform3ui) X(glUniform4ui) \
    X(glUniform1uiv) X(glUniform2uiv) X(glUniform3uiv) X(glUniform4uiv) \
    X(glClearBufferiv) X(glClearBufferuiv) X(glClearBufferfv) X(glClearBufferfi) \
    X(glGetStringi) X(glCopyBufferSubData) X(glGetUniformIndices) \
    X(glGetActiveUniformsiv) X(glGetUniformBlockIndex) X(glGetActiveUniformBlockiv) \
    X(glGetActiveUniformBlockName) X(glUniformBlockBinding) \
    X(glDrawArraysInstanced) X(glDrawElementsInstanced) X(glFenceSync) \
    X(glIsSync) X(glDeleteSync) X(glClientWaitSync) X(glWaitSync) \
    X(glGetInteger64v) X(glGetSynciv) X(glGetInteger64i_v) \
    X(glGetBufferParameteri64v) X(glGenSamplers) X(glDeleteSamplers) \
    X(glIsSampler) X(glBindSampler) X(glSamplerParameteri) X(glSamplerParameteriv) \
    X(glSamplerParameterf) X(glSamplerParameterfv) X(glGetSamplerParameteriv) \
    X(glGetSamplerParameterfv) X(glVertexAttribDivisor) X(glBindTransformFeedback) \
    X(glDeleteTransformFeedbacks) X(glGenTransformFeedbacks) X(glIsTransformFeedback) \
    X(glPauseTransformFeedback) X(glResumeTransformFeedback) X(glDrawTransformFeedback) \
    X(glInvalidateFramebuffer) X(glInvalidateSubFramebuffer) \
    X(glDispatchCompute) X(glDispatchComputeIndirect) X(glFramebufferParameteri) \
    X(glGetFramebufferParameteriv) X(glGetInternalformati64v) \
    X(glInvalidateTexSubImage) X(glInvalidateTexImage) X(glInvalidateBufferSubData) \
    X(glInvalidateBufferData) X(glGetProgramInterfaceiv) X(glGetProgramResourceIndex) \
    X(glGetProgramResourceName) X(glGetProgramResourceiv) \
    X(glGetProgramResourceLocation) X(glGetProgramResourceLocationIndex) \
    X(glShaderStorageBlockBinding) X(glTexBufferRange) X(glTexStorage2DMultisample) \
    X(glTexStorage3DMultisample) X(glTextureView) X(glBindImageTexture) \
    X(glMemoryBarrier) X(glMemoryBarrierByRegion) X(glGetMultisamplefv) \
    X(glSampleMaski) X(glTexStorage2D) X(glTexStorage3D) X(glGetTexLevelParameteriv) \
    X(glGetTexLevelParameterfv) X(glBindVertexBuffer) X(glVertexAttribFormat) \
    X(glVertexAttribIFormat) X(glVertexAttribBinding) X(glVertexBindingDivisor) \
    X(glBlendBarrier) X(glCopyImageSubData) X(glDebugMessageControl) \
    X(glDebugMessageInsert) X(glPushDebugGroup) X(glPopDebugGroup) \
    X(glObjectLabel) X(glGetObjectLabel) X(glObjectPtrLabel) X(glGetObjectPtrLabel) \
    X(glGetPointerv) X(glEnablei) X(glDisablei) X(glBlendEquationi) \
    X(glBlendEquationSeparatei) X(glBlendFunci) X(glBlendFuncSeparatei) \
    X(glColorMaski) X(glIsEnabledi) X(glDrawElementsBaseVertex) \
    X(glDrawRangeElementsBaseVertex) X(glDrawElementsInstancedBaseVertex) \
    X(glDrawArraysIndirect) X(glDrawElementsIndirect) X(glFramebufferTexture) \
    X(glPrimitiveBoundingBox) X(glGetGraphicsResetStatus) X(glReadnPixels) \
    X(glGetnUniformfv) X(glGetnUniformiv) X(glGetnUniformuiv) X(glMinSampleShading) \
    X(glPatchParameteri) X(glTexParameterIiv) X(glTexParameterIuiv) \
    X(glGetTexParameterIiv) X(glGetTexParameterIuiv) X(glSamplerParameterIiv) \
    X(glSamplerParameterIuiv) X(glGetSamplerParameterIiv) X(glGetSamplerParameterIuiv) \
    X(glGetQueryObjectiv) X(glGetQueryObjecti64v) X(glGetQueryObjectui64v) \
    X(glGetQueryBufferObjectiv) X(glGetQueryBufferObjectuiv) \
    X(glGetQueryBufferObjecti64v) X(glGetQueryBufferObjectui64v) \
    X(glGetInternalformativ) X(glTexBuffer) \
    X(glGetProgramBinary) X(glProgramBinary) X(glProgramParameteri) \
    X(glEGLImageTargetTexture2DOES) X(glEGLImageTargetRenderbufferStorageOES) \
    X(glFramebufferTexture2DMultisampleEXT) X(glFramebufferTextureMultiviewOVR) \
    X(glTextureStorage2DEXT) X(glTextureStorage3DEXT) \
    X(glMultiDrawArraysEXT) X(glMultiDrawElementsEXT) \
    X(glMultiDrawArraysIndirectEXT) X(glMultiDrawElementsIndirectEXT) \
    X(glQueryCounterEXT) \
    X(glFramebufferTexture2DDownsampleIMG) X(glFramebufferTextureLayerDownsampleIMG) \
    X(glFramebufferTextureMultisampleMultiviewOVR)

#define DECL_FP(n) static void *p_##n;
GL_FUNCS(DECL_FP)
#undef DECL_FP

#define EGL_FUNCS(X) \
    X(eglGetDisplay) X(eglGetPlatformDisplay) X(eglInitialize) X(eglTerminate) \
    X(eglQueryString) X(eglGetError) X(eglGetConfigs) X(eglChooseConfig) \
    X(eglGetConfigAttrib) X(eglCreateContext) X(eglDestroyContext) \
    X(eglMakeCurrent) X(eglGetCurrentContext) X(eglGetCurrentSurface) \
    X(eglGetCurrentDisplay) X(eglReleaseThread) X(eglCreateWindowSurface) \
    X(eglCreatePbufferSurface) X(eglCreatePixmapSurface) X(eglDestroySurface) \
    X(eglQuerySurface) X(eglSurfaceAttrib) X(eglSwapBuffers) \
    X(eglSwapBuffersWithDamageKHR) X(eglSwapInterval) X(eglCopyBuffers) \
    X(eglBindTexImage) X(eglReleaseTexImage) X(eglBindAPI) X(eglQueryAPI) \
    X(eglGetProcAddress) X(eglWaitClient) X(eglWaitGL) X(eglWaitNative) \
    X(eglCreateSync) X(eglDestroySync) X(eglClientWaitSync) X(eglWaitSync) \
    X(eglGetSyncAttrib)

#define DECL_FPE(n) static void *p_##n;
EGL_FUNCS(DECL_FPE)
#undef DECL_FPE

/* Stub para funciones que el driver no expone (ni por dlsym ni por
 * eglGetProcAddress). Devuelve 0 en el registro de retorno: para funciones
 * void el caller lo ignora, para las que devuelven valor (GLboolean, GLint,
 * GLuint, void*, GLsync) es el "no-op/error" razonable. Evita segfaults. */
static uintptr_t sc_noop_zero(void) { return 0; }

static int load_one(void *h, const char *n, void **out) {
    *out = dlsym(h, n);
    if (!*out && p_eglGetProcAddress)
        *out = ((void*(*)(const char*))p_eglGetProcAddress)(n);
    if (!*out) {
        LOG("[scutumd] %s no disponible, stub instalado\n", n);
        *out = (void*)sc_noop_zero;
        return -1;
    }
    return 0;
}

static int load_libs(void) {
    const char *dir = getenv("SCUTUM_LIBDIR");
    char buf[512];
    void *egl = NULL, *gles = NULL;
    if (dir && *dir) {
        snprintf(buf, sizeof buf, "%s/libEGL.so", dir);
        egl = dlopen(buf, RTLD_NOW | RTLD_GLOBAL);
        snprintf(buf, sizeof buf, "%s/libGLESv2.so", dir);
        gles = dlopen(buf, RTLD_NOW | RTLD_GLOBAL);
    }
    if (!egl)  egl  = dlopen("libEGL.so",    RTLD_NOW | RTLD_GLOBAL);
    if (!gles) gles = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
    if (!egl)  egl  = dlopen("/system/lib64/libEGL.so",    RTLD_NOW | RTLD_GLOBAL);
    if (!gles) gles = dlopen("/system/lib64/libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
    if (!egl)  egl  = dlopen("/system/lib/libEGL.so",      RTLD_NOW | RTLD_GLOBAL);
    if (!gles) gles = dlopen("/system/lib/libGLESv2.so",   RTLD_NOW | RTLD_GLOBAL);
    if (!egl || !gles) {
        ERR("[scutumd] no pude cargar libEGL/libGLESv2: %s\n", dlerror());
        return -1;
    }
    g_egl_handle = egl; g_gles_handle = gles;
/* EGL primero: load_one usa p_eglGetProcAddress como fallback para GL. */
#define LOAD_FPE(n) load_one(g_egl_handle, #n, &p_##n);
    EGL_FUNCS(LOAD_FPE)
#undef LOAD_FPE
#define LOAD_FP(n) load_one(g_gles_handle, #n, &p_##n);
    GL_FUNCS(LOAD_FP)
#undef LOAD_FP
    return 0;
}

/* ============================================================ rd/wr */

struct rd { const uint8_t *p; size_t n, o; };
struct wr { uint8_t *p; size_t n, cap; };

static int rd_u32(struct rd *r, uint32_t *v) { if (r->o + 4 > r->n) return -1; memcpy(v, r->p + r->o, 4); r->o += 4; return 0; }
static int rd_i32(struct rd *r, int32_t *v)  { return rd_u32(r, (uint32_t *)v); }
static int rd_f32(struct rd *r, float *v)    { return rd_u32(r, (uint32_t *)v); }
static int rd_u64(struct rd *r, uint64_t *v) { if (r->o + 8 > r->n) return -1; memcpy(v, r->p + r->o, 8); r->o += 8; return 0; }
static int rd_blob(struct rd *r, const uint8_t **p, uint32_t *len) {
    uint32_t n; if (rd_u32(r, &n)) return -1;
    if (r->o + n > r->n) return -1;
    *p = r->p + r->o; *len = n; r->o += n; return 0;
}

static int wr_reserve(struct wr *w, size_t extra) {
    if (w->cap >= w->n + extra) return 0;
    size_t nc = w->cap ? w->cap : 4096;
    while (nc < w->n + extra) nc *= 2;
    if (nc > SC_MAX_PAYLOAD) return -1;
    uint8_t *np = realloc(w->p, nc); if (!np) return -1;
    w->p = np; w->cap = nc; return 0;
}
static int wr_bytes(struct wr *w, const void *d, size_t n) {
    if (wr_reserve(w, n)) return -1;
    if (n) memcpy(w->p + w->n, d, n);
    w->n += n; return 0;
}
static int wr_u32(struct wr *w, uint32_t v) { return wr_bytes(w, &v, 4); }
static int wr_i32(struct wr *w, int32_t  v) { return wr_bytes(w, &v, 4); }
static int wr_f32(struct wr *w, float    v) { return wr_bytes(w, &v, 4); }
static int wr_u64(struct wr *w, uint64_t v) { return wr_bytes(w, &v, 8); }

/* ============================================================ cliente */

struct client {
    int fd;
    uint32_t thread_id;
    pthread_t th;
    uint8_t *inbuf; size_t inlen, incap;
};

static int c_read_msg(struct client *c, struct sc_msg *h, uint8_t **payload_out,
                      int *fds_out, uint32_t *n_fds_out)
{
    struct iovec iov = { h, sizeof *h };
    char cmsgbuf[CMSG_SPACE(SC_MAX_FDS * sizeof(int))];
    struct msghdr mh; memset(&mh, 0, sizeof mh);
    mh.msg_iov = &iov; mh.msg_iovlen = 1;
    if (fds_out) { mh.msg_control = cmsgbuf; mh.msg_controllen = sizeof cmsgbuf; }
    ssize_t r;
    do { r = recvmsg(c->fd, &mh, MSG_WAITALL); } while (r < 0 && errno == EINTR);
    if (r == 0) return -ECONNRESET;
    if (r < 0) return -errno;
    if ((size_t)r != sizeof *h) return -EPROTO;
    if (fds_out && n_fds_out) {
        uint32_t got = 0;
        for (struct cmsghdr *cm = CMSG_FIRSTHDR(&mh); cm; cm = CMSG_NXTHDR(&mh, cm)) {
            if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS) {
                size_t n = (cm->cmsg_len - CMSG_LEN(0)) / sizeof(int);
                if (n > SC_MAX_FDS) n = SC_MAX_FDS;
                memcpy(fds_out, CMSG_DATA(cm), n * sizeof(int));
                got = (uint32_t)n;
                for (size_t k = n; k < (cm->cmsg_len - CMSG_LEN(0)) / sizeof(int); k++)
                    close(((const int *)CMSG_DATA(cm))[k]);
            }
        }
        *n_fds_out = got;
    }
    if (h->magic != SC_MAGIC) return -EPROTO;
    if (h->len > SC_MAX_PAYLOAD) return -EPROTO;
    if (h->len) {
        if (c->incap < h->len) {
            uint8_t *np = realloc(c->inbuf, h->len);
            if (!np) return -ENOMEM;
            c->inbuf = np; c->incap = h->len;
        }
        size_t want = h->len, got = 0;
        while (got < want) {
            ssize_t rr = read(c->fd, c->inbuf + got, want - got);
            if (rr < 0) { if (errno == EINTR) continue; return -errno; }
            if (rr == 0) return -ECONNRESET;
            got += (size_t)rr;
        }
        c->inlen = want;
        *payload_out = c->inbuf;
    } else {
        c->inlen = 0; *payload_out = NULL;
    }
    return 0;
}

static int c_send_hdr_payload(struct client *c, uint32_t op, uint32_t req,
                              const void *payload, uint32_t len)
{
    struct sc_msg h = { SC_MAGIC, op, len, req, 0, 0 };
    struct iovec iov[2];
    iov[0].iov_base = &h; iov[0].iov_len = sizeof h;
    iov[1].iov_base = (void *)payload; iov[1].iov_len = len;
    struct msghdr mh; memset(&mh, 0, sizeof mh);
    mh.msg_iov = iov; mh.msg_iovlen = len ? 2 : 1;
    ssize_t w;
    do { w = sendmsg(c->fd, &mh, MSG_NOSIGNAL); } while (w < 0 && errno == EINTR);
    if (w < 0) return -errno;
    if ((size_t)w != sizeof h + len) return -EIO;
    return 0;
}

static int c_send_sync(struct client *c, uint32_t req, int32_t result,
                       const void *payload, size_t plen)
{
    uint8_t *buf = malloc(4 + plen);
    if (!buf) return -ENOMEM;
    memcpy(buf, &result, 4);
    if (plen) memcpy(buf + 4, payload, plen);
    int rc = c_send_hdr_payload(c, SC_OP_GL_SYNC, req, buf, (uint32_t)(4 + plen));
    free(buf); return rc;
}

static int c_send_error(struct client *c, uint32_t req, uint32_t klass,
                        uint32_t code, const char *msg)
{
    uint32_t ml = msg ? (uint32_t)strlen(msg) : 0;
    uint8_t *buf = malloc(12 + ml);
    if (!buf) return -ENOMEM;
    memcpy(buf, &klass, 4); memcpy(buf + 4, &code, 4); memcpy(buf + 8, &ml, 4);
    if (ml) memcpy(buf + 12, msg, ml);
    int rc = c_send_hdr_payload(c, SC_OP_ERROR, req, buf, 12 + ml);
    free(buf); return rc;
}

/* ============================================================ GL dispatch */

#define ARG_U32(r, v) do { if (rd_u32((r), &(v))) return -EPROTO; } while (0)
#define ARG_I32(r, v) do { if (rd_i32((r), &(v))) return -EPROTO; } while (0)
#define ARG_F32(r, v) do { if (rd_f32((r), &(v))) return -EPROTO; } while (0)
#define ARG_U64(r, v) do { if (rd_u64((r), &(v))) return -EPROTO; } while (0)

#define FN(name) ((void (*)(void))(p_##name))

/* Helper: calcula tamaño en bytes de un pixel segun format/type, igual que el shim. */
static size_t pixel_size(GLenum fmt, GLenum type) {
    /* Tipos packed: tamano fijo por pixel, independiente del format.
     * Espeja sc_pixel_size() del shim (fix #41). */
    switch (type) {
    case GL_UNSIGNED_SHORT_5_6_5:
    /* GL_UNSIGNED_SHORT_5_6_5_REV tiene el MISMO valor (0x8368) que
     * GL_UNSIGNED_INT_2_10_10_10_REV; no podemos listar ambos. */
    case GL_UNSIGNED_SHORT_4_4_4_4:
    case GL_UNSIGNED_SHORT_5_5_5_1:
        return 2;
    case GL_UNSIGNED_INT_2_10_10_10_REV:
    case GL_UNSIGNED_INT_10_10_10_2:
    case GL_UNSIGNED_INT_24_8:
    case GL_UNSIGNED_INT_5_9_9_9_REV:
        return 4;
    case GL_FLOAT_32_UNSIGNED_INT_24_8_REV:
        return 8;
    default:
        break;
    }
    size_t c;
    switch (fmt) {
    case GL_RED: case GL_RED_INTEGER: case GL_ALPHA:
    case GL_LUMINANCE: case GL_DEPTH_COMPONENT: case GL_STENCIL_INDEX:
        c = 1; break;
    case GL_RG: case GL_LUMINANCE_ALPHA: case GL_DEPTH_STENCIL:
        c = 2; break;
    case GL_RGB: case GL_RGB_INTEGER:
        c = 3; break;
    case GL_RGBA: case GL_RGBA_INTEGER:
    case GL_BGRA_EXT:  /* == GL_BGRA == 0x80E1; 4 canales */
    case GL_RGB10_A2: case GL_RGB10_A2UI:
    case GL_RGBA32F: case GL_RGBA32I: case GL_RGBA32UI:
        c = 4; break;
    default: return 0;
    }
    size_t t;
    switch (type) {
    case GL_UNSIGNED_BYTE: case GL_BYTE: t = 1; break;
    case GL_UNSIGNED_SHORT: case GL_SHORT: case GL_HALF_FLOAT: t = 2; break;
    case GL_UNSIGNED_INT: case GL_INT: case GL_FLOAT: t = 4; break;
    default: return 0;
    }
    return c * t;
}


static uint32_t sc_getv_count(GLenum p) {
    switch (p) {
    case GL_VIEWPORT:                     /* 0x0BA2 */
    case GL_SCISSOR_BOX:                  /* 0x0C10 */
    case GL_COLOR_CLEAR_VALUE:            /* 0x0C22 */
    case GL_COLOR_WRITEMASK:              /* 0x0C23 */
    case GL_BLEND_COLOR:                  /* 0x8005 */
        return 4;
    case GL_MAX_VIEWPORT_DIMS:            /* 0x0D3A */
    case GL_DEPTH_RANGE:                  /* 0x0B70 */
    case GL_ALIASED_POINT_SIZE_RANGE:     /* 0x846D */
    case GL_ALIASED_LINE_WIDTH_RANGE:     /* 0x846E */
        return 2;
    case GL_MAX_COMPUTE_WORK_GROUP_COUNT: /* 0x91BE */
    case GL_MAX_COMPUTE_WORK_GROUP_SIZE:  /* 0x91BF */
        return 3;
    default:
        return 1;
    }
}

static int32_t exec_gl_one(uint32_t op, struct rd *r, struct wr *w) {
    uint32_t u0, u1, u2, u3;
    int32_t  i0, i1, i2, i3;
    float    f0, f1, f2, f3;
    uint64_t q0;
    const uint8_t *blob = NULL; uint32_t blen = 0;

    switch (op) {
    case SC_GL_glActiveTexture: ARG_U32(r,u0); ((void(*)(GLenum))p_glActiveTexture)(u0); return 0;
    case SC_GL_glAttachShader:  ARG_U32(r,u0); ARG_U32(r,u1); ((void(*)(GLuint,GLuint))p_glAttachShader)(u0,u1); return 0;
    case SC_GL_glBindAttribLocation: {
        ARG_U32(r,u0); ARG_U32(r,u1);
        if (rd_blob(r,&blob,&blen)) return -EPROTO;
        char *nm = malloc(blen+1); memcpy(nm,blob,blen); nm[blen]=0;
        ((void(*)(GLuint,GLuint,const GLchar*))p_glBindAttribLocation)(u0,u1,nm);
        free(nm); return 0;
    }
    case SC_GL_glBindBuffer: ARG_U32(r,u0); ARG_U32(r,u1); ((void(*)(GLenum,GLuint))p_glBindBuffer)(u0,u1); return 0;
    case SC_GL_glBindFramebuffer: ARG_U32(r,u0); ARG_U32(r,u1); ((void(*)(GLenum,GLuint))p_glBindFramebuffer)(u0,u1); return 0;
    case SC_GL_glBindRenderbuffer: ARG_U32(r,u0); ARG_U32(r,u1); ((void(*)(GLenum,GLuint))p_glBindRenderbuffer)(u0,u1); return 0;
    case SC_GL_glBindTexture: ARG_U32(r,u0); ARG_U32(r,u1); ((void(*)(GLenum,GLuint))p_glBindTexture)(u0,u1); return 0;
    case SC_GL_glBlendColor: ARG_F32(r,f0); ARG_F32(r,f1); ARG_F32(r,f2); ARG_F32(r,f3); ((void(*)(GLfloat,GLfloat,GLfloat,GLfloat))p_glBlendColor)(f0,f1,f2,f3); return 0;
    case SC_GL_glBlendEquation: ARG_U32(r,u0); ((void(*)(GLenum))p_glBlendEquation)(u0); return 0;
    case SC_GL_glBlendEquationSeparate: ARG_U32(r,u0); ARG_U32(r,u1); ((void(*)(GLenum,GLenum))p_glBlendEquationSeparate)(u0,u1); return 0;
    case SC_GL_glBlendFunc: ARG_U32(r,u0); ARG_U32(r,u1); ((void(*)(GLenum,GLenum))p_glBlendFunc)(u0,u1); return 0;
    case SC_GL_glBlendFuncSeparate: ARG_U32(r,u0); ARG_U32(r,u1); ARG_U32(r,u2); ARG_U32(r,u3); ((void(*)(GLenum,GLenum,GLenum,GLenum))p_glBlendFuncSeparate)(u0,u1,u2,u3); return 0;
    case SC_GL_glBufferData: {
        ARG_U32(r,u0); ARG_U64(r,q0);
        if (rd_blob(r,&blob,&blen)) return -EPROTO;
        ARG_U32(r,u1);
        ((void(*)(GLenum,GLsizeiptr,const void*,GLenum))p_glBufferData)(u0,(GLsizeiptr)q0,blen?blob:NULL,u1);
        return 0;
    }
    case SC_GL_glBufferSubData: {
        ARG_U32(r,u0); ARG_U64(r,q0); uint64_t sz; ARG_U64(r,sz);
        if (rd_blob(r,&blob,&blen)) return -EPROTO;
        ((void(*)(GLenum,GLintptr,GLsizeiptr,const void*))p_glBufferSubData)(u0,(GLintptr)q0,(GLsizeiptr)sz,blen?blob:NULL);
        return 0;
    }
    case SC_GL_glClear: ARG_U32(r,u0); ((void(*)(GLbitfield))p_glClear)(u0); return 0;
    case SC_GL_glClearColor: ARG_F32(r,f0); ARG_F32(r,f1); ARG_F32(r,f2); ARG_F32(r,f3); ((void(*)(GLfloat,GLfloat,GLfloat,GLfloat))p_glClearColor)(f0,f1,f2,f3); return 0;
    case SC_GL_glClearDepthf: ARG_F32(r,f0); ((void(*)(GLfloat))p_glClearDepthf)(f0); return 0;
    case SC_GL_glClearStencil: ARG_I32(r,i0); ((void(*)(GLint))p_glClearStencil)(i0); return 0;
    case SC_GL_glColorMask: ARG_U32(r,u0); ARG_U32(r,u1); ARG_U32(r,u2); ARG_U32(r,u3); ((void(*)(GLboolean,GLboolean,GLboolean,GLboolean))p_glColorMask)(u0,u1,u2,u3); return 0;
    case SC_GL_glCompileShader: {
        ARG_U32(r,u0);
        ((void(*)(GLuint))p_glCompileShader)(u0);
        /* Chequeo sincronico post-compile: solo con SCUTUM_CHECK_PROGRAMS=1.
         * Dos round-trips extra por shader matan el arranque de STK. */
        static int chk_prog = -1;
        if (chk_prog < 0) { const char *e = getenv("SCUTUM_CHECK_PROGRAMS"); chk_prog = (e && *e == '1'); }
        if (chk_prog) {
            GLint ok = 1, srclen = 0;
            ((void(*)(GLuint,GLenum,GLint*))p_glGetShaderiv)(u0, 0x8B81 /*COMPILE_STATUS*/, &ok);
            if (!ok) {
                char log[4096] = {0}; GLsizei ll = 0;
                ((void(*)(GLuint,GLsizei,GLsizei*,GLchar*))p_glGetShaderInfoLog)(u0, sizeof log - 1, &ll, log);
                ((void(*)(GLuint,GLenum,GLint*))p_glGetShaderiv)(u0, 0x8B88 /*SOURCE_LENGTH*/, &srclen);
                char src2[600] = {0}; GLsizei sl = 0;
                ((void(*)(GLuint,GLsizei,GLsizei*,GLchar*))p_glGetShaderSource)(u0, sizeof src2 - 1, &sl, src2);
                ERR("[scutumd] COMPILE FAIL shader=%u srclen=%d loglen=%d\n--- log ---\n%s\n--- src head ---\n%s\n-----\n",
                    u0, srclen, (int)ll, log, src2);
            }
        }
        return 0;
    }
    case SC_GL_glCompressedTexImage2D: {
        ARG_U32(r,u0); ARG_I32(r,i0); ARG_U32(r,u1);
        ARG_I32(r,i1); ARG_I32(r,i2); int32_t b,isz;
        ARG_I32(r,b); ARG_I32(r,isz);
        if (rd_blob(r,&blob,&blen)) return -EPROTO;
        ((void(*)(GLenum,GLint,GLenum,GLsizei,GLsizei,GLint,GLsizei,const void*))p_glCompressedTexImage2D)(
            u0,i0,u1,i1,i2,b,isz,blen?blob:NULL);
        return 0;
    }
    case SC_GL_glCompressedTexSubImage2D: {
        ARG_U32(r,u0); ARG_I32(r,i0); ARG_I32(r,i1); ARG_I32(r,i2);
        ARG_I32(r,i3); int32_t hh,isz; ARG_I32(r,hh); ARG_U32(r,u1); ARG_I32(r,isz);
        if (rd_blob(r,&blob,&blen)) return -EPROTO;
        ((void(*)(GLenum,GLint,GLint,GLint,GLsizei,GLsizei,GLenum,GLsizei,const void*))p_glCompressedTexSubImage2D)(
            u0,i0,i1,i2,i3,hh,u1,isz,blen?blob:NULL);
        return 0;
    }
    case SC_GL_glCopyTexImage2D: {
        ARG_U32(r,u0); ARG_I32(r,i0); ARG_U32(r,u1); ARG_I32(r,i1); ARG_I32(r,i2);
        ARG_I32(r,i3); int32_t hh,b; ARG_I32(r,hh); ARG_I32(r,b);
        ((void(*)(GLenum,GLint,GLenum,GLint,GLint,GLsizei,GLsizei,GLint))p_glCopyTexImage2D)(u0,i0,u1,i1,i2,i3,hh,b);
        return 0;
    }
    case SC_GL_glCopyTexSubImage2D: {
        ARG_U32(r,u0); ARG_I32(r,i0); ARG_I32(r,i1); ARG_I32(r,i2);
        ARG_I32(r,i3); int32_t y,w,h; ARG_I32(r,y); ARG_I32(r,w); ARG_I32(r,h);
        ((void(*)(GLenum,GLint,GLint,GLint,GLint,GLint,GLsizei,GLsizei))p_glCopyTexSubImage2D)(u0,i0,i1,i2,i3,y,w,h);
        return 0;
    }
    case SC_GL_glCullFace: ARG_U32(r,u0); ((void(*)(GLenum))p_glCullFace)(u0); return 0;
    case SC_GL_glDeleteBuffers: case SC_GL_glDeleteFramebuffers:
    case SC_GL_glDeleteRenderbuffers: case SC_GL_glDeleteTextures:
    case SC_GL_glDeleteQueries: case SC_GL_glDeleteVertexArrays:
    case SC_GL_glDeleteSamplers: case SC_GL_glDeleteTransformFeedbacks: {
        uint32_t n; ARG_U32(r,n);
        if (n > 65536) return -EPROTO;
        GLuint *ids = n ? malloc((size_t)n*4) : NULL;
        for (uint32_t i = 0; i < n; i++) { ARG_U32(r,u0); ids[i]=u0; }
        void *fn =
            op==SC_GL_glDeleteBuffers ? p_glDeleteBuffers :
            op==SC_GL_glDeleteFramebuffers ? p_glDeleteFramebuffers :
            op==SC_GL_glDeleteRenderbuffers ? p_glDeleteRenderbuffers :
            op==SC_GL_glDeleteTextures ? p_glDeleteTextures :
            op==SC_GL_glDeleteQueries ? p_glDeleteQueries :
            op==SC_GL_glDeleteVertexArrays ? p_glDeleteVertexArrays :
            op==SC_GL_glDeleteSamplers ? p_glDeleteSamplers :
            p_glDeleteTransformFeedbacks;
        ((void(*)(GLsizei,const GLuint*))fn)((GLsizei)n, ids);
        free(ids); return 0;
    }
    case SC_GL_glDeleteProgram: ARG_U32(r,u0); ((void(*)(GLuint))p_glDeleteProgram)(u0); return 0;
    case SC_GL_glDeleteShader:  ARG_U32(r,u0); ((void(*)(GLuint))p_glDeleteShader)(u0); return 0;
    case SC_GL_glDepthFunc: ARG_U32(r,u0); ((void(*)(GLenum))p_glDepthFunc)(u0); return 0;
    case SC_GL_glDepthMask: ARG_U32(r,u0); ((void(*)(GLboolean))p_glDepthMask)(u0); return 0;
    case SC_GL_glDepthRangef: ARG_F32(r,f0); ARG_F32(r,f1); ((void(*)(GLfloat,GLfloat))p_glDepthRangef)(f0,f1); return 0;
    case SC_GL_glDetachShader: ARG_U32(r,u0); ARG_U32(r,u1); ((void(*)(GLuint,GLuint))p_glDetachShader)(u0,u1); return 0;
    case SC_GL_glDisable: ARG_U32(r,u0); ((void(*)(GLenum))p_glDisable)(u0); return 0;
    case SC_GL_glDisableVertexAttribArray: ARG_U32(r,u0); ((void(*)(GLuint))p_glDisableVertexAttribArray)(u0); return 0;
    case SC_GL_glDrawArrays: ARG_U32(r,u0); ARG_I32(r,i0); ARG_I32(r,i1); ((void(*)(GLenum,GLint,GLsizei))p_glDrawArrays)(u0,i0,i1); return 0;
    case SC_GL_glDrawElements: {
        ARG_U32(r,u0); ARG_I32(r,i0); ARG_U32(r,u1); ARG_U64(r,q0);
        ((void(*)(GLenum,GLsizei,GLenum,const void*))p_glDrawElements)(u0,i0,u1,(const void*)(uintptr_t)q0);
        return 0;
    }
    case SC_GL_glEnable: ARG_U32(r,u0); ((void(*)(GLenum))p_glEnable)(u0); return 0;
    case SC_GL_glEnableVertexAttribArray: ARG_U32(r,u0); ((void(*)(GLuint))p_glEnableVertexAttribArray)(u0); return 0;
    case SC_GL_glFinish: ((void(*)(void))p_glFinish)(); return 0;
    case SC_GL_glFlush:  ((void(*)(void))p_glFlush)();  return 0;
    case SC_GL_glFramebufferRenderbuffer: ARG_U32(r,u0); ARG_U32(r,u1); ARG_U32(r,u2); ARG_U32(r,u3); ((void(*)(GLenum,GLenum,GLenum,GLuint))p_glFramebufferRenderbuffer)(u0,u1,u2,u3); return 0;
    case SC_GL_glFramebufferTexture2D: ARG_U32(r,u0); ARG_U32(r,u1); ARG_U32(r,u2); ARG_U32(r,u3); ARG_I32(r,i0); ((void(*)(GLenum,GLenum,GLenum,GLuint,GLint))p_glFramebufferTexture2D)(u0,u1,u2,u3,i0); return 0;
    case SC_GL_glFrontFace: ARG_U32(r,u0); ((void(*)(GLenum))p_glFrontFace)(u0); return 0;
    case SC_GL_glGenBuffers: case SC_GL_glGenFramebuffers:
    case SC_GL_glGenRenderbuffers: case SC_GL_glGenTextures:
    case SC_GL_glGenQueries: case SC_GL_glGenVertexArrays:
    case SC_GL_glGenSamplers: case SC_GL_glGenTransformFeedbacks: {
        int32_t n; ARG_I32(r,n);
        if (n < 0 || n > 65536) return -EPROTO;
        GLuint *ids = n ? malloc((size_t)n*4) : NULL;
        void *fn =
            op==SC_GL_glGenBuffers ? p_glGenBuffers :
            op==SC_GL_glGenFramebuffers ? p_glGenFramebuffers :
            op==SC_GL_glGenRenderbuffers ? p_glGenRenderbuffers :
            op==SC_GL_glGenTextures ? p_glGenTextures :
            op==SC_GL_glGenQueries ? p_glGenQueries :
            op==SC_GL_glGenVertexArrays ? p_glGenVertexArrays :
            op==SC_GL_glGenSamplers ? p_glGenSamplers :
            p_glGenTransformFeedbacks;
        ((void(*)(GLsizei,GLuint*))fn)((GLsizei)n, ids);
        if (w) { wr_u32(w,(uint32_t)n); for (int32_t i=0;i<n;i++) wr_u32(w,ids[i]); }
        free(ids); return 0;
    }
    case SC_GL_glGenerateMipmap: ARG_U32(r,u0); ((void(*)(GLenum))p_glGenerateMipmap)(u0); return 0;
    case SC_GL_glHint: ARG_U32(r,u0); ARG_U32(r,u1); ((void(*)(GLenum,GLenum))p_glHint)(u0,u1); return 0;
    case SC_GL_glLineWidth: ARG_F32(r,f0); ((void(*)(GLfloat))p_glLineWidth)(f0); return 0;
    case SC_GL_glLinkProgram: {
        ARG_U32(r,u0);
        ((void(*)(GLuint))p_glLinkProgram)(u0);
        static int chk_link = -1;
        if (chk_link < 0) { const char *e = getenv("SCUTUM_CHECK_PROGRAMS"); chk_link = (e && *e == '1'); }
        if (chk_link) {
            GLint ok = 1;
            ((void(*)(GLuint,GLenum,GLint*))p_glGetProgramiv)(u0, 0x8B82 /*LINK_STATUS*/, &ok);
            if (!ok) {
                char log[4096] = {0}; GLsizei ll = 0;
                ((void(*)(GLuint,GLsizei,GLsizei*,GLchar*))p_glGetProgramInfoLog)(u0, sizeof log - 1, &ll, log);
                ERR("[scutumd] LINK FAIL program=%u loglen=%d\n%s\n", u0, (int)ll, log);
            }
        }
        return 0;
    }
    case SC_GL_glPixelStorei: ARG_U32(r,u0); ARG_I32(r,i0); ((void(*)(GLenum,GLint))p_glPixelStorei)(u0,i0); return 0;
    case SC_GL_glPolygonOffset: ARG_F32(r,f0); ARG_F32(r,f1); ((void(*)(GLfloat,GLfloat))p_glPolygonOffset)(f0,f1); return 0;
    case SC_GL_glReleaseShaderCompiler: ((void(*)(void))p_glReleaseShaderCompiler)(); return 0;
    case SC_GL_glRenderbufferStorage: ARG_U32(r,u0); ARG_U32(r,u1); ARG_I32(r,i0); ARG_I32(r,i1); ((void(*)(GLenum,GLenum,GLsizei,GLsizei))p_glRenderbufferStorage)(u0,u1,i0,i1); return 0;
    case SC_GL_glSampleCoverage: ARG_F32(r,f0); ARG_U32(r,u0); ((void(*)(GLfloat,GLboolean))p_glSampleCoverage)(f0,u0); return 0;
    case SC_GL_glScissor: ARG_I32(r,i0); ARG_I32(r,i1); ARG_I32(r,i2); ARG_I32(r,i3); ((void(*)(GLint,GLint,GLsizei,GLsizei))p_glScissor)(i0,i1,i2,i3); return 0;
    case SC_GL_glShaderBinary: {
        uint32_t n; ARG_U32(r,n);
        if (n > 4096) return -EPROTO;
        GLuint *sh = n?malloc((size_t)n*4):NULL;
        for (uint32_t i=0;i<n;i++) { ARG_U32(r,u0); sh[i]=u0; }
        ARG_U32(r,u1);
        if (rd_blob(r,&blob,&blen)) { free(sh); return -EPROTO; }
        ((void(*)(GLsizei,const GLuint*,GLenum,const void*,GLsizei))p_glShaderBinary)((GLsizei)n,sh,u1,blob,(GLsizei)blen);
        free(sh); return 0;
    }
    case SC_GL_glShaderSource: {
        ARG_U32(r,u0);
        int32_t cnt; ARG_I32(r,cnt);
        if (cnt < 0 || cnt > 1024) return -EPROTO;
        const GLchar **strs = cnt?malloc(sizeof(GLchar*)*cnt):NULL;
        for (int32_t i=0;i<cnt;i++) {
            if (rd_blob(r,&blob,&blen)) {
                for (int32_t j=0;j<i;j++) free((void*)strs[j]);
                free(strs); return -EPROTO;
            }
            char *s = malloc(blen+1); memcpy(s,blob,blen); s[blen]=0;
            strs[i]=s;
        }
        ((void(*)(GLuint,GLsizei,const GLchar*const*,const GLint*))p_glShaderSource)(u0,cnt,strs,NULL);
        for (int32_t i=0;i<cnt;i++) free((void*)strs[i]);
        free(strs); return 0;
    }
    case SC_GL_glStencilFunc: ARG_U32(r,u0); ARG_I32(r,i0); ARG_U32(r,u1); ((void(*)(GLenum,GLint,GLuint))p_glStencilFunc)(u0,i0,u1); return 0;
    case SC_GL_glStencilFuncSeparate: ARG_U32(r,u0); ARG_U32(r,u1); ARG_I32(r,i0); ARG_U32(r,u2); ((void(*)(GLenum,GLenum,GLint,GLuint))p_glStencilFuncSeparate)(u0,u1,i0,u2); return 0;
    case SC_GL_glStencilMask: ARG_U32(r,u0); ((void(*)(GLuint))p_glStencilMask)(u0); return 0;
    case SC_GL_glStencilMaskSeparate: ARG_U32(r,u0); ARG_U32(r,u1); ((void(*)(GLenum,GLuint))p_glStencilMaskSeparate)(u0,u1); return 0;
    case SC_GL_glStencilOp: ARG_U32(r,u0); ARG_U32(r,u1); ARG_U32(r,u2); ((void(*)(GLenum,GLenum,GLenum))p_glStencilOp)(u0,u1,u2); return 0;
    case SC_GL_glStencilOpSeparate: ARG_U32(r,u0); ARG_U32(r,u1); ARG_U32(r,u2); ARG_U32(r,u3); ((void(*)(GLenum,GLenum,GLenum,GLenum))p_glStencilOpSeparate)(u0,u1,u2,u3); return 0;
    case SC_GL_glTexImage2D: {
        ARG_U32(r,u0); ARG_I32(r,i0); ARG_I32(r,i1);
        ARG_I32(r,i2); int32_t hh,b; ARG_I32(r,hh); ARG_I32(r,b);
        ARG_U32(r,u1); ARG_U32(r,u2);
        if (rd_blob(r,&blob,&blen)) return -EPROTO;
        ((void(*)(GLenum,GLint,GLint,GLsizei,GLsizei,GLint,GLenum,GLenum,const void*))p_glTexImage2D)(
            u0,i0,i1,i2,hh,b,u1,u2,blen?blob:NULL);
        return 0;
    }
    case SC_GL_glTexSubImage2D: {
        ARG_U32(r,u0); ARG_I32(r,i0); ARG_I32(r,i1); ARG_I32(r,i2);
        ARG_I32(r,i3); int32_t hh; ARG_I32(r,hh); ARG_U32(r,u1); ARG_U32(r,u2);
        if (rd_blob(r,&blob,&blen)) return -EPROTO;
        ((void(*)(GLenum,GLint,GLint,GLint,GLsizei,GLsizei,GLenum,GLenum,const void*))p_glTexSubImage2D)(
            u0,i0,i1,i2,i3,hh,u1,u2,blen?blob:NULL);
        return 0;
    }
    case SC_GL_glTexParameterf: ARG_U32(r,u0); ARG_U32(r,u1); ARG_F32(r,f0); ((void(*)(GLenum,GLenum,GLfloat))p_glTexParameterf)(u0,u1,f0); return 0;
    case SC_GL_glTexParameterfv: { ARG_U32(r,u0); ARG_U32(r,u1); ARG_F32(r,f0); ((void(*)(GLenum,GLenum,const GLfloat*))p_glTexParameterfv)(u0,u1,&f0); return 0; }
    case SC_GL_glTexParameteri: ARG_U32(r,u0); ARG_U32(r,u1); ARG_I32(r,i0); ((void(*)(GLenum,GLenum,GLint))p_glTexParameteri)(u0,u1,i0); return 0;
    case SC_GL_glTexParameteriv: { ARG_U32(r,u0); ARG_U32(r,u1); ARG_I32(r,i0); ((void(*)(GLenum,GLenum,const GLint*))p_glTexParameteriv)(u0,u1,&i0); return 0; }

    case SC_GL_glUniform1f: ARG_I32(r,i0); ARG_F32(r,f0); ((void(*)(GLint,GLfloat))p_glUniform1f)(i0,f0); return 0;
    case SC_GL_glUniform2f: ARG_I32(r,i0); ARG_F32(r,f0); ARG_F32(r,f1); ((void(*)(GLint,GLfloat,GLfloat))p_glUniform2f)(i0,f0,f1); return 0;
    case SC_GL_glUniform3f: ARG_I32(r,i0); ARG_F32(r,f0); ARG_F32(r,f1); ARG_F32(r,f2); ((void(*)(GLint,GLfloat,GLfloat,GLfloat))p_glUniform3f)(i0,f0,f1,f2); return 0;
    case SC_GL_glUniform4f: ARG_I32(r,i0); ARG_F32(r,f0); ARG_F32(r,f1); ARG_F32(r,f2); ARG_F32(r,f3); ((void(*)(GLint,GLfloat,GLfloat,GLfloat,GLfloat))p_glUniform4f)(i0,f0,f1,f2,f3); return 0;
    case SC_GL_glUniform1i: ARG_I32(r,i0); ARG_I32(r,i1); ((void(*)(GLint,GLint))p_glUniform1i)(i0,i1); return 0;
    case SC_GL_glUniform2i: ARG_I32(r,i0); ARG_I32(r,i1); ARG_I32(r,i2); ((void(*)(GLint,GLint,GLint))p_glUniform2i)(i0,i1,i2); return 0;
    case SC_GL_glUniform3i: ARG_I32(r,i0); ARG_I32(r,i1); ARG_I32(r,i2); ARG_I32(r,i3); ((void(*)(GLint,GLint,GLint,GLint))p_glUniform3i)(i0,i1,i2,i3); return 0;
    case SC_GL_glUniform4i: { ARG_I32(r,i0); ARG_I32(r,i1); ARG_I32(r,i2); ARG_I32(r,i3); int32_t i4; ARG_I32(r,i4);
        ((void(*)(GLint,GLint,GLint,GLint,GLint))p_glUniform4i)(i0,i1,i2,i3,i4); return 0; }

    case SC_GL_glUniform1fv: case SC_GL_glUniform2fv:
    case SC_GL_glUniform3fv: case SC_GL_glUniform4fv: {
        int32_t loc,n; ARG_I32(r,loc); ARG_I32(r,n);
        if (n < 0 || n > 65536) return -EPROTO;
        int comp = op==SC_GL_glUniform1fv?1:op==SC_GL_glUniform2fv?2:op==SC_GL_glUniform3fv?3:4;
        GLfloat *v = n?malloc((size_t)n*comp*4):NULL;
        for (int32_t i=0;i<n*comp;i++) { ARG_F32(r,f0); v[i]=f0; }
        void *fn = op==SC_GL_glUniform1fv?p_glUniform1fv:op==SC_GL_glUniform2fv?p_glUniform2fv:op==SC_GL_glUniform3fv?p_glUniform3fv:p_glUniform4fv;
        ((void(*)(GLint,GLsizei,const GLfloat*))fn)(loc,n,v);
        free(v); return 0;
    }
    case SC_GL_glUniform1iv: case SC_GL_glUniform2iv:
    case SC_GL_glUniform3iv: case SC_GL_glUniform4iv: {
        int32_t loc,n; ARG_I32(r,loc); ARG_I32(r,n);
        if (n < 0 || n > 65536) return -EPROTO;
        int comp = op==SC_GL_glUniform1iv?1:op==SC_GL_glUniform2iv?2:op==SC_GL_glUniform3iv?3:4;
        GLint *v = n?malloc((size_t)n*comp*4):NULL;
        for (int32_t i=0;i<n*comp;i++) { ARG_I32(r,i0); v[i]=i0; }
        void *fn = op==SC_GL_glUniform1iv?p_glUniform1iv:op==SC_GL_glUniform2iv?p_glUniform2iv:op==SC_GL_glUniform3iv?p_glUniform3iv:p_glUniform4iv;
        ((void(*)(GLint,GLsizei,const GLint*))fn)(loc,n,v);
        free(v); return 0;
    }
    case SC_GL_glUniformMatrix2fv: case SC_GL_glUniformMatrix3fv:
    case SC_GL_glUniformMatrix4fv: {
        int32_t loc,n; uint32_t tr; ARG_I32(r,loc); ARG_I32(r,n); ARG_U32(r,tr);
        if (n < 0 || n > 65536) return -EPROTO;
        int dim = op==SC_GL_glUniformMatrix2fv?2:op==SC_GL_glUniformMatrix3fv?3:4;
        GLfloat *v = n?malloc((size_t)n*dim*dim*4):NULL;
        for (int32_t i=0;i<n*dim*dim;i++) { ARG_F32(r,f0); v[i]=f0; }
        void *fn = op==SC_GL_glUniformMatrix2fv?p_glUniformMatrix2fv:op==SC_GL_glUniformMatrix3fv?p_glUniformMatrix3fv:p_glUniformMatrix4fv;
        ((void(*)(GLint,GLsizei,GLboolean,const GLfloat*))fn)(loc,n,(GLboolean)tr,v);
        free(v); return 0;
    }
    case SC_GL_glUniformMatrix2x3fv: case SC_GL_glUniformMatrix3x2fv:
    case SC_GL_glUniformMatrix2x4fv: case SC_GL_glUniformMatrix4x2fv:
    case SC_GL_glUniformMatrix3x4fv: case SC_GL_glUniformMatrix4x3fv: {
        int32_t loc,n; uint32_t tr; ARG_I32(r,loc); ARG_I32(r,n); ARG_U32(r,tr);
        if (n < 0 || n > 65536) return -EPROTO;
        int dim = (op==SC_GL_glUniformMatrix2x3fv||op==SC_GL_glUniformMatrix3x2fv)?6:
                  (op==SC_GL_glUniformMatrix2x4fv||op==SC_GL_glUniformMatrix4x2fv)?8:12;
        GLfloat *v = n?malloc((size_t)n*dim*4):NULL;
        for (int32_t i=0;i<n*dim;i++) { ARG_F32(r,f0); v[i]=f0; }
        void *fn =
            op==SC_GL_glUniformMatrix2x3fv?p_glUniformMatrix2x3fv:
            op==SC_GL_glUniformMatrix3x2fv?p_glUniformMatrix3x2fv:
            op==SC_GL_glUniformMatrix2x4fv?p_glUniformMatrix2x4fv:
            op==SC_GL_glUniformMatrix4x2fv?p_glUniformMatrix4x2fv:
            op==SC_GL_glUniformMatrix3x4fv?p_glUniformMatrix3x4fv:
                                           p_glUniformMatrix4x3fv;
        ((void(*)(GLint,GLsizei,GLboolean,const GLfloat*))fn)(loc,n,(GLboolean)tr,v);
        free(v); return 0;
    }
    case SC_GL_glUseProgram: ARG_U32(r,u0); ((void(*)(GLuint))p_glUseProgram)(u0); return 0;
    case SC_GL_glValidateProgram: ARG_U32(r,u0); ((void(*)(GLuint))p_glValidateProgram)(u0); return 0;
    case SC_GL_glVertexAttrib1f: { ARG_U32(r,u0); ARG_F32(r,f0); ((void(*)(GLuint,GLfloat))p_glVertexAttrib1f)(u0,f0); return 0; }
    case SC_GL_glVertexAttrib2f: { ARG_U32(r,u0); ARG_F32(r,f0); ARG_F32(r,f1); ((void(*)(GLuint,GLfloat,GLfloat))p_glVertexAttrib2f)(u0,f0,f1); return 0; }
    case SC_GL_glVertexAttrib3f: { ARG_U32(r,u0); ARG_F32(r,f0); ARG_F32(r,f1); ARG_F32(r,f2); ((void(*)(GLuint,GLfloat,GLfloat,GLfloat))p_glVertexAttrib3f)(u0,f0,f1,f2); return 0; }
    case SC_GL_glVertexAttrib4f: { ARG_U32(r,u0); ARG_F32(r,f0); ARG_F32(r,f1); ARG_F32(r,f2); ARG_F32(r,f3); ((void(*)(GLuint,GLfloat,GLfloat,GLfloat,GLfloat))p_glVertexAttrib4f)(u0,f0,f1,f2,f3); return 0; }
    case SC_GL_glVertexAttrib1fv: { ARG_U32(r,u0); GLfloat v[1]; for (int i=0;i<1;i++) ARG_F32(r,v[i]); ((void(*)(GLuint,const GLfloat*))p_glVertexAttrib1fv)(u0,v); return 0; }
    case SC_GL_glVertexAttrib2fv: { ARG_U32(r,u0); GLfloat v[2]; for (int i=0;i<2;i++) ARG_F32(r,v[i]); ((void(*)(GLuint,const GLfloat*))p_glVertexAttrib2fv)(u0,v); return 0; }
    case SC_GL_glVertexAttrib3fv: { ARG_U32(r,u0); GLfloat v[3]; for (int i=0;i<3;i++) ARG_F32(r,v[i]); ((void(*)(GLuint,const GLfloat*))p_glVertexAttrib3fv)(u0,v); return 0; }
    case SC_GL_glVertexAttrib4fv: { ARG_U32(r,u0); GLfloat v[4]; for (int i=0;i<4;i++) ARG_F32(r,v[i]); ((void(*)(GLuint,const GLfloat*))p_glVertexAttrib4fv)(u0,v); return 0; }
    case SC_GL_glVertexAttribPointer: {
        ARG_U32(r,u0); ARG_I32(r,i0); ARG_U32(r,u1); ARG_U32(r,u2);
        ARG_I32(r,i1); ARG_U64(r,q0);
        ((void(*)(GLuint,GLint,GLenum,GLboolean,GLsizei,const void*))p_glVertexAttribPointer)(
            u0,i0,u1,u2,i1,(const void*)(uintptr_t)q0);
        return 0;
    }
    case SC_GL_glViewport: ARG_I32(r,i0); ARG_I32(r,i1); ARG_I32(r,i2); ARG_I32(r,i3); ((void(*)(GLint,GLint,GLsizei,GLsizei))p_glViewport)(i0,i1,i2,i3); return 0;

    /* ---------- sync con retorno ---------- */
    case SC_GL_glCheckFramebufferStatus: ARG_U32(r,u0); return (int32_t)((GLenum(*)(GLenum))p_glCheckFramebufferStatus)(u0);
    case SC_GL_glCreateProgram: return (int32_t)((GLuint(*)(void))p_glCreateProgram)();
    case SC_GL_glCreateShader:  ARG_U32(r,u0); return (int32_t)((GLuint(*)(GLenum))p_glCreateShader)(u0);

    case SC_GL_glGetActiveAttrib: case SC_GL_glGetActiveUniform: {
        ARG_U32(r,u0); ARG_U32(r,u1); ARG_I32(r,i0);
        GLsizei bufSize = i0 < 1 ? 1 : i0 > 65536 ? 65536 : i0;
        GLsizei len=0; GLint sz=0; GLenum ty=0;
        char *nm = malloc((size_t)bufSize);
        void *fn = op==SC_GL_glGetActiveAttrib?p_glGetActiveAttrib:p_glGetActiveUniform;
        ((void(*)(GLuint,GLuint,GLsizei,GLsizei*,GLint*,GLenum*,GLchar*))fn)(u0,u1,bufSize,&len,&sz,&ty,nm);
        if (w) { wr_i32(w,sz); wr_u32(w,(uint32_t)ty); wr_u32(w,(uint32_t)len); if (len>0) wr_bytes(w,nm,(size_t)len); }
        free(nm); return 0;
    }
    case SC_GL_glGetAttachedShaders: {
        ARG_U32(r,u0); ARG_I32(r,i0);
        GLsizei maxCount = i0 < 0 ? 0 : i0 > 65536 ? 65536 : i0;
        GLuint *ids = maxCount?malloc((size_t)maxCount*4):NULL;
        GLsizei cnt = 0;
        ((void(*)(GLuint,GLsizei,GLsizei*,GLuint*))p_glGetAttachedShaders)(u0,maxCount,&cnt,ids);
        if (w) { wr_u32(w,(uint32_t)cnt); for (GLsizei i=0;i<cnt;i++) wr_u32(w,ids[i]); }
        free(ids); return 0;
    }
    case SC_GL_glGetAttribLocation: {
        ARG_U32(r,u0); if (rd_blob(r,&blob,&blen)) return -EPROTO;
        char *nm = malloc(blen+1); memcpy(nm,blob,blen); nm[blen]=0;
        GLint rv = ((GLint(*)(GLuint,const GLchar*))p_glGetAttribLocation)(u0,nm);
        free(nm); return rv;
    }
    case SC_GL_glGetBooleanv: {
        ARG_U32(r,u0);
        GLboolean tmp[256] = {0};
        ((void(*)(GLenum,GLboolean*))p_glGetBooleanv)(u0,tmp);
        if (w) { uint32_t n=(uint32_t)sc_getv_count((GLenum)u0); wr_u32(w,n); for (uint32_t i=0;i<n;i++) wr_u32(w,tmp[i]); }
        return 0;
    }
    case SC_GL_glGetFloatv: {
        ARG_U32(r,u0); GLfloat tmp[256] = {0};
        ((void(*)(GLenum,GLfloat*))p_glGetFloatv)(u0,tmp);
        if (w) { uint32_t n=(uint32_t)sc_getv_count((GLenum)u0); wr_u32(w,n); for (uint32_t i=0;i<n;i++) wr_f32(w,tmp[i]); }
        return 0;
    }
        case SC_GL_glGetIntegerv: {
        ARG_U32(r,u0); GLint tmp[256] = {0};
        ((void(*)(GLenum,GLint*))p_glGetIntegerv)(u0,tmp);
        if (w) {
            uint32_t n;
            if (u0 == GL_COMPRESSED_TEXTURE_FORMATS) {
                /* la cantidad real la dicta NUM_COMPRESSED_TEXTURE_FORMATS */
                GLint cnt = 0;
                ((void(*)(GLenum,GLint*))p_glGetIntegerv)(GL_NUM_COMPRESSED_TEXTURE_FORMATS, &cnt);
                if (cnt < 0) cnt = 0;
                if (cnt > 256) cnt = 256;
                n = (uint32_t)cnt;
            } else if (u0 == 0x87FF /*GL_PROGRAM_BINARY_FORMATS(_OES)*/) {
                GLint cnt = 0;
                ((void(*)(GLenum,GLint*))p_glGetIntegerv)(0x87FE /*NUM_PROGRAM_BINARY_FORMATS*/, &cnt);
                if (cnt < 0) cnt = 0;
                if (cnt > 256) cnt = 256;
                n = (uint32_t)cnt;
            } else {
                n = sc_getv_count((GLenum)u0);
            }
            wr_u32(w, n);
            for (uint32_t i = 0; i < n; i++) wr_i32(w, tmp[i]);
        }
        return 0;
    }
    case SC_GL_glGetBufferParameteriv: { ARG_U32(r,u0); ARG_U32(r,u1); GLint v=0; ((void(*)(GLenum,GLenum,GLint*))p_glGetBufferParameteriv)(u0,u1,&v); if (w) wr_i32(w,v); return 0; }
    case SC_GL_glGetError: return (int32_t)((GLenum(*)(void))p_glGetError)();
    case SC_GL_glGetFramebufferAttachmentParameteriv: { ARG_U32(r,u0); ARG_U32(r,u1); ARG_U32(r,u2); GLint v=0; ((void(*)(GLenum,GLenum,GLenum,GLint*))p_glGetFramebufferAttachmentParameteriv)(u0,u1,u2,&v); if (w) wr_i32(w,v); return 0; }
    case SC_GL_glGetProgramiv: { ARG_U32(r,u0); ARG_U32(r,u1); GLint v=0; ((void(*)(GLuint,GLenum,GLint*))p_glGetProgramiv)(u0,u1,&v); if (w) wr_i32(w,v); return 0; }
    case SC_GL_glGetShaderiv: { ARG_U32(r,u0); ARG_U32(r,u1); GLint v=0; ((void(*)(GLuint,GLenum,GLint*))p_glGetShaderiv)(u0,u1,&v); if (w) wr_i32(w,v); return 0; }
    case SC_GL_glGetRenderbufferParameteriv: { ARG_U32(r,u0); ARG_U32(r,u1); GLint v=0; ((void(*)(GLenum,GLenum,GLint*))p_glGetRenderbufferParameteriv)(u0,u1,&v); if (w) wr_i32(w,v); return 0; }
    case SC_GL_glGetShaderPrecisionFormat: { ARG_U32(r,u0); ARG_U32(r,u1); GLint rg[2]={0,0}, pc=0; ((void(*)(GLenum,GLenum,GLint*,GLint*))p_glGetShaderPrecisionFormat)(u0,u1,rg,&pc); if (w) { wr_i32(w,rg[0]); wr_i32(w,rg[1]); } return 0; }
    case SC_GL_glGetShaderSource: {
        ARG_U32(r,u0); ARG_I32(r,i0);
        GLsizei bufSize = i0<1?1:i0>65536?65536:i0;
        GLsizei len = 0;
        char *src = malloc((size_t)bufSize);
        ((void(*)(GLuint,GLsizei,GLsizei*,GLchar*))p_glGetShaderSource)(u0,bufSize,&len,src);
        if (w) { wr_u32(w,(uint32_t)len); if (len>0) wr_bytes(w,src,(size_t)len); }
        free(src); return 0;
    }
    case SC_GL_glGetProgramInfoLog: case SC_GL_glGetShaderInfoLog: {
        ARG_U32(r,u0); ARG_I32(r,i0);
        GLsizei bufSize = i0<1?1:i0>65536?65536:i0;
        GLsizei len = 0;
        char *log = malloc((size_t)bufSize);
        if (op==SC_GL_glGetProgramInfoLog)
            ((void(*)(GLuint,GLsizei,GLsizei*,GLchar*))p_glGetProgramInfoLog)(u0,bufSize,&len,log);
        else
            ((void(*)(GLuint,GLsizei,GLsizei*,GLchar*))p_glGetShaderInfoLog)(u0,bufSize,&len,log);
        if (w) { wr_u32(w,(uint32_t)len); if (len>0) wr_bytes(w,log,(size_t)len); }
        free(log); return 0;
    }
    case SC_GL_glGetString: {
        ARG_U32(r,u0);
        const GLubyte *s = ((const GLubyte*(*)(GLenum))p_glGetString)(u0);
        if (w) { if (!s) wr_u32(w,0); else { size_t n=strlen((const char*)s); wr_u32(w,(uint32_t)n); wr_bytes(w,s,n); } }
        return 0;
    }
    case SC_GL_glGetStringi: {
        ARG_U32(r,u0); ARG_U32(r,u1);
        const GLubyte *s = ((const GLubyte*(*)(GLenum,GLuint))p_glGetStringi)(u0,u1);
        if (w) { if (!s) wr_u32(w,0); else { size_t n=strlen((const char*)s); wr_u32(w,(uint32_t)n); wr_bytes(w,s,n); } }
        return 0;
    }
    case SC_GL_glGetTexParameterfv: { ARG_U32(r,u0); ARG_U32(r,u1); GLfloat v=0; ((void(*)(GLenum,GLenum,GLfloat*))p_glGetTexParameterfv)(u0,u1,&v); if (w) wr_f32(w,v); return 0; }
    case SC_GL_glGetTexParameteriv: { ARG_U32(r,u0); ARG_U32(r,u1); GLint v=0; ((void(*)(GLenum,GLenum,GLint*))p_glGetTexParameteriv)(u0,u1,&v); if (w) wr_i32(w,v); return 0; }
    case SC_GL_glGetUniformfv: { ARG_U32(r,u0); ARG_I32(r,i0); GLfloat v=0; ((void(*)(GLuint,GLint,GLfloat*))p_glGetUniformfv)(u0,i0,&v); if (w) wr_f32(w,v); return 0; }
    case SC_GL_glGetUniformiv: { ARG_U32(r,u0); ARG_I32(r,i0); GLint v=0; ((void(*)(GLuint,GLint,GLint*))p_glGetUniformiv)(u0,i0,&v); if (w) wr_i32(w,v); return 0; }
    case SC_GL_glGetUniformLocation: {
        ARG_U32(r,u0); if (rd_blob(r,&blob,&blen)) return -EPROTO;
        char *nm = malloc(blen+1); memcpy(nm,blob,blen); nm[blen]=0;
        GLint rv = ((GLint(*)(GLuint,const GLchar*))p_glGetUniformLocation)(u0,nm);
        free(nm); return rv;
    }
    case SC_GL_glGetVertexAttribfv: { ARG_U32(r,u0); ARG_U32(r,u1); GLfloat v=0; ((void(*)(GLuint,GLenum,GLfloat*))p_glGetVertexAttribfv)(u0,u1,&v); if (w) wr_f32(w,v); return 0; }
    case SC_GL_glGetVertexAttribiv: { ARG_U32(r,u0); ARG_U32(r,u1); GLint v=0; ((void(*)(GLuint,GLenum,GLint*))p_glGetVertexAttribiv)(u0,u1,&v); if (w) wr_i32(w,v); return 0; }
    case SC_GL_glGetVertexAttribPointerv: { ARG_U32(r,u0); ARG_U32(r,u1); void *p=NULL; ((void(*)(GLuint,GLenum,void**))p_glGetVertexAttribPointerv)(u0,u1,&p); if (w) wr_u64(w,(uint64_t)(uintptr_t)p); return 0; }
    case SC_GL_glIsBuffer: ARG_U32(r,u0); return (int32_t)((GLboolean(*)(GLuint))p_glIsBuffer)(u0);
    case SC_GL_glIsEnabled: ARG_U32(r,u0); return (int32_t)((GLboolean(*)(GLenum))p_glIsEnabled)(u0);
    case SC_GL_glIsFramebuffer: ARG_U32(r,u0); return (int32_t)((GLboolean(*)(GLuint))p_glIsFramebuffer)(u0);
    case SC_GL_glIsProgram: ARG_U32(r,u0); return (int32_t)((GLboolean(*)(GLuint))p_glIsProgram)(u0);
    case SC_GL_glIsRenderbuffer: ARG_U32(r,u0); return (int32_t)((GLboolean(*)(GLuint))p_glIsRenderbuffer)(u0);
    case SC_GL_glIsShader: ARG_U32(r,u0); return (int32_t)((GLboolean(*)(GLuint))p_glIsShader)(u0);
    case SC_GL_glIsTexture: ARG_U32(r,u0); return (int32_t)((GLboolean(*)(GLuint))p_glIsTexture)(u0);
    case SC_GL_glReadPixels: {
        ARG_I32(r,i0); ARG_I32(r,i1); ARG_I32(r,i2); ARG_I32(r,i3);
        ARG_U32(r,u0); ARG_U32(r,u1);
        size_t ps = pixel_size((GLenum)u0,(GLenum)u1);
        if (!ps) ps = 4;
        if (i2 < 0 || i3 < 0) return -EPROTO;
        size_t total = ps * (size_t)i2 * (size_t)i3;
        if (total > SC_MAX_PAYLOAD - 8) return -EMSGSIZE;
        uint8_t *pix = total ? malloc(total) : NULL;
        ((void(*)(GLint,GLint,GLsizei,GLsizei,GLenum,GLenum,void*))p_glReadPixels)(i0,i1,i2,i3,u0,u1,pix);
        if (w && total) wr_bytes(w,pix,total);
        free(pix); return 0;
    }

    /* ---------- ES 3.0 ---------- */
    case SC_GL_glReadBuffer: ARG_U32(r,u0); ((void(*)(GLenum))p_glReadBuffer)(u0); return 0;
    case SC_GL_glDrawRangeElements: { ARG_U32(r,u0); ARG_U32(r,u1); ARG_U32(r,u2); ARG_I32(r,i0); ARG_U32(r,u3); ARG_U64(r,q0);
        ((void(*)(GLenum,GLuint,GLuint,GLsizei,GLenum,const void*))p_glDrawRangeElements)(u0,u1,u2,i0,u3,(const void*)(uintptr_t)q0); return 0; }
    case SC_GL_glTexImage3D: {
        ARG_U32(r,u0); ARG_I32(r,i0); ARG_I32(r,i1); ARG_I32(r,i2); ARG_I32(r,i3);
        int32_t d,b; ARG_I32(r,d); ARG_I32(r,b);
        ARG_U32(r,u1); ARG_U32(r,u2);
        if (rd_blob(r,&blob,&blen)) return -EPROTO;
        ((void(*)(GLenum,GLint,GLint,GLsizei,GLsizei,GLsizei,GLint,GLenum,GLenum,const void*))p_glTexImage3D)(
            u0,i0,i1,i2,i3,d,b,u1,u2,blen?blob:NULL);
        return 0;
    }
    case SC_GL_glTexSubImage3D: {
        ARG_U32(r,u0); ARG_I32(r,i0); ARG_I32(r,i1); ARG_I32(r,i2); ARG_I32(r,i3);
        int32_t w2,h2,d2; ARG_I32(r,w2); ARG_I32(r,h2); ARG_I32(r,d2);
        ARG_U32(r,u1); ARG_U32(r,u2);
        if (rd_blob(r,&blob,&blen)) return -EPROTO;
        ((void(*)(GLenum,GLint,GLint,GLint,GLint,GLsizei,GLsizei,GLsizei,GLenum,GLenum,const void*))p_glTexSubImage3D)(
            u0,i0,i1,i2,i3,w2,h2,d2,u1,u2,blen?blob:NULL);
        return 0;
    }
    case SC_GL_glCopyTexSubImage3D: { ARG_U32(r,u0); ARG_I32(r,i0); ARG_I32(r,i1); ARG_I32(r,i2); ARG_I32(r,i3);
        int32_t x,y,w,h; ARG_I32(r,x); ARG_I32(r,y); ARG_I32(r,w); ARG_I32(r,h);
        ((void(*)(GLenum,GLint,GLint,GLint,GLint,GLint,GLint,GLsizei,GLsizei))p_glCopyTexSubImage3D)(u0,i0,i1,i2,i3,x,y,w,h); return 0; }
    case SC_GL_glCompressedTexImage3D: {
        ARG_U32(r,u0); ARG_I32(r,i0); ARG_U32(r,u1); ARG_I32(r,i1); ARG_I32(r,i2);
        int32_t d,b,isz; ARG_I32(r,d); ARG_I32(r,b); ARG_I32(r,isz);
        if (rd_blob(r,&blob,&blen)) return -EPROTO;
        ((void(*)(GLenum,GLint,GLenum,GLsizei,GLsizei,GLsizei,GLint,GLsizei,const void*))p_glCompressedTexImage3D)(
            u0,i0,u1,i1,i2,d,b,isz,blen?blob:NULL); return 0;
    }
    case SC_GL_glCompressedTexSubImage3D: {
        ARG_U32(r,u0); ARG_I32(r,i0); ARG_I32(r,i1); ARG_I32(r,i2);
        int32_t z,w,h,d,isz;
        ARG_I32(r,z); ARG_I32(r,w); ARG_I32(r,h); ARG_I32(r,d);
        ARG_U32(r,u1); ARG_I32(r,isz);
        if (rd_blob(r,&blob,&blen)) return -EPROTO;
        ((void(*)(GLenum,GLint,GLint,GLint,GLint,GLsizei,GLsizei,GLsizei,GLenum,GLsizei,const void*))p_glCompressedTexSubImage3D)(
            u0,i0,i1,i2,z,w,h,d,u1,isz,blen?blob:NULL); return 0;
    }
    case SC_GL_glIsQuery: ARG_U32(r,u0); return (int32_t)((GLboolean(*)(GLuint))p_glIsQuery)(u0);
    case SC_GL_glBeginQuery: ARG_U32(r,u0); ARG_U32(r,u1); ((void(*)(GLenum,GLuint))p_glBeginQuery)(u0,u1); return 0;
    case SC_GL_glEndQuery: ARG_U32(r,u0); ((void(*)(GLenum))p_glEndQuery)(u0); return 0;
    case SC_GL_glGetQueryiv: { ARG_U32(r,u0); ARG_U32(r,u1); GLint v=0; ((void(*)(GLenum,GLenum,GLint*))p_glGetQueryiv)(u0,u1,&v); if (w) wr_i32(w,v); return 0; }
    case SC_GL_glGetQueryObjectuiv: { ARG_U32(r,u0); ARG_U32(r,u1); GLuint v=0; ((void(*)(GLuint,GLenum,GLuint*))p_glGetQueryObjectuiv)(u0,u1,&v); if (w) wr_u32(w,v); return 0; }
    case SC_GL_glUnmapBuffer: ARG_U32(r,u0); return (int32_t)((GLboolean(*)(GLenum))p_glUnmapBuffer)(u0);
    case SC_GL_glGetBufferPointerv: { ARG_U32(r,u0); ARG_U32(r,u1); void *p=NULL; ((void(*)(GLenum,GLenum,void**))p_glGetBufferPointerv)(u0,u1,&p); if (w) wr_u64(w,(uint64_t)(uintptr_t)p); return 0; }
    case SC_GL_glDrawBuffers: {
        int32_t n; ARG_I32(r,n);
        if (n < 0 || n > 1024) return -EPROTO;
        GLenum *b = n?malloc((size_t)n*4):NULL;
        for (int32_t i=0;i<n;i++) { ARG_U32(r,u0); b[i]=(GLenum)u0; }
        ((void(*)(GLsizei,const GLenum*))p_glDrawBuffers)(n,b);
        free(b); return 0;
    }
    case SC_GL_glBlitFramebuffer: { ARG_I32(r,i0); ARG_I32(r,i1); ARG_I32(r,i2); ARG_I32(r,i3);
        int32_t a,b,c,d; ARG_I32(r,a); ARG_I32(r,b); ARG_I32(r,c); ARG_I32(r,d);
        ARG_U32(r,u0); ARG_U32(r,u1);
        ((void(*)(GLint,GLint,GLint,GLint,GLint,GLint,GLint,GLint,GLbitfield,GLenum))p_glBlitFramebuffer)(i0,i1,i2,i3,a,b,c,d,(GLbitfield)u0,u1); return 0; }
    case SC_GL_glRenderbufferStorageMultisample: { ARG_U32(r,u0); ARG_I32(r,i0); ARG_U32(r,u1); ARG_I32(r,i1); ARG_I32(r,i2);
        ((void(*)(GLenum,GLsizei,GLenum,GLsizei,GLsizei))p_glRenderbufferStorageMultisample)(u0,i0,u1,i1,i2); return 0; }
    case SC_GL_glFramebufferTextureLayer: { ARG_U32(r,u0); ARG_U32(r,u1); ARG_U32(r,u2); ARG_I32(r,i0); ARG_I32(r,i1);
        ((void(*)(GLenum,GLenum,GLuint,GLint,GLint))p_glFramebufferTextureLayer)(u0,u1,u2,i0,i1); return 0; }
    case SC_GL_glMapBufferRange: {
        /* El shim mantiene una copia "sombra" del mapeo (ver sc_gles.c). Aca solo
         * mapeamos para LEER el contenido actual, lo devolvemos y desmapeamos de
         * inmediato: nunca dejamos un buffer mapeado.
         * resp: [u64 handle=0][u32 n][bytes n] */
        ARG_U32(r,u0); ARG_U64(r,q0); uint64_t len; ARG_U64(r,len); ARG_U32(r,u1);
        if (len > SC_MAX_PAYLOAD - 64) { if (w) { wr_u64(w,0); wr_u32(w,0); } return 0; }
        void *p = ((void*(*)(GLenum,GLintptr,GLsizeiptr,GLbitfield))p_glMapBufferRange)(u0,(GLintptr)q0,(GLsizeiptr)len,GL_MAP_READ_BIT);
        if (w) {
            wr_u64(w,0);
            if (p) { wr_u32(w,(uint32_t)len); wr_bytes(w,p,(size_t)len); }
            else   { wr_u32(w,0); }
        }
        if (p) ((GLboolean(*)(GLenum))p_glUnmapBuffer)(u0);
        return 0;
    }
    case SC_GL_glFlushMappedBufferRange: { ARG_U32(r,u0); ARG_U64(r,q0); uint64_t l; ARG_U64(r,l); ((void(*)(GLenum,GLintptr,GLsizeiptr))p_glFlushMappedBufferRange)(u0,(GLintptr)q0,(GLsizeiptr)l); return 0; }
    case SC_GL_glBindVertexArray: ARG_U32(r,u0); ((void(*)(GLuint))p_glBindVertexArray)(u0); return 0;
    case SC_GL_glIsVertexArray: ARG_U32(r,u0); return (int32_t)((GLboolean(*)(GLuint))p_glIsVertexArray)(u0);
    case SC_GL_glGetIntegeri_v: { ARG_U32(r,u0); ARG_U32(r,u1); GLint v=0; ((void(*)(GLenum,GLuint,GLint*))p_glGetIntegeri_v)(u0,u1,&v); if (w) wr_i32(w,v); return 0; }
    case SC_GL_glBeginTransformFeedback: ARG_U32(r,u0); ((void(*)(GLenum))p_glBeginTransformFeedback)(u0); return 0;
    case SC_GL_glEndTransformFeedback: ((void(*)(void))p_glEndTransformFeedback)(); return 0;
    case SC_GL_glBindBufferRange: { ARG_U32(r,u0); ARG_U32(r,u1); ARG_U32(r,u2); ARG_U64(r,q0); uint64_t sz; ARG_U64(r,sz);
        ((void(*)(GLenum,GLuint,GLuint,GLintptr,GLsizeiptr))p_glBindBufferRange)(u0,u1,u2,(GLintptr)q0,(GLsizeiptr)sz); return 0; }
    case SC_GL_glBindBufferBase: ARG_U32(r,u0); ARG_U32(r,u1); ARG_U32(r,u2); ((void(*)(GLenum,GLuint,GLuint))p_glBindBufferBase)(u0,u1,u2); return 0;
    case SC_GL_glTransformFeedbackVaryings: {
        ARG_U32(r,u0); int32_t n; ARG_I32(r,n);
        if (n < 0 || n > 1024) return -EPROTO;
        const GLchar **vs = n?malloc(sizeof(GLchar*)*n):NULL;
        for (int32_t i=0;i<n;i++) {
            if (rd_blob(r,&blob,&blen)) {
                for (int32_t j=0;j<i;j++) free((void*)vs[j]);
                free(vs); return -EPROTO;
            }
            char *s = malloc(blen+1); memcpy(s,blob,blen); s[blen]=0; vs[i]=s;
        }
        ARG_U32(r,u1);
        ((void(*)(GLuint,GLsizei,const GLchar*const*,GLenum))p_glTransformFeedbackVaryings)(u0,n,vs,u1);
        for (int32_t i=0;i<n;i++) free((void*)vs[i]);
        free(vs); return 0;
    }
    case SC_GL_glGetTransformFeedbackVarying: {
        ARG_U32(r,u0); ARG_U32(r,u1); ARG_I32(r,i0);
        GLsizei bufSize = i0<1?1:i0>65536?65536:i0;
        GLsizei len=0, size=0; GLenum ty=0;
        char *nm = malloc((size_t)bufSize);
        ((void(*)(GLuint,GLuint,GLsizei,GLsizei*,GLsizei*,GLenum*,GLchar*))p_glGetTransformFeedbackVarying)(u0,u1,bufSize,&len,&size,&ty,nm);
        if (w) { wr_i32(w,size); wr_u32(w,ty); wr_u32(w,(uint32_t)len); if (len>0) wr_bytes(w,nm,(size_t)len); }
        free(nm); return 0;
    }
    case SC_GL_glVertexAttribIPointer: { ARG_U32(r,u0); ARG_I32(r,i0); ARG_U32(r,u1); ARG_I32(r,i1); ARG_U64(r,q0);
        ((void(*)(GLuint,GLint,GLenum,GLsizei,const void*))p_glVertexAttribIPointer)(u0,i0,u1,i1,(const void*)(uintptr_t)q0); return 0; }
    case SC_GL_glGetVertexAttribIiv: { ARG_U32(r,u0); ARG_U32(r,u1); GLint v=0; ((void(*)(GLuint,GLenum,GLint*))p_glGetVertexAttribIiv)(u0,u1,&v); if (w) wr_i32(w,v); return 0; }
    case SC_GL_glGetVertexAttribIuiv: { ARG_U32(r,u0); ARG_U32(r,u1); GLuint v=0; ((void(*)(GLuint,GLenum,GLuint*))p_glGetVertexAttribIuiv)(u0,u1,&v); if (w) wr_u32(w,v); return 0; }
    case SC_GL_glVertexAttribI4i: { ARG_U32(r,u0); ARG_I32(r,i0); ARG_I32(r,i1); ARG_I32(r,i2); ARG_I32(r,i3); ((void(*)(GLuint,GLint,GLint,GLint,GLint))p_glVertexAttribI4i)(u0,i0,i1,i2,i3); return 0; }
    case SC_GL_glVertexAttribI4ui: { ARG_U32(r,u0); ARG_U32(r,u1); ARG_U32(r,u2); ARG_U32(r,u3); uint32_t a; ARG_U32(r,a);
        ((void(*)(GLuint,GLuint,GLuint,GLuint,GLuint))p_glVertexAttribI4ui)(u0,u1,u2,u3,a); return 0; }
    case SC_GL_glVertexAttribI4iv: { ARG_U32(r,u0); GLint v[4]; for (int i=0;i<4;i++) ARG_I32(r,v[i]); ((void(*)(GLuint,const GLint*))p_glVertexAttribI4iv)(u0,v); return 0; }
    case SC_GL_glVertexAttribI4uiv: { ARG_U32(r,u0); GLuint v[4]; for (int i=0;i<4;i++) ARG_U32(r,v[i]); ((void(*)(GLuint,const GLuint*))p_glVertexAttribI4uiv)(u0,v); return 0; }
    case SC_GL_glGetUniformuiv: { ARG_U32(r,u0); ARG_I32(r,i0); GLuint v=0; ((void(*)(GLuint,GLint,GLuint*))p_glGetUniformuiv)(u0,i0,&v); if (w) wr_u32(w,v); return 0; }
    case SC_GL_glGetFragDataLocation: {
        ARG_U32(r,u0); if (rd_blob(r,&blob,&blen)) return -EPROTO;
        char *nm = malloc(blen+1); memcpy(nm,blob,blen); nm[blen]=0;
        GLint rv = ((GLint(*)(GLuint,const GLchar*))p_glGetFragDataLocation)(u0,nm);
        free(nm); return rv;
    }
    case SC_GL_glUniform1ui: ARG_I32(r,i0); ARG_U32(r,u0); ((void(*)(GLint,GLuint))p_glUniform1ui)(i0,u0); return 0;
    case SC_GL_glUniform2ui: ARG_I32(r,i0); ARG_U32(r,u0); ARG_U32(r,u1); ((void(*)(GLint,GLuint,GLuint))p_glUniform2ui)(i0,u0,u1); return 0;
    case SC_GL_glUniform3ui: ARG_I32(r,i0); ARG_U32(r,u0); ARG_U32(r,u1); ARG_U32(r,u2); ((void(*)(GLint,GLuint,GLuint,GLuint))p_glUniform3ui)(i0,u0,u1,u2); return 0;
    case SC_GL_glUniform4ui: { ARG_I32(r,i0); ARG_U32(r,u0); ARG_U32(r,u1); ARG_U32(r,u2); ARG_U32(r,u3);
        ((void(*)(GLint,GLuint,GLuint,GLuint,GLuint))p_glUniform4ui)(i0,u0,u1,u2,u3); return 0; }
    case SC_GL_glUniform1uiv: case SC_GL_glUniform2uiv:
    case SC_GL_glUniform3uiv: case SC_GL_glUniform4uiv: {
        int32_t loc,n; ARG_I32(r,loc); ARG_I32(r,n);
        if (n < 0 || n > 65536) return -EPROTO;
        int comp = op==SC_GL_glUniform1uiv?1:op==SC_GL_glUniform2uiv?2:op==SC_GL_glUniform3uiv?3:4;
        GLuint *v = n?malloc((size_t)n*comp*4):NULL;
        for (int32_t i=0;i<n*comp;i++) { ARG_U32(r,u0); v[i]=u0; }
        void *fn = op==SC_GL_glUniform1uiv?p_glUniform1uiv:op==SC_GL_glUniform2uiv?p_glUniform2uiv:op==SC_GL_glUniform3uiv?p_glUniform3uiv:p_glUniform4uiv;
        ((void(*)(GLint,GLsizei,const GLuint*))fn)(loc,n,v);
        free(v); return 0;
    }
    case SC_GL_glClearBufferiv: { ARG_U32(r,u0); ARG_I32(r,i0); GLint v[4]; for (int i=0;i<4;i++) ARG_I32(r,v[i]); ((void(*)(GLenum,GLint,const GLint*))p_glClearBufferiv)(u0,i0,v); return 0; }
    case SC_GL_glClearBufferuiv: { ARG_U32(r,u0); ARG_I32(r,i0); GLuint v[4]; for (int i=0;i<4;i++) ARG_U32(r,v[i]); ((void(*)(GLenum,GLint,const GLuint*))p_glClearBufferuiv)(u0,i0,v); return 0; }
    case SC_GL_glClearBufferfv: { ARG_U32(r,u0); ARG_I32(r,i0); GLfloat v[4]; for (int i=0;i<4;i++) ARG_F32(r,v[i]); ((void(*)(GLenum,GLint,const GLfloat*))p_glClearBufferfv)(u0,i0,v); return 0; }
    case SC_GL_glClearBufferfi: { ARG_U32(r,u0); ARG_I32(r,i0); ARG_F32(r,f0); ARG_I32(r,i1); ((void(*)(GLenum,GLint,GLfloat,GLint))p_glClearBufferfi)(u0,i0,f0,i1); return 0; }
    case SC_GL_glCopyBufferSubData: { ARG_U32(r,u0); ARG_U32(r,u1); ARG_U64(r,q0); uint64_t wo,sz; ARG_U64(r,wo); ARG_U64(r,sz);
        ((void(*)(GLenum,GLenum,GLintptr,GLintptr,GLsizeiptr))p_glCopyBufferSubData)(u0,u1,(GLintptr)q0,(GLintptr)wo,(GLsizeiptr)sz); return 0; }
    case SC_GL_glGetUniformIndices: {
        ARG_U32(r,u0); int32_t n; ARG_I32(r,n);
        if (n < 0 || n > 4096) return -EPROTO;
        const GLchar **nms = n?malloc(sizeof(GLchar*)*n):NULL;
        for (int32_t i=0;i<n;i++) {
            if (rd_blob(r,&blob,&blen)) { for (int32_t j=0;j<i;j++) free((void*)nms[j]); free(nms); return -EPROTO; }
            char *s = malloc(blen+1); memcpy(s,blob,blen); s[blen]=0; nms[i]=s;
        }
        GLuint *out = n?malloc((size_t)n*4):NULL;
        ((void(*)(GLuint,GLsizei,const GLchar*const*,GLuint*))p_glGetUniformIndices)(u0,n,nms,out);
        if (w) { wr_u32(w,(uint32_t)n); for (int32_t i=0;i<n;i++) wr_u32(w,out[i]); }
        for (int32_t i=0;i<n;i++) free((void*)nms[i]);
        free(nms); free(out); return 0;
    }
    case SC_GL_glGetActiveUniformsiv: {
        ARG_U32(r,u0); int32_t n; ARG_I32(r,n);
        if (n < 0 || n > 4096) return -EPROTO;
        GLuint *idx = n?malloc((size_t)n*4):NULL;
        for (int32_t i=0;i<n;i++) { ARG_U32(r,u0); idx[i]=u0; }
        ARG_U32(r,u1);
        GLint *out = n?malloc((size_t)n*4):NULL;
        ((void(*)(GLuint,GLsizei,const GLuint*,GLenum,GLint*))p_glGetActiveUniformsiv)(u0,n,idx,u1,out);
        if (w) { wr_u32(w,(uint32_t)n); for (int32_t i=0;i<n;i++) wr_i32(w,out[i]); }
        free(idx); free(out); return 0;
    }
    case SC_GL_glGetUniformBlockIndex: {
        ARG_U32(r,u0); if (rd_blob(r,&blob,&blen)) return -EPROTO;
        char *nm = malloc(blen+1); memcpy(nm,blob,blen); nm[blen]=0;
        GLuint rv = ((GLuint(*)(GLuint,const GLchar*))p_glGetUniformBlockIndex)(u0,nm);
        free(nm); return (int32_t)rv;
    }
    case SC_GL_glGetActiveUniformBlockiv: { ARG_U32(r,u0); ARG_U32(r,u1); ARG_U32(r,u2); GLint v=0; ((void(*)(GLuint,GLuint,GLenum,GLint*))p_glGetActiveUniformBlockiv)(u0,u1,u2,&v); if (w) wr_i32(w,v); return 0; }
    case SC_GL_glGetActiveUniformBlockName: {
        ARG_U32(r,u0); ARG_U32(r,u1); ARG_I32(r,i0);
        GLsizei bufSize = i0<1?1:i0>65536?65536:i0;
        GLsizei len = 0;
        char *nm = malloc((size_t)bufSize);
        ((void(*)(GLuint,GLuint,GLsizei,GLsizei*,GLchar*))p_glGetActiveUniformBlockName)(u0,u1,bufSize,&len,nm);
        if (w) { wr_u32(w,(uint32_t)len); if (len>0) wr_bytes(w,nm,(size_t)len); }
        free(nm); return 0;
    }
    case SC_GL_glUniformBlockBinding: ARG_U32(r,u0); ARG_U32(r,u1); ARG_U32(r,u2); ((void(*)(GLuint,GLuint,GLuint))p_glUniformBlockBinding)(u0,u1,u2); return 0;
    case SC_GL_glDrawArraysInstanced: { ARG_U32(r,u0); ARG_I32(r,i0); ARG_I32(r,i1); ARG_I32(r,i2); ((void(*)(GLenum,GLint,GLsizei,GLsizei))p_glDrawArraysInstanced)(u0,i0,i1,i2); return 0; }
    case SC_GL_glDrawElementsInstanced: { ARG_U32(r,u0); ARG_I32(r,i0); ARG_U32(r,u1); ARG_U64(r,q0); ARG_I32(r,i1);
        ((void(*)(GLenum,GLsizei,GLenum,const void*,GLsizei))p_glDrawElementsInstanced)(u0,i0,u1,(const void*)(uintptr_t)q0,i1); return 0; }
    case SC_GL_glFenceSync: {
        ARG_U32(r,u0); ARG_U32(r,u1);
        GLsync s = ((GLsync(*)(GLenum,GLbitfield))p_glFenceSync)(u0,u1);
        if (w) wr_u64(w,(uint64_t)(uintptr_t)s);
        return 0;
    }
    case SC_GL_glIsSync: { ARG_U64(r,q0); return (int32_t)((GLboolean(*)(GLsync))p_glIsSync)((GLsync)(uintptr_t)q0); }
    case SC_GL_glDeleteSync: { ARG_U64(r,q0); ((void(*)(GLsync))p_glDeleteSync)((GLsync)(uintptr_t)q0); return 0; }
    case SC_GL_glClientWaitSync: { ARG_U64(r,q0); ARG_U32(r,u0); uint64_t t; ARG_U64(r,t);
        return (int32_t)((GLenum(*)(GLsync,GLbitfield,GLuint64))p_glClientWaitSync)((GLsync)(uintptr_t)q0,u0,t); }
    case SC_GL_glWaitSync: { ARG_U64(r,q0); ARG_U32(r,u0); uint64_t t; ARG_U64(r,t); ((void(*)(GLsync,GLbitfield,GLuint64))p_glWaitSync)((GLsync)(uintptr_t)q0,u0,t); return 0; }
    case SC_GL_glGetInteger64v: {
        ARG_U32(r,u0); GLint64 tmp[16] = {0};
        ((void(*)(GLenum,GLint64*))p_glGetInteger64v)(u0,tmp);
        if (w) { wr_u32(w,16); for (int i=0;i<16;i++) wr_u64(w,(uint64_t)tmp[i]); }
        return 0;
    }
    case SC_GL_glGetSynciv: {
        ARG_U64(r,q0); ARG_U32(r,u0); ARG_I32(r,i0);
        GLsizei bufSize = i0<0?0:i0>64?64:i0;
        GLsizei len=0;
        GLint vals[64] = {0};
        ((void(*)(GLsync,GLenum,GLsizei,GLsizei*,GLint*))p_glGetSynciv)((GLsync)(uintptr_t)q0,u0,bufSize,&len,vals);
        if (w) { wr_u32(w,(uint32_t)len); for (GLsizei i=0;i<len;i++) wr_i32(w,vals[i]); }
        return 0;
    }
    case SC_GL_glGetInteger64i_v: { ARG_U32(r,u0); ARG_U32(r,u1); GLint64 v=0; ((void(*)(GLenum,GLuint,GLint64*))p_glGetInteger64i_v)(u0,u1,&v); if (w) wr_u64(w,(uint64_t)v); return 0; }
    case SC_GL_glGetBufferParameteri64v: { ARG_U32(r,u0); ARG_U32(r,u1); GLint64 v=0; ((void(*)(GLenum,GLenum,GLint64*))p_glGetBufferParameteri64v)(u0,u1,&v); if (w) wr_u64(w,(uint64_t)v); return 0; }
    case SC_GL_glIsSampler: ARG_U32(r,u0); return (int32_t)((GLboolean(*)(GLuint))p_glIsSampler)(u0);
    case SC_GL_glBindSampler: ARG_U32(r,u0); ARG_U32(r,u1); ((void(*)(GLuint,GLuint))p_glBindSampler)(u0,u1); return 0;
    case SC_GL_glSamplerParameteri: ARG_U32(r,u0); ARG_U32(r,u1); ARG_I32(r,i0); ((void(*)(GLuint,GLenum,GLint))p_glSamplerParameteri)(u0,u1,i0); return 0;
    case SC_GL_glSamplerParameteriv: { ARG_U32(r,u0); ARG_U32(r,u1); ARG_I32(r,i0); ((void(*)(GLuint,GLenum,const GLint*))p_glSamplerParameteriv)(u0,u1,&i0); return 0; }
    case SC_GL_glSamplerParameterf: ARG_U32(r,u0); ARG_U32(r,u1); ARG_F32(r,f0); ((void(*)(GLuint,GLenum,GLfloat))p_glSamplerParameterf)(u0,u1,f0); return 0;
    case SC_GL_glSamplerParameterfv: { ARG_U32(r,u0); ARG_U32(r,u1); ARG_F32(r,f0); ((void(*)(GLuint,GLenum,const GLfloat*))p_glSamplerParameterfv)(u0,u1,&f0); return 0; }
    case SC_GL_glGetSamplerParameteriv: { ARG_U32(r,u0); ARG_U32(r,u1); GLint v=0; ((void(*)(GLuint,GLenum,GLint*))p_glGetSamplerParameteriv)(u0,u1,&v); if (w) wr_i32(w,v); return 0; }
    case SC_GL_glGetSamplerParameterfv: { ARG_U32(r,u0); ARG_U32(r,u1); GLfloat v=0; ((void(*)(GLuint,GLenum,GLfloat*))p_glGetSamplerParameterfv)(u0,u1,&v); if (w) wr_f32(w,v); return 0; }
    case SC_GL_glVertexAttribDivisor: ARG_U32(r,u0); ARG_U32(r,u1); ((void(*)(GLuint,GLuint))p_glVertexAttribDivisor)(u0,u1); return 0;
    case SC_GL_glBindTransformFeedback: ARG_U32(r,u0); ARG_U32(r,u1); ((void(*)(GLenum,GLuint))p_glBindTransformFeedback)(u0,u1); return 0;
    case SC_GL_glIsTransformFeedback: ARG_U32(r,u0); return (int32_t)((GLboolean(*)(GLuint))p_glIsTransformFeedback)(u0);
    case SC_GL_glPauseTransformFeedback: ((void(*)(void))p_glPauseTransformFeedback)(); return 0;
    case SC_GL_glResumeTransformFeedback: ((void(*)(void))p_glResumeTransformFeedback)(); return 0;
    case SC_GL_glDrawTransformFeedback: ARG_U32(r,u0); ARG_U32(r,u1); ((void(*)(GLenum,GLuint))p_glDrawTransformFeedback)(u0,u1); return 0;
    case SC_GL_glInvalidateFramebuffer: {
        ARG_U32(r,u0); int32_t n; ARG_I32(r,n);
        if (n < 0 || n > 1024) return -EPROTO;
        GLenum *a = n?malloc((size_t)n*4):NULL;
        for (int32_t i=0;i<n;i++) { ARG_U32(r,u0); a[i]=(GLenum)u0; }
        ((void(*)(GLenum,GLsizei,const GLenum*))p_glInvalidateFramebuffer)(u0,n,a);
        free(a); return 0;
    }
    case SC_GL_glInvalidateSubFramebuffer: {
        ARG_U32(r,u0); int32_t n; ARG_I32(r,n);
        if (n < 0 || n > 1024) return -EPROTO;
        GLenum *a = n?malloc((size_t)n*4):NULL;
        for (int32_t i=0;i<n;i++) { ARG_U32(r,u1); a[i]=(GLenum)u1; }
        int32_t x,y,w2,h2; ARG_I32(r,x); ARG_I32(r,y); ARG_I32(r,w2); ARG_I32(r,h2);
        ((void(*)(GLenum,GLsizei,const GLenum*,GLint,GLint,GLsizei,GLsizei))p_glInvalidateSubFramebuffer)(u0,n,a,x,y,w2,h2);
        free(a); return 0;
    }

    /* ---------- ES 3.1 ---------- */
    case SC_GL_glDispatchCompute: { ARG_U32(r,u0); ARG_U32(r,u1); ARG_U32(r,u2); ((void(*)(GLuint,GLuint,GLuint))p_glDispatchCompute)(u0,u1,u2); return 0; }
    case SC_GL_glDispatchComputeIndirect: { ARG_U64(r,q0); ((void(*)(GLintptr))p_glDispatchComputeIndirect)((GLintptr)q0); return 0; }
    case SC_GL_glFramebufferParameteri: { ARG_U32(r,u0); ARG_U32(r,u1); ARG_I32(r,i0); ((void(*)(GLenum,GLenum,GLint))p_glFramebufferParameteri)(u0,u1,i0); return 0; }
    case SC_GL_glGetFramebufferParameteriv: { ARG_U32(r,u0); ARG_U32(r,u1); GLint v=0; ((void(*)(GLenum,GLenum,GLint*))p_glGetFramebufferParameteriv)(u0,u1,&v); if (w) wr_i32(w,v); return 0; }
    case SC_GL_glGetInternalformati64v: {
        ARG_U32(r,u0); ARG_U32(r,u1); ARG_U32(r,u2); ARG_I32(r,i0);
        GLsizei bufSize = i0<0?0:i0>64?64:i0;
        GLint64 *out = bufSize?malloc((size_t)bufSize*8):NULL;
        ((void(*)(GLenum,GLenum,GLenum,GLsizei,GLint64*))p_glGetInternalformati64v)(u0,u1,u2,bufSize,out);
        if (w) { wr_u32(w,(uint32_t)bufSize); for (GLsizei i=0;i<bufSize;i++) wr_u64(w,(uint64_t)out[i]); }
        free(out); return 0;
    }
    case SC_GL_glInvalidateTexSubImage: { ARG_U32(r,u0); ARG_I32(r,i0); ARG_I32(r,i1); ARG_I32(r,i2); ARG_I32(r,i3);
        int32_t w2,h2,d; ARG_I32(r,w2); ARG_I32(r,h2); ARG_I32(r,d);
        ((void(*)(GLuint,GLint,GLint,GLint,GLint,GLsizei,GLsizei,GLsizei))p_glInvalidateTexSubImage)(u0,i0,i1,i2,i3,w2,h2,d); return 0; }
    case SC_GL_glInvalidateTexImage: { ARG_U32(r,u0); ARG_I32(r,i0); ((void(*)(GLuint,GLint))p_glInvalidateTexImage)(u0,i0); return 0; }
    case SC_GL_glInvalidateBufferSubData: { ARG_U32(r,u0); ARG_U64(r,q0); uint64_t sz; ARG_U64(r,sz);
        ((void(*)(GLuint,GLintptr,GLsizeiptr))p_glInvalidateBufferSubData)(u0,(GLintptr)q0,(GLsizeiptr)sz); return 0; }
    case SC_GL_glInvalidateBufferData: { ARG_U32(r,u0); ((void(*)(GLuint))p_glInvalidateBufferData)(u0); return 0; }
    case SC_GL_glGetProgramInterfaceiv: { ARG_U32(r,u0); ARG_U32(r,u1); ARG_U32(r,u2); GLint v=0; ((void(*)(GLuint,GLenum,GLenum,GLint*))p_glGetProgramInterfaceiv)(u0,u1,u2,&v); if (w) wr_i32(w,v); return 0; }
    case SC_GL_glGetProgramResourceIndex: {
        ARG_U32(r,u0); ARG_U32(r,u1); if (rd_blob(r,&blob,&blen)) return -EPROTO;
        char *nm = malloc(blen+1); memcpy(nm,blob,blen); nm[blen]=0;
        GLuint rv = ((GLuint(*)(GLuint,GLenum,const GLchar*))p_glGetProgramResourceIndex)(u0,u1,nm);
        free(nm); return (int32_t)rv;
    }
    case SC_GL_glGetProgramResourceName: {
        ARG_U32(r,u0); ARG_U32(r,u1); ARG_U32(r,u2); ARG_I32(r,i0);
        GLsizei bufSize = i0<1?1:i0>65536?65536:i0;
        GLsizei len=0;
        char *nm = malloc((size_t)bufSize);
        ((void(*)(GLuint,GLenum,GLuint,GLsizei,GLsizei*,GLchar*))p_glGetProgramResourceName)(u0,u1,u2,bufSize,&len,nm);
        if (w) { wr_u32(w,(uint32_t)len); if (len>0) wr_bytes(w,nm,(size_t)len); }
        free(nm); return 0;
    }
    case SC_GL_glGetProgramResourceiv: {
        ARG_U32(r,u0); ARG_U32(r,u1); ARG_U32(r,u2);
        int32_t np; ARG_I32(r,np);
        if (np < 0 || np > 4096) return -EPROTO;
        GLenum *props = np?malloc((size_t)np*4):NULL;
        for (int32_t i=0;i<np;i++) { ARG_U32(r,u3); props[i]=(GLenum)u3; }
        ARG_I32(r,i0);
        GLsizei bufSize = i0<0?0:i0>4096?4096:i0;
        GLsizei len=0;
        GLint *out = bufSize?malloc((size_t)bufSize*4):NULL;
        ((void(*)(GLuint,GLenum,GLuint,GLsizei,const GLenum*,GLsizei,GLsizei*,GLint*))p_glGetProgramResourceiv)(
            u0,u1,u2,np,props,bufSize,&len,out);
        if (w) { wr_u32(w,(uint32_t)len); for (GLsizei i=0;i<len;i++) wr_i32(w,out[i]); }
        free(props); free(out); return 0;
    }
    case SC_GL_glGetProgramResourceLocation: {
        ARG_U32(r,u0); ARG_U32(r,u1); if (rd_blob(r,&blob,&blen)) return -EPROTO;
        char *nm = malloc(blen+1); memcpy(nm,blob,blen); nm[blen]=0;
        GLint rv = ((GLint(*)(GLuint,GLenum,const GLchar*))p_glGetProgramResourceLocation)(u0,u1,nm);
        free(nm); return rv;
    }
    case SC_GL_glGetProgramResourceLocationIndex: {
        ARG_U32(r,u0); ARG_U32(r,u1); if (rd_blob(r,&blob,&blen)) return -EPROTO;
        char *nm = malloc(blen+1); memcpy(nm,blob,blen); nm[blen]=0;
        GLint rv = ((GLint(*)(GLuint,GLenum,const GLchar*))p_glGetProgramResourceLocationIndex)(u0,u1,nm);
        free(nm); return rv;
    }
    case SC_GL_glShaderStorageBlockBinding: { ARG_U32(r,u0); ARG_U32(r,u1); ARG_U32(r,u2); ((void(*)(GLuint,GLuint,GLuint))p_glShaderStorageBlockBinding)(u0,u1,u2); return 0; }
    case SC_GL_glTexBufferRange: { ARG_U32(r,u0); ARG_U32(r,u1); ARG_U32(r,u2); ARG_U64(r,q0); uint64_t sz; ARG_U64(r,sz);
        ((void(*)(GLenum,GLenum,GLuint,GLintptr,GLsizeiptr))p_glTexBufferRange)(u0,u1,u2,(GLintptr)q0,(GLsizeiptr)sz); return 0; }
    case SC_GL_glTexStorage2DMultisample: { ARG_U32(r,u0); ARG_I32(r,i0); ARG_U32(r,u1); ARG_I32(r,i1); ARG_I32(r,i2); ARG_U32(r,u2);
        ((void(*)(GLenum,GLsizei,GLenum,GLsizei,GLsizei,GLboolean))p_glTexStorage2DMultisample)(u0,i0,u1,i1,i2,u2); return 0; }
    case SC_GL_glTexStorage3DMultisample: { ARG_U32(r,u0); ARG_I32(r,i0); ARG_U32(r,u1); ARG_I32(r,i1); ARG_I32(r,i2);
        int32_t d; ARG_I32(r,d); ARG_U32(r,u2);
        ((void(*)(GLenum,GLsizei,GLenum,GLsizei,GLsizei,GLsizei,GLboolean))p_glTexStorage3DMultisample)(u0,i0,u1,i1,i2,d,u2); return 0; }
    case SC_GL_glTextureView: { ARG_U32(r,u0); ARG_U32(r,u1); ARG_U32(r,u2); ARG_U32(r,u3); ARG_U32(r,u2);
        /* 8 args: texture,target,origtexture,internalformat,minlevel,numlevels,minlayer,numlayers */
        uint32_t ta,tb,tc,td,te,tf,tg,th; ARG_U32(r,ta); ARG_U32(r,tb); ARG_U32(r,tc); ARG_U32(r,td);
        ARG_U32(r,te); ARG_U32(r,tf); ARG_U32(r,tg); ARG_U32(r,th);
        ((void(*)(GLuint,GLenum,GLuint,GLenum,GLuint,GLuint,GLuint,GLuint))p_glTextureView)(ta,tb,tc,td,te,tf,tg,th); return 0; }
    case SC_GL_glBindImageTexture: { ARG_U32(r,u0); ARG_U32(r,u1); ARG_I32(r,i0); ARG_U32(r,u2); ARG_I32(r,i1); ARG_U32(r,u3);
        uint32_t fmt; ARG_U32(r,fmt);
        ((void(*)(GLuint,GLuint,GLint,GLboolean,GLint,GLenum,GLenum))p_glBindImageTexture)(u0,u1,i0,u2,i1,u3,fmt); return 0; }
    case SC_GL_glMemoryBarrier: { ARG_U32(r,u0); ((void(*)(GLbitfield))p_glMemoryBarrier)(u0); return 0; }
    case SC_GL_glMemoryBarrierByRegion: { ARG_U32(r,u0); ((void(*)(GLbitfield))p_glMemoryBarrierByRegion)(u0); return 0; }
    case SC_GL_glGetMultisamplefv: { ARG_U32(r,u0); ARG_U32(r,u1); GLfloat v=0; ((void(*)(GLenum,GLuint,GLfloat*))p_glGetMultisamplefv)(u0,u1,&v); if (w) wr_f32(w,v); return 0; }
    case SC_GL_glSampleMaski: { ARG_U32(r,u0); ARG_U32(r,u1); ((void(*)(GLuint,GLbitfield))p_glSampleMaski)(u0,u1); return 0; }
    case SC_GL_glTexStorage2D: { ARG_U32(r,u0); ARG_I32(r,i0); ARG_U32(r,u1); ARG_I32(r,i1); ARG_I32(r,i2);
        ((void(*)(GLenum,GLsizei,GLenum,GLsizei,GLsizei))p_glTexStorage2D)(u0,i0,u1,i1,i2); return 0; }
    case SC_GL_glTexStorage3D: { ARG_U32(r,u0); ARG_I32(r,i0); ARG_U32(r,u1); ARG_I32(r,i1); ARG_I32(r,i2); ARG_I32(r,i3);
        ((void(*)(GLenum,GLsizei,GLenum,GLsizei,GLsizei,GLsizei))p_glTexStorage3D)(u0,i0,u1,i1,i2,i3); return 0; }
    case SC_GL_glGetTexLevelParameteriv: { ARG_U32(r,u0); ARG_I32(r,i0); ARG_U32(r,u1); GLint v=0; ((void(*)(GLenum,GLint,GLenum,GLint*))p_glGetTexLevelParameteriv)(u0,i0,u1,&v); if (w) wr_i32(w,v); return 0; }
    case SC_GL_glGetTexLevelParameterfv: { ARG_U32(r,u0); ARG_I32(r,i0); ARG_U32(r,u1); GLfloat v=0; ((void(*)(GLenum,GLint,GLenum,GLfloat*))p_glGetTexLevelParameterfv)(u0,i0,u1,&v); if (w) wr_f32(w,v); return 0; }
    case SC_GL_glBindVertexBuffer: { ARG_U32(r,u0); ARG_U32(r,u1); ARG_U64(r,q0); ARG_I32(r,i0);
        ((void(*)(GLuint,GLuint,GLintptr,GLsizei))p_glBindVertexBuffer)(u0,u1,(GLintptr)q0,i0); return 0; }
    case SC_GL_glVertexAttribFormat: { ARG_U32(r,u0); ARG_I32(r,i0); ARG_U32(r,u1); ARG_U32(r,u2); ARG_U32(r,u3);
        ((void(*)(GLuint,GLint,GLenum,GLboolean,GLuint))p_glVertexAttribFormat)(u0,i0,u1,u2,u3); return 0; }
    case SC_GL_glVertexAttribIFormat: { ARG_U32(r,u0); ARG_I32(r,i0); ARG_U32(r,u1); ARG_U32(r,u2);
        ((void(*)(GLuint,GLint,GLenum,GLuint))p_glVertexAttribIFormat)(u0,i0,u1,u2); return 0; }
    case SC_GL_glVertexAttribBinding: { ARG_U32(r,u0); ARG_U32(r,u1); ((void(*)(GLuint,GLuint))p_glVertexAttribBinding)(u0,u1); return 0; }
    case SC_GL_glVertexBindingDivisor: { ARG_U32(r,u0); ARG_U32(r,u1); ((void(*)(GLuint,GLuint))p_glVertexBindingDivisor)(u0,u1); return 0; }

    /* ---------- ES 3.2 ---------- */
    case SC_GL_glBlendBarrier: ((void(*)(void))p_glBlendBarrier)(); return 0;
    case SC_GL_glCopyImageSubData: {
        ARG_U32(r,u0); ARG_U32(r,u1); ARG_I32(r,i0); ARG_I32(r,i1); ARG_I32(r,i2); ARG_I32(r,i3);
        uint32_t dst,dt; ARG_U32(r,dst); ARG_U32(r,dt);
        int32_t dl,dx,dy,dz; ARG_I32(r,dl); ARG_I32(r,dx); ARG_I32(r,dy); ARG_I32(r,dz);
        int32_t w2,h2,d2; ARG_I32(r,w2); ARG_I32(r,h2); ARG_I32(r,d2);
        ((void(*)(GLuint,GLenum,GLint,GLint,GLint,GLint,GLuint,GLenum,GLint,GLint,GLint,GLint,GLsizei,GLsizei,GLsizei))p_glCopyImageSubData)(
            u0,u1,i0,i1,i2,i3,dst,dt,dl,dx,dy,dz,w2,h2,d2);
        return 0;
    }
    case SC_GL_glDebugMessageControl: {
        ARG_U32(r,u0); ARG_U32(r,u1); ARG_U32(r,u2);
        uint32_t n; ARG_U32(r,n);
        GLuint *ids = n?malloc((size_t)n*4):NULL;
        for (uint32_t i=0;i<n;i++) { ARG_U32(r,u3); ids[i]=u3; }
        ARG_U32(r,u3);
        ((void(*)(GLenum,GLenum,GLenum,GLsizei,const GLuint*,GLboolean))p_glDebugMessageControl)(u0,u1,u2,(GLsizei)n,ids,u3);
        free(ids); return 0;
    }
    case SC_GL_glDebugMessageInsert: {
        ARG_U32(r,u0); ARG_U32(r,u1); ARG_U32(r,u2); ARG_U32(r,u3); ARG_I32(r,i0);
        if (rd_blob(r,&blob,&blen)) return -EPROTO;
        ((void(*)(GLenum,GLenum,GLuint,GLenum,GLsizei,const GLchar*))p_glDebugMessageInsert)(u0,u1,u2,u3,i0,(const GLchar*)blob);
        return 0;
    }
    case SC_GL_glPushDebugGroup: {
        ARG_U32(r,u0); ARG_U32(r,u1);
        if (rd_blob(r,&blob,&blen)) return -EPROTO;
        ((void(*)(GLenum,GLuint,GLsizei,const GLchar*))p_glPushDebugGroup)(u0,u1,(GLsizei)blen,(const GLchar*)blob);
        return 0;
    }
    case SC_GL_glPopDebugGroup: ((void(*)(void))p_glPopDebugGroup)(); return 0;
    case SC_GL_glObjectLabel: {
        ARG_U32(r,u0); ARG_U32(r,u1);
        if (rd_blob(r,&blob,&blen)) return -EPROTO;
        ((void(*)(GLenum,GLuint,GLsizei,const GLchar*))p_glObjectLabel)(u0,u1,(GLsizei)blen,(const GLchar*)blob);
        return 0;
    }
    case SC_GL_glGetObjectLabel: {
        ARG_U32(r,u0); ARG_U32(r,u1); ARG_I32(r,i0);
        GLsizei bufSize = i0<1?1:i0>65536?65536:i0;
        GLsizei len=0;
        char *lbl = malloc((size_t)bufSize);
        ((void(*)(GLenum,GLuint,GLsizei,GLsizei*,GLchar*))p_glGetObjectLabel)(u0,u1,bufSize,&len,lbl);
        if (w) { wr_u32(w,(uint32_t)len); if (len>0) wr_bytes(w,lbl,(size_t)len); }
        free(lbl); return 0;
    }
    case SC_GL_glObjectPtrLabel: {
        ARG_U64(r,q0);
        if (rd_blob(r,&blob,&blen)) return -EPROTO;
        ((void(*)(const void*,GLsizei,const GLchar*))p_glObjectPtrLabel)((const void*)(uintptr_t)q0,(GLsizei)blen,(const GLchar*)blob);
        return 0;
    }
    case SC_GL_glGetObjectPtrLabel: {
        ARG_U64(r,q0); ARG_I32(r,i0);
        GLsizei bufSize = i0<1?1:i0>65536?65536:i0;
        GLsizei len=0;
        char *lbl = malloc((size_t)bufSize);
        ((void(*)(const void*,GLsizei,GLsizei*,GLchar*))p_glGetObjectPtrLabel)((const void*)(uintptr_t)q0,bufSize,&len,lbl);
        if (w) { wr_u32(w,(uint32_t)len); if (len>0) wr_bytes(w,lbl,(size_t)len); }
        free(lbl); return 0;
    }
    case SC_GL_glGetPointerv: { ARG_U32(r,u0); void *p=NULL; ((void(*)(GLenum,void**))p_glGetPointerv)(u0,&p); if (w) wr_u64(w,(uint64_t)(uintptr_t)p); return 0; }
    case SC_GL_glEnablei: { ARG_U32(r,u0); ARG_U32(r,u1); ((void(*)(GLenum,GLuint))p_glEnablei)(u0,u1); return 0; }
    case SC_GL_glDisablei: { ARG_U32(r,u0); ARG_U32(r,u1); ((void(*)(GLenum,GLuint))p_glDisablei)(u0,u1); return 0; }
    case SC_GL_glBlendEquationi: { ARG_U32(r,u0); ARG_U32(r,u1); ((void(*)(GLuint,GLenum))p_glBlendEquationi)(u0,u1); return 0; }
    case SC_GL_glBlendEquationSeparatei: { ARG_U32(r,u0); ARG_U32(r,u1); ARG_U32(r,u2); ((void(*)(GLuint,GLenum,GLenum))p_glBlendEquationSeparatei)(u0,u1,u2); return 0; }
    case SC_GL_glBlendFunci: { ARG_U32(r,u0); ARG_U32(r,u1); ARG_U32(r,u2); ((void(*)(GLuint,GLenum,GLenum))p_glBlendFunci)(u0,u1,u2); return 0; }
    case SC_GL_glBlendFuncSeparatei: { ARG_U32(r,u0); ARG_U32(r,u1); ARG_U32(r,u2); ARG_U32(r,u3); uint32_t a; ARG_U32(r,a);
        ((void(*)(GLuint,GLenum,GLenum,GLenum,GLenum))p_glBlendFuncSeparatei)(u0,u1,u2,u3,a); return 0; }
    case SC_GL_glColorMaski: { ARG_U32(r,u0); ARG_U32(r,u1); ARG_U32(r,u2); ARG_U32(r,u3); uint32_t a; ARG_U32(r,a);
        ((void(*)(GLuint,GLboolean,GLboolean,GLboolean,GLboolean))p_glColorMaski)(u0,u1,u2,u3,a); return 0; }
    case SC_GL_glIsEnabledi: { ARG_U32(r,u0); ARG_U32(r,u1); return (int32_t)((GLboolean(*)(GLenum,GLuint))p_glIsEnabledi)(u0,u1); }
    case SC_GL_glDrawElementsBaseVertex: { ARG_U32(r,u0); ARG_I32(r,i0); ARG_U32(r,u1); ARG_U64(r,q0); ARG_I32(r,i1);
        ((void(*)(GLenum,GLsizei,GLenum,const void*,GLint))p_glDrawElementsBaseVertex)(u0,i0,u1,(const void*)(uintptr_t)q0,i1); return 0; }
    case SC_GL_glDrawRangeElementsBaseVertex: { ARG_U32(r,u0); ARG_U32(r,u1); ARG_U32(r,u2); ARG_I32(r,i0); ARG_U32(r,u3); ARG_U64(r,q0); ARG_I32(r,i1);
        ((void(*)(GLenum,GLuint,GLuint,GLsizei,GLenum,const void*,GLint))p_glDrawRangeElementsBaseVertex)(u0,u1,u2,i0,u3,(const void*)(uintptr_t)q0,i1); return 0; }
    case SC_GL_glDrawElementsInstancedBaseVertex: { ARG_U32(r,u0); ARG_I32(r,i0); ARG_U32(r,u1); ARG_U64(r,q0); ARG_I32(r,i1); ARG_I32(r,i2);
        ((void(*)(GLenum,GLsizei,GLenum,const void*,GLsizei,GLint))p_glDrawElementsInstancedBaseVertex)(u0,i0,u1,(const void*)(uintptr_t)q0,i1,i2); return 0; }
    case SC_GL_glDrawArraysIndirect: { ARG_U32(r,u0); ARG_U64(r,q0); ((void(*)(GLenum,const void*))p_glDrawArraysIndirect)(u0,(const void*)(uintptr_t)q0); return 0; }
    case SC_GL_glDrawElementsIndirect: { ARG_U32(r,u0); ARG_U32(r,u1); ARG_U64(r,q0); ((void(*)(GLenum,GLenum,const void*))p_glDrawElementsIndirect)(u0,u1,(const void*)(uintptr_t)q0); return 0; }
    case SC_GL_glFramebufferTexture: { ARG_U32(r,u0); ARG_U32(r,u1); ARG_U32(r,u2); ARG_I32(r,i0); ((void(*)(GLenum,GLenum,GLuint,GLint))p_glFramebufferTexture)(u0,u1,u2,i0); return 0; }
    case SC_GL_glPrimitiveBoundingBox: { ARG_F32(r,f0); ARG_F32(r,f1); ARG_F32(r,f2); ARG_F32(r,f3);
        float a,b,c,d; ARG_F32(r,a); ARG_F32(r,b); ARG_F32(r,c); ARG_F32(r,d);
        ((void(*)(GLfloat,GLfloat,GLfloat,GLfloat,GLfloat,GLfloat,GLfloat,GLfloat))p_glPrimitiveBoundingBox)(f0,f1,f2,f3,a,b,c,d); return 0; }
    case SC_GL_glGetGraphicsResetStatus: return (int32_t)((GLenum(*)(void))p_glGetGraphicsResetStatus)();
    case SC_GL_glReadnPixels: {
        ARG_I32(r,i0); ARG_I32(r,i1); ARG_I32(r,i2); ARG_I32(r,i3);
        ARG_U32(r,u0); ARG_U32(r,u1); ARG_I32(r,i2);
        size_t ps = pixel_size((GLenum)u0,(GLenum)u1); if (!ps) ps = 4;
        int32_t bufSize; ARG_I32(r,bufSize);
        size_t total = ps * (size_t)i2 * (size_t)i3;
        if (total > (size_t)bufSize) total = (size_t)bufSize;
        if (total > SC_MAX_PAYLOAD-8) return -EMSGSIZE;
        uint8_t *pix = total?malloc(total):NULL;
        ((void(*)(GLint,GLint,GLsizei,GLsizei,GLenum,GLenum,GLsizei,void*))p_glReadnPixels)(i0,i1,i2,i3,u0,u1,bufSize,pix);
        if (w && total) wr_bytes(w,pix,total);
        free(pix); return 0;
    }
    case SC_GL_glGetnUniformfv: {
        ARG_U32(r,u0); ARG_I32(r,i0); ARG_I32(r,i1);
        GLsizei bs = i1<0?0:i1>4096?4096:i1;
        GLfloat *v = bs?malloc((size_t)bs*4):NULL;
        ((void(*)(GLuint,GLint,GLsizei,GLfloat*))p_glGetnUniformfv)(u0,i0,bs,v);
        if (w) { wr_u32(w,(uint32_t)bs); for (GLsizei i=0;i<bs;i++) wr_f32(w,v[i]); }
        free(v); return 0;
    }
    case SC_GL_glGetnUniformiv: {
        ARG_U32(r,u0); ARG_I32(r,i0); ARG_I32(r,i1);
        GLsizei bs = i1<0?0:i1>4096?4096:i1;
        GLint *v = bs?malloc((size_t)bs*4):NULL;
        ((void(*)(GLuint,GLint,GLsizei,GLint*))p_glGetnUniformiv)(u0,i0,bs,v);
        if (w) { wr_u32(w,(uint32_t)bs); for (GLsizei i=0;i<bs;i++) wr_i32(w,v[i]); }
        free(v); return 0;
    }
    case SC_GL_glGetnUniformuiv: {
        ARG_U32(r,u0); ARG_I32(r,i0); ARG_I32(r,i1);
        GLsizei bs = i1<0?0:i1>4096?4096:i1;
        GLuint *v = bs?malloc((size_t)bs*4):NULL;
        ((void(*)(GLuint,GLint,GLsizei,GLuint*))p_glGetnUniformuiv)(u0,i0,bs,v);
        if (w) { wr_u32(w,(uint32_t)bs); for (GLsizei i=0;i<bs;i++) wr_u32(w,v[i]); }
        free(v); return 0;
    }
    case SC_GL_glMinSampleShading: { ARG_F32(r,f0); ((void(*)(GLfloat))p_glMinSampleShading)(f0); return 0; }
    case SC_GL_glPatchParameteri: { ARG_U32(r,u0); ARG_I32(r,i0); ((void(*)(GLenum,GLint))p_glPatchParameteri)(u0,i0); return 0; }
    case SC_GL_glTexParameterIiv: { ARG_U32(r,u0); ARG_U32(r,u1); ARG_I32(r,i0); ((void(*)(GLenum,GLenum,const GLint*))p_glTexParameterIiv)(u0,u1,&i0); return 0; }
    case SC_GL_glTexParameterIuiv: { ARG_U32(r,u0); ARG_U32(r,u1); ARG_U32(r,u2); ((void(*)(GLenum,GLenum,const GLuint*))p_glTexParameterIuiv)(u0,u1,&u2); return 0; }
    case SC_GL_glGetTexParameterIiv: { ARG_U32(r,u0); ARG_U32(r,u1); GLint v=0; ((void(*)(GLenum,GLenum,GLint*))p_glGetTexParameterIiv)(u0,u1,&v); if (w) wr_i32(w,v); return 0; }
    case SC_GL_glGetTexParameterIuiv: { ARG_U32(r,u0); ARG_U32(r,u1); GLuint v=0; ((void(*)(GLenum,GLenum,GLuint*))p_glGetTexParameterIuiv)(u0,u1,&v); if (w) wr_u32(w,v); return 0; }
    case SC_GL_glSamplerParameterIiv: { ARG_U32(r,u0); ARG_U32(r,u1); ARG_I32(r,i0); ((void(*)(GLuint,GLenum,const GLint*))p_glSamplerParameterIiv)(u0,u1,&i0); return 0; }
    case SC_GL_glSamplerParameterIuiv: { ARG_U32(r,u0); ARG_U32(r,u1); ARG_U32(r,u2); ((void(*)(GLuint,GLenum,const GLuint*))p_glSamplerParameterIuiv)(u0,u1,&u2); return 0; }
    case SC_GL_glGetSamplerParameterIiv: { ARG_U32(r,u0); ARG_U32(r,u1); GLint v=0; ((void(*)(GLuint,GLenum,GLint*))p_glGetSamplerParameterIiv)(u0,u1,&v); if (w) wr_i32(w,v); return 0; }
    case SC_GL_glGetSamplerParameterIuiv: { ARG_U32(r,u0); ARG_U32(r,u1); GLuint v=0; ((void(*)(GLuint,GLenum,GLuint*))p_glGetSamplerParameterIuiv)(u0,u1,&v); if (w) wr_u32(w,v); return 0; }
    case SC_GL_glGetQueryObjectiv: { ARG_U32(r,u0); ARG_U32(r,u1); GLint v=0; ((void(*)(GLuint,GLenum,GLint*))p_glGetQueryObjectiv)(u0,u1,&v); if (w) wr_i32(w,v); return 0; }
    case SC_GL_glGetQueryObjecti64v: { ARG_U32(r,u0); ARG_U32(r,u1); GLint64 v=0; ((void(*)(GLuint,GLenum,GLint64*))p_glGetQueryObjecti64v)(u0,u1,&v); if (w) wr_u64(w,(uint64_t)v); return 0; }
    case SC_GL_glGetQueryObjectui64v: { ARG_U32(r,u0); ARG_U32(r,u1); GLuint64 v=0; ((void(*)(GLuint,GLenum,GLuint64*))p_glGetQueryObjectui64v)(u0,u1,&v); if (w) wr_u64(w,(uint64_t)v); return 0; }
    case SC_GL_glGetQueryBufferObjectiv: { ARG_U32(r,u0); ARG_U32(r,u1); ARG_U32(r,u2); ARG_U64(r,q0);
        ((void(*)(GLuint,GLuint,GLenum,GLintptr))p_glGetQueryBufferObjectiv)(u0,u1,u2,(GLintptr)q0); return 0; }
    case SC_GL_glGetQueryBufferObjectuiv: { ARG_U32(r,u0); ARG_U32(r,u1); ARG_U32(r,u2); ARG_U64(r,q0);
        ((void(*)(GLuint,GLuint,GLenum,GLintptr))p_glGetQueryBufferObjectuiv)(u0,u1,u2,(GLintptr)q0); return 0; }
    case SC_GL_glGetQueryBufferObjecti64v: { ARG_U32(r,u0); ARG_U32(r,u1); ARG_U32(r,u2); ARG_U64(r,q0);
        ((void(*)(GLuint,GLuint,GLenum,GLintptr))p_glGetQueryBufferObjecti64v)(u0,u1,u2,(GLintptr)q0); return 0; }
    case SC_GL_glGetQueryBufferObjectui64v: { ARG_U32(r,u0); ARG_U32(r,u1); ARG_U32(r,u2); ARG_U64(r,q0);
        ((void(*)(GLuint,GLuint,GLenum,GLintptr))p_glGetQueryBufferObjectui64v)(u0,u1,u2,(GLintptr)q0); return 0; }
    case SC_GL_glGetInternalformativ: {
        ARG_U32(r,u0); ARG_U32(r,u1); ARG_U32(r,u2); ARG_I32(r,i0);
        GLsizei bs = i0<0?0:i0>4096?4096:i0;
        GLint *v = bs?malloc((size_t)bs*4):NULL;
        ((void(*)(GLenum,GLenum,GLenum,GLsizei,GLint*))p_glGetInternalformativ)(u0,u1,u2,bs,v);
        if (w) { wr_u32(w,(uint32_t)bs); for (GLsizei i=0;i<bs;i++) wr_i32(w,v[i]); }
        free(v); return 0;
    }
    case SC_GL_glTexBuffer: { ARG_U32(r,u0); ARG_U32(r,u1); ARG_U32(r,u2); ((void(*)(GLenum,GLenum,GLuint))p_glTexBuffer)(u0,u1,u2); return 0; }

    /* ---------- extensiones ---------- */
    case SC_GL_glGetProgramBinary: {
        ARG_U32(r,u0); ARG_I32(r,i0);
        GLsizei bufSize = i0<0?0:i0;
        GLsizei len=0; GLenum bf=0;
        uint8_t *bin = bufSize?malloc((size_t)bufSize):NULL;
        ((void(*)(GLuint,GLsizei,GLsizei*,GLenum*,void*))p_glGetProgramBinary)(u0,bufSize,&len,&bf,bin);
        if (w) { wr_u32(w,(uint32_t)bf); wr_u32(w,(uint32_t)len); if (len>0) wr_bytes(w,bin,(size_t)len); }
        free(bin); return 0;
    }
    case SC_GL_glProgramBinary: {
        ARG_U32(r,u0); ARG_U32(r,u1);
        if (rd_blob(r,&blob,&blen)) return -EPROTO;
        ((void(*)(GLuint,GLenum,const void*,GLsizei))p_glProgramBinary)(u0,u1,blob,(GLsizei)blen); return 0;
    }
    case SC_GL_glProgramParameteri: { ARG_U32(r,u0); ARG_U32(r,u1); ARG_I32(r,i0); ((void(*)(GLuint,GLenum,GLint))p_glProgramParameteri)(u0,u1,i0); return 0; }
    case SC_GL_glEGLImageTargetTexture2DOES: { ARG_U32(r,u0); ARG_U64(r,q0); ((void(*)(GLenum,GLeglImageOES))p_glEGLImageTargetTexture2DOES)(u0,(GLeglImageOES)(uintptr_t)q0); return 0; }
    case SC_GL_glEGLImageTargetRenderbufferStorageOES: { ARG_U32(r,u0); ARG_U64(r,q0); ((void(*)(GLenum,GLeglImageOES))p_glEGLImageTargetRenderbufferStorageOES)(u0,(GLeglImageOES)(uintptr_t)q0); return 0; }
    case SC_GL_glFramebufferTexture2DMultisampleEXT: { ARG_U32(r,u0); ARG_U32(r,u1); ARG_U32(r,u2); ARG_U32(r,u3); ARG_I32(r,i0); ARG_I32(r,i1);
        ((void(*)(GLenum,GLenum,GLenum,GLuint,GLint,GLsizei))p_glFramebufferTexture2DMultisampleEXT)(u0,u1,u2,u3,i0,i1); return 0; }
    case SC_GL_glFramebufferTextureMultiviewOVR: { ARG_U32(r,u0); ARG_U32(r,u1); ARG_U32(r,u2); ARG_I32(r,i0); ARG_I32(r,i1); ARG_I32(r,i2);
        ((void(*)(GLenum,GLenum,GLuint,GLint,GLint,GLsizei))p_glFramebufferTextureMultiviewOVR)(u0,u1,u2,i0,i1,i2); return 0; }
    case SC_GL_glTextureStorage2DEXT: { ARG_U32(r,u0); ARG_U32(r,u1); ARG_I32(r,i0); ARG_U32(r,u2); ARG_I32(r,i1); ARG_I32(r,i2);
        ((void(*)(GLuint,GLenum,GLsizei,GLenum,GLsizei,GLsizei))p_glTextureStorage2DEXT)(u0,u1,i0,u2,i1,i2); return 0; }
    case SC_GL_glTextureStorage3DEXT: { ARG_U32(r,u0); ARG_U32(r,u1); ARG_I32(r,i0); ARG_U32(r,u2); ARG_I32(r,i1); ARG_I32(r,i2); ARG_I32(r,i3);
        ((void(*)(GLuint,GLenum,GLsizei,GLenum,GLsizei,GLsizei,GLsizei))p_glTextureStorage3DEXT)(u0,u1,i0,u2,i1,i2,i3); return 0; }
    case SC_GL_glMultiDrawArraysEXT: {
        ARG_U32(r,u0); ARG_I32(r,i0);
        if (i0<0 || i0>65536) return -EPROTO;
        GLint *first = i0?malloc((size_t)i0*4):NULL;
        GLsizei *count = i0?malloc((size_t)i0*4):NULL;
        for (int32_t i=0;i<i0;i++) { ARG_I32(r,i1); first[i]=i1; ARG_I32(r,i2); count[i]=i2; }
        ((void(*)(GLenum,const GLint*,const GLsizei*,GLsizei))p_glMultiDrawArraysEXT)(u0,first,count,i0);
        free(first); free(count); return 0;
    }
    case SC_GL_glMultiDrawElementsEXT: {
        ARG_U32(r,u0); ARG_I32(r,i0);
        if (i0<0 || i0>65536) return -EPROTO;
        GLsizei *count = i0?malloc((size_t)i0*4):NULL;
        GLenum ty=0; const void **idx = i0?malloc(sizeof(void*)*i0):NULL;
        for (int32_t i=0;i<i0;i++) {
            ARG_I32(r,i1); count[i]=i1;
            ARG_U32(r,u1); ty=(GLenum)u1;
            ARG_U64(r,q0); idx[i]=(const void*)(uintptr_t)q0;
        }
        ((void(*)(GLenum,const GLsizei*,GLenum,const void*const*,GLsizei))p_glMultiDrawElementsEXT)(u0,count,ty,idx,i0);
        free(count); free(idx); return 0;
    }
    case SC_GL_glMultiDrawArraysIndirectEXT: { ARG_U32(r,u0); ARG_U64(r,q0); ARG_I32(r,i0); ARG_I32(r,i1);
        ((void(*)(GLenum,const void*,GLsizei,GLsizei))p_glMultiDrawArraysIndirectEXT)(u0,(const void*)(uintptr_t)q0,i0,i1); return 0; }
    case SC_GL_glMultiDrawElementsIndirectEXT: { ARG_U32(r,u0); ARG_U32(r,u1); ARG_U64(r,q0); ARG_I32(r,i0); ARG_I32(r,i1);
        ((void(*)(GLenum,GLenum,const void*,GLsizei,GLsizei))p_glMultiDrawElementsIndirectEXT)(u0,u1,(const void*)(uintptr_t)q0,i0,i1); return 0; }
    case SC_GL_glQueryCounterEXT: { ARG_U32(r,u0); ARG_U32(r,u1); ((void(*)(GLuint,GLenum))p_glQueryCounterEXT)(u0,u1); return 0; }
    case SC_GL_glFramebufferTexture2DDownsampleIMG: { ARG_U32(r,u0); ARG_U32(r,u1); ARG_U32(r,u2); ARG_U32(r,u3); ARG_I32(r,i0); ARG_I32(r,i1); ARG_I32(r,i2);
        ((void(*)(GLenum,GLenum,GLenum,GLuint,GLint,GLint,GLint))p_glFramebufferTexture2DDownsampleIMG)(u0,u1,u2,u3,i0,i1,i2); return 0; }
    case SC_GL_glFramebufferTextureLayerDownsampleIMG: { ARG_U32(r,u0); ARG_U32(r,u1); ARG_U32(r,u2); ARG_I32(r,i0); ARG_I32(r,i1); ARG_I32(r,i2); ARG_I32(r,i3);
        ((void(*)(GLenum,GLenum,GLuint,GLint,GLint,GLint,GLint))p_glFramebufferTextureLayerDownsampleIMG)(u0,u1,u2,i0,i1,i2,i3); return 0; }
    case SC_GL_glFramebufferTextureMultisampleMultiviewOVR: { ARG_U32(r,u0); ARG_U32(r,u1); ARG_U32(r,u2); ARG_I32(r,i0); ARG_I32(r,i1); ARG_I32(r,i2); ARG_I32(r,i3);
        ((void(*)(GLenum,GLenum,GLuint,GLint,GLsizei,GLint,GLsizei))p_glFramebufferTextureMultisampleMultiviewOVR)(u0,u1,u2,i0,i1,i2,i3); return 0; }

    /* Debug callback: no-op en el daemon (el callback se dispara en el shim
     * con SC_GL_EV_DEBUG_CB_FIRE, no acá). */
    case SC_GL_glDebugMessageCallback: return 0;
    case SC_GL_glGetDebugMessageLog: if (w) wr_u32(w,0); return 0;

    default: return -EPROTO;
    }
}

/* ============================================================ EGL dispatch */


/* ============================================================ present por AHardwareBuffer
 * Lectura rapida del frame: la GPU hace blit (con flip) del default framebuffer a una textura
 * respaldada por un AHardwareBuffer con CPU_READ_OFTEN (memoria CACHEADA). Leer directo del
 * pbuffer (memoria no cacheable) corre a ~50 MB/s; esto evita esa lectura. */
static __thread int g_req_fd = -1;   /* fd recibido (SCM_RIGHTS) con el request en curso */

struct ahb_desc { uint32_t width, height, layers, format; uint64_t usage; uint32_t stride, rfu0; uint64_t rfu1; };
static struct {
    int tried, ok;
    int (*allocate)(const struct ahb_desc*, void**);
    void (*release)(void*);
    int (*lock)(void*, uint64_t, int32_t, const void*, void**);
    int (*unlock)(void*, int32_t*);
    void (*describe)(const void*, struct ahb_desc*);
    void *(*getbuf)(void*);                 /* eglGetNativeClientBufferANDROID */
    void *(*mkimg)(void*, void*, unsigned, void*, const int32_t*);   /* eglCreateImageKHR */
    unsigned (*destroyimg)(void*, void*);   /* eglDestroyImageKHR */
    void *ctx; uint32_t w, h, stride_px;
    void *buf, *img; GLuint tex, fbo;
} g_ahb;

static void ahb_teardown(void) {
    if (g_ahb.fbo) ((void(*)(GLsizei,const GLuint*))p_glDeleteFramebuffers)(1, &g_ahb.fbo);
    if (g_ahb.tex) ((void(*)(GLsizei,const GLuint*))p_glDeleteTextures)(1, &g_ahb.tex);
    g_ahb.fbo = g_ahb.tex = 0;
    if (g_ahb.img && g_ahb.destroyimg) g_ahb.destroyimg(((void*(*)(void))p_eglGetCurrentDisplay)(), g_ahb.img);
    if (g_ahb.buf && g_ahb.release) g_ahb.release(g_ahb.buf);
    g_ahb.img = g_ahb.buf = NULL; g_ahb.ctx = NULL; g_ahb.w = g_ahb.h = 0;
}

/* Devuelve 1 si 'out' quedo con el frame BGRX arriba->abajo; 0 si no se pudo (usar fallback). */
static int present_ahb(uint32_t pw, uint32_t ph, uint8_t *out, int pst) {
    typedef void (*fn_geti)(GLenum, GLint*);
    typedef void (*fn_bindfb)(GLenum, GLuint);
    if (!g_ahb.tried) {
        g_ahb.tried = 1;
        void *h = dlopen("libnativewindow.so", RTLD_NOW);
        if (!h) h = dlopen("libandroid.so", RTLD_NOW);
        if (h) {
            g_ahb.allocate = dlsym(h, "AHardwareBuffer_allocate");
            g_ahb.release  = dlsym(h, "AHardwareBuffer_release");
            g_ahb.lock     = dlsym(h, "AHardwareBuffer_lock");
            g_ahb.unlock   = dlsym(h, "AHardwareBuffer_unlock");
            g_ahb.describe = dlsym(h, "AHardwareBuffer_describe");
        }
        if (p_eglGetProcAddress) {
            void *(*gpa)(const char*) = (void*(*)(const char*))p_eglGetProcAddress;
            g_ahb.getbuf     = gpa("eglGetNativeClientBufferANDROID");
            g_ahb.mkimg      = gpa("eglCreateImageKHR");
            g_ahb.destroyimg = gpa("eglDestroyImageKHR");
        }
        g_ahb.ok = g_ahb.allocate && g_ahb.release && g_ahb.lock && g_ahb.unlock && g_ahb.describe
                && g_ahb.getbuf && g_ahb.mkimg && g_ahb.destroyimg && p_glEGLImageTargetTexture2DOES;
        if (!g_ahb.ok) ERR("[scutumd] present AHB no disponible (libs/extensiones faltan), uso readback directo\n");
    }
    if (!g_ahb.ok) return 0;        /* sticky-off: si ya fallo, no reintentar en esta sesion. */
        if (!g_ahb.ok && g_ahb.tried) return 0;


    void *cc = ((void*(*)(void))p_eglGetCurrentContext)();
    if (g_ahb.buf && (g_ahb.ctx != cc || g_ahb.w != pw || g_ahb.h != ph)) ahb_teardown();
    if (!g_ahb.buf) {
        struct ahb_desc desc; memset(&desc, 0, sizeof desc);
        desc.width = pw; desc.height = ph; desc.layers = 1;
        desc.format = 1;                         /* AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM */
        desc.usage  = 3 /*CPU_READ_OFTEN*/ | 0x100 /*GPU_SAMPLED_IMAGE*/ | 0x200 /*GPU_COLOR_OUTPUT*/;
        if (g_ahb.allocate(&desc, &g_ahb.buf) != 0 || !g_ahb.buf) {
            ERR("[scutumd] AHardwareBuffer_allocate %ux%u fallo -> readback directo\n", pw, ph);
            g_ahb.ok = 0; g_ahb.buf = NULL; return 0;
        }
        struct ahb_desc got; g_ahb.describe(g_ahb.buf, &got);
        g_ahb.stride_px = got.stride ? got.stride : pw;
        void *cb = g_ahb.getbuf(g_ahb.buf);
        const int32_t attr[] = { 0x30D2 /*EGL_IMAGE_PRESERVED_KHR*/, 1, 0x3038 /*EGL_NONE*/ };
        g_ahb.img = g_ahb.mkimg(((void*(*)(void))p_eglGetCurrentDisplay)(), NULL /*EGL_NO_CONTEXT*/,
                                0x3140 /*EGL_NATIVE_BUFFER_ANDROID*/, cb, attr);
        if (!g_ahb.img) {
            ERR("[scutumd] eglCreateImageKHR(AHB) fallo (eglGetError=0x%x) -> readback directo\n", (unsigned)((EGLint(*)(void))p_eglGetError)());
            /* NO liberar el AHB aca. El destructor del RefBase de Android
             * (libutils.so, decStrong) crashea con SIGSEGV cuando el AHB
             * nunca se enlazo bien a un EGLImage. Eso corrompe la libEGL
             * de Android de forma irreversible y todas las operaciones EGL
             * posteriores fallan -> RE no crea surface. El AHB se libera
             * solo cuando el fd subyacente se cierra. */
            g_ahb.buf = NULL; g_ahb.ok = 0; return 0;
        }
        GLint ptex = 0, pdraw = 0, pread = 0;
        ((fn_geti)p_glGetIntegerv)(0x8069 /*TEXTURE_BINDING_2D*/, &ptex);
        ((fn_geti)p_glGetIntegerv)(0x8CA6, &pdraw);
        ((fn_geti)p_glGetIntegerv)(0x8CAA, &pread);
        ((void(*)(GLsizei,GLuint*))p_glGenTextures)(1, &g_ahb.tex);
        ((void(*)(GLenum,GLuint))p_glBindTexture)(GL_TEXTURE_2D, g_ahb.tex);
        ((void(*)(GLenum,GLeglImageOES))p_glEGLImageTargetTexture2DOES)(GL_TEXTURE_2D, (GLeglImageOES)g_ahb.img);
        ((void(*)(GLenum,GLuint))p_glBindTexture)(GL_TEXTURE_2D, (GLuint)ptex);
        ((void(*)(GLsizei,GLuint*))p_glGenFramebuffers)(1, &g_ahb.fbo);
        ((fn_bindfb)p_glBindFramebuffer)(GL_FRAMEBUFFER, g_ahb.fbo);
        ((void(*)(GLenum,GLenum,GLenum,GLuint,GLint))p_glFramebufferTexture2D)(GL_FRAMEBUFFER, 0x8CE0 /*COLOR_ATTACHMENT0*/, GL_TEXTURE_2D, g_ahb.tex, 0);
        GLenum st = ((GLenum(*)(GLenum))p_glCheckFramebufferStatus)(GL_FRAMEBUFFER);
        ((fn_bindfb)p_glBindFramebuffer)(0x8CA9, (GLuint)pdraw);
        ((fn_bindfb)p_glBindFramebuffer)(0x8CA8, (GLuint)pread);
        if (st != 0x8CD5 /*FRAMEBUFFER_COMPLETE*/) {
            ERR("[scutumd] FBO sobre AHB incompleto (0x%x) -> readback directo\n", (unsigned)st);
            /* mismo motivo que arriba: no liberar nada, Android limpia por fd. */
            g_ahb.buf = NULL; g_ahb.img = NULL; g_ahb.ok = 0; return 0;
        }
        g_ahb.ctx = cc; g_ahb.w = pw; g_ahb.h = ph;
        ERR("[scutumd] present por AHardwareBuffer activo: %ux%u stride=%u px\n", pw, ph, g_ahb.stride_px);
    }

    struct timespec t0, t1, t2;
    if (pst) clock_gettime(CLOCK_MONOTONIC, &t0);
    GLint pdraw = 0, pread = 0;
    ((fn_geti)p_glGetIntegerv)(0x8CA6, &pdraw);
    ((fn_geti)p_glGetIntegerv)(0x8CAA, &pread);
    GLboolean sc = ((GLboolean(*)(GLenum))p_glIsEnabled)(0x0C11 /*SCISSOR_TEST*/);
    if (sc) ((void(*)(GLenum))p_glDisable)(0x0C11);
    ((fn_bindfb)p_glBindFramebuffer)(0x8CA8, 0);
    ((fn_bindfb)p_glBindFramebuffer)(0x8CA9, g_ahb.fbo);
    ((void(*)(GLint,GLint,GLint,GLint,GLint,GLint,GLint,GLint,GLbitfield,GLenum))p_glBlitFramebuffer)(
        0, 0, (GLint)pw, (GLint)ph, 0, (GLint)ph, (GLint)pw, 0, 0x4000 /*COLOR_BUFFER_BIT*/, 0x2600 /*NEAREST*/);
    ((fn_bindfb)p_glBindFramebuffer)(0x8CA9, (GLuint)pdraw);
    ((fn_bindfb)p_glBindFramebuffer)(0x8CA8, (GLuint)pread);
    if (sc) ((void(*)(GLenum))p_glEnable)(0x0C11);
    ((void(*)(void))p_glFinish)();
    if (pst) clock_gettime(CLOCK_MONOTONIC, &t1);

    void *ptr = NULL;
    if (g_ahb.lock(g_ahb.buf, 3 /*CPU_READ_OFTEN*/, -1, NULL, &ptr) != 0 || !ptr) {
        ERR("[scutumd] AHardwareBuffer_lock fallo -> readback directo\n");
        ahb_teardown(); g_ahb.ok = 0; return 0;
    }
    size_t src_stride = (size_t)g_ahb.stride_px * 4, dst_stride = (size_t)pw * 4;
    for (uint32_t y = 0; y < ph; y++) {
        const uint32_t *s = (const uint32_t *)((const uint8_t *)ptr + y * src_stride);
        uint32_t *o = (uint32_t *)(out + y * dst_stride);
        for (uint32_t x = 0; x < pw; x++) {
            uint32_t v = s[x];
            o[x] = (v & 0xFF00FF00u) | ((v & 0xFFu) << 16) | ((v >> 16) & 0xFFu);
        }
    }
    g_ahb.unlock(g_ahb.buf, NULL);
    if (pst) {
        clock_gettime(CLOCK_MONOTONIC, &t2);
        static double a1, a2; static unsigned n;
        a1 += (t1.tv_sec-t0.tv_sec)*1e3 + (t1.tv_nsec-t0.tv_nsec)/1e6;
        a2 += (t2.tv_sec-t1.tv_sec)*1e3 + (t2.tv_nsec-t1.tv_nsec)/1e6;
        if (++n == 30) {
            ERR("[scutumd-stats] present(ahb) x30: blit+flip GPU + glFinish %.1f ms | lock+copy+swizzle %.1f ms (medias)\n", a1/30, a2/30);
            n = 0; a1 = a2 = 0;
        }
    }
    return 1;
}

static int32_t exec_egl_one(uint32_t op, struct rd *r, struct wr *w) {
    uint64_t q0, q1, q2, q3;
    uint32_t u0;
    int32_t  i0, i1;
    const uint8_t *blob = NULL; uint32_t blen = 0;

    switch (op) {
    case SC_EGL_GET_DISPLAY: {
        ARG_U64(r, q0);
        EGLDisplay d = ((EGLDisplay(*)(EGLNativeDisplayType))p_eglGetDisplay)(
            (EGLNativeDisplayType)(uintptr_t)q0);
        if (w) wr_u64(w, (uint64_t)(uintptr_t)d);
        return 1;
    }
    case SC_EGL_GET_PLATFORM_DISPLAY: {
        ARG_U32(r, u0); ARG_U64(r, q0);
        uint32_t nattr; ARG_U32(r, nattr);
        if (nattr > 256) return -EPROTO;
        /* El shim emite los attrs como i32 (formato EXT). Si se usa la
         * variante core con EGLAttrib (i64), hay que arreglar sc_egl.c. */
        EGLAttrib attrs[257];
        for (uint32_t i = 0; i < nattr; i++) { ARG_I32(r, i0); attrs[i] = (EGLAttrib)i0; }
        if (nattr == 0) { attrs[0] = EGL_NONE; }
        EGLDisplay d = EGL_NO_DISPLAY;
        if (p_eglGetPlatformDisplay) {
            d = ((EGLDisplay(*)(EGLenum,void*,const EGLAttrib*))p_eglGetPlatformDisplay)(
                (EGLenum)u0, (void*)(uintptr_t)q0, attrs);
        }
        if (w) wr_u64(w, (uint64_t)(uintptr_t)d);
        return 1;
    }
    case SC_EGL_INITIALIZE: {
        ARG_U64(r, q0);
        EGLint maj = 0, mnr = 0;
        EGLBoolean ok = ((EGLBoolean(*)(EGLDisplay,EGLint*,EGLint*))p_eglInitialize)(
            (EGLDisplay)(uintptr_t)q0, &maj, &mnr);
        if (w) { wr_i32(w, maj); wr_i32(w, mnr); }
        return ok ? 1 : 0;
    }
    case SC_EGL_TERMINATE: {
        ARG_U64(r, q0);
        EGLBoolean ok = ((EGLBoolean(*)(EGLDisplay))p_eglTerminate)((EGLDisplay)(uintptr_t)q0);
        return ok ? 1 : 0;
    }
    case SC_EGL_QUERY_STRING: {
        ARG_U64(r, q0); ARG_I32(r, i0);
        const char *s = ((const char*(*)(EGLDisplay,EGLint))p_eglQueryString)(
            (EGLDisplay)(uintptr_t)q0, i0);
        if (w) {
            size_t n = s ? strlen(s) : 0;
            wr_u32(w, (uint32_t)n);
            if (n) wr_bytes(w, s, n);
        }
        return 1;
    }
    case SC_EGL_GET_ERROR:
        return (int32_t)((EGLint(*)(void))p_eglGetError)();

    case SC_EGL_GET_CONFIGS: {
        ARG_U64(r, q0); ARG_U32(r, u0);
        EGLint maxc = (EGLint)u0;
        if (maxc < 0) maxc = 0;
        if (maxc > 4096) maxc = 4096;
        EGLint nout = 0;
        EGLConfig *cfgs = maxc ? malloc(sizeof(EGLConfig) * (size_t)maxc) : NULL;
        EGLBoolean ok = ((EGLBoolean(*)(EGLDisplay,EGLConfig*,EGLint,EGLint*))p_eglGetConfigs)(
            (EGLDisplay)(uintptr_t)q0, cfgs, maxc, &nout);
        if (w) {
            /* count-only (maxc=0, cfgs=NULL): devolvemos el total del driver.
             * Antes un `if (!cfgs) n = 0;` tiraba el count a 0 y gl4es no
             * encontraba GLX visual -> RE no abria ventana. */
            EGLint n     = (maxc > 0 && nout > maxc) ? maxc : nout;
            EGLint nw    = cfgs ? n : 0;
            wr_u32(w, (uint32_t)n);
            for (EGLint i = 0; i < nw; i++) wr_u64(w, (uint64_t)(uintptr_t)cfgs[i]);
        }
        free(cfgs);
        return ok ? 1 : 0;
    }
    case SC_EGL_CHOOSE_CONFIG: {
        ARG_U64(r, q0);
        uint32_t nattr; ARG_U32(r, nattr);
        if (nattr > 256) return -EPROTO;
        EGLint attrs[257];
        for (uint32_t i = 0; i < nattr; i++) { ARG_I32(r, i0); attrs[i] = i0; }
        if (nattr == 0) attrs[0] = EGL_NONE;
        ARG_U32(r, u0);
        EGLint maxc = (EGLint)u0;
        if (maxc < 0) maxc = 0;
        if (maxc > 4096) maxc = 4096;
        EGLint nout = 0;
        EGLConfig *cfgs = maxc ? malloc(sizeof(EGLConfig) * (size_t)maxc) : NULL;
        EGLBoolean ok = ((EGLBoolean(*)(EGLDisplay,const EGLint*,EGLConfig*,EGLint,EGLint*))p_eglChooseConfig)(
            (EGLDisplay)(uintptr_t)q0, attrs, cfgs, maxc, &nout);
        if (w) {
            /* idem GET_CONFIGS: count-only no debe pisar el count del driver. */
            EGLint n     = (maxc > 0 && nout > maxc) ? maxc : nout;
            EGLint nw    = cfgs ? n : 0;
            wr_u32(w, (uint32_t)n);
            for (EGLint i = 0; i < nw; i++) wr_u64(w, (uint64_t)(uintptr_t)cfgs[i]);
        }
        free(cfgs);
        return ok ? 1 : 0;
    }
    case SC_EGL_GET_CONFIG_ATTRIB: {
        ARG_U64(r, q0); ARG_U64(r, q1); ARG_I32(r, i0);
        EGLint val = 0;
        EGLBoolean ok = ((EGLBoolean(*)(EGLDisplay,EGLConfig,EGLint,EGLint*))p_eglGetConfigAttrib)(
            (EGLDisplay)(uintptr_t)q0, (EGLConfig)(uintptr_t)q1, i0, &val);
        if (w) wr_i32(w, val);
        return ok ? 1 : 0;
    }

    case SC_EGL_CREATE_CONTEXT: {
        ARG_U64(r, q0); ARG_U64(r, q1); ARG_U64(r, q2);
        uint32_t nattr; ARG_U32(r, nattr);
        if (nattr > 256) return -EPROTO;
        EGLint attrs[257];
        for (uint32_t i = 0; i < nattr; i++) { ARG_I32(r, i0); attrs[i] = i0; }
        if (nattr == 0) attrs[0] = EGL_NONE;
        EGLContext ctx = ((EGLContext(*)(EGLDisplay,EGLConfig,EGLContext,const EGLint*))p_eglCreateContext)(
            (EGLDisplay)(uintptr_t)q0, (EGLConfig)(uintptr_t)q1,
            (EGLContext)(uintptr_t)q2, attrs);
        if (w) wr_u64(w, (uint64_t)(uintptr_t)ctx);
        return 1;
    }
    case SC_EGL_DESTROY_CONTEXT: {
        ARG_U64(r, q0); ARG_U64(r, q1);
        EGLBoolean ok = ((EGLBoolean(*)(EGLDisplay,EGLContext))p_eglDestroyContext)(
            (EGLDisplay)(uintptr_t)q0, (EGLContext)(uintptr_t)q1);
        return ok ? 1 : 0;
    }
    case SC_EGL_MAKE_CURRENT: {
        ARG_U64(r, q0); ARG_U64(r, q1); ARG_U64(r, q2); ARG_U64(r, q3);
        EGLBoolean ok = ((EGLBoolean(*)(EGLDisplay,EGLSurface,EGLSurface,EGLContext))p_eglMakeCurrent)(
            (EGLDisplay)(uintptr_t)q0, (EGLSurface)(uintptr_t)q1,
            (EGLSurface)(uintptr_t)q2, (EGLContext)(uintptr_t)q3);
        return ok ? 1 : 0;
    }
    case SC_EGL_GET_CURRENT_CONTEXT: {
        EGLContext c = ((EGLContext(*)(void))p_eglGetCurrentContext)();
        if (w) wr_u64(w, (uint64_t)(uintptr_t)c);
        return 1;
    }
    case SC_EGL_GET_CURRENT_SURFACE: {
        ARG_I32(r, i0);
        EGLSurface s = ((EGLSurface(*)(EGLint))p_eglGetCurrentSurface)(i0);
        if (w) wr_u64(w, (uint64_t)(uintptr_t)s);
        return 1;
    }
    case SC_EGL_GET_CURRENT_DISPLAY: {
        EGLDisplay d = ((EGLDisplay(*)(void))p_eglGetCurrentDisplay)();
        if (w) wr_u64(w, (uint64_t)(uintptr_t)d);
        return 1;
    }
    case SC_EGL_RELEASE_THREAD: {
        EGLBoolean ok = ((EGLBoolean(*)(void))p_eglReleaseThread)();
        return ok ? 1 : 0;
    }

    case SC_EGL_CREATE_WINDOW_SURFACE: {
        ARG_U64(r, q0); ARG_U64(r, q1); ARG_U64(r, q2);
        uint32_t nattr; ARG_U32(r, nattr);
        if (nattr > 256) return -EPROTO;
        EGLint attrs[257];
        for (uint32_t i = 0; i < nattr; i++) { ARG_I32(r, i0); attrs[i] = i0; }
        if (nattr == 0) attrs[0] = EGL_NONE;
        uint32_t nfds = 0;
        if (r->o < r->n) { if (rd_u32(r, &nfds)) return -EPROTO; }
        EGLSurface s = ((EGLSurface(*)(EGLDisplay,EGLConfig,EGLNativeWindowType,const EGLint*))p_eglCreateWindowSurface)(
            (EGLDisplay)(uintptr_t)q0, (EGLConfig)(uintptr_t)q1,
            (EGLNativeWindowType)(uintptr_t)q2, attrs);
        if (w) wr_u64(w, (uint64_t)(uintptr_t)s);
        return 1;
    }
    case SC_EGL_CREATE_PBUFFER_SURFACE: {
        ARG_U64(r, q0); ARG_U64(r, q1);
        uint32_t nattr; ARG_U32(r, nattr);
        if (nattr > 256) return -EPROTO;
        EGLint attrs[257];
        for (uint32_t i = 0; i < nattr; i++) { ARG_I32(r, i0); attrs[i] = i0; }
        if (nattr == 0) attrs[0] = EGL_NONE;
        EGLSurface s = ((EGLSurface(*)(EGLDisplay,EGLConfig,const EGLint*))p_eglCreatePbufferSurface)(
            (EGLDisplay)(uintptr_t)q0, (EGLConfig)(uintptr_t)q1, attrs);
        if (w) wr_u64(w, (uint64_t)(uintptr_t)s);
        return 1;
    }
    case SC_EGL_CREATE_PIXMAP_SURFACE: {
        ARG_U64(r, q0); ARG_U64(r, q1); ARG_U64(r, q2);
        uint32_t nattr; ARG_U32(r, nattr);
        if (nattr > 256) return -EPROTO;
        EGLint attrs[257];
        for (uint32_t i = 0; i < nattr; i++) { ARG_I32(r, i0); attrs[i] = i0; }
        if (nattr == 0) attrs[0] = EGL_NONE;
        EGLSurface s = ((EGLSurface(*)(EGLDisplay,EGLConfig,EGLNativePixmapType,const EGLint*))p_eglCreatePixmapSurface)(
            (EGLDisplay)(uintptr_t)q0, (EGLConfig)(uintptr_t)q1,
            (EGLNativePixmapType)(uintptr_t)q2, attrs);
        if (w) wr_u64(w, (uint64_t)(uintptr_t)s);
        return 1;
    }
    case SC_EGL_DESTROY_SURFACE: {
        ARG_U64(r, q0); ARG_U64(r, q1);
        EGLBoolean ok = ((EGLBoolean(*)(EGLDisplay,EGLSurface))p_eglDestroySurface)(
            (EGLDisplay)(uintptr_t)q0, (EGLSurface)(uintptr_t)q1);
        return ok ? 1 : 0;
    }
    case SC_EGL_QUERY_SURFACE: {
        ARG_U64(r, q0); ARG_U64(r, q1); ARG_I32(r, i0);
        EGLint v = 0;
        EGLBoolean ok = ((EGLBoolean(*)(EGLDisplay,EGLSurface,EGLint,EGLint*))p_eglQuerySurface)(
            (EGLDisplay)(uintptr_t)q0, (EGLSurface)(uintptr_t)q1, i0, &v);
        if (w) wr_i32(w, v);
        return ok ? 1 : 0;
    }
    case SC_EGL_SURFACE_ATTRIB: {
        ARG_U64(r, q0); ARG_U64(r, q1); ARG_I32(r, i0); ARG_I32(r, i1);
        EGLBoolean ok = ((EGLBoolean(*)(EGLDisplay,EGLSurface,EGLint,EGLint))p_eglSurfaceAttrib)(
            (EGLDisplay)(uintptr_t)q0, (EGLSurface)(uintptr_t)q1, i0, i1);
        return ok ? 1 : 0;
    }
    case SC_EGL_PRESENT: {
        ARG_U32(r, u0); ARG_I32(r, i0);
        uint32_t pw = u0, ph = (uint32_t)i0;
        if (!pw || !ph || pw > 16384 || ph > 16384) return -EPROTO;
        size_t stride = (size_t)pw * 4, total = stride * ph;
        if (total > SC_MAX_PAYLOAD - 8) return -EMSGSIZE;
        static __thread uint8_t *rb = NULL, *out = NULL; static __thread size_t cap = 0;
        if (cap < total) {
            free(rb); free(out);
            rb = malloc(total); out = malloc(total); cap = (rb && out) ? total : 0;
            if (!cap) { free(rb); free(out); rb = out = NULL; return -ENOMEM; }
        }
        /* Destino: memoria compartida con el shim (MIT-SHM) o buffer propio + respuesta por socket. */
        uint32_t slot = 0xFFFFFFFFu, attach = 0;
        if (r->o + 8 <= r->n) { rd_u32(r, &slot); rd_u32(r, &attach); }
        static __thread struct { void *p; size_t sz; } smap[2];
        uint8_t *dst = out; int to_shm = 0;
        if (slot < 2) {
            if (attach && g_req_fd >= 0) {
                if (smap[slot].p) munmap(smap[slot].p, smap[slot].sz);
                void *mp = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, g_req_fd, 0);
                smap[slot].p = (mp == MAP_FAILED) ? NULL : mp;
                smap[slot].sz = total;
                if (!smap[slot].p) ERR("[scutumd] mmap(shm fd) fallo errno=%d -> frames por socket\n", errno);
            }
            if (smap[slot].p && smap[slot].sz == total) { dst = smap[slot].p; to_shm = 1; }
        }
        /* Modo: por defecto AHardwareBuffer (GPU blit + lectura cacheada); SCUTUM_PRESENT=sync|pbo. pbo -> PBO doble (1 frame de latencia, medido MAS LENTO).
         * SCUTUM_BGRA=1 -> leer GL_BGRA_EXT (sin swizzle R/B). */
        static int mode = -1, use_bgra = 0, pst = -1;
        if (mode < 0) {
            const char *e = getenv("SCUTUM_PRESENT"); mode = (e && !strcmp(e, "pbo")) ? 1 : (e && !strcmp(e, "sync")) ? 0 : 2;
            const char *b2 = getenv("SCUTUM_BGRA");   use_bgra = (b2 && *b2 == '1');
            const char *s = getenv("SCUTUM_STATS");   pst = (s && *s == '1');
        }
        if (mode == 2) {
            if (present_ahb(pw, ph, dst, pst)) { if (!to_shm && w) wr_bytes(w, dst, total); return to_shm ? 1 : (slot < 2 ? 2 : 1); }
            mode = 0;   /* AHB no disponible: readback directo de ahi en adelante */
        }
        const GLenum rfmt = use_bgra ? 0x80E1 /*GL_BGRA_EXT*/ : GL_RGBA;
        typedef void (*fn_rp)(GLint,GLint,GLsizei,GLsizei,GLenum,GLenum,void*);
        #define MS(x,y) (((y).tv_sec-(x).tv_sec)*1e3 + ((y).tv_nsec-(x).tv_nsec)/1e6)
        static double a_fin, a_rd, a_cv; static unsigned pn;
        struct timespec ta, tb, tc, td;

        GLint prev_fbo = 0, prev_pbo = 0;
        ((void(*)(GLenum,GLint*))p_glGetIntegerv)(0x8CA6, &prev_fbo);
        ((void(*)(GLenum,GLint*))p_glGetIntegerv)(0x88ED, &prev_pbo);
        ((void(*)(GLenum,GLuint))p_glBindFramebuffer)(GL_FRAMEBUFFER, 0);
        if (pst) clock_gettime(CLOCK_MONOTONIC, &ta);

        {   /* SONDA: SCUTUM_PROBE=1 -> separa costo fijo de costo por pixel (cada 60 frames) */
            static int probe = -1; static unsigned pc;
            if (probe < 0) { const char *e = getenv("SCUTUM_PROBE"); probe = (e && *e == '1'); }
            if (probe && !mode && (pc++ % 60) == 0) {
                struct timespec p0, p1, p2, p3, p4, p5;
                if (prev_pbo) ((void(*)(GLenum,GLuint))p_glBindBuffer)(0x88EB, 0);
                clock_gettime(CLOCK_MONOTONIC, &p0);
                ((void(*)(void))p_glFinish)();
                clock_gettime(CLOCK_MONOTONIC, &p1);
                ((fn_rp)p_glReadPixels)(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rb);
                clock_gettime(CLOCK_MONOTONIC, &p2);
                ((fn_rp)p_glReadPixels)(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rb);
                clock_gettime(CLOCK_MONOTONIC, &p3);
                ((fn_rp)p_glReadPixels)(0, 0, (GLsizei)(pw < 128 ? pw : 128), (GLsizei)(ph < 128 ? ph : 128), GL_RGBA, GL_UNSIGNED_BYTE, rb);
                clock_gettime(CLOCK_MONOTONIC, &p4);
                ((fn_rp)p_glReadPixels)(0, 0, (GLsizei)pw, (GLsizei)ph, GL_RGBA, GL_UNSIGNED_BYTE, rb);
                clock_gettime(CLOCK_MONOTONIC, &p5);
                ERR("[scutumd-probe] glFinish %.2f | 1x1 (1a) %.2f | 1x1 (2a) %.2f | 128x128 %.2f | %ux%u completo %.2f ms\n",
                    MS(p0,p1), MS(p1,p2), MS(p2,p3), MS(p3,p4), pw, ph, MS(p4,p5));
            }
        }
        const uint8_t *src = rb;
        if (!mode) {
            if (prev_pbo) ((void(*)(GLenum,GLuint))p_glBindBuffer)(0x88EB, 0);
            ((fn_rp)p_glReadPixels)(0, 0, (GLsizei)pw, (GLsizei)ph, rfmt, GL_UNSIGNED_BYTE, rb);
        } else {
            /* PBO doble: el readback de este frame va a pbo[cur] (async, la GPU lo copia);
             * se devuelve el del frame anterior pbo[prev], ya completo. */
            static EGLContext pctx; static GLuint pbo[2]; static uint32_t pbw, pbh;
            static unsigned pframe; static int ready;
            EGLContext cc = ((EGLContext(*)(void))p_eglGetCurrentContext)();
            if (!ready || cc != pctx || pbw != pw || pbh != ph) {
                ((void(*)(GLsizei,GLuint*))p_glGenBuffers)(2, pbo);
                for (int k = 0; k < 2; k++) {
                    ((void(*)(GLenum,GLuint))p_glBindBuffer)(0x88EB, pbo[k]);
                    ((void(*)(GLenum,GLsizeiptr,const void*,GLenum))p_glBufferData)(0x88EB, (GLsizeiptr)total, NULL, 0x88E1 /*STREAM_READ*/);
                }
                pctx = cc; pbw = pw; pbh = ph; pframe = 0; ready = 1;
            }
            unsigned cur = pframe & 1, prv = cur ^ 1;
            ((void(*)(GLenum,GLuint))p_glBindBuffer)(0x88EB, pbo[cur]);
            ((fn_rp)p_glReadPixels)(0, 0, (GLsizei)pw, (GLsizei)ph, rfmt, GL_UNSIGNED_BYTE, (void*)0);
            /* primer frame: leer el mismo (bloqueante); luego, el anterior */
            unsigned rd = pframe ? prv : cur;
            if (rd != cur) ((void(*)(GLenum,GLuint))p_glBindBuffer)(0x88EB, pbo[rd]);
            if (pst) clock_gettime(CLOCK_MONOTONIC, &tb);
            void *mp = ((void*(*)(GLenum,GLintptr,GLsizeiptr,GLbitfield))p_glMapBufferRange)(0x88EB, 0, (GLsizeiptr)total, GL_MAP_READ_BIT);
            if (mp) { memcpy(rb, mp, total); ((GLboolean(*)(GLenum))p_glUnmapBuffer)(0x88EB); }
            else memset(rb, 0, total);
            pframe++;
            ((void(*)(GLenum,GLuint))p_glBindBuffer)(0x88EB, 0);
        }
        if (pst) clock_gettime(CLOCK_MONOTONIC, &tc);
        ((void(*)(GLenum,GLuint))p_glBindFramebuffer)(GL_FRAMEBUFFER, (GLuint)prev_fbo);
        ((void(*)(GLenum,GLuint))p_glBindBuffer)(0x88EB, (GLuint)prev_pbo);

        /* flip vertical (+ RGBA->BGRA si se leyó RGBA) en una pasada */
        for (uint32_t y = 0; y < ph; y++) {
            const uint32_t *s = (const uint32_t *)(src + (size_t)(ph - 1 - y) * stride);
            uint32_t *o = (uint32_t *)(dst + (size_t)y * stride);
            if (use_bgra) memcpy(o, s, stride);
            else for (uint32_t x = 0; x < pw; x++) {
                uint32_t v = s[x];
                o[x] = (v & 0xFF00FF00u) | ((v & 0xFFu) << 16) | ((v >> 16) & 0xFFu);
            }
        }
        if (pst) {
            clock_gettime(CLOCK_MONOTONIC, &td);
            a_fin += mode ? MS(ta,tb) : 0; a_rd += mode ? MS(tb,tc) : MS(ta,tc); a_cv += MS(tc,td);
            if (++pn == 30) {
                ERR("[scutumd-stats] present(%s%s) x30: issue-readpixels %.1f ms | %s %.1f ms | flip+swizzle %.1f ms (medias)\n",
                    mode ? "pbo" : "sync", use_bgra ? ",bgra" : ",rgba",
                    a_fin/30, mode ? "map+copy" : "readpixels", a_rd/30, a_cv/30);
                pn = 0; a_fin = a_rd = a_cv = 0;
            }
        }
        if (to_shm) return 1;                 /* el frame ya esta en la memoria compartida */
        if (w) wr_bytes(w, dst, total);
        return slot < 2 ? 2 : 1;              /* 2 = "shm no disponible, los bytes siguen" */
    }
    case SC_EGL_SWAP_BUFFERS: {
        ARG_U64(r, q0); ARG_U64(r, q1);
        uint32_t nfds = 0;
        if (r->o < r->n) { if (rd_u32(r, &nfds)) return -EPROTO; }
        EGLBoolean ok = ((EGLBoolean(*)(EGLDisplay,EGLSurface))p_eglSwapBuffers)(
            (EGLDisplay)(uintptr_t)q0, (EGLSurface)(uintptr_t)q1);
        return ok ? 1 : 0;
    }
    case SC_EGL_SWAP_BUFFERS_WITH_DAMAGE: {
        ARG_U64(r, q0); ARG_U64(r, q1); ARG_U32(r, u0);
        int32_t n = (int32_t)u0;
        if (n < 0 || n > 4096) return -EPROTO;
        EGLint *rects = n ? malloc((size_t)n * 4 * 4) : NULL;
        for (int32_t i = 0; i < n * 4; i++) { ARG_I32(r, i0); rects[i] = i0; }
        EGLBoolean ok = EGL_TRUE;
        if (p_eglSwapBuffersWithDamageKHR)
            ok = ((EGLBoolean(*)(EGLDisplay,EGLSurface,EGLint*,EGLint))p_eglSwapBuffersWithDamageKHR)(
                (EGLDisplay)(uintptr_t)q0, (EGLSurface)(uintptr_t)q1, rects, n);
        free(rects);
        return ok ? 1 : 0;
    }
    case SC_EGL_SWAP_INTERVAL: {
        ARG_U64(r, q0); ARG_I32(r, i0);
        EGLBoolean ok = ((EGLBoolean(*)(EGLDisplay,EGLint))p_eglSwapInterval)(
            (EGLDisplay)(uintptr_t)q0, i0);
        return ok ? 1 : 0;
    }
    case SC_EGL_COPY_BUFFERS: {
        ARG_U64(r, q0); ARG_U64(r, q1); ARG_U64(r, q2);
        uint32_t nfds = 0;
        if (r->o < r->n) { if (rd_u32(r, &nfds)) return -EPROTO; }
        EGLBoolean ok = ((EGLBoolean(*)(EGLDisplay,EGLSurface,EGLNativePixmapType))p_eglCopyBuffers)(
            (EGLDisplay)(uintptr_t)q0, (EGLSurface)(uintptr_t)q1,
            (EGLNativePixmapType)(uintptr_t)q2);
        return ok ? 1 : 0;
    }
    case SC_EGL_BIND_TEX_IMAGE: {
        ARG_U64(r, q0); ARG_U64(r, q1); ARG_I32(r, i0);
        EGLBoolean ok = ((EGLBoolean(*)(EGLDisplay,EGLSurface,EGLint))p_eglBindTexImage)(
            (EGLDisplay)(uintptr_t)q0, (EGLSurface)(uintptr_t)q1, i0);
        return ok ? 1 : 0;
    }
    case SC_EGL_RELEASE_TEX_IMAGE: {
        ARG_U64(r, q0); ARG_U64(r, q1); ARG_I32(r, i0);
        EGLBoolean ok = ((EGLBoolean(*)(EGLDisplay,EGLSurface,EGLint))p_eglReleaseTexImage)(
            (EGLDisplay)(uintptr_t)q0, (EGLSurface)(uintptr_t)q1, i0);
        return ok ? 1 : 0;
    }

    case SC_EGL_BIND_API: {
        ARG_U32(r, u0);
        EGLBoolean ok = ((EGLBoolean(*)(EGLenum))p_eglBindAPI)((EGLenum)u0);
        return ok ? 1 : 0;
    }
    case SC_EGL_QUERY_API:
        return (int32_t)((EGLenum(*)(void))p_eglQueryAPI)();

    case SC_EGL_GET_PROC_ADDRESS: {
        if (rd_blob(r, &blob, &blen)) return -EPROTO;
        char name[256];
        size_t l = blen < sizeof(name) - 1 ? blen : sizeof(name) - 1;
        memcpy(name, blob, l);
        name[l] = 0;
        void *fn = NULL;
        uint32_t kind = 0;
        if ((fn = dlsym(g_gles_handle, name))) kind = 2;
        else if ((fn = dlsym(g_egl_handle, name))) kind = 1;
        else if (!strcmp(name, "glGetProgramBinaryOES") || !strcmp(name, "glProgramBinaryOES")) {
            /* El cliente tiene shim propio (alias del core); basta con anunciarlo. */
            fn = (void*)p_glGetProgramBinary; kind = 2;
        }
        if (w) {
            wr_u32(w, fn ? 1 : 0);
            wr_u32(w, kind);
            wr_u32(w, 0);
        }
        return 1;
    }

    case SC_EGL_WAIT_CLIENT: return ((EGLBoolean(*)(void))p_eglWaitClient)() ? 1 : 0;
    case SC_EGL_WAIT_GL:     return ((EGLBoolean(*)(void))p_eglWaitGL)() ? 1 : 0;
    case SC_EGL_WAIT_NATIVE: { ARG_I32(r, i0); return ((EGLBoolean(*)(EGLint))p_eglWaitNative)(i0) ? 1 : 0; }

    case SC_EGL_CREATE_SYNC: {
        ARG_U64(r, q0); ARG_U32(r, u0);
        uint32_t nattr; ARG_U32(r, nattr);
        if (nattr > 256) return -EPROTO;
        EGLAttrib attrs[257];
        for (uint32_t i = 0; i < nattr; i++) { ARG_U64(r, q1); attrs[i] = (EGLAttrib)q1; }
        if (nattr == 0) attrs[0] = EGL_NONE;
        EGLSync s = ((EGLSync(*)(EGLDisplay,EGLenum,const EGLAttrib*))p_eglCreateSync)(
            (EGLDisplay)(uintptr_t)q0, (EGLenum)u0, attrs);
        if (w) wr_u64(w, (uint64_t)(uintptr_t)s);
        return 1;
    }
    case SC_EGL_DESTROY_SYNC: {
        ARG_U64(r, q0); ARG_U64(r, q1);
        EGLBoolean ok = ((EGLBoolean(*)(EGLDisplay,EGLSync))p_eglDestroySync)(
            (EGLDisplay)(uintptr_t)q0, (EGLSync)(uintptr_t)q1);
        return ok ? 1 : 0;
    }
    case SC_EGL_CLIENT_WAIT_SYNC: {
        ARG_U64(r, q0); ARG_U64(r, q1); ARG_I32(r, i0); ARG_U64(r, q2);
        return (int32_t)((EGLint(*)(EGLDisplay,EGLSync,EGLint,EGLTime))p_eglClientWaitSync)(
            (EGLDisplay)(uintptr_t)q0, (EGLSync)(uintptr_t)q1, i0, (EGLTime)q2);
    }
    case SC_EGL_WAIT_SYNC: {
        ARG_U64(r, q0); ARG_U64(r, q1); ARG_I32(r, i0);
        EGLBoolean ok = ((EGLBoolean(*)(EGLDisplay,EGLSync,EGLint))p_eglWaitSync)(
            (EGLDisplay)(uintptr_t)q0, (EGLSync)(uintptr_t)q1, i0);
        return ok ? 1 : 0;
    }
    case SC_EGL_GET_SYNC_ATTRIB: {
        ARG_U64(r, q0); ARG_U64(r, q1); ARG_I32(r, i0);
        EGLAttrib v = 0;
        EGLBoolean ok = ((EGLBoolean(*)(EGLDisplay,EGLSync,EGLint,EGLAttrib*))p_eglGetSyncAttrib)(
            (EGLDisplay)(uintptr_t)q0, (EGLSync)(uintptr_t)q1, i0, &v);
        if (w) wr_u64(w, (uint64_t)v);
        return ok ? 1 : 0;
    }

    default: return -EPROTO;
    }
}


/* ---- DIAG: SCUTUM_GLERR=1 -> tras cada op de batch, glGetError() y reporte
 * (op + codigo) de los primeros errores de cada par. Sirve para ver QUE llamada
 * GL falla en el driver Mali. Ojo: en este modo glGetError "consume" el error,
 * asi que la app ya no lo ve (solo para diagnostico). */
static int g_glerr = 0;
static const char *const k_gl_names[] = {
    "glActiveTexture",
    "glAttachShader",
    "glBindAttribLocation",
    "glBindBuffer",
    "glBindFramebuffer",
    "glBindRenderbuffer",
    "glBindTexture",
    "glBlendColor",
    "glBlendEquation",
    "glBlendEquationSeparate",
    "glBlendFunc",
    "glBlendFuncSeparate",
    "glBufferData",
    "glBufferSubData",
    "glCheckFramebufferStatus",
    "glClear",
    "glClearColor",
    "glClearDepthf",
    "glClearStencil",
    "glColorMask",
    "glCompileShader",
    "glCompressedTexImage2D",
    "glCompressedTexSubImage2D",
    "glCopyTexImage2D",
    "glCopyTexSubImage2D",
    "glCreateProgram",
    "glCreateShader",
    "glCullFace",
    "glDeleteBuffers",
    "glDeleteFramebuffers",
    "glDeleteProgram",
    "glDeleteRenderbuffers",
    "glDeleteShader",
    "glDeleteTextures",
    "glDepthFunc",
    "glDepthMask",
    "glDepthRangef",
    "glDetachShader",
    "glDisable",
    "glDisableVertexAttribArray",
    "glDrawArrays",
    "glDrawElements",
    "glEnable",
    "glEnableVertexAttribArray",
    "glFinish",
    "glFlush",
    "glFramebufferRenderbuffer",
    "glFramebufferTexture2D",
    "glFrontFace",
    "glGenBuffers",
    "glGenerateMipmap",
    "glGenFramebuffers",
    "glGenRenderbuffers",
    "glGenTextures",
    "glGetActiveAttrib",
    "glGetActiveUniform",
    "glGetAttachedShaders",
    "glGetAttribLocation",
    "glGetBooleanv",
    "glGetBufferParameteriv",
    "glGetError",
    "glGetFloatv",
    "glGetFramebufferAttachmentParameteriv",
    "glGetIntegerv",
    "glGetProgramInfoLog",
    "glGetProgramiv",
    "glGetRenderbufferParameteriv",
    "glGetShaderInfoLog",
    "glGetShaderiv",
    "glGetShaderPrecisionFormat",
    "glGetShaderSource",
    "glGetString",
    "glGetTexParameterfv",
    "glGetTexParameteriv",
    "glGetUniformfv",
    "glGetUniformiv",
    "glGetUniformLocation",
    "glGetVertexAttribfv",
    "glGetVertexAttribiv",
    "glGetVertexAttribPointerv",
    "glHint",
    "glIsBuffer",
    "glIsEnabled",
    "glIsFramebuffer",
    "glIsProgram",
    "glIsRenderbuffer",
    "glIsShader",
    "glIsTexture",
    "glLineWidth",
    "glLinkProgram",
    "glPixelStorei",
    "glPolygonOffset",
    "glReadPixels",
    "glReleaseShaderCompiler",
    "glRenderbufferStorage",
    "glSampleCoverage",
    "glScissor",
    "glShaderBinary",
    "glShaderSource",
    "glStencilFunc",
    "glStencilFuncSeparate",
    "glStencilMask",
    "glStencilMaskSeparate",
    "glStencilOp",
    "glStencilOpSeparate",
    "glTexImage2D",
    "glTexParameterf",
    "glTexParameterfv",
    "glTexParameteri",
    "glTexParameteriv",
    "glTexSubImage2D",
    "glUniform1f",
    "glUniform1fv",
    "glUniform1i",
    "glUniform1iv",
    "glUniform2f",
    "glUniform2fv",
    "glUniform2i",
    "glUniform2iv",
    "glUniform3f",
    "glUniform3fv",
    "glUniform3i",
    "glUniform3iv",
    "glUniform4f",
    "glUniform4fv",
    "glUniform4i",
    "glUniform4iv",
    "glUniformMatrix2fv",
    "glUniformMatrix3fv",
    "glUniformMatrix4fv",
    "glUseProgram",
    "glValidateProgram",
    "glVertexAttrib1f",
    "glVertexAttrib1fv",
    "glVertexAttrib2f",
    "glVertexAttrib2fv",
    "glVertexAttrib3f",
    "glVertexAttrib3fv",
    "glVertexAttrib4f",
    "glVertexAttrib4fv",
    "glVertexAttribPointer",
    "glViewport",
    "glReadBuffer",
    "glDrawRangeElements",
    "glTexImage3D",
    "glTexSubImage3D",
    "glCopyTexSubImage3D",
    "glCompressedTexImage3D",
    "glCompressedTexSubImage3D",
    "glGenQueries",
    "glDeleteQueries",
    "glIsQuery",
    "glBeginQuery",
    "glEndQuery",
    "glGetQueryiv",
    "glGetQueryObjectuiv",
    "glUnmapBuffer",
    "glGetBufferPointerv",
    "glDrawBuffers",
    "glUniformMatrix2x3fv",
    "glUniformMatrix3x2fv",
    "glUniformMatrix2x4fv",
    "glUniformMatrix4x2fv",
    "glUniformMatrix3x4fv",
    "glUniformMatrix4x3fv",
    "glBlitFramebuffer",
    "glRenderbufferStorageMultisample",
    "glFramebufferTextureLayer",
    "glMapBufferRange",
    "glFlushMappedBufferRange",
    "glBindVertexArray",
    "glDeleteVertexArrays",
    "glGenVertexArrays",
    "glIsVertexArray",
    "glGetIntegeri_v",
    "glBeginTransformFeedback",
    "glEndTransformFeedback",
    "glBindBufferRange",
    "glBindBufferBase",
    "glTransformFeedbackVaryings",
    "glGetTransformFeedbackVarying",
    "glVertexAttribIPointer",
    "glGetVertexAttribIiv",
    "glGetVertexAttribIuiv",
    "glVertexAttribI4i",
    "glVertexAttribI4ui",
    "glVertexAttribI4iv",
    "glVertexAttribI4uiv",
    "glGetUniformuiv",
    "glGetFragDataLocation",
    "glUniform1ui",
    "glUniform2ui",
    "glUniform3ui",
    "glUniform4ui",
    "glUniform1uiv",
    "glUniform2uiv",
    "glUniform3uiv",
    "glUniform4uiv",
    "glClearBufferiv",
    "glClearBufferuiv",
    "glClearBufferfv",
    "glClearBufferfi",
    "glGetStringi",
    "glCopyBufferSubData",
    "glGetUniformIndices",
    "glGetActiveUniformsiv",
    "glGetUniformBlockIndex",
    "glGetActiveUniformBlockiv",
    "glGetActiveUniformBlockName",
    "glUniformBlockBinding",
    "glDrawArraysInstanced",
    "glDrawElementsInstanced",
    "glFenceSync",
    "glIsSync",
    "glDeleteSync",
    "glClientWaitSync",
    "glWaitSync",
    "glGetInteger64v",
    "glGetSynciv",
    "glGetInteger64i_v",
    "glGetBufferParameteri64v",
    "glGenSamplers",
    "glDeleteSamplers",
    "glIsSampler",
    "glBindSampler",
    "glSamplerParameteri",
    "glSamplerParameteriv",
    "glSamplerParameterf",
    "glSamplerParameterfv",
    "glGetSamplerParameteriv",
    "glGetSamplerParameterfv",
    "glVertexAttribDivisor",
    "glBindTransformFeedback",
    "glDeleteTransformFeedbacks",
    "glGenTransformFeedbacks",
    "glIsTransformFeedback",
    "glPauseTransformFeedback",
    "glResumeTransformFeedback",
    "glDrawTransformFeedback",
    "glInvalidateFramebuffer",
    "glInvalidateSubFramebuffer",
    "glDispatchCompute",
    "glDispatchComputeIndirect",
    "glFramebufferParameteri",
    "glGetFramebufferParameteriv",
    "glGetInternalformati64v",
    "glInvalidateTexSubImage",
    "glInvalidateTexImage",
    "glInvalidateBufferSubData",
    "glInvalidateBufferData",
    "glGetProgramInterfaceiv",
    "glGetProgramResourceIndex",
    "glGetProgramResourceName",
    "glGetProgramResourceiv",
    "glGetProgramResourceLocation",
    "glGetProgramResourceLocationIndex",
    "glShaderStorageBlockBinding",
    "glTexBufferRange",
    "glTexStorage2DMultisample",
    "glTexStorage3DMultisample",
    "glTextureView",
    "glBindImageTexture",
    "glMemoryBarrier",
    "glMemoryBarrierByRegion",
    "glGetMultisamplefv",
    "glSampleMaski",
    "glTexStorage2D",
    "glTexStorage3D",
    "glGetTexLevelParameteriv",
    "glGetTexLevelParameterfv",
    "glBindVertexBuffer",
    "glVertexAttribFormat",
    "glVertexAttribIFormat",
    "glVertexAttribBinding",
    "glVertexBindingDivisor",
    "glBlendBarrier",
    "glCopyImageSubData",
    "glDebugMessageControl",
    "glDebugMessageInsert",
    "glDebugMessageCallback",
    "glGetDebugMessageLog",
    "glPushDebugGroup",
    "glPopDebugGroup",
    "glObjectLabel",
    "glGetObjectLabel",
    "glObjectPtrLabel",
    "glGetObjectPtrLabel",
    "glGetPointerv",
    "glEnablei",
    "glDisablei",
    "glBlendEquationi",
    "glBlendEquationSeparatei",
    "glBlendFunci",
    "glBlendFuncSeparatei",
    "glColorMaski",
    "glIsEnabledi",
    "glDrawElementsBaseVertex",
    "glDrawRangeElementsBaseVertex",
    "glDrawElementsInstancedBaseVertex",
    "glDrawArraysIndirect",
    "glDrawElementsIndirect",
    "glFramebufferTexture",
    "glPrimitiveBoundingBox",
    "glGetGraphicsResetStatus",
    "glReadnPixels",
    "glGetnUniformfv",
    "glGetnUniformiv",
    "glGetnUniformuiv",
    "glMinSampleShading",
    "glPatchParameteri",
    "glTexParameterIiv",
    "glTexParameterIuiv",
    "glGetTexParameterIiv",
    "glGetTexParameterIuiv",
    "glSamplerParameterIiv",
    "glSamplerParameterIuiv",
    "glGetSamplerParameterIiv",
    "glGetSamplerParameterIuiv",
    "glGetQueryObjectiv",
    "glGetQueryObjecti64v",
    "glGetQueryObjectui64v",
    "glGetQueryBufferObjectiv",
    "glGetQueryBufferObjectuiv",
    "glGetQueryBufferObjecti64v",
    "glGetQueryBufferObjectui64v",
    "glGetInternalformativ",
    "glTexBuffer",
    "glGetProgramBinary",
    "glProgramBinary",
    "glProgramParameteri",
    "glEGLImageTargetTexture2DOES",
    "glEGLImageTargetRenderbufferStorageOES",
    "glBlendEquationOES",
    "glBlendEquationSeparateOES",
    "glBlendFuncSeparateOES",
    "glBlendFuncSeparateiOES",
    "glBlendEquationiOES",
    "glBlendEquationSeparateiOES",
    "glBlendFunciOES",
    "glDrawArraysInstancedANGLE",
    "glDrawElementsInstancedANGLE",
    "glVertexAttribDivisorANGLE",
    "glGenVertexArraysOES",
    "glBindVertexArrayOES",
    "glDeleteVertexArraysOES",
    "glIsVertexArrayOES",
    "glRenderbufferStorageMultisampleANGLE",
    "glFramebufferTexture2DMultisampleEXT",
    "glFramebufferTextureMultiviewOVR",
    "glTexStorage2DEXT",
    "glTexStorage3DEXT",
    "glTextureStorage2DEXT",
    "glTextureStorage3DEXT",
    "glCopyImageSubDataEXT",
    "glDiscardFramebufferEXT",
    "glMultiDrawArraysEXT",
    "glMultiDrawElementsEXT",
    "glMultiDrawArraysIndirectEXT",
    "glMultiDrawElementsIndirectEXT",
    "glDebugMessageCallbackKHR",
    "glDebugMessageControlKHR",
    "glDebugMessageInsertKHR",
    "glGetDebugMessageLogKHR",
    "glPushDebugGroupKHR",
    "glPopDebugGroupKHR",
    "glObjectLabelKHR",
    "glGetObjectLabelKHR",
    "glObjectPtrLabelKHR",
    "glGetObjectPtrLabelKHR",
    "glGetPointervKHR",
    "glQueryCounterEXT",
    "glGetQueryObjecti64vEXT",
    "glGetQueryObjectui64vEXT",
    "glGetQueryObjectivEXT",
    "glGetQueryObjectuivEXT",
    "glBlendBarrierKHR",
    "glBlendBarrierNV",
    "glTexBufferEXT",
    "glTexBufferOES",
    "glTexBufferRangeEXT",
    "glTexBufferRangeOES",
    "glFramebufferTexture2DDownsampleIMG",
    "glFramebufferTextureLayerDownsampleIMG",
    "glFramebufferTextureMultisampleMultiviewOVR"
};
#define K_GL_NAMES_N ((uint32_t)(sizeof k_gl_names / sizeof k_gl_names[0]))
static void glerr_report(uint32_t fn, unsigned err) {
    static unsigned seen[1024][2]; static unsigned cnt[1024]; static int n = 0;
    int k = -1;
    for (int i = 0; i < n; i++) if (seen[i][0] == fn && seen[i][1] == err) { k = i; break; }
    if (k < 0 && n < 1024) { k = n++; seen[k][0] = fn; seen[k][1] = err; cnt[k] = 0; }
    if (k < 0) return;
    cnt[k]++;
    if (cnt[k] <= 3 || cnt[k] % 2000 == 0) {
        uint32_t idx = fn - 0x80000000u;
        ERR("[scutumd] GLERR op=0x%x (%s) err=0x%x (x%u)\n", fn,
            idx < K_GL_NAMES_N ? k_gl_names[idx] : "?", err, cnt[k]);
    }
}

/* ============================================================ cliente */

static void *client_thread(void *arg) {
    struct client *c = arg;
    for (;;) {
        struct sc_msg h;
        uint8_t *payload = NULL;
        int fds[SC_MAX_FDS]; uint32_t n_fds = 0;
        int rc = c_read_msg(c, &h, &payload, fds, &n_fds);
        if (rc) {
            LOG("[scutumd] cliente %d desconectado (rc=%d)\n", c->fd, rc);
            break;
        }
        g_req_fd = -1;
        for (uint32_t i = 0; i < n_fds; i++) {
            if (i == 0 && (h.op == SC_OP_EGL || h.op == SC_OP_GL_SYNC)) g_req_fd = fds[0];
            else close(fds[i]);
        }

        switch (h.op) {
        case SC_OP_HELLO: {
            uint32_t ver = 0;
            if (h.len >= 4) memcpy(&ver, payload, 4);
            if (ver != SC_PROTO_VERSION) {
                c_send_error(c, h.req_id, SC_E_PROTO, SC_PERR_VERSION, "bad proto version");
                goto done;
            }
            uint32_t ack[2] = { SC_PROTO_VERSION,
                SC_CAP_EGL_15 | SC_CAP_GLES_30 | SC_CAP_GLES_31 | SC_CAP_GLES_32 };
            c_send_hdr_payload(c, SC_OP_HELLO_ACK, h.req_id, ack, sizeof ack);
            break;
        }
        case SC_OP_PING:
            c_send_hdr_payload(c, SC_OP_PONG, h.req_id, NULL, 0);
            break;
        case SC_OP_QUIT:
            goto done;

        case SC_OP_EGL:
        case SC_OP_GL_SYNC: {
            struct rd r = { payload, h.len, 0 };
            uint32_t first_op = 0;
            if (rd_u32(&r, &first_op)) {
                c_send_error(c, h.req_id, SC_E_PROTO, SC_PERR_TRUNCATED, "empty sync payload");
                break;
            }
            /* GL ops son 0x8000_0000+; EGL ops son 0x0100-0x012A. */
            int is_egl = (h.op == SC_OP_EGL) || (first_op < 0x80000000u);
            struct wr w = {0};
            int32_t rv = is_egl ? exec_egl_one(first_op, &r, &w)
                                : exec_gl_one(first_op, &r, &w);
            if (g_req_fd >= 0) { close(g_req_fd); g_req_fd = -1; }
            /* FIX: un resultado negativo NO siempre es error. glGetUniformLocation,
             * glGetAttribLocation, glGetFragDataLocation, glGetProgramResourceLocation*
             * devuelven -1 legitimamente ("no existe"), y GL_INVALID_INDEX
             * (0xFFFFFFFF) de glGetUniformBlockIndex/glGetProgramResourceIndex cae en
             * -1 al castear a int32. Los errores reales del daemon son -EPROTO,
             * -EMSGSIZE, etc. (< -1). Antes -1 se respondia como SC_OP_ERROR y el
             * shim cerraba la conexion / perdia el contexto -> pantalla gris. */
            if (rv < -1) {
                ERR("[scutumd] SYNC FAIL op=0x%x (%s) rv=%d consumido=%zu de %zu\n",
                    (unsigned)first_op, is_egl ? "EGL" : "GL", (int)rv, r.o, r.n);
                c_send_error(c, h.req_id, SC_E_PROTO, SC_PERR_TRUNCATED, "bad args");
            } else {
                c_send_sync(c, h.req_id, rv, w.p, w.n);
            }
            free(w.p);
            break;
        }

        case SC_OP_GL_BATCH: {
            struct rd r = { payload, h.len, 0 };
            uint32_t n_inst = 0;
            if (rd_u32(&r, &n_inst)) break;
            for (uint32_t i = 0; i < n_inst; i++) {
                uint32_t fn = 0, ab = 0;
                if (rd_u32(&r, &fn)) break;
                if (rd_u32(&r, &ab)) break;
                size_t save = r.o;
                if (g_glerr) while (((GLenum(*)(void))p_glGetError)()) {}
                (void)exec_gl_one(fn, &r, NULL);
                if (g_glerr) { GLenum ge = ((GLenum(*)(void))p_glGetError)(); if (ge) glerr_report(fn, ge); }
                size_t consumed = r.o - save;
                if (consumed != ab) {
                    LOG("[scutumd] batch: fn=0x%x consumio %zu, esperado %u\n",
                        fn, consumed, ab);
                    r.o = save + ab;
                }
                if (r.o > r.n) break;
            }
            break;
        }

        case SC_OP_FRAGMENT:
            /* No soportado por ahora. */
            c_send_error(c, h.req_id, SC_E_PROTO, SC_PERR_FRAG, "fragment no soportado");
            break;

        default:
            c_send_error(c, h.req_id, SC_E_PROTO, SC_PERR_OPCODE, "unknown msg op");
            break;
        }
    }
done:
    if (c->fd >= 0) close(c->fd);
    free(c->inbuf);
    free(c);
    return NULL;
}

/* ============================================================ main */

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    if (getenv("SCUTUM_VERBOSE")) g_verbose = 1;
    { const char *e = getenv("SCUTUM_GLERR"); if (e && *e == '1') g_glerr = 1; }
    signal(SIGPIPE, SIG_IGN);

    if (load_libs()) {
        ERR("[scutumd] fallo carga de libEGL/libGLESv2\n");
        return 1;
    }

    const char *path = getenv("SCUTUM_SOCK");
    if (!path || !*path) path = SC_SOCK_PATH;
    if (strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        ERR("[scutumd] socket path demasiado largo\n");
        return 1;
    }

    unlink(path);

    int lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (lfd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind"); close(lfd); return 1;
    }
    if (listen(lfd, 16) < 0) {
        perror("listen"); close(lfd); return 1;
    }
    chmod(path, 0666);
    ERR("[scutumd] listening on %s\n", path);

    for (;;) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }
        struct client *c = calloc(1, sizeof *c);
        if (!c) { close(cfd); continue; }
        c->fd = cfd;
        c->thread_id = 0;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&c->th, &attr, client_thread, c) != 0) {
            close(cfd); free(c);
        }
        pthread_attr_destroy(&attr);
    }
    close(lfd);
    return 0;
}
