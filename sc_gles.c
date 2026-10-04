/*
 * sc_gles.c — shim de GLES 2.0/3.0/3.1/3.2 + extensiones (glibc, container).
 *
 * ============================================================
 * BUGS CORREGIDOS EN ESTA REVISIÓN
 * ============================================================
 *  #1  glShaderSource reescrita. Layout emitido:
 *        [u32 shader][i32 count][ [u32 len][bytes] ] * count
 *      que es lo que scutumd.c::exec_gl_one lee vía rd_blob.
 *      Si string == NULL con count > 0, emite [u32 0] por slot (antes
 *      sólo se emitían blobs cuando string != NULL, y el daemon rompía
 *      con -EPROTO al leer un payload truncado).
 *  #2  emit_blob() eliminada.
 *  #5  glReadPixels: copia directo al buffer del usuario con
 *      sc_sync_recv_bytes(pixels, sz). 1080p RGBA ya no se trunca.
 *  #6  glReadnPixels: mismo tratamiento, respetando bufSize.
 *  #7  glGen*: buffer dimensionado con `n` real vía read_gl_ids().
 *  #8  glGetAttachedShaders: idem, con maxCount real.
 *  #9  glGetActive*, glGetShaderSource, glGetProgramInfoLog,
 *      glGetShaderInfoLog/glGetActiveUniformBlockName/
 *      glGetProgramResourceName/glGetObjectLabel/glGetObjectPtrLabel/
 *      glGetTransformFeedbackVarying: buffers dinámicos con bufSize.
 * #10  glGetString/glGetStringi: 16 KiB en stack (cualquier driver real
 *      entra).
 * #12  sc_shim_lookup maneja kind==1 (EGL) devolviendo
 *      eglSwapBuffersWithDamageKHR y eglGetPlatformDisplayEXT.
 * #13  Macros de uniform/vertex attrib sin warnings.
 * #14  Loops con GLsizei donde aplica.
 * #36  (nuevo) read_named_string() y GETV_IMPL() dimensionan con bufSize.
 * #37  (nuevo) glGetTransformFeedbackVarying header es 12 bytes, no 16.
 * #38  (nuevo) glGetInteger64v respeta count del daemon.
 * #39  (nuevo) glGetnUniform* respetan bufSize del usuario.
 * #40  (nuevo) glGetProgramBinary respeta len <= bufSize.
 * #41  (NUEVO en esta revisión) sc_pixel_size() ahora devuelve el tamaño
 *      correcto para tipos packed (GL_UNSIGNED_SHORT_{5_6_5,5_6_5_REV,
 *      4_4_4_4,5_5_5_1} = 2 bytes; GL_UNSIGNED_INT_{2_10_10_10_REV,
 *      10_10_10_2,24_8,5_9_9_9_REV} = 4 bytes). Antes devolvía c*t y en
 *      glReadPixels escribía 3x-4x más bytes que los que el usuario tenía
 *      reservados → overflow del buffer del usuario. El daemon tiene el
 *      mismo cálculo; arreglar sólo el shim es SEGURO (uploads: el daemon
 *      pasa el blob tal cual al driver; readbacks: leemos menos que el
 *      daemon envía y descartamos el sobrante) pero conviene corregirlo
 *      también para no desperdiciar transferencia.
 * #42  (NUEVO en esta revisión) emit_i32v/emit_f32v/emit_u64v eliminadas
 *      (dead code, -Wunused-function).
 *
 * ============================================================
 * BUGS PENDIENTES EN OTROS ARCHIVOS (documentados, no resueltos acá)
 * ============================================================
 *  #3  glMapBufferRange/glUnmapBuffer: falta writeback (SC_GL_EV_MAP_WRITEBACK).
 *      Requiere estado thread-local en sc_core.c y un case en scutumd.c.
 *  #4  glGetShaderPrecisionFormat: el daemon no manda `precision`. El shim
 *      lee hasta 12 bytes y usa el tercero si está; si no, deja 0.
 * #11  glDebugMessageCallback: sigue no-op (falta hilo de escucha).
 * #15  eglGetPlatformDisplay vs EXT: mismo opcode, distinto layout de attrs.
 *      Se arregla en sc_egl.c + scutumd.c.
 * #32  SC_OP_EGL nunca se manda desde el shim (el daemon ya lo tolera).
 * #34  Layouts cruzados verificados contra scutumd.c. Los desajustes
 *      detectados (glShaderSource, glTextureView, buffers fijos) corregidos.
 * #41  El daemon debe replicar el fix de sc_pixel_size para eficiencia.
 */
#define GL_GLEXT_PROTOTYPES
#define EGL_EGLEXT_PROTOTYPES
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <GLES3/gl3.h>
#include <GLES3/gl31.h>
#include <GLES3/gl32.h>
#include <GLES3/gl3ext.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdlib.h>

#include "scutum.h"
#include "sc_core.h"
#include "sc_gl_enum.h"

/* Algunos headers glibc no definen el nombre sin sufijo _OES. Mismo valor. */
#ifndef GL_UNSIGNED_INT_10_10_10_2
#define GL_UNSIGNED_INT_10_10_10_2 0x8DF6
#endif

/* ============================================================ helpers */

/*
 * Tamaño en bytes por pixel.
 *
 * FIX #41: los tipos packed tienen tamaño FIJO por pixel, independiente del
 * format. Antes se computaba c*t y para format=GL_RGB, type=GL_UNSIGNED_SHORT_5_6_5
 * devolvía 6 en vez de 2. En glReadPixels eso escribía 3x más bytes que el
 * buffer del usuario → overflow. La versión vieja tenía `if (fmt == GL_UNSIGNED_SHORT_5_6_5)
 * return t;` comparando un format con un type — nunca matcheaba.
 *
 * IMPORTANTE: scutumd.c::pixel_size() tiene el mismo cálculo viejo. Arreglar
 * SÓLO el shim es seguro porque:
 *   - uploads: el daemon pasa el blob tal cual al driver, que lee lo correcto.
 *   - readbacks: el daemon envía de más; el shim lee lo correcto e ignora el
 *     sobrante.
 * El daemon debería arreglarse igual para no transmitir bytes basura.
 */
static size_t sc_pixel_size(GLenum fmt, GLenum type) {
    /* Tipos packed con tamaño fijo por pixel. */
    switch (type) {
    case GL_UNSIGNED_SHORT_5_6_5:
    /* GL_UNSIGNED_SHORT_5_6_5_REV tiene el MISMO valor (0x8368) que
     * GL_UNSIGNED_INT_2_10_10_10_REV; no se puede listar ambos. */
    case GL_UNSIGNED_SHORT_4_4_4_4:
    case GL_UNSIGNED_SHORT_5_5_5_1:
        return 2;
    case GL_UNSIGNED_INT_2_10_10_10_REV:
    case GL_UNSIGNED_INT_10_10_10_2:
    case GL_UNSIGNED_INT_24_8:
    case GL_UNSIGNED_INT_5_9_9_9_REV:
        return 4;
    case GL_FLOAT_32_UNSIGNED_INT_24_8_REV:
        /* 8 bytes (float + uint32 empaquetados). c*t más abajo también da 8
         * para GL_DEPTH_STENCIL, pero lo dejamos explícito. */
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
    case GL_BGRA_EXT:  /* 4 canales */
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

/* Emite array de u32 con prefijo [u32 n]. NULL o n<=0 => [u32 0]. */
static void emit_u32v(const GLuint *v, int n) {
    sc_emit_u32((uint32_t)(v && n > 0 ? n : 0));
    if (v && n > 0) for (int i = 0; i < n; i++) sc_emit_u32(v[i]);
}

/* Emite punteros opacos (offsets en VBO, GLsync, etc.) como u64. */
static inline void emit_ptr(const void *p) { sc_emit_u64((uint64_t)(uintptr_t)p); }

/* Lee todo el payload de la última respuesta sync a un buffer local. */
static size_t read_payload(void *out, size_t max) {
    return sc_sync_recv_bytes(out, max);
}

/* Lee [u32 n][u32 ids...] y escribe min(n, req_n) en out. Dimensiona con
 * req_n real del usuario (no cap fijo). */
static uint32_t read_gl_ids(uint32_t req_n, GLuint *out) {
    if (!out || req_n == 0) return 0;
    size_t need = 4 + (size_t)req_n * 4;
    uint8_t *tmp = malloc(need);
    if (!tmp) return 0;
    size_t got = sc_sync_recv_bytes(tmp, need);
    if (got < 4) { free(tmp); return 0; }
    uint32_t c;
    memcpy(&c, tmp, 4);
    if (c > req_n) c = req_n;
    if (c > (got - 4) / 4) c = (uint32_t)((got - 4) / 4);
    if (c) memcpy(out, tmp + 4, (size_t)c * 4);
    free(tmp);
    return c;
}

/* Helper genérico para glGen*. Emite i32 n, lee [u32 count][ids...]. */
static void gen_ids(uint32_t op, GLsizei n, GLuint *out) {
    sc_sync_begin(op);
    sc_emit_i32(n);
    if (sc_sync_send() != 0) return;
    if (n <= 0 || !out) return;
    read_gl_ids((uint32_t)n, out);
}

/* Lee un string con prefijo [u32 len] y copia respetando bufSize. */
static void read_named_string(GLsizei bufSize, GLsizei *length, GLchar *dst) {
    if (bufSize < 0) bufSize = 0;
    size_t cap = 4 + (bufSize > 0 ? (size_t)bufSize : 256);
    uint8_t *tmp = malloc(cap);
    if (!tmp) return;
    size_t got = sc_sync_recv_bytes(tmp, cap);
    if (got < 4) { free(tmp); return; }
    uint32_t nl; memcpy(&nl, tmp, 4);
    if (nl > got - 4) nl = (uint32_t)(got - 4);
    if (length) *length = (GLsizei)nl;
    if (dst && bufSize > 0) {
        GLsizei k = (GLsizei)nl < bufSize - 1 ? (GLsizei)nl : bufSize - 1;
        if (k < 0) k = 0;
        memcpy(dst, tmp + 4, (size_t)k);
        dst[k] = 0;
    }
    free(tmp);
}

/* ============================================================ ES 2.0 / 3.0 — void */

void glActiveTexture(GLenum texture) { sc_batch_begin(SC_GL_glActiveTexture); sc_emit_u32(texture); sc_batch_end(); }
void glAttachShader(GLuint program, GLuint shader) { sc_batch_begin(SC_GL_glAttachShader); sc_emit_u32(program); sc_emit_u32(shader); sc_batch_end(); }
void glBindAttribLocation(GLuint program, GLuint index, const GLchar *name) { sc_batch_begin(SC_GL_glBindAttribLocation); sc_emit_u32(program); sc_emit_u32(index); sc_emit_string(name); sc_batch_end(); }
void glBindBuffer(GLenum target, GLuint buffer) { sc_batch_begin(SC_GL_glBindBuffer); sc_emit_u32(target); sc_emit_u32(buffer); sc_batch_end(); }
void glBindFramebuffer(GLenum target, GLuint framebuffer) { sc_batch_begin(SC_GL_glBindFramebuffer); sc_emit_u32(target); sc_emit_u32(framebuffer); sc_batch_end(); }
void glBindRenderbuffer(GLenum target, GLuint renderbuffer) { sc_batch_begin(SC_GL_glBindRenderbuffer); sc_emit_u32(target); sc_emit_u32(renderbuffer); sc_batch_end(); }
void glBindTexture(GLenum target, GLuint texture) { sc_batch_begin(SC_GL_glBindTexture); sc_emit_u32(target); sc_emit_u32(texture); sc_batch_end(); }
void glBlendColor(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha) { sc_batch_begin(SC_GL_glBlendColor); sc_emit_f32(red); sc_emit_f32(green); sc_emit_f32(blue); sc_emit_f32(alpha); sc_batch_end(); }
void glBlendEquation(GLenum mode) { sc_batch_begin(SC_GL_glBlendEquation); sc_emit_u32(mode); sc_batch_end(); }
void glBlendEquationSeparate(GLenum modeRGB, GLenum modeAlpha) { sc_batch_begin(SC_GL_glBlendEquationSeparate); sc_emit_u32(modeRGB); sc_emit_u32(modeAlpha); sc_batch_end(); }
void glBlendFunc(GLenum sfactor, GLenum dfactor) { sc_batch_begin(SC_GL_glBlendFunc); sc_emit_u32(sfactor); sc_emit_u32(dfactor); sc_batch_end(); }
void glBlendFuncSeparate(GLenum sfRGB, GLenum dfRGB, GLenum sfA, GLenum dfA) { sc_batch_begin(SC_GL_glBlendFuncSeparate); sc_emit_u32(sfRGB); sc_emit_u32(dfRGB); sc_emit_u32(sfA); sc_emit_u32(dfA); sc_batch_end(); }
void glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage) { sc_batch_begin(SC_GL_glBufferData); sc_emit_u32(target); sc_emit_u64((uint64_t)size); sc_emit_bytes(data, data ? (size_t)size : 0); sc_emit_u32(usage); sc_batch_end(); }
void glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void *data) { sc_batch_begin(SC_GL_glBufferSubData); sc_emit_u32(target); sc_emit_u64((uint64_t)offset); sc_emit_u64((uint64_t)size); sc_emit_bytes(data, data ? (size_t)size : 0); sc_batch_end(); }
void glClear(GLbitfield mask) { sc_batch_begin(SC_GL_glClear); sc_emit_u32((uint32_t)mask); sc_batch_end(); }
void glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { sc_batch_begin(SC_GL_glClearColor); sc_emit_f32(r); sc_emit_f32(g); sc_emit_f32(b); sc_emit_f32(a); sc_batch_end(); }
void glClearDepthf(GLfloat d) { sc_batch_begin(SC_GL_glClearDepthf); sc_emit_f32(d); sc_batch_end(); }
void glClearStencil(GLint s) { sc_batch_begin(SC_GL_glClearStencil); sc_emit_i32(s); sc_batch_end(); }
void glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) { sc_batch_begin(SC_GL_glColorMask); sc_emit_u32(r); sc_emit_u32(g); sc_emit_u32(b); sc_emit_u32(a); sc_batch_end(); }
void glCompileShader(GLuint shader) { sc_batch_begin(SC_GL_glCompileShader); sc_emit_u32(shader); sc_batch_end(); }

void glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const void *data) {
    sc_batch_begin(SC_GL_glCompressedTexImage2D);
    sc_emit_u32(target); sc_emit_i32(level); sc_emit_u32(internalformat);
    sc_emit_i32(width); sc_emit_i32(height); sc_emit_i32(border); sc_emit_i32(imageSize);
    sc_emit_bytes(data, data ? (size_t)imageSize : 0);
    sc_batch_end();
}
void glCompressedTexSubImage2D(GLenum target, GLint level, GLint xoff, GLint yoff, GLsizei width, GLsizei height, GLenum format, GLsizei imageSize, const void *data) {
    sc_batch_begin(SC_GL_glCompressedTexSubImage2D);
    sc_emit_u32(target); sc_emit_i32(level); sc_emit_i32(xoff); sc_emit_i32(yoff);
    sc_emit_i32(width); sc_emit_i32(height); sc_emit_u32(format); sc_emit_i32(imageSize);
    sc_emit_bytes(data, data ? (size_t)imageSize : 0);
    sc_batch_end();
}
void glCopyTexImage2D(GLenum target, GLint level, GLenum ifmt, GLint x, GLint y, GLsizei w, GLsizei h, GLint border) { sc_batch_begin(SC_GL_glCopyTexImage2D); sc_emit_u32(target); sc_emit_i32(level); sc_emit_u32(ifmt); sc_emit_i32(x); sc_emit_i32(y); sc_emit_i32(w); sc_emit_i32(h); sc_emit_i32(border); sc_batch_end(); }
void glCopyTexSubImage2D(GLenum target, GLint level, GLint xoff, GLint yoff, GLint x, GLint y, GLsizei w, GLsizei h) { sc_batch_begin(SC_GL_glCopyTexSubImage2D); sc_emit_u32(target); sc_emit_i32(level); sc_emit_i32(xoff); sc_emit_i32(yoff); sc_emit_i32(x); sc_emit_i32(y); sc_emit_i32(w); sc_emit_i32(h); sc_batch_end(); }
void glCullFace(GLenum mode) { sc_batch_begin(SC_GL_glCullFace); sc_emit_u32(mode); sc_batch_end(); }

void glDeleteBuffers(GLsizei n, const GLuint *buffers) { sc_batch_begin(SC_GL_glDeleteBuffers); emit_u32v(buffers, n); sc_batch_end(); }
void glDeleteFramebuffers(GLsizei n, const GLuint *fb) { sc_batch_begin(SC_GL_glDeleteFramebuffers); emit_u32v(fb, n); sc_batch_end(); }
void glDeleteProgram(GLuint program) { sc_batch_begin(SC_GL_glDeleteProgram); sc_emit_u32(program); sc_batch_end(); }
void glDeleteRenderbuffers(GLsizei n, const GLuint *rb) { sc_batch_begin(SC_GL_glDeleteRenderbuffers); emit_u32v(rb, n); sc_batch_end(); }
void glDeleteShader(GLuint shader) { sc_batch_begin(SC_GL_glDeleteShader); sc_emit_u32(shader); sc_batch_end(); }
void glDeleteTextures(GLsizei n, const GLuint *textures) { sc_batch_begin(SC_GL_glDeleteTextures); emit_u32v(textures, n); sc_batch_end(); }
void glDepthFunc(GLenum func) { sc_batch_begin(SC_GL_glDepthFunc); sc_emit_u32(func); sc_batch_end(); }
void glDepthMask(GLboolean flag) { sc_batch_begin(SC_GL_glDepthMask); sc_emit_u32(flag); sc_batch_end(); }
void glDepthRangef(GLfloat n, GLfloat f) { sc_batch_begin(SC_GL_glDepthRangef); sc_emit_f32(n); sc_emit_f32(f); sc_batch_end(); }
void glDetachShader(GLuint program, GLuint shader) { sc_batch_begin(SC_GL_glDetachShader); sc_emit_u32(program); sc_emit_u32(shader); sc_batch_end(); }
void glDisable(GLenum cap) { sc_batch_begin(SC_GL_glDisable); sc_emit_u32(cap); sc_batch_end(); }
void glDisableVertexAttribArray(GLuint index) { sc_batch_begin(SC_GL_glDisableVertexAttribArray); sc_emit_u32(index); sc_batch_end(); }
void glDrawArrays(GLenum mode, GLint first, GLsizei count) { sc_batch_begin(SC_GL_glDrawArrays); sc_emit_u32(mode); sc_emit_i32(first); sc_emit_i32(count); sc_batch_end(); }
void glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices) { sc_batch_begin(SC_GL_glDrawElements); sc_emit_u32(mode); sc_emit_i32(count); sc_emit_u32(type); emit_ptr(indices); sc_batch_end(); }
void glEnable(GLenum cap) { sc_batch_begin(SC_GL_glEnable); sc_emit_u32(cap); sc_batch_end(); }
void glEnableVertexAttribArray(GLuint index) { sc_batch_begin(SC_GL_glEnableVertexAttribArray); sc_emit_u32(index); sc_batch_end(); }
void glFinish(void) { sc_batch_flush(); sc_sync_begin(SC_GL_glFinish); sc_sync_send(); }
void glFlush(void) { sc_batch_flush(); }

void glFramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum rbtarget, GLuint rb) { sc_batch_begin(SC_GL_glFramebufferRenderbuffer); sc_emit_u32(target); sc_emit_u32(attachment); sc_emit_u32(rbtarget); sc_emit_u32(rb); sc_batch_end(); }
void glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level) { sc_batch_begin(SC_GL_glFramebufferTexture2D); sc_emit_u32(target); sc_emit_u32(attachment); sc_emit_u32(textarget); sc_emit_u32(texture); sc_emit_i32(level); sc_batch_end(); }
void glFrontFace(GLenum mode) { sc_batch_begin(SC_GL_glFrontFace); sc_emit_u32(mode); sc_batch_end(); }

void glGenBuffers(GLsizei n, GLuint *buffers)              { gen_ids(SC_GL_glGenBuffers, n, buffers); }
void glGenFramebuffers(GLsizei n, GLuint *fb)              { gen_ids(SC_GL_glGenFramebuffers, n, fb); }
void glGenRenderbuffers(GLsizei n, GLuint *rb)             { gen_ids(SC_GL_glGenRenderbuffers, n, rb); }
void glGenTextures(GLsizei n, GLuint *textures)            { gen_ids(SC_GL_glGenTextures, n, textures); }
void glGenQueries(GLsizei n, GLuint *ids)                  { gen_ids(SC_GL_glGenQueries, n, ids); }
void glGenVertexArrays(GLsizei n, GLuint *va)              { gen_ids(SC_GL_glGenVertexArrays, n, va); }
void glGenSamplers(GLsizei n, GLuint *s)                   { gen_ids(SC_GL_glGenSamplers, n, s); }
void glGenTransformFeedbacks(GLsizei n, GLuint *ids)       { gen_ids(SC_GL_glGenTransformFeedbacks, n, ids); }

void glGenerateMipmap(GLenum target) { sc_batch_begin(SC_GL_glGenerateMipmap); sc_emit_u32(target); sc_batch_end(); }
void glHint(GLenum target, GLenum mode) { sc_batch_begin(SC_GL_glHint); sc_emit_u32(target); sc_emit_u32(mode); sc_batch_end(); }
void glLineWidth(GLfloat w) { sc_batch_begin(SC_GL_glLineWidth); sc_emit_f32(w); sc_batch_end(); }
void glLinkProgram(GLuint program) { sc_batch_begin(SC_GL_glLinkProgram); sc_emit_u32(program); sc_batch_end(); }
void glPixelStorei(GLenum pname, GLint param) { sc_batch_begin(SC_GL_glPixelStorei); sc_emit_u32(pname); sc_emit_i32(param); sc_batch_end(); }
void glPolygonOffset(GLfloat factor, GLfloat units) { sc_batch_begin(SC_GL_glPolygonOffset); sc_emit_f32(factor); sc_emit_f32(units); sc_batch_end(); }
void glReleaseShaderCompiler(void) { sc_batch_begin(SC_GL_glReleaseShaderCompiler); sc_batch_end(); }
void glRenderbufferStorage(GLenum target, GLenum ifmt, GLsizei w, GLsizei h) { sc_batch_begin(SC_GL_glRenderbufferStorage); sc_emit_u32(target); sc_emit_u32(ifmt); sc_emit_i32(w); sc_emit_i32(h); sc_batch_end(); }
void glSampleCoverage(GLfloat value, GLboolean invert) { sc_batch_begin(SC_GL_glSampleCoverage); sc_emit_f32(value); sc_emit_u32(invert); sc_batch_end(); }
void glScissor(GLint x, GLint y, GLsizei w, GLsizei h) { sc_batch_begin(SC_GL_glScissor); sc_emit_i32(x); sc_emit_i32(y); sc_emit_i32(w); sc_emit_i32(h); sc_batch_end(); }

void glShaderBinary(GLsizei count, const GLuint *shaders, GLenum binaryformat, const void *binary, GLsizei length) {
    sc_batch_begin(SC_GL_glShaderBinary);
    emit_u32v(shaders, count);
    sc_emit_u32(binaryformat);
    sc_emit_bytes(binary, binary ? (size_t)length : 0);
    sc_batch_end();
}

/*
 * glShaderSource — FIX #1.
 * Emite lo que el daemon lee:
 *   [u32 shader][i32 count][ [u32 len][bytes] ] * count
 * FIX #2 (esta revisión): si string == NULL con count > 0, emitimos [u32 0]
 * por cada slot para que el daemon no rompa con -EPROTO.
 */
void glShaderSource(GLuint shader, GLsizei count, const GLchar *const*string, const GLint *length) {
    /* DIAG: SC_TRACE_SHADERS=1 -> volcar lo que manda GL4ES */
    static int trace = -1;
    if (trace < 0) { const char *e = getenv("SC_TRACE_SHADERS"); trace = (e && *e == '1'); }
    if (trace) {
        fprintf(stderr, "[sc-shader] id=%u count=%d\n", shader, (int)count);
        for (GLsizei i = 0; i < count && string; i++) {
            const char *s = string[i];
            long n = !s ? 0 : (length && length[i] >= 0) ? length[i] : (long)strlen(s);
            fprintf(stderr, "[sc-shader]  piece %d len=%ld: \"", (int)i, n);
            for (long k = 0; s && k < n && k < 80; k++) {
                unsigned char c = (unsigned char)s[k];
                if (c == '\n') fputs("\\n", stderr);
                else if (c >= 32 && c < 127) fputc(c, stderr);
                else fprintf(stderr, "\\x%02x", c);
            }
            fputs("\"\n", stderr);
        }
    }
    sc_batch_begin(SC_GL_glShaderSource);
    sc_emit_u32(shader);
    sc_emit_i32(count);
    if (count > 0) {
        for (GLsizei i = 0; i < count; i++) {
            const GLchar *s = string ? string[i] : NULL;
            if (!s) { sc_emit_u32(0); continue; }
            size_t n = (length && length[i] >= 0) ? (size_t)length[i] : strlen(s);
            sc_emit_bytes(s, n);
        }
    }
    sc_batch_end();
}

void glStencilFunc(GLenum func, GLint ref, GLuint mask) { sc_batch_begin(SC_GL_glStencilFunc); sc_emit_u32(func); sc_emit_i32(ref); sc_emit_u32(mask); sc_batch_end(); }
void glStencilFuncSeparate(GLenum face, GLenum func, GLint ref, GLuint mask) { sc_batch_begin(SC_GL_glStencilFuncSeparate); sc_emit_u32(face); sc_emit_u32(func); sc_emit_i32(ref); sc_emit_u32(mask); sc_batch_end(); }
void glStencilMask(GLuint mask) { sc_batch_begin(SC_GL_glStencilMask); sc_emit_u32(mask); sc_batch_end(); }
void glStencilMaskSeparate(GLenum face, GLuint mask) { sc_batch_begin(SC_GL_glStencilMaskSeparate); sc_emit_u32(face); sc_emit_u32(mask); sc_batch_end(); }
void glStencilOp(GLenum sfail, GLenum dpfail, GLenum dppass) { sc_batch_begin(SC_GL_glStencilOp); sc_emit_u32(sfail); sc_emit_u32(dpfail); sc_emit_u32(dppass); sc_batch_end(); }
void glStencilOpSeparate(GLenum face, GLenum sfail, GLenum dpfail, GLenum dppass) { sc_batch_begin(SC_GL_glStencilOpSeparate); sc_emit_u32(face); sc_emit_u32(sfail); sc_emit_u32(dpfail); sc_emit_u32(dppass); sc_batch_end(); }

void glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void *pixels) {
    sc_batch_begin(SC_GL_glTexImage2D);
    sc_emit_u32(target); sc_emit_i32(level); sc_emit_i32(internalformat);
    sc_emit_i32(width); sc_emit_i32(height); sc_emit_i32(border);
    sc_emit_u32(format); sc_emit_u32(type);
    size_t sz = pixels ? sc_pixel_size(format, type) * (size_t)width * (size_t)height : 0;
    sc_emit_bytes(pixels, sz);
    sc_batch_end();
}
void glTexSubImage2D(GLenum target, GLint level, GLint xoff, GLint yoff, GLsizei width, GLsizei height, GLenum format, GLenum type, const void *pixels) {
    sc_batch_begin(SC_GL_glTexSubImage2D);
    sc_emit_u32(target); sc_emit_i32(level); sc_emit_i32(xoff); sc_emit_i32(yoff);
    sc_emit_i32(width); sc_emit_i32(height); sc_emit_u32(format); sc_emit_u32(type);
    size_t sz = pixels ? sc_pixel_size(format, type) * (size_t)width * (size_t)height : 0;
    sc_emit_bytes(pixels, sz);
    sc_batch_end();
}
void glTexParameterf(GLenum target, GLenum pname, GLfloat param) { sc_batch_begin(SC_GL_glTexParameterf); sc_emit_u32(target); sc_emit_u32(pname); sc_emit_f32(param); sc_batch_end(); }
void glTexParameterfv(GLenum target, GLenum pname, const GLfloat *params) { sc_batch_begin(SC_GL_glTexParameterfv); sc_emit_u32(target); sc_emit_u32(pname); sc_emit_f32(params ? params[0] : 0.0f); sc_batch_end(); }
void glTexParameteri(GLenum target, GLenum pname, GLint param) { sc_batch_begin(SC_GL_glTexParameteri); sc_emit_u32(target); sc_emit_u32(pname); sc_emit_i32(param); sc_batch_end(); }
void glTexParameteriv(GLenum target, GLenum pname, const GLint *params) { sc_batch_begin(SC_GL_glTexParameteriv); sc_emit_u32(target); sc_emit_u32(pname); sc_emit_i32(params ? params[0] : 0); sc_batch_end(); }

void glUniform1f(GLint loc, GLfloat a) {
    sc_batch_begin(SC_GL_glUniform1f); sc_emit_i32(loc); sc_emit_f32(a);
    sc_batch_end();
}
void glUniform2f(GLint loc, GLfloat a, GLfloat b) {
    sc_batch_begin(SC_GL_glUniform2f); sc_emit_i32(loc); sc_emit_f32(a); sc_emit_f32(b);
    sc_batch_end();
}
void glUniform3f(GLint loc, GLfloat a, GLfloat b, GLfloat c) {
    sc_batch_begin(SC_GL_glUniform3f); sc_emit_i32(loc); sc_emit_f32(a); sc_emit_f32(b); sc_emit_f32(c);
    sc_batch_end();
}
void glUniform4f(GLint loc, GLfloat a, GLfloat b, GLfloat c, GLfloat d) {
    sc_batch_begin(SC_GL_glUniform4f); sc_emit_i32(loc); sc_emit_f32(a); sc_emit_f32(b); sc_emit_f32(c); sc_emit_f32(d);
    sc_batch_end();
}

void glUniform1i(GLint loc, GLint a) {
    sc_batch_begin(SC_GL_glUniform1i); sc_emit_i32(loc); sc_emit_i32(a);
    sc_batch_end();
}
void glUniform2i(GLint loc, GLint a, GLint b) {
    sc_batch_begin(SC_GL_glUniform2i); sc_emit_i32(loc); sc_emit_i32(a); sc_emit_i32(b);
    sc_batch_end();
}
void glUniform3i(GLint loc, GLint a, GLint b, GLint c) {
    sc_batch_begin(SC_GL_glUniform3i); sc_emit_i32(loc); sc_emit_i32(a); sc_emit_i32(b); sc_emit_i32(c);
    sc_batch_end();
}
void glUniform4i(GLint loc, GLint a, GLint b, GLint c, GLint d) {
    sc_batch_begin(SC_GL_glUniform4i); sc_emit_i32(loc); sc_emit_i32(a); sc_emit_i32(b); sc_emit_i32(c); sc_emit_i32(d);
    sc_batch_end();
}

#define UNIF_FV(n) \
void glUniform##n##fv(GLint loc, GLsizei count, const GLfloat *v) { \
    sc_batch_begin(SC_GL_glUniform##n##fv); sc_emit_i32(loc); sc_emit_i32(count); \
    if (v && count > 0) { \
        size_t total = (size_t)count * (size_t)(n); \
        for (size_t i = 0; i < total; i++) sc_emit_f32(v[i]); \
    } \
    sc_batch_end(); }
UNIF_FV(1) UNIF_FV(2) UNIF_FV(3) UNIF_FV(4)
#undef UNIF_FV

#define UNIF_IV(n) \
void glUniform##n##iv(GLint loc, GLsizei count, const GLint *v) { \
    sc_batch_begin(SC_GL_glUniform##n##iv); sc_emit_i32(loc); sc_emit_i32(count); \
    if (v && count > 0) { \
        size_t total = (size_t)count * (size_t)(n); \
        for (size_t i = 0; i < total; i++) sc_emit_i32(v[i]); \
    } \
    sc_batch_end(); }
UNIF_IV(1) UNIF_IV(2) UNIF_IV(3) UNIF_IV(4)
#undef UNIF_IV

#define UNIF_MAT(n) \
void glUniformMatrix##n##fv(GLint loc, GLsizei count, GLboolean transpose, const GLfloat *v) { \
    sc_batch_begin(SC_GL_glUniformMatrix##n##fv); sc_emit_i32(loc); sc_emit_i32(count); sc_emit_u32(transpose); \
    if (v && count > 0) { \
        size_t total = (size_t)count * (size_t)(n) * (size_t)(n); \
        for (size_t i = 0; i < total; i++) sc_emit_f32(v[i]); \
    } \
    sc_batch_end(); }
UNIF_MAT(2) UNIF_MAT(3) UNIF_MAT(4)
#undef UNIF_MAT

void glUseProgram(GLuint program) { sc_batch_begin(SC_GL_glUseProgram); sc_emit_u32(program); sc_batch_end(); }
void glValidateProgram(GLuint program) { sc_batch_begin(SC_GL_glValidateProgram); sc_emit_u32(program); sc_batch_end(); }

void glVertexAttrib1f(GLuint idx, GLfloat a) {
    sc_batch_begin(SC_GL_glVertexAttrib1f); sc_emit_u32(idx); sc_emit_f32(a);
    sc_batch_end();
}
void glVertexAttrib2f(GLuint idx, GLfloat a, GLfloat b) {
    sc_batch_begin(SC_GL_glVertexAttrib2f); sc_emit_u32(idx); sc_emit_f32(a); sc_emit_f32(b);
    sc_batch_end();
}
void glVertexAttrib3f(GLuint idx, GLfloat a, GLfloat b, GLfloat c) {
    sc_batch_begin(SC_GL_glVertexAttrib3f); sc_emit_u32(idx); sc_emit_f32(a); sc_emit_f32(b); sc_emit_f32(c);
    sc_batch_end();
}
void glVertexAttrib4f(GLuint idx, GLfloat a, GLfloat b, GLfloat c, GLfloat d) {
    sc_batch_begin(SC_GL_glVertexAttrib4f); sc_emit_u32(idx); sc_emit_f32(a); sc_emit_f32(b); sc_emit_f32(c); sc_emit_f32(d);
    sc_batch_end();
}

#define VATT_FV(n) \
void glVertexAttrib##n##fv(GLuint idx, const GLfloat *v) { \
    sc_batch_begin(SC_GL_glVertexAttrib##n##fv); sc_emit_u32(idx); \
    if (v) for (int i = 0; i < (n); i++) sc_emit_f32(v[i]); \
    sc_batch_end(); }
VATT_FV(1) VATT_FV(2) VATT_FV(3) VATT_FV(4)
#undef VATT_FV

void glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer) {
    sc_batch_begin(SC_GL_glVertexAttribPointer);
    sc_emit_u32(index); sc_emit_i32(size); sc_emit_u32(type); sc_emit_u32(normalized);
    sc_emit_i32(stride); emit_ptr(pointer);
    sc_batch_end();
}
void glViewport(GLint x, GLint y, GLsizei w, GLsizei h) { sc_batch_begin(SC_GL_glViewport); sc_emit_i32(x); sc_emit_i32(y); sc_emit_i32(w); sc_emit_i32(h); sc_batch_end(); }

/* ============================================================ ES 2.0 / 3.0 — sync */

GLenum glCheckFramebufferStatus(GLenum target) { sc_sync_begin(SC_GL_glCheckFramebufferStatus); sc_emit_u32(target); if (sc_sync_send() != 0) return 0; return (GLenum)sc_sync_result(); }
GLuint glCreateProgram(void) { sc_sync_begin(SC_GL_glCreateProgram); if (sc_sync_send() != 0) return 0; return (GLuint)sc_sync_result(); }
GLuint glCreateShader(GLenum type) { sc_sync_begin(SC_GL_glCreateShader); sc_emit_u32(type); if (sc_sync_send() != 0) return 0; return (GLuint)sc_sync_result(); }

static void glGetActive_common(uint32_t op, GLuint program, GLuint index,
                               GLsizei bufSize, GLsizei *length, GLint *size,
                               GLenum *type, GLchar *name)
{
    sc_sync_begin(op);
    sc_emit_u32(program); sc_emit_u32(index); sc_emit_i32(bufSize);
    if (sc_sync_send() != 0) return;
    /* resp: [i32 size][u32 type][u32 nl][name bytes] — header 12 bytes */
    size_t cap = 12 + (bufSize > 0 ? (size_t)bufSize : 256);
    uint8_t *tmp = malloc(cap);
    if (!tmp) return;
    size_t got = sc_sync_recv_bytes(tmp, cap);
    if (got < 12) { free(tmp); return; }
    if (size) memcpy(size, tmp, 4);
    if (type) { uint32_t t; memcpy(&t, tmp + 4, 4); *type = (GLenum)t; }
    uint32_t nl; memcpy(&nl, tmp + 8, 4);
    if (nl > got - 12) nl = (uint32_t)(got - 12);
    if (name && bufSize > 0) {
        GLsizei k = (GLsizei)nl < bufSize - 1 ? (GLsizei)nl : bufSize - 1;
        if (k < 0) k = 0;
        memcpy(name, tmp + 12, (size_t)k);
        name[k] = 0;
        if (length) *length = k;
    } else if (length) *length = (GLsizei)nl;
    free(tmp);
}
void glGetActiveAttrib(GLuint program, GLuint index, GLsizei bufSize, GLsizei *length, GLint *size, GLenum *type, GLchar *name) {
    glGetActive_common(SC_GL_glGetActiveAttrib, program, index, bufSize, length, size, type, name);
}
void glGetActiveUniform(GLuint program, GLuint index, GLsizei bufSize, GLsizei *length, GLint *size, GLenum *type, GLchar *name) {
    glGetActive_common(SC_GL_glGetActiveUniform, program, index, bufSize, length, size, type, name);
}

void glGetAttachedShaders(GLuint program, GLsizei maxCount, GLsizei *count, GLuint *shaders) {
    sc_sync_begin(SC_GL_glGetAttachedShaders);
    sc_emit_u32(program); sc_emit_i32(maxCount);
    if (sc_sync_send() != 0) return;
    if (maxCount < 0) maxCount = 0;
    uint32_t c = read_gl_ids((uint32_t)maxCount, shaders);
    if (count) *count = (GLsizei)c;
}
GLint glGetAttribLocation(GLuint program, const GLchar *name) { sc_sync_begin(SC_GL_glGetAttribLocation); sc_emit_u32(program); sc_emit_string(name); if (sc_sync_send() != 0) return -1; return sc_sync_result(); }

#define GETV_IMPL(name, op, T) \
void name(GLenum pname, T *out) { \
    sc_sync_begin(op); sc_emit_u32(pname); \
    if (sc_sync_send() != 0) return; \
    if (!out) return; \
    size_t cap = 4 + 4096 * sizeof(T); \
    uint8_t *tmp = malloc(cap); \
    if (!tmp) return; \
    size_t got = sc_sync_recv_bytes(tmp, cap); \
    if (got < 4) { free(tmp); return; } \
    uint32_t n; memcpy(&n, tmp, 4); \
    uint32_t avail = (uint32_t)((got - 4) / sizeof(T)); \
    if (n > avail) n = avail; \
    if (n > 4096) n = 4096; \
    memcpy(out, tmp + 4, (size_t)n * sizeof(T)); \
    free(tmp); \
}
GETV_IMPL(glGetBooleanv, SC_GL_glGetBooleanv, GLboolean)
GETV_IMPL(glGetFloatv,   SC_GL_glGetFloatv,   GLfloat)
GETV_IMPL(glGetIntegerv, SC_GL_glGetIntegerv, GLint)
#undef GETV_IMPL

void glGetBufferParameteriv(GLenum target, GLenum pname, GLint *out) {
    sc_sync_begin(SC_GL_glGetBufferParameteriv); sc_emit_u32(target); sc_emit_u32(pname);
    if (sc_sync_send() != 0) return;
    int32_t v = 0; sc_sync_recv_bytes(&v, 4); if (out) *out = v;
}
void glGetFramebufferAttachmentParameteriv(GLenum target, GLenum attachment, GLenum pname, GLint *out) {
    sc_sync_begin(SC_GL_glGetFramebufferAttachmentParameteriv);
    sc_emit_u32(target); sc_emit_u32(attachment); sc_emit_u32(pname);
    if (sc_sync_send() != 0) return;
    int32_t v = 0; sc_sync_recv_bytes(&v, 4); if (out) *out = v;
}
void glGetRenderbufferParameteriv(GLenum target, GLenum pname, GLint *out) {
    sc_sync_begin(SC_GL_glGetRenderbufferParameteriv); sc_emit_u32(target); sc_emit_u32(pname);
    if (sc_sync_send() != 0) return;
    int32_t v = 0; sc_sync_recv_bytes(&v, 4); if (out) *out = v;
}
void glGetShaderiv(GLuint shader, GLenum pname, GLint *out) {
    sc_sync_begin(SC_GL_glGetShaderiv); sc_emit_u32(shader); sc_emit_u32(pname);
    if (sc_sync_send() != 0) return;
    int32_t v = 0; sc_sync_recv_bytes(&v, 4); if (out) *out = v;
}
void glGetProgramiv(GLuint program, GLenum pname, GLint *out) {
    sc_sync_begin(SC_GL_glGetProgramiv); sc_emit_u32(program); sc_emit_u32(pname);
    if (sc_sync_send() != 0) return;
    int32_t v = 0; sc_sync_recv_bytes(&v, 4); if (out) *out = v;
}
/* #4: el daemon actual manda 8 bytes (range[2]); si manda 12 (range + precision),
 * usamos el tercero. Mientras tanto precision = 0. */
void glGetShaderPrecisionFormat(GLenum st, GLenum pt, GLint *range, GLint *precision) {
    sc_sync_begin(SC_GL_glGetShaderPrecisionFormat); sc_emit_u32(st); sc_emit_u32(pt);
    if (sc_sync_send() != 0) return;
    int32_t v[3] = {0, 0, 0};
    size_t got = sc_sync_recv_bytes(v, sizeof v);
    if (range) { range[0] = v[0]; range[1] = v[1]; }
    if (precision) *precision = (got >= 12) ? v[2] : 0;
}

void glGetShaderSource(GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *source) {
    sc_sync_begin(SC_GL_glGetShaderSource); sc_emit_u32(shader); sc_emit_i32(bufSize);
    if (sc_sync_send() != 0) return;
    read_named_string(bufSize, length, source);
}

const GLubyte *glGetString(GLenum name) {
    sc_sync_begin(SC_GL_glGetString); sc_emit_u32(name);
    if (sc_sync_send() != 0) return NULL;
    uint8_t buf[16384];
    size_t got = read_payload(buf, sizeof buf);
    if (got < 4) return NULL;
    uint32_t len; memcpy(&len, buf, 4);
    if (len > got - 4) len = (uint32_t)(got - 4);
    return (const GLubyte *)sc_string_intern_slot(buf + 4, len, name, 0);
}

void glGetTexParameterfv(GLenum target, GLenum pname, GLfloat *out) {
    sc_sync_begin(SC_GL_glGetTexParameterfv); sc_emit_u32(target); sc_emit_u32(pname);
    if (sc_sync_send() != 0) return;
    float v = 0; sc_sync_recv_bytes(&v, 4); if (out) *out = v;
}
void glGetTexParameteriv(GLenum target, GLenum pname, GLint *out) {
    sc_sync_begin(SC_GL_glGetTexParameteriv); sc_emit_u32(target); sc_emit_u32(pname);
    if (sc_sync_send() != 0) return;
    int32_t v = 0; sc_sync_recv_bytes(&v, 4); if (out) *out = v;
}
void glGetUniformfv(GLuint program, GLint loc, GLfloat *out) {
    sc_sync_begin(SC_GL_glGetUniformfv); sc_emit_u32(program); sc_emit_i32(loc);
    if (sc_sync_send() != 0) return;
    float v = 0; sc_sync_recv_bytes(&v, 4); if (out) *out = v;
}
void glGetUniformiv(GLuint program, GLint loc, GLint *out) {
    sc_sync_begin(SC_GL_glGetUniformiv); sc_emit_u32(program); sc_emit_i32(loc);
    if (sc_sync_send() != 0) return;
    int32_t v = 0; sc_sync_recv_bytes(&v, 4); if (out) *out = v;
}
GLint glGetUniformLocation(GLuint program, const GLchar *name) {
    sc_sync_begin(SC_GL_glGetUniformLocation); sc_emit_u32(program); sc_emit_string(name);
    if (sc_sync_send() != 0) return -1;
    return sc_sync_result();
}
void glGetVertexAttribfv(GLuint index, GLenum pname, GLfloat *out) {
    sc_sync_begin(SC_GL_glGetVertexAttribfv); sc_emit_u32(index); sc_emit_u32(pname);
    if (sc_sync_send() != 0) return;
    float v = 0; sc_sync_recv_bytes(&v, 4); if (out) *out = v;
}
void glGetVertexAttribiv(GLuint index, GLenum pname, GLint *out) {
    sc_sync_begin(SC_GL_glGetVertexAttribiv); sc_emit_u32(index); sc_emit_u32(pname);
    if (sc_sync_send() != 0) return;
    int32_t v = 0; sc_sync_recv_bytes(&v, 4); if (out) *out = v;
}
void glGetVertexAttribPointerv(GLuint index, GLenum pname, void **pointer) {
    sc_sync_begin(SC_GL_glGetVertexAttribPointerv); sc_emit_u32(index); sc_emit_u32(pname);
    if (sc_sync_send() != 0) return;
    uint64_t v = 0; sc_sync_recv_bytes(&v, 8); if (pointer) *pointer = (void *)(uintptr_t)v;
}
GLboolean glIsBuffer(GLuint buffer) { sc_sync_begin(SC_GL_glIsBuffer); sc_emit_u32(buffer); if (sc_sync_send() != 0) return 0; return (GLboolean)sc_sync_result(); }
GLboolean glIsEnabled(GLenum cap) { sc_sync_begin(SC_GL_glIsEnabled); sc_emit_u32(cap); if (sc_sync_send() != 0) return 0; return (GLboolean)sc_sync_result(); }
GLboolean glIsFramebuffer(GLuint fb) { sc_sync_begin(SC_GL_glIsFramebuffer); sc_emit_u32(fb); if (sc_sync_send() != 0) return 0; return (GLboolean)sc_sync_result(); }
GLboolean glIsProgram(GLuint p) { sc_sync_begin(SC_GL_glIsProgram); sc_emit_u32(p); if (sc_sync_send() != 0) return 0; return (GLboolean)sc_sync_result(); }
GLboolean glIsRenderbuffer(GLuint rb) { sc_sync_begin(SC_GL_glIsRenderbuffer); sc_emit_u32(rb); if (sc_sync_send() != 0) return 0; return (GLboolean)sc_sync_result(); }
GLboolean glIsShader(GLuint s) { sc_sync_begin(SC_GL_glIsShader); sc_emit_u32(s); if (sc_sync_send() != 0) return 0; return (GLboolean)sc_sync_result(); }
GLboolean glIsTexture(GLuint t) { sc_sync_begin(SC_GL_glIsTexture); sc_emit_u32(t); if (sc_sync_send() != 0) return 0; return (GLboolean)sc_sync_result(); }

void glGetProgramInfoLog(GLuint program, GLsizei bufSize, GLsizei *length, GLchar *infoLog) {
    sc_sync_begin(SC_GL_glGetProgramInfoLog); sc_emit_u32(program); sc_emit_i32(bufSize);
    if (sc_sync_send() != 0) return;
    read_named_string(bufSize, length, infoLog);
}
void glGetShaderInfoLog(GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog) {
    sc_sync_begin(SC_GL_glGetShaderInfoLog); sc_emit_u32(shader); sc_emit_i32(bufSize);
    if (sc_sync_send() != 0) return;
    read_named_string(bufSize, length, infoLog);
}

/* FIX #5: copia directo al buffer del usuario con sc_pixel_size() corregida
 * (#41). Un readback 1080p RGBA (8 MB) funciona; con packed types el shim
 * escribe exactamente lo que el usuario reservó. */
void glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void *pixels) {
    sc_sync_begin(SC_GL_glReadPixels);
    sc_emit_i32(x); sc_emit_i32(y); sc_emit_i32(w); sc_emit_i32(h);
    sc_emit_u32(format); sc_emit_u32(type);
    if (sc_sync_send() != 0) return;
    if (!pixels || w <= 0 || h <= 0) return;
    size_t ps = sc_pixel_size(format, type);
    if (!ps) return;
    size_t sz = ps * (size_t)w * (size_t)h;
    if (!sz) return;
    sc_sync_recv_bytes(pixels, sz);
}

GLenum glGetError(void) {
    uint32_t local = sc_last_gl_error();
    if (local) { sc_clear_gl_error(); return local; }
    /* Camino rapido: STK/gl4es llaman glGetError decenas de veces por frame y
     * cada llamada es un round-trip que ademas espera todo el batch pendiente.
     * Por defecto no se consulta al daemon (SC_GLERR_SYNC=1 lo restaura). */
    static int sync_err = -1;
    if (sync_err < 0) { const char *e = getenv("SC_GLERR_SYNC"); sync_err = (e && *e == '1'); }
    if (!sync_err) return 0;
    sc_sync_begin(SC_GL_glGetError);
    if (sc_sync_send() != 0) return 0x0500;
    return (GLenum)sc_sync_result();
}

/* ============================================================ ES 3.0 nuevas — void */

void glReadBuffer(GLenum src) { sc_batch_begin(SC_GL_glReadBuffer); sc_emit_u32(src); sc_batch_end(); }
void glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void *indices) { sc_batch_begin(SC_GL_glDrawRangeElements); sc_emit_u32(mode); sc_emit_u32(start); sc_emit_u32(end); sc_emit_i32(count); sc_emit_u32(type); emit_ptr(indices); sc_batch_end(); }
void glTexImage3D(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLsizei d, GLint border, GLenum format, GLenum type, const void *pixels) {
    sc_batch_begin(SC_GL_glTexImage3D);
    sc_emit_u32(target); sc_emit_i32(level); sc_emit_i32(ifmt);
    sc_emit_i32(w); sc_emit_i32(h); sc_emit_i32(d); sc_emit_i32(border);
    sc_emit_u32(format); sc_emit_u32(type);
    size_t sz = pixels ? sc_pixel_size(format, type) * (size_t)w * (size_t)h * (size_t)d : 0;
    sc_emit_bytes(pixels, sz);
    sc_batch_end();
}
void glTexSubImage3D(GLenum target, GLint level, GLint xo, GLint yo, GLint zo, GLsizei w, GLsizei h, GLsizei d, GLenum format, GLenum type, const void *pixels) {
    sc_batch_begin(SC_GL_glTexSubImage3D);
    sc_emit_u32(target); sc_emit_i32(level);
    sc_emit_i32(xo); sc_emit_i32(yo); sc_emit_i32(zo);
    sc_emit_i32(w); sc_emit_i32(h); sc_emit_i32(d);
    sc_emit_u32(format); sc_emit_u32(type);
    size_t sz = pixels ? sc_pixel_size(format, type) * (size_t)w * (size_t)h * (size_t)d : 0;
    sc_emit_bytes(pixels, sz);
    sc_batch_end();
}
void glCopyTexSubImage3D(GLenum t, GLint l, GLint xo, GLint yo, GLint zo, GLint x, GLint y, GLsizei w, GLsizei h) { sc_batch_begin(SC_GL_glCopyTexSubImage3D); sc_emit_u32(t); sc_emit_i32(l); sc_emit_i32(xo); sc_emit_i32(yo); sc_emit_i32(zo); sc_emit_i32(x); sc_emit_i32(y); sc_emit_i32(w); sc_emit_i32(h); sc_batch_end(); }
void glCompressedTexImage3D(GLenum t, GLint l, GLenum ifmt, GLsizei w, GLsizei h, GLsizei d, GLint b, GLsizei imageSize, const void *data) { sc_batch_begin(SC_GL_glCompressedTexImage3D); sc_emit_u32(t); sc_emit_i32(l); sc_emit_u32(ifmt); sc_emit_i32(w); sc_emit_i32(h); sc_emit_i32(d); sc_emit_i32(b); sc_emit_i32(imageSize); sc_emit_bytes(data, data ? (size_t)imageSize : 0); sc_batch_end(); }
void glCompressedTexSubImage3D(GLenum t, GLint l, GLint xo, GLint yo, GLint zo, GLsizei w, GLsizei h, GLsizei d, GLenum fmt, GLsizei imageSize, const void *data) { sc_batch_begin(SC_GL_glCompressedTexSubImage3D); sc_emit_u32(t); sc_emit_i32(l); sc_emit_i32(xo); sc_emit_i32(yo); sc_emit_i32(zo); sc_emit_i32(w); sc_emit_i32(h); sc_emit_i32(d); sc_emit_u32(fmt); sc_emit_i32(imageSize); sc_emit_bytes(data, data ? (size_t)imageSize : 0); sc_batch_end(); }

void glDeleteQueries(GLsizei n, const GLuint *ids) { sc_batch_begin(SC_GL_glDeleteQueries); emit_u32v(ids, n); sc_batch_end(); }
GLboolean glIsQuery(GLuint id) { sc_sync_begin(SC_GL_glIsQuery); sc_emit_u32(id); if (sc_sync_send() != 0) return 0; return (GLboolean)sc_sync_result(); }
void glBeginQuery(GLenum target, GLuint id) { sc_batch_begin(SC_GL_glBeginQuery); sc_emit_u32(target); sc_emit_u32(id); sc_batch_end(); }
void glEndQuery(GLenum target) { sc_batch_begin(SC_GL_glEndQuery); sc_emit_u32(target); sc_batch_end(); }
void glGetQueryiv(GLenum target, GLenum pname, GLint *out) {
    sc_sync_begin(SC_GL_glGetQueryiv); sc_emit_u32(target); sc_emit_u32(pname);
    if (sc_sync_send() != 0) return;
    int32_t v = 0; sc_sync_recv_bytes(&v, 4); if (out) *out = v;
}
void glGetQueryObjectuiv(GLuint id, GLenum pname, GLuint *out) {
    sc_sync_begin(SC_GL_glGetQueryObjectuiv); sc_emit_u32(id); sc_emit_u32(pname);
    if (sc_sync_send() != 0) return;
    uint32_t v = 0; sc_sync_recv_bytes(&v, 4); if (out) *out = v;
}
void glGetBufferPointerv(GLenum target, GLenum pname, void **params) {
    sc_sync_begin(SC_GL_glGetBufferPointerv); sc_emit_u32(target); sc_emit_u32(pname);
    if (sc_sync_send() != 0) return;
    uint64_t v = 0; sc_sync_recv_bytes(&v, 8); if (params) *params = (void *)(uintptr_t)v;
}
void glDrawBuffers(GLsizei n, const GLenum *bufs) { sc_batch_begin(SC_GL_glDrawBuffers); sc_emit_i32(n); if (bufs) for (GLsizei i = 0; i < n; i++) sc_emit_u32(bufs[i]); sc_batch_end(); }

#define MAT_NM(n, m) \
void glUniformMatrix##n##x##m##fv(GLint loc, GLsizei count, GLboolean tr, const GLfloat *v) { \
    sc_batch_begin(SC_GL_glUniformMatrix##n##x##m##fv); sc_emit_i32(loc); sc_emit_i32(count); sc_emit_u32(tr); \
    if (v && count > 0) { \
        size_t total = (size_t)count * (size_t)(n) * (size_t)(m); \
        for (size_t i = 0; i < total; i++) sc_emit_f32(v[i]); \
    } \
    sc_batch_end(); }
MAT_NM(2, 3) MAT_NM(3, 2) MAT_NM(2, 4) MAT_NM(4, 2) MAT_NM(3, 4) MAT_NM(4, 3)
#undef MAT_NM

void glBlitFramebuffer(GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLbitfield mask, GLenum filter) {
    sc_batch_begin(SC_GL_glBlitFramebuffer);
    sc_emit_i32(sx0); sc_emit_i32(sy0); sc_emit_i32(sx1); sc_emit_i32(sy1);
    sc_emit_i32(dx0); sc_emit_i32(dy0); sc_emit_i32(dx1); sc_emit_i32(dy1);
    sc_emit_u32((uint32_t)mask); sc_emit_u32(filter);
    sc_batch_end();
}
void glRenderbufferStorageMultisample(GLenum t, GLsizei s, GLenum ifmt, GLsizei w, GLsizei h) { sc_batch_begin(SC_GL_glRenderbufferStorageMultisample); sc_emit_u32(t); sc_emit_i32(s); sc_emit_u32(ifmt); sc_emit_i32(w); sc_emit_i32(h); sc_batch_end(); }
void glFramebufferTextureLayer(GLenum t, GLenum att, GLuint tex, GLint lvl, GLint layer) { sc_batch_begin(SC_GL_glFramebufferTextureLayer); sc_emit_u32(t); sc_emit_u32(att); sc_emit_u32(tex); sc_emit_i32(lvl); sc_emit_i32(layer); sc_batch_end(); }

/*
 * glMapBufferRange / glFlushMappedBufferRange / glUnmapBuffer — FIX #3.
 *
 * El daemon no puede entregarle a la app un puntero a memoria de la GPU (viven
 * en procesos distintos). Antes devolviamos un malloc() sin inicializar y
 * NUNCA lo subiamos: todo lo que la app escribia por el mapeo se perdia
 * (instancias, matrices de los karts, etc.).
 *
 * Ahora el mapeo es una copia "sombra" en el shim:
 *   - Map: reserva `length` bytes. Si la app no invalida el rango, trae el
 *     contenido actual desde el daemon (el daemon mapea, copia y desmapea).
 *   - Flush (FLUSH_EXPLICIT): sube solo el subrango con glBufferSubData.
 *   - Unmap: si hubo WRITE y no FLUSH_EXPLICIT, sube todo el rango.
 * El daemon nunca deja un buffer mapeado, asi que glBufferData/SubData
 * posteriores no chocan con un mapeo vivo.
 */
#define SC_MAP_READ_BIT        0x0001u
#define SC_MAP_WRITE_BIT       0x0002u
#define SC_MAP_INVAL_RANGE_BIT 0x0004u
#define SC_MAP_INVAL_BUF_BIT   0x0008u
#define SC_MAP_FLUSH_EXPL_BIT  0x0010u
#define SC_MAX_MAPS 16

struct sc_map { int used; GLenum target; GLintptr off; GLsizeiptr len; GLbitfield access; uint8_t *ptr; };
static __thread struct sc_map g_maps[SC_MAX_MAPS];

static struct sc_map *map_find(GLenum target) {
    for (int i = 0; i < SC_MAX_MAPS; i++)
        if (g_maps[i].used && g_maps[i].target == target) return &g_maps[i];
    return NULL;
}

static int map_trace(void) {
    static int t = -1;
    if (t < 0) { const char *e = getenv("SC_TRACE_MAP"); t = (e && *e == '1'); }
    return t;
}

static void map_upload(GLenum target, GLintptr off, const void *data, GLsizeiptr len) {
    sc_batch_begin(SC_GL_glBufferSubData);
    sc_emit_u32(target); sc_emit_u64((uint64_t)off); sc_emit_u64((uint64_t)len);
    sc_emit_bytes(data, (size_t)len);
    sc_batch_end();
}

void *glMapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access) {
    if (length <= 0 || offset < 0) { sc_set_gl_error(GL_INVALID_VALUE); return NULL; }
    struct sc_map *m = map_find(target);
    if (m) { sc_set_gl_error(GL_INVALID_OPERATION); return NULL; }   /* ya mapeado */
    for (int i = 0; i < SC_MAX_MAPS; i++) if (!g_maps[i].used) { m = &g_maps[i]; break; }
    if (!m) { sc_set_gl_error(GL_OUT_OF_MEMORY); return NULL; }

    uint8_t *p = calloc(1, (size_t)length);
    if (!p) { sc_set_gl_error(GL_OUT_OF_MEMORY); return NULL; }

    int need_fetch = (access & SC_MAP_READ_BIT) ||
                     !(access & (SC_MAP_INVAL_RANGE_BIT | SC_MAP_INVAL_BUF_BIT));
    if (need_fetch) {
        sc_sync_begin(SC_GL_glMapBufferRange);
        sc_emit_u32(target); sc_emit_u64((uint64_t)offset); sc_emit_u64((uint64_t)length);
        sc_emit_u32((uint32_t)access);
        if (sc_sync_send() == 0) {
            /* resp: [i32 result][u64 handle][u32 n][bytes n] */
            size_t cap = 12 + (size_t)length;
            uint8_t *tmp = malloc(cap);
            if (tmp) {
                size_t got = sc_sync_recv_bytes(tmp, cap);
                if (got >= 12) {
                    uint32_t n; memcpy(&n, tmp + 8, 4);
                    if (n > got - 12) n = (uint32_t)(got - 12);
                    if (n > (uint32_t)length) n = (uint32_t)length;
                    memcpy(p, tmp + 12, n);
                }
                free(tmp);
            }
        }
    }
    m->used = 1; m->target = target; m->off = offset; m->len = length;
    m->access = access; m->ptr = p;
    if (map_trace())
        fprintf(stderr, "[sc-map] MAP target=0x%x off=%ld len=%ld access=0x%x fetch=%d\n",
                (unsigned)target, (long)offset, (long)length, (unsigned)access, need_fetch);
    return p;
}

void glFlushMappedBufferRange(GLenum target, GLintptr offset, GLsizeiptr length) {
    struct sc_map *m = map_find(target);
    if (!m || offset < 0 || length <= 0 || offset + length > m->len) return;
    if (map_trace())
        fprintf(stderr, "[sc-map] FLUSH target=0x%x off=%ld len=%ld\n",
                (unsigned)target, (long)offset, (long)length);
    map_upload(target, m->off + offset, m->ptr + offset, length);
}

GLboolean glUnmapBuffer(GLenum target) {
    struct sc_map *m = map_find(target);
    if (!m) { sc_set_gl_error(GL_INVALID_OPERATION); return GL_FALSE; }
    if ((m->access & SC_MAP_WRITE_BIT) && !(m->access & SC_MAP_FLUSH_EXPL_BIT))
        map_upload(target, m->off, m->ptr, m->len);
    if (map_trace())
        fprintf(stderr, "[sc-map] UNMAP target=0x%x len=%ld uploaded=%d\n",
                (unsigned)target, (long)m->len,
                (m->access & SC_MAP_WRITE_BIT) && !(m->access & SC_MAP_FLUSH_EXPL_BIT));
    free(m->ptr);
    memset(m, 0, sizeof *m);
    return GL_TRUE;
}

void glDeleteVertexArrays(GLsizei n, const GLuint *vs) { sc_batch_begin(SC_GL_glDeleteVertexArrays); emit_u32v(vs, n); sc_batch_end(); }
void glBindVertexArray(GLuint va) { sc_batch_begin(SC_GL_glBindVertexArray); sc_emit_u32(va); sc_batch_end(); }
GLboolean glIsVertexArray(GLuint va) { sc_sync_begin(SC_GL_glIsVertexArray); sc_emit_u32(va); if (sc_sync_send() != 0) return 0; return (GLboolean)sc_sync_result(); }
void glGetIntegeri_v(GLenum target, GLuint index, GLint *out) {
    sc_sync_begin(SC_GL_glGetIntegeri_v); sc_emit_u32(target); sc_emit_u32(index);
    if (sc_sync_send() != 0) return;
    int32_t v = 0; sc_sync_recv_bytes(&v, 4); if (out) *out = v;
}
void glBeginTransformFeedback(GLenum prim) { sc_batch_begin(SC_GL_glBeginTransformFeedback); sc_emit_u32(prim); sc_batch_end(); }
void glEndTransformFeedback(void) { sc_batch_begin(SC_GL_glEndTransformFeedback); sc_batch_end(); }
void glBindBufferRange(GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size) { sc_batch_begin(SC_GL_glBindBufferRange); sc_emit_u32(target); sc_emit_u32(index); sc_emit_u32(buffer); sc_emit_u64((uint64_t)offset); sc_emit_u64((uint64_t)size); sc_batch_end(); }
void glBindBufferBase(GLenum target, GLuint index, GLuint buffer) { sc_batch_begin(SC_GL_glBindBufferBase); sc_emit_u32(target); sc_emit_u32(index); sc_emit_u32(buffer); sc_batch_end(); }

void glTransformFeedbackVaryings(GLuint program, GLsizei count, const GLchar *const*varyings, GLenum bufferMode) {
    sc_batch_begin(SC_GL_glTransformFeedbackVaryings);
    sc_emit_u32(program); sc_emit_i32(count);
    for (GLsizei i = 0; i < count; i++) sc_emit_string(varyings ? varyings[i] : NULL);
    sc_emit_u32(bufferMode);
    sc_batch_end();
}
void glGetTransformFeedbackVarying(GLuint program, GLuint index, GLsizei bufSize, GLsizei *length, GLsizei *size, GLenum *type, GLchar *name) {
    sc_sync_begin(SC_GL_glGetTransformFeedbackVarying);
    sc_emit_u32(program); sc_emit_u32(index); sc_emit_i32(bufSize);
    if (sc_sync_send() != 0) return;
    size_t cap = 12 + (bufSize > 0 ? (size_t)bufSize : 256);
    uint8_t *tmp = malloc(cap);
    if (!tmp) return;
    size_t got = sc_sync_recv_bytes(tmp, cap);
    if (got < 12) { free(tmp); return; }
    int32_t s; uint32_t t, nl;
    memcpy(&s, tmp, 4); memcpy(&t, tmp + 4, 4); memcpy(&nl, tmp + 8, 4);
    if (size) *size = (GLsizei)s;
    if (type) *type = (GLenum)t;
    if (length) *length = (GLsizei)nl;
    if (name && bufSize > 0) {
        GLsizei k = (GLsizei)nl < bufSize - 1 ? (GLsizei)nl : bufSize - 1;
        if (k < 0) k = 0;
        if ((size_t)k + 12 <= got) { memcpy(name, tmp + 12, (size_t)k); name[k] = 0; }
    }
    free(tmp);
}
void glVertexAttribIPointer(GLuint idx, GLint size, GLenum type, GLsizei stride, const void *pointer) { sc_batch_begin(SC_GL_glVertexAttribIPointer); sc_emit_u32(idx); sc_emit_i32(size); sc_emit_u32(type); sc_emit_i32(stride); emit_ptr(pointer); sc_batch_end(); }
void glGetVertexAttribIiv(GLuint idx, GLenum pname, GLint *out) {
    sc_sync_begin(SC_GL_glGetVertexAttribIiv); sc_emit_u32(idx); sc_emit_u32(pname);
    if (sc_sync_send() != 0) return;
    int32_t v = 0; sc_sync_recv_bytes(&v, 4); if (out) *out = v;
}
void glGetVertexAttribIuiv(GLuint idx, GLenum pname, GLuint *out) {
    sc_sync_begin(SC_GL_glGetVertexAttribIuiv); sc_emit_u32(idx); sc_emit_u32(pname);
    if (sc_sync_send() != 0) return;
    uint32_t v = 0; sc_sync_recv_bytes(&v, 4); if (out) *out = v;
}
void glVertexAttribI4i(GLuint idx, GLint a, GLint b, GLint c, GLint d) { sc_batch_begin(SC_GL_glVertexAttribI4i); sc_emit_u32(idx); sc_emit_i32(a); sc_emit_i32(b); sc_emit_i32(c); sc_emit_i32(d); sc_batch_end(); }
void glVertexAttribI4ui(GLuint idx, GLuint a, GLuint b, GLuint c, GLuint d) { sc_batch_begin(SC_GL_glVertexAttribI4ui); sc_emit_u32(idx); sc_emit_u32(a); sc_emit_u32(b); sc_emit_u32(c); sc_emit_u32(d); sc_batch_end(); }
void glVertexAttribI4iv(GLuint idx, const GLint *v) { sc_batch_begin(SC_GL_glVertexAttribI4iv); sc_emit_u32(idx); if (v) for (int i = 0; i < 4; i++) sc_emit_i32(v[i]); sc_batch_end(); }
void glVertexAttribI4uiv(GLuint idx, const GLuint *v) { sc_batch_begin(SC_GL_glVertexAttribI4uiv); sc_emit_u32(idx); if (v) for (int i = 0; i < 4; i++) sc_emit_u32(v[i]); sc_batch_end(); }
void glGetUniformuiv(GLuint p, GLint loc, GLuint *out) {
    sc_sync_begin(SC_GL_glGetUniformuiv); sc_emit_u32(p); sc_emit_i32(loc);
    if (sc_sync_send() != 0) return;
    uint32_t v = 0; sc_sync_recv_bytes(&v, 4); if (out) *out = v;
}
GLint glGetFragDataLocation(GLuint program, const GLchar *name) { sc_sync_begin(SC_GL_glGetFragDataLocation); sc_emit_u32(program); sc_emit_string(name); if (sc_sync_send() != 0) return -1; return sc_sync_result(); }
void glUniform1ui(GLint loc, GLuint v) { sc_batch_begin(SC_GL_glUniform1ui); sc_emit_i32(loc); sc_emit_u32(v); sc_batch_end(); }
void glUniform2ui(GLint loc, GLuint a, GLuint b) { sc_batch_begin(SC_GL_glUniform2ui); sc_emit_i32(loc); sc_emit_u32(a); sc_emit_u32(b); sc_batch_end(); }
void glUniform3ui(GLint loc, GLuint a, GLuint b, GLuint c) { sc_batch_begin(SC_GL_glUniform3ui); sc_emit_i32(loc); sc_emit_u32(a); sc_emit_u32(b); sc_emit_u32(c); sc_batch_end(); }
void glUniform4ui(GLint loc, GLuint a, GLuint b, GLuint c, GLuint d) { sc_batch_begin(SC_GL_glUniform4ui); sc_emit_i32(loc); sc_emit_u32(a); sc_emit_u32(b); sc_emit_u32(c); sc_emit_u32(d); sc_batch_end(); }
void glUniform1uiv(GLint loc, GLsizei n, const GLuint *v) { sc_batch_begin(SC_GL_glUniform1uiv); sc_emit_i32(loc); sc_emit_i32(n); if (v) for (GLsizei i = 0; i < n; i++) sc_emit_u32(v[i]); sc_batch_end(); }
void glUniform2uiv(GLint loc, GLsizei n, const GLuint *v) { sc_batch_begin(SC_GL_glUniform2uiv); sc_emit_i32(loc); sc_emit_i32(n); if (v) for (GLsizei i = 0; i < n*2; i++) sc_emit_u32(v[i]); sc_batch_end(); }
void glUniform3uiv(GLint loc, GLsizei n, const GLuint *v) { sc_batch_begin(SC_GL_glUniform3uiv); sc_emit_i32(loc); sc_emit_i32(n); if (v) for (GLsizei i = 0; i < n*3; i++) sc_emit_u32(v[i]); sc_batch_end(); }
void glUniform4uiv(GLint loc, GLsizei n, const GLuint *v) { sc_batch_begin(SC_GL_glUniform4uiv); sc_emit_i32(loc); sc_emit_i32(n); if (v) for (GLsizei i = 0; i < n*4; i++) sc_emit_u32(v[i]); sc_batch_end(); }
void glClearBufferiv(GLenum buf, GLint drawbuffer, const GLint *v) { sc_batch_begin(SC_GL_glClearBufferiv); sc_emit_u32(buf); sc_emit_i32(drawbuffer); if (v) for (int i = 0; i < 4; i++) sc_emit_i32(v[i]); sc_batch_end(); }
void glClearBufferuiv(GLenum buf, GLint drawbuffer, const GLuint *v) { sc_batch_begin(SC_GL_glClearBufferuiv); sc_emit_u32(buf); sc_emit_i32(drawbuffer); if (v) for (int i = 0; i < 4; i++) sc_emit_u32(v[i]); sc_batch_end(); }
void glClearBufferfv(GLenum buf, GLint drawbuffer, const GLfloat *v) { sc_batch_begin(SC_GL_glClearBufferfv); sc_emit_u32(buf); sc_emit_i32(drawbuffer); if (v) for (int i = 0; i < 4; i++) sc_emit_f32(v[i]); sc_batch_end(); }
void glClearBufferfi(GLenum buf, GLint drawbuffer, GLfloat depth, GLint stencil) { sc_batch_begin(SC_GL_glClearBufferfi); sc_emit_u32(buf); sc_emit_i32(drawbuffer); sc_emit_f32(depth); sc_emit_i32(stencil); sc_batch_end(); }

const GLubyte *glGetStringi(GLenum name, GLuint index) {
    sc_sync_begin(SC_GL_glGetStringi); sc_emit_u32(name); sc_emit_u32(index);
    if (sc_sync_send() != 0) return NULL;
    uint8_t buf[16384];
    size_t got = read_payload(buf, sizeof buf);
    if (got < 4) return NULL;
    uint32_t len; memcpy(&len, buf, 4);
    if (len > got - 4) len = (uint32_t)(got - 4);
    return (const GLubyte *)sc_string_intern_slot(buf + 4, len, name, index);
}

void glCopyBufferSubData(GLenum r, GLenum w, GLintptr ro, GLintptr wo, GLsizeiptr sz) { sc_batch_begin(SC_GL_glCopyBufferSubData); sc_emit_u32(r); sc_emit_u32(w); sc_emit_u64((uint64_t)ro); sc_emit_u64((uint64_t)wo); sc_emit_u64((uint64_t)sz); sc_batch_end(); }
void glGetUniformIndices(GLuint program, GLsizei n, const GLchar *const*names, GLuint *out) {
    sc_sync_begin(SC_GL_glGetUniformIndices);
    sc_emit_u32(program); sc_emit_i32(n);
    for (GLsizei i = 0; i < n; i++) sc_emit_string(names ? names[i] : NULL);
    if (sc_sync_send() != 0) return;
    read_gl_ids((uint32_t)(n > 0 ? n : 0), out);
}
void glGetActiveUniformsiv(GLuint program, GLsizei n, const GLuint *idx, GLenum pname, GLint *out) {
    sc_sync_begin(SC_GL_glGetActiveUniformsiv);
    sc_emit_u32(program); sc_emit_i32(n);
    if (idx) for (GLsizei i = 0; i < n; i++) sc_emit_u32(idx[i]);
    sc_emit_u32(pname);
    if (sc_sync_send() != 0) return;
    if (n <= 0 || !out) return;
    size_t cap = 4 + (size_t)n * 4;
    uint8_t *tmp = malloc(cap);
    if (!tmp) return;
    size_t got = sc_sync_recv_bytes(tmp, cap);
    if (got < 4) { free(tmp); return; }
    uint32_t c; memcpy(&c, tmp, 4);
    if (c > (uint32_t)n) c = (uint32_t)n;
    if (c > (got - 4) / 4) c = (uint32_t)((got - 4) / 4);
    memcpy(out, tmp + 4, (size_t)c * 4);
    free(tmp);
}
GLuint glGetUniformBlockIndex(GLuint program, const GLchar *name) { sc_sync_begin(SC_GL_glGetUniformBlockIndex); sc_emit_u32(program); sc_emit_string(name); if (sc_sync_send() != 0) return 0xFFFFFFFFu; return (GLuint)sc_sync_result(); }
void glGetActiveUniformBlockiv(GLuint p, GLuint idx, GLenum pname, GLint *out) {
    sc_sync_begin(SC_GL_glGetActiveUniformBlockiv); sc_emit_u32(p); sc_emit_u32(idx); sc_emit_u32(pname);
    if (sc_sync_send() != 0) return;
    int32_t v = 0; sc_sync_recv_bytes(&v, 4); if (out) *out = v;
}
void glGetActiveUniformBlockName(GLuint p, GLuint idx, GLsizei bufSize, GLsizei *length, GLchar *name) {
    sc_sync_begin(SC_GL_glGetActiveUniformBlockName); sc_emit_u32(p); sc_emit_u32(idx); sc_emit_i32(bufSize);
    if (sc_sync_send() != 0) return;
    read_named_string(bufSize, length, name);
}
void glUniformBlockBinding(GLuint p, GLuint bi, GLuint bb) { sc_batch_begin(SC_GL_glUniformBlockBinding); sc_emit_u32(p); sc_emit_u32(bi); sc_emit_u32(bb); sc_batch_end(); }
void glDrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei inst) { sc_batch_begin(SC_GL_glDrawArraysInstanced); sc_emit_u32(mode); sc_emit_i32(first); sc_emit_i32(count); sc_emit_i32(inst); sc_batch_end(); }
void glDrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void *indices, GLsizei inst) { sc_batch_begin(SC_GL_glDrawElementsInstanced); sc_emit_u32(mode); sc_emit_i32(count); sc_emit_u32(type); emit_ptr(indices); sc_emit_i32(inst); sc_batch_end(); }

GLsync glFenceSync(GLenum cond, GLbitfield flags) {
    sc_sync_begin(SC_GL_glFenceSync); sc_emit_u32(cond); sc_emit_u32((uint32_t)flags);
    if (sc_sync_send() != 0) return (GLsync)0;
    return (GLsync)(uintptr_t)sc_sync_recv_u64();
}
GLboolean glIsSync(GLsync sync) { sc_sync_begin(SC_GL_glIsSync); sc_emit_u64((uint64_t)(uintptr_t)sync); if (sc_sync_send() != 0) return 0; return (GLboolean)sc_sync_result(); }
void glDeleteSync(GLsync sync) { sc_batch_begin(SC_GL_glDeleteSync); sc_emit_u64((uint64_t)(uintptr_t)sync); sc_batch_end(); }
GLenum glClientWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout) {
    sc_batch_flush();
    sc_sync_begin(SC_GL_glClientWaitSync);
    sc_emit_u64((uint64_t)(uintptr_t)sync); sc_emit_u32((uint32_t)flags); sc_emit_u64(timeout);
    if (sc_sync_send() != 0) return 0x911A;
    return (GLenum)sc_sync_result();
}
void glWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout) { sc_batch_flush(); sc_sync_begin(SC_GL_glWaitSync); sc_emit_u64((uint64_t)(uintptr_t)sync); sc_emit_u32((uint32_t)flags); sc_emit_u64(timeout); sc_sync_send(); }
void glGetInteger64v(GLenum pname, GLint64 *out) {
    sc_sync_begin(SC_GL_glGetInteger64v); sc_emit_u32(pname);
    if (sc_sync_send() != 0) return;
    if (!out) return;
    size_t cap = 4 + 256 * 8;
    uint8_t *tmp = malloc(cap);
    if (!tmp) return;
    size_t got = sc_sync_recv_bytes(tmp, cap);
    if (got < 4) { free(tmp); return; }
    uint32_t n; memcpy(&n, tmp, 4);
    uint32_t avail = (uint32_t)((got - 4) / 8);
    if (n > avail) n = avail;
    if (n > 256) n = 256;
    memcpy(out, tmp + 4, (size_t)n * 8);
    free(tmp);
}
void glGetSynciv(GLsync sync, GLenum pname, GLsizei bufSize, GLsizei *length, GLint *values) {
    sc_sync_begin(SC_GL_glGetSynciv);
    sc_emit_u64((uint64_t)(uintptr_t)sync); sc_emit_u32(pname); sc_emit_i32(bufSize);
    if (sc_sync_send() != 0) return;
    if (bufSize < 0) bufSize = 0;
    size_t cap = 4 + (bufSize > 0 ? (size_t)bufSize * 4 : 256);
    uint8_t *tmp = malloc(cap);
    if (!tmp) return;
    size_t got = sc_sync_recv_bytes(tmp, cap);
    if (got < 4) { free(tmp); return; }
    uint32_t n; memcpy(&n, tmp, 4);
    if (n > (uint32_t)bufSize) n = (uint32_t)bufSize;
    if (n > (got - 4) / 4) n = (uint32_t)((got - 4) / 4);
    if (length) *length = (GLsizei)n;
    if (values && n > 0) memcpy(values, tmp + 4, (size_t)n * 4);
    free(tmp);
}
void glGetInteger64i_v(GLenum target, GLuint index, GLint64 *out) {
    sc_sync_begin(SC_GL_glGetInteger64i_v); sc_emit_u32(target); sc_emit_u32(index);
    if (sc_sync_send() != 0) return;
    int64_t v = 0; sc_sync_recv_bytes(&v, 8); if (out) *out = v;
}
void glGetBufferParameteri64v(GLenum target, GLenum pname, GLint64 *out) {
    sc_sync_begin(SC_GL_glGetBufferParameteri64v); sc_emit_u32(target); sc_emit_u32(pname);
    if (sc_sync_send() != 0) return;
    int64_t v = 0; sc_sync_recv_bytes(&v, 8); if (out) *out = v;
}
void glDeleteSamplers(GLsizei n, const GLuint *s) { sc_batch_begin(SC_GL_glDeleteSamplers); emit_u32v(s, n); sc_batch_end(); }
GLboolean glIsSampler(GLuint s) { sc_sync_begin(SC_GL_glIsSampler); sc_emit_u32(s); if (sc_sync_send() != 0) return 0; return (GLboolean)sc_sync_result(); }
void glBindSampler(GLuint unit, GLuint s) { sc_batch_begin(SC_GL_glBindSampler); sc_emit_u32(unit); sc_emit_u32(s); sc_batch_end(); }
void glSamplerParameteri(GLuint s, GLenum p, GLint v) { sc_batch_begin(SC_GL_glSamplerParameteri); sc_emit_u32(s); sc_emit_u32(p); sc_emit_i32(v); sc_batch_end(); }
void glSamplerParameteriv(GLuint s, GLenum p, const GLint *v) { sc_batch_begin(SC_GL_glSamplerParameteriv); sc_emit_u32(s); sc_emit_u32(p); sc_emit_i32(v ? v[0] : 0); sc_batch_end(); }
void glSamplerParameterf(GLuint s, GLenum p, GLfloat v) { sc_batch_begin(SC_GL_glSamplerParameterf); sc_emit_u32(s); sc_emit_u32(p); sc_emit_f32(v); sc_batch_end(); }
void glSamplerParameterfv(GLuint s, GLenum p, const GLfloat *v) { sc_batch_begin(SC_GL_glSamplerParameterfv); sc_emit_u32(s); sc_emit_u32(p); sc_emit_f32(v ? v[0] : 0); sc_batch_end(); }
void glGetSamplerParameteriv(GLuint s, GLenum p, GLint *out) {
    sc_sync_begin(SC_GL_glGetSamplerParameteriv); sc_emit_u32(s); sc_emit_u32(p);
    if (sc_sync_send() != 0) return;
    int32_t v = 0; sc_sync_recv_bytes(&v, 4); if (out) *out = v;
}
void glGetSamplerParameterfv(GLuint s, GLenum p, GLfloat *out) {
    sc_sync_begin(SC_GL_glGetSamplerParameterfv); sc_emit_u32(s); sc_emit_u32(p);
    if (sc_sync_send() != 0) return;
    float v = 0; sc_sync_recv_bytes(&v, 4); if (out) *out = v;
}
void glVertexAttribDivisor(GLuint idx, GLuint div) { sc_batch_begin(SC_GL_glVertexAttribDivisor); sc_emit_u32(idx); sc_emit_u32(div); sc_batch_end(); }
void glBindTransformFeedback(GLenum t, GLuint id) { sc_batch_begin(SC_GL_glBindTransformFeedback); sc_emit_u32(t); sc_emit_u32(id); sc_batch_end(); }
void glDeleteTransformFeedbacks(GLsizei n, const GLuint *ids) { sc_batch_begin(SC_GL_glDeleteTransformFeedbacks); emit_u32v(ids, n); sc_batch_end(); }
GLboolean glIsTransformFeedback(GLuint id) { sc_sync_begin(SC_GL_glIsTransformFeedback); sc_emit_u32(id); if (sc_sync_send() != 0) return 0; return (GLboolean)sc_sync_result(); }
void glPauseTransformFeedback(void) { sc_batch_begin(SC_GL_glPauseTransformFeedback); sc_batch_end(); }
void glResumeTransformFeedback(void) { sc_batch_begin(SC_GL_glResumeTransformFeedback); sc_batch_end(); }
void glDrawTransformFeedback(GLenum mode, GLuint id) { sc_batch_begin(SC_GL_glDrawTransformFeedback); sc_emit_u32(mode); sc_emit_u32(id); sc_batch_end(); }
void glInvalidateFramebuffer(GLenum t, GLsizei n, const GLenum *atts) { sc_batch_begin(SC_GL_glInvalidateFramebuffer); sc_emit_u32(t); sc_emit_i32(n); if (atts) for (GLsizei i = 0; i < n; i++) sc_emit_u32(atts[i]); sc_batch_end(); }
void glInvalidateSubFramebuffer(GLenum t, GLsizei n, const GLenum *atts, GLint x, GLint y, GLsizei w, GLsizei h) { sc_batch_begin(SC_GL_glInvalidateSubFramebuffer); sc_emit_u32(t); sc_emit_i32(n); if (atts) for (GLsizei i = 0; i < n; i++) sc_emit_u32(atts[i]); sc_emit_i32(x); sc_emit_i32(y); sc_emit_i32(w); sc_emit_i32(h); sc_batch_end(); }

/* ============================================================ ES 3.1 */

void glDispatchCompute(GLuint x, GLuint y, GLuint z) { sc_batch_begin(SC_GL_glDispatchCompute); sc_emit_u32(x); sc_emit_u32(y); sc_emit_u32(z); sc_batch_end(); }
void glDispatchComputeIndirect(GLintptr off) { sc_batch_begin(SC_GL_glDispatchComputeIndirect); sc_emit_u64((uint64_t)off); sc_batch_end(); }
void glFramebufferParameteri(GLenum t, GLenum p, GLint v) { sc_batch_begin(SC_GL_glFramebufferParameteri); sc_emit_u32(t); sc_emit_u32(p); sc_emit_i32(v); sc_batch_end(); }
void glGetFramebufferParameteriv(GLenum t, GLenum p, GLint *out) {
    sc_sync_begin(SC_GL_glGetFramebufferParameteriv); sc_emit_u32(t); sc_emit_u32(p);
    if (sc_sync_send() != 0) return;
    int32_t v = 0; sc_sync_recv_bytes(&v, 4); if (out) *out = v;
}
void glGetInternalformati64v(GLenum t, GLenum fmt, GLenum p, GLsizei bufSize, GLint64 *out) {
    sc_sync_begin(SC_GL_glGetInternalformati64v); sc_emit_u32(t); sc_emit_u32(fmt); sc_emit_u32(p); sc_emit_i32(bufSize);
    if (sc_sync_send() != 0) return;
    if (bufSize < 0) bufSize = 0;
    size_t cap = 4 + (bufSize > 0 ? (size_t)bufSize * 8 : 256);
    uint8_t *tmp = malloc(cap);
    if (!tmp) return;
    size_t got = sc_sync_recv_bytes(tmp, cap);
    if (got < 4) { free(tmp); return; }
    uint32_t n; memcpy(&n, tmp, 4);
    if (n > (uint32_t)bufSize) n = (uint32_t)bufSize;
    if (n > (got - 4) / 8) n = (uint32_t)((got - 4) / 8);
    if (out && n > 0) memcpy(out, tmp + 4, (size_t)n * 8);
    free(tmp);
}
void glInvalidateTexSubImage(GLuint tex, GLint lvl, GLint xo, GLint yo, GLint zo, GLsizei w, GLsizei h, GLsizei d) { sc_batch_begin(SC_GL_glInvalidateTexSubImage); sc_emit_u32(tex); sc_emit_i32(lvl); sc_emit_i32(xo); sc_emit_i32(yo); sc_emit_i32(zo); sc_emit_i32(w); sc_emit_i32(h); sc_emit_i32(d); sc_batch_end(); }
void glInvalidateTexImage(GLuint tex, GLint lvl) { sc_batch_begin(SC_GL_glInvalidateTexImage); sc_emit_u32(tex); sc_emit_i32(lvl); sc_batch_end(); }
void glInvalidateBufferSubData(GLuint b, GLintptr off, GLsizeiptr sz) { sc_batch_begin(SC_GL_glInvalidateBufferSubData); sc_emit_u32(b); sc_emit_u64((uint64_t)off); sc_emit_u64((uint64_t)sz); sc_batch_end(); }
void glInvalidateBufferData(GLuint b) { sc_batch_begin(SC_GL_glInvalidateBufferData); sc_emit_u32(b); sc_batch_end(); }
void glGetProgramInterfaceiv(GLuint p, GLenum iface, GLenum pname, GLint *out) {
    sc_sync_begin(SC_GL_glGetProgramInterfaceiv); sc_emit_u32(p); sc_emit_u32(iface); sc_emit_u32(pname);
    if (sc_sync_send() != 0) return;
    int32_t v = 0; sc_sync_recv_bytes(&v, 4); if (out) *out = v;
}
GLuint glGetProgramResourceIndex(GLuint p, GLenum iface, const GLchar *name) { sc_sync_begin(SC_GL_glGetProgramResourceIndex); sc_emit_u32(p); sc_emit_u32(iface); sc_emit_string(name); if (sc_sync_send() != 0) return 0xFFFFFFFFu; return (GLuint)sc_sync_result(); }
void glGetProgramResourceName(GLuint p, GLenum iface, GLuint idx, GLsizei bufSize, GLsizei *length, GLchar *name) {
    sc_sync_begin(SC_GL_glGetProgramResourceName); sc_emit_u32(p); sc_emit_u32(iface); sc_emit_u32(idx); sc_emit_i32(bufSize);
    if (sc_sync_send() != 0) return;
    read_named_string(bufSize, length, name);
}
void glGetProgramResourceiv(GLuint p, GLenum iface, GLuint idx, GLsizei nProps, const GLenum *props, GLsizei bufSize, GLsizei *length, GLint *out) {
    sc_sync_begin(SC_GL_glGetProgramResourceiv);
    sc_emit_u32(p); sc_emit_u32(iface); sc_emit_u32(idx); sc_emit_i32(nProps);
    if (props) for (GLsizei i = 0; i < nProps; i++) sc_emit_u32(props[i]);
    sc_emit_i32(bufSize);
    if (sc_sync_send() != 0) return;
    if (bufSize < 0) bufSize = 0;
    size_t cap = 4 + (bufSize > 0 ? (size_t)bufSize * 4 : 256);
    uint8_t *tmp = malloc(cap);
    if (!tmp) return;
    size_t got = sc_sync_recv_bytes(tmp, cap);
    if (got < 4) { free(tmp); return; }
    uint32_t n; memcpy(&n, tmp, 4);
    if (n > (uint32_t)bufSize) n = (uint32_t)bufSize;
    if (n > (got - 4) / 4) n = (uint32_t)((got - 4) / 4);
    if (length) *length = (GLsizei)n;
    if (out && n > 0) memcpy(out, tmp + 4, (size_t)n * 4);
    free(tmp);
}
GLint glGetProgramResourceLocation(GLuint p, GLenum iface, const GLchar *name) { sc_sync_begin(SC_GL_glGetProgramResourceLocation); sc_emit_u32(p); sc_emit_u32(iface); sc_emit_string(name); if (sc_sync_send() != 0) return -1; return sc_sync_result(); }
GLint glGetProgramResourceLocationIndex(GLuint p, GLenum iface, const GLchar *name) { sc_sync_begin(SC_GL_glGetProgramResourceLocationIndex); sc_emit_u32(p); sc_emit_u32(iface); sc_emit_string(name); if (sc_sync_send() != 0) return -1; return sc_sync_result(); }
void glShaderStorageBlockBinding(GLuint p, GLuint sbi, GLuint bb) { sc_batch_begin(SC_GL_glShaderStorageBlockBinding); sc_emit_u32(p); sc_emit_u32(sbi); sc_emit_u32(bb); sc_batch_end(); }
void glTexBufferRange(GLenum t, GLenum ifmt, GLuint b, GLintptr off, GLsizeiptr sz) { sc_batch_begin(SC_GL_glTexBufferRange); sc_emit_u32(t); sc_emit_u32(ifmt); sc_emit_u32(b); sc_emit_u64((uint64_t)off); sc_emit_u64((uint64_t)sz); sc_batch_end(); }
void glTexStorage2DMultisample(GLenum t, GLsizei s, GLenum ifmt, GLsizei w, GLsizei h, GLboolean fixeds) { sc_batch_begin(SC_GL_glTexStorage2DMultisample); sc_emit_u32(t); sc_emit_i32(s); sc_emit_u32(ifmt); sc_emit_i32(w); sc_emit_i32(h); sc_emit_u32(fixeds); sc_batch_end(); }
void glTexStorage3DMultisample(GLenum t, GLsizei s, GLenum ifmt, GLsizei w, GLsizei h, GLsizei d, GLboolean fixeds) { sc_batch_begin(SC_GL_glTexStorage3DMultisample); sc_emit_u32(t); sc_emit_i32(s); sc_emit_u32(ifmt); sc_emit_i32(w); sc_emit_i32(h); sc_emit_i32(d); sc_emit_u32(fixeds); sc_batch_end(); }
void glTextureView(GLuint tex, GLenum t, GLuint orig, GLenum ifmt, GLuint minl, GLuint numl, GLuint minlay, GLuint numlay) { sc_batch_begin(SC_GL_glTextureView); sc_emit_u32(tex); sc_emit_u32(t); sc_emit_u32(orig); sc_emit_u32(ifmt); sc_emit_u32(minl); sc_emit_u32(numl); sc_emit_u32(minlay); sc_emit_u32(numlay); sc_batch_end(); }
void glBindImageTexture(GLuint u, GLuint tex, GLint lvl, GLboolean layered, GLint layer, GLenum access, GLenum fmt) { sc_batch_begin(SC_GL_glBindImageTexture); sc_emit_u32(u); sc_emit_u32(tex); sc_emit_i32(lvl); sc_emit_u32(layered); sc_emit_i32(layer); sc_emit_u32(access); sc_emit_u32(fmt); sc_batch_end(); }
void glMemoryBarrier(GLbitfield b) { sc_batch_begin(SC_GL_glMemoryBarrier); sc_emit_u32((uint32_t)b); sc_batch_end(); }
void glMemoryBarrierByRegion(GLbitfield b) { sc_batch_begin(SC_GL_glMemoryBarrierByRegion); sc_emit_u32((uint32_t)b); sc_batch_end(); }
void glGetMultisamplefv(GLenum pname, GLuint idx, GLfloat *out) {
    sc_sync_begin(SC_GL_glGetMultisamplefv); sc_emit_u32(pname); sc_emit_u32(idx);
    if (sc_sync_send() != 0) return;
    float v = 0; sc_sync_recv_bytes(&v, 4); if (out) *out = v;
}
void glSampleMaski(GLuint idx, GLbitfield mask) { sc_batch_begin(SC_GL_glSampleMaski); sc_emit_u32(idx); sc_emit_u32((uint32_t)mask); sc_batch_end(); }
void glTexStorage2D(GLenum t, GLsizei lvls, GLenum ifmt, GLsizei w, GLsizei h) { sc_batch_begin(SC_GL_glTexStorage2D); sc_emit_u32(t); sc_emit_i32(lvls); sc_emit_u32(ifmt); sc_emit_i32(w); sc_emit_i32(h); sc_batch_end(); }
void glTexStorage3D(GLenum t, GLsizei lvls, GLenum ifmt, GLsizei w, GLsizei h, GLsizei d) { sc_batch_begin(SC_GL_glTexStorage3D); sc_emit_u32(t); sc_emit_i32(lvls); sc_emit_u32(ifmt); sc_emit_i32(w); sc_emit_i32(h); sc_emit_i32(d); sc_batch_end(); }
void glGetTexLevelParameteriv(GLenum t, GLint lvl, GLenum p, GLint *out) {
    sc_sync_begin(SC_GL_glGetTexLevelParameteriv); sc_emit_u32(t); sc_emit_i32(lvl); sc_emit_u32(p);
    if (sc_sync_send() != 0) return;
    int32_t v = 0; sc_sync_recv_bytes(&v, 4); if (out) *out = v;
}
void glGetTexLevelParameterfv(GLenum t, GLint lvl, GLenum p, GLfloat *out) {
    sc_sync_begin(SC_GL_glGetTexLevelParameterfv); sc_emit_u32(t); sc_emit_i32(lvl); sc_emit_u32(p);
    if (sc_sync_send() != 0) return;
    float v = 0; sc_sync_recv_bytes(&v, 4); if (out) *out = v;
}
void glBindVertexBuffer(GLuint bi, GLuint buf, GLintptr off, GLsizei stride) { sc_batch_begin(SC_GL_glBindVertexBuffer); sc_emit_u32(bi); sc_emit_u32(buf); sc_emit_u64((uint64_t)off); sc_emit_i32(stride); sc_batch_end(); }
void glVertexAttribFormat(GLuint idx, GLint size, GLenum type, GLboolean norm, GLuint reloff) { sc_batch_begin(SC_GL_glVertexAttribFormat); sc_emit_u32(idx); sc_emit_i32(size); sc_emit_u32(type); sc_emit_u32(norm); sc_emit_u32(reloff); sc_batch_end(); }
void glVertexAttribIFormat(GLuint idx, GLint size, GLenum type, GLuint reloff) { sc_batch_begin(SC_GL_glVertexAttribIFormat); sc_emit_u32(idx); sc_emit_i32(size); sc_emit_u32(type); sc_emit_u32(reloff); sc_batch_end(); }
void glVertexAttribBinding(GLuint ai, GLuint bb) { sc_batch_begin(SC_GL_glVertexAttribBinding); sc_emit_u32(ai); sc_emit_u32(bb); sc_batch_end(); }
void glVertexBindingDivisor(GLuint bi, GLuint div) { sc_batch_begin(SC_GL_glVertexBindingDivisor); sc_emit_u32(bi); sc_emit_u32(div); sc_batch_end(); }

/* ============================================================ ES 3.2 */

void glBlendBarrier(void) { sc_batch_begin(SC_GL_glBlendBarrier); sc_batch_end(); }
void glCopyImageSubData(GLuint src, GLenum st, GLint sl, GLint sx, GLint sy, GLint sz, GLuint dst, GLenum dt, GLint dl, GLint dx, GLint dy, GLint dz, GLsizei w, GLsizei h, GLsizei d) {
    sc_batch_begin(SC_GL_glCopyImageSubData);
    sc_emit_u32(src); sc_emit_u32(st); sc_emit_i32(sl); sc_emit_i32(sx); sc_emit_i32(sy); sc_emit_i32(sz);
    sc_emit_u32(dst); sc_emit_u32(dt); sc_emit_i32(dl); sc_emit_i32(dx); sc_emit_i32(dy); sc_emit_i32(dz);
    sc_emit_i32(w); sc_emit_i32(h); sc_emit_i32(d);
    sc_batch_end();
}
void glDebugMessageControl(GLenum src, GLenum type, GLenum sev, GLsizei n, const GLuint *ids, GLboolean en) {
    sc_batch_begin(SC_GL_glDebugMessageControl);
    sc_emit_u32(src); sc_emit_u32(type); sc_emit_u32(sev);
    emit_u32v(ids, n);
    sc_emit_u32(en);
    sc_batch_end();
}
void glDebugMessageInsert(GLenum src, GLenum type, GLuint id, GLenum sev, GLsizei len, const GLchar *buf) { sc_batch_begin(SC_GL_glDebugMessageInsert); sc_emit_u32(src); sc_emit_u32(type); sc_emit_u32(id); sc_emit_u32(sev); sc_emit_i32(len); sc_emit_bytes(buf, buf ? (len >= 0 ? (size_t)len : strlen(buf)) : 0); sc_batch_end(); }
/* #11: no-op documentado; requiere hilo de escucha en sc_core.c. */
void glDebugMessageCallback(GLDEBUGPROC cb, const void *user) { (void)cb; (void)user; }
GLuint glGetDebugMessageLog(GLuint count, GLsizei bufSize, GLenum *sources, GLenum *types, GLuint *ids, GLenum *severities, GLsizei *lengths, GLchar *log) { (void)count;(void)bufSize;(void)sources;(void)types;(void)ids;(void)severities;(void)lengths;(void)log; return 0; }
void glPushDebugGroup(GLenum src, GLuint id, GLsizei len, const GLchar *msg) { sc_batch_begin(SC_GL_glPushDebugGroup); sc_emit_u32(src); sc_emit_u32(id); sc_emit_bytes(msg, msg ? (len >= 0 ? (size_t)len : strlen(msg)) : 0); sc_batch_end(); }
void glPopDebugGroup(void) { sc_batch_begin(SC_GL_glPopDebugGroup); sc_batch_end(); }
void glObjectLabel(GLenum id, GLuint name, GLsizei len, const GLchar *label) { sc_batch_begin(SC_GL_glObjectLabel); sc_emit_u32(id); sc_emit_u32(name); sc_emit_bytes(label, label ? (len >= 0 ? (size_t)len : strlen(label)) : 0); sc_batch_end(); }
void glGetObjectLabel(GLenum id, GLuint name, GLsizei bufSize, GLsizei *length, GLchar *label) {
    sc_sync_begin(SC_GL_glGetObjectLabel); sc_emit_u32(id); sc_emit_u32(name); sc_emit_i32(bufSize);
    if (sc_sync_send() != 0) return;
    read_named_string(bufSize, length, label);
}
void glObjectPtrLabel(const void *p, GLsizei len, const GLchar *label) { sc_batch_begin(SC_GL_glObjectPtrLabel); sc_emit_u64((uint64_t)(uintptr_t)p); sc_emit_bytes(label, label ? (len >= 0 ? (size_t)len : strlen(label)) : 0); sc_batch_end(); }
void glGetObjectPtrLabel(const void *p, GLsizei bufSize, GLsizei *length, GLchar *label) {
    sc_sync_begin(SC_GL_glGetObjectPtrLabel); sc_emit_u64((uint64_t)(uintptr_t)p); sc_emit_i32(bufSize);
    if (sc_sync_send() != 0) return;
    read_named_string(bufSize, length, label);
}
void glGetPointerv(GLenum pname, void **params) { sc_sync_begin(SC_GL_glGetPointerv); sc_emit_u32(pname); if (sc_sync_send() != 0) return; uint64_t v = 0; sc_sync_recv_bytes(&v, 8); if (params) *params = (void *)(uintptr_t)v; }
void glEnablei(GLenum t, GLuint i) { sc_batch_begin(SC_GL_glEnablei); sc_emit_u32(t); sc_emit_u32(i); sc_batch_end(); }
void glDisablei(GLenum t, GLuint i) { sc_batch_begin(SC_GL_glDisablei); sc_emit_u32(t); sc_emit_u32(i); sc_batch_end(); }
void glBlendEquationi(GLuint i, GLenum m) { sc_batch_begin(SC_GL_glBlendEquationi); sc_emit_u32(i); sc_emit_u32(m); sc_batch_end(); }
void glBlendEquationSeparatei(GLuint i, GLenum mr, GLenum ma) { sc_batch_begin(SC_GL_glBlendEquationSeparatei); sc_emit_u32(i); sc_emit_u32(mr); sc_emit_u32(ma); sc_batch_end(); }
void glBlendFunci(GLuint i, GLenum s, GLenum d) { sc_batch_begin(SC_GL_glBlendFunci); sc_emit_u32(i); sc_emit_u32(s); sc_emit_u32(d); sc_batch_end(); }
void glBlendFuncSeparatei(GLuint i, GLenum sr, GLenum dr, GLenum sa, GLenum da) { sc_batch_begin(SC_GL_glBlendFuncSeparatei); sc_emit_u32(i); sc_emit_u32(sr); sc_emit_u32(dr); sc_emit_u32(sa); sc_emit_u32(da); sc_batch_end(); }
void glColorMaski(GLuint i, GLboolean r, GLboolean g, GLboolean b, GLboolean a) { sc_batch_begin(SC_GL_glColorMaski); sc_emit_u32(i); sc_emit_u32(r); sc_emit_u32(g); sc_emit_u32(b); sc_emit_u32(a); sc_batch_end(); }
GLboolean glIsEnabledi(GLenum t, GLuint i) { sc_sync_begin(SC_GL_glIsEnabledi); sc_emit_u32(t); sc_emit_u32(i); if (sc_sync_send() != 0) return 0; return (GLboolean)sc_sync_result(); }
void glDrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const void *indices, GLint base) { sc_batch_begin(SC_GL_glDrawElementsBaseVertex); sc_emit_u32(mode); sc_emit_i32(count); sc_emit_u32(type); emit_ptr(indices); sc_emit_i32(base); sc_batch_end(); }
void glDrawRangeElementsBaseVertex(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void *indices, GLint base) { sc_batch_begin(SC_GL_glDrawRangeElementsBaseVertex); sc_emit_u32(mode); sc_emit_u32(start); sc_emit_u32(end); sc_emit_i32(count); sc_emit_u32(type); emit_ptr(indices); sc_emit_i32(base); sc_batch_end(); }
void glDrawElementsInstancedBaseVertex(GLenum mode, GLsizei count, GLenum type, const void *indices, GLsizei inst, GLint base) { sc_batch_begin(SC_GL_glDrawElementsInstancedBaseVertex); sc_emit_u32(mode); sc_emit_i32(count); sc_emit_u32(type); emit_ptr(indices); sc_emit_i32(inst); sc_emit_i32(base); sc_batch_end(); }
void glDrawArraysIndirect(GLenum mode, const void *indirect) { sc_batch_begin(SC_GL_glDrawArraysIndirect); sc_emit_u32(mode); emit_ptr(indirect); sc_batch_end(); }
void glDrawElementsIndirect(GLenum mode, GLenum type, const void *indirect) { sc_batch_begin(SC_GL_glDrawElementsIndirect); sc_emit_u32(mode); sc_emit_u32(type); emit_ptr(indirect); sc_batch_end(); }
void glFramebufferTexture(GLenum t, GLenum att, GLuint tex, GLint lvl) { sc_batch_begin(SC_GL_glFramebufferTexture); sc_emit_u32(t); sc_emit_u32(att); sc_emit_u32(tex); sc_emit_i32(lvl); sc_batch_end(); }
void glPrimitiveBoundingBox(GLfloat a, GLfloat b, GLfloat c, GLfloat d, GLfloat e, GLfloat f, GLfloat g, GLfloat h) { sc_batch_begin(SC_GL_glPrimitiveBoundingBox); sc_emit_f32(a); sc_emit_f32(b); sc_emit_f32(c); sc_emit_f32(d); sc_emit_f32(e); sc_emit_f32(f); sc_emit_f32(g); sc_emit_f32(h); sc_batch_end(); }
GLenum glGetGraphicsResetStatus(void) { sc_sync_begin(SC_GL_glGetGraphicsResetStatus); if (sc_sync_send() != 0) return 0; return (GLenum)sc_sync_result(); }
void glReadnPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, GLsizei bufSize, void *pixels) {
    sc_sync_begin(SC_GL_glReadnPixels);
    sc_emit_i32(x); sc_emit_i32(y); sc_emit_i32(w); sc_emit_i32(h);
    sc_emit_u32(fmt); sc_emit_u32(type); sc_emit_i32(bufSize);
    if (sc_sync_send() != 0) return;
    if (!pixels || bufSize <= 0 || w <= 0 || h <= 0) return;
    size_t sz = sc_pixel_size(fmt, type) * (size_t)w * (size_t)h;
    if (sz > (size_t)bufSize) sz = (size_t)bufSize;
    sc_sync_recv_bytes(pixels, sz);
}
#define GETN_UNIF(name, op, T) \
void name(GLuint p, GLint loc, GLsizei bufSize, T *out) { \
    sc_sync_begin(op); sc_emit_u32(p); sc_emit_i32(loc); sc_emit_i32(bufSize); \
    if (sc_sync_send() != 0) return; \
    if (bufSize <= 0 || !out) return; \
    size_t cap = 4 + (size_t)bufSize * sizeof(T); \
    uint8_t *tmp = malloc(cap); \
    if (!tmp) return; \
    size_t got = sc_sync_recv_bytes(tmp, cap); \
    if (got < 4) { free(tmp); return; } \
    uint32_t n; memcpy(&n, tmp, 4); \
    if (n > (uint32_t)bufSize) n = (uint32_t)bufSize; \
    if (n > (got - 4) / sizeof(T)) n = (uint32_t)((got - 4) / sizeof(T)); \
    memcpy(out, tmp + 4, (size_t)n * sizeof(T)); \
    free(tmp); \
}
GETN_UNIF(glGetnUniformfv, SC_GL_glGetnUniformfv, GLfloat)
GETN_UNIF(glGetnUniformiv, SC_GL_glGetnUniformiv, GLint)
GETN_UNIF(glGetnUniformuiv, SC_GL_glGetnUniformuiv, GLuint)
#undef GETN_UNIF

void glMinSampleShading(GLfloat v) { sc_batch_begin(SC_GL_glMinSampleShading); sc_emit_f32(v); sc_batch_end(); }
void glPatchParameteri(GLenum pname, GLint v) { sc_batch_begin(SC_GL_glPatchParameteri); sc_emit_u32(pname); sc_emit_i32(v); sc_batch_end(); }
void glTexParameterIiv(GLenum t, GLenum p, const GLint *v) { sc_batch_begin(SC_GL_glTexParameterIiv); sc_emit_u32(t); sc_emit_u32(p); sc_emit_i32(v ? v[0] : 0); sc_batch_end(); }
void glTexParameterIuiv(GLenum t, GLenum p, const GLuint *v) { sc_batch_begin(SC_GL_glTexParameterIuiv); sc_emit_u32(t); sc_emit_u32(p); sc_emit_u32(v ? v[0] : 0); sc_batch_end(); }
void glGetTexParameterIiv(GLenum t, GLenum p, GLint *out) { sc_sync_begin(SC_GL_glGetTexParameterIiv); sc_emit_u32(t); sc_emit_u32(p); if (sc_sync_send() != 0) return; int32_t v = 0; sc_sync_recv_bytes(&v, 4); if (out) *out = v; }
void glGetTexParameterIuiv(GLenum t, GLenum p, GLuint *out) { sc_sync_begin(SC_GL_glGetTexParameterIuiv); sc_emit_u32(t); sc_emit_u32(p); if (sc_sync_send() != 0) return; uint32_t v = 0; sc_sync_recv_bytes(&v, 4); if (out) *out = v; }
void glSamplerParameterIiv(GLuint s, GLenum p, const GLint *v) { sc_batch_begin(SC_GL_glSamplerParameterIiv); sc_emit_u32(s); sc_emit_u32(p); sc_emit_i32(v ? v[0] : 0); sc_batch_end(); }
void glSamplerParameterIuiv(GLuint s, GLenum p, const GLuint *v) { sc_batch_begin(SC_GL_glSamplerParameterIuiv); sc_emit_u32(s); sc_emit_u32(p); sc_emit_u32(v ? v[0] : 0); sc_batch_end(); }
void glGetSamplerParameterIiv(GLuint s, GLenum p, GLint *out) { sc_sync_begin(SC_GL_glGetSamplerParameterIiv); sc_emit_u32(s); sc_emit_u32(p); if (sc_sync_send() != 0) return; int32_t v = 0; sc_sync_recv_bytes(&v, 4); if (out) *out = v; }
void glGetSamplerParameterIuiv(GLuint s, GLenum p, GLuint *out) { sc_sync_begin(SC_GL_glGetSamplerParameterIuiv); sc_emit_u32(s); sc_emit_u32(p); if (sc_sync_send() != 0) return; uint32_t v = 0; sc_sync_recv_bytes(&v, 4); if (out) *out = v; }
void glGetQueryObjectiv(GLuint id, GLenum p, GLint *out) { sc_sync_begin(SC_GL_glGetQueryObjectiv); sc_emit_u32(id); sc_emit_u32(p); if (sc_sync_send() != 0) return; int32_t v = 0; sc_sync_recv_bytes(&v, 4); if (out) *out = v; }
void glGetQueryObjecti64v(GLuint id, GLenum p, GLint64 *out) { sc_sync_begin(SC_GL_glGetQueryObjecti64v); sc_emit_u32(id); sc_emit_u32(p); if (sc_sync_send() != 0) return; int64_t v = 0; sc_sync_recv_bytes(&v, 8); if (out) *out = v; }
void glGetQueryObjectui64v(GLuint id, GLenum p, GLuint64 *out) { sc_sync_begin(SC_GL_glGetQueryObjectui64v); sc_emit_u32(id); sc_emit_u32(p); if (sc_sync_send() != 0) return; uint64_t v = 0; sc_sync_recv_bytes(&v, 8); if (out) *out = v; }
void glGetQueryBufferObjectiv(GLuint id, GLuint buf, GLenum p, GLintptr off) { sc_batch_begin(SC_GL_glGetQueryBufferObjectiv); sc_emit_u32(id); sc_emit_u32(buf); sc_emit_u32(p); sc_emit_u64((uint64_t)off); sc_batch_end(); }
void glGetQueryBufferObjectuiv(GLuint id, GLuint buf, GLenum p, GLintptr off) { sc_batch_begin(SC_GL_glGetQueryBufferObjectuiv); sc_emit_u32(id); sc_emit_u32(buf); sc_emit_u32(p); sc_emit_u64((uint64_t)off); sc_batch_end(); }
void glGetQueryBufferObjecti64v(GLuint id, GLuint buf, GLenum p, GLintptr off) { sc_batch_begin(SC_GL_glGetQueryBufferObjecti64v); sc_emit_u32(id); sc_emit_u32(buf); sc_emit_u32(p); sc_emit_u64((uint64_t)off); sc_batch_end(); }
void glGetQueryBufferObjectui64v(GLuint id, GLuint buf, GLenum p, GLintptr off) { sc_batch_begin(SC_GL_glGetQueryBufferObjectui64v); sc_emit_u32(id); sc_emit_u32(buf); sc_emit_u32(p); sc_emit_u64((uint64_t)off); sc_batch_end(); }

void glGetInternalformativ(GLenum t, GLenum fmt, GLenum p, GLsizei bufSize, GLint *out) {
    sc_sync_begin(SC_GL_glGetInternalformativ); sc_emit_u32(t); sc_emit_u32(fmt); sc_emit_u32(p); sc_emit_i32(bufSize);
    if (sc_sync_send() != 0) return;
    if (bufSize < 0) bufSize = 0;
    size_t cap = 4 + (bufSize > 0 ? (size_t)bufSize * 4 : 256);
    uint8_t *tmp = malloc(cap);
    if (!tmp) return;
    size_t got = sc_sync_recv_bytes(tmp, cap);
    if (got < 4) { free(tmp); return; }
    uint32_t n; memcpy(&n, tmp, 4);
    if (n > (uint32_t)bufSize) n = (uint32_t)bufSize;
    if (n > (got - 4) / 4) n = (uint32_t)((got - 4) / 4);
    if (out && n > 0) memcpy(out, tmp + 4, (size_t)n * 4);
    free(tmp);
}
void glTexBuffer(GLenum t, GLenum ifmt, GLuint buf) { sc_batch_begin(SC_GL_glTexBuffer); sc_emit_u32(t); sc_emit_u32(ifmt); sc_emit_u32(buf); sc_batch_end(); }

/* ============================================================ extensiones */

void glGetProgramBinary(GLuint p, GLsizei bufSize, GLsizei *length, GLenum *binaryFormat, void *binary) {
    sc_sync_begin(SC_GL_glGetProgramBinary); sc_emit_u32(p); sc_emit_i32(bufSize);
    if (sc_sync_send() != 0) return;
    if (bufSize < 0) bufSize = 0;
    size_t cap = 8 + (bufSize > 0 ? (size_t)bufSize : 256);
    uint8_t *tmp = malloc(cap);
    if (!tmp) return;
    size_t got = sc_sync_recv_bytes(tmp, cap);
    if (got < 8) { free(tmp); return; }
    uint32_t fmt, len;
    memcpy(&fmt, tmp, 4); memcpy(&len, tmp + 4, 4);
    if (binaryFormat) *binaryFormat = (GLenum)fmt;
    if (length) *length = (GLsizei)len;
    if (binary && bufSize > 0 && len > 0) {
        size_t k = len < (uint32_t)bufSize ? len : (uint32_t)bufSize;
        if (k > got - 8) k = got - 8;
        memcpy(binary, tmp + 8, k);
    }
    free(tmp);
}
void glProgramBinary(GLuint p, GLenum fmt, const void *binary, GLsizei length) {
    sc_batch_begin(SC_GL_glProgramBinary);
    sc_emit_u32(p); sc_emit_u32(fmt);
    sc_emit_bytes(binary, binary ? (size_t)length : 0);
    sc_batch_end();
}
void glProgramParameteri(GLuint p, GLenum pname, GLint value) { sc_batch_begin(SC_GL_glProgramParameteri); sc_emit_u32(p); sc_emit_u32(pname); sc_emit_i32(value); sc_batch_end(); }

void glEGLImageTargetTexture2DOES(GLenum target, GLeglImageOES image) { sc_batch_begin(SC_GL_glEGLImageTargetTexture2DOES); sc_emit_u32(target); sc_emit_u64((uint64_t)(uintptr_t)image); sc_batch_end(); }
void glEGLImageTargetRenderbufferStorageOES(GLenum target, GLeglImageOES image) { sc_batch_begin(SC_GL_glEGLImageTargetRenderbufferStorageOES); sc_emit_u32(target); sc_emit_u64((uint64_t)(uintptr_t)image); sc_batch_end(); }

void glBlendEquationOES(GLenum m) { glBlendEquation(m); }
void glBlendEquationSeparateOES(GLenum mr, GLenum ma) { glBlendEquationSeparate(mr, ma); }
void glBlendFuncSeparateOES(GLenum sr, GLenum dr, GLenum sa, GLenum da) { glBlendFuncSeparate(sr, dr, sa, da); }
void glBlendFuncSeparateiOES(GLuint i, GLenum sr, GLenum dr, GLenum sa, GLenum da) { glBlendFuncSeparatei(i, sr, dr, sa, da); }
void glBlendEquationiOES(GLuint i, GLenum m) { glBlendEquationi(i, m); }
void glBlendEquationSeparateiOES(GLuint i, GLenum mr, GLenum ma) { glBlendEquationSeparatei(i, mr, ma); }
void glBlendFunciOES(GLuint i, GLenum s, GLenum d) { glBlendFunci(i, s, d); }

void glDrawArraysInstancedANGLE(GLenum mode, GLint first, GLsizei count, GLsizei inst) { glDrawArraysInstanced(mode, first, count, inst); }
void glDrawElementsInstancedANGLE(GLenum mode, GLsizei count, GLenum type, const void *indices, GLsizei inst) { glDrawElementsInstanced(mode, count, type, indices, inst); }
void glVertexAttribDivisorANGLE(GLuint idx, GLuint div) { glVertexAttribDivisor(idx, div); }

void glGenVertexArraysOES(GLsizei n, GLuint *vs) { glGenVertexArrays(n, vs); }
void glBindVertexArrayOES(GLuint va) { glBindVertexArray(va); }
void glDeleteVertexArraysOES(GLsizei n, const GLuint *vs) { glDeleteVertexArrays(n, vs); }
GLboolean glIsVertexArrayOES(GLuint va) { return glIsVertexArray(va); }
void glRenderbufferStorageMultisampleANGLE(GLenum t, GLsizei s, GLenum ifmt, GLsizei w, GLsizei h) { glRenderbufferStorageMultisample(t, s, ifmt, w, h); }

void glFramebufferTexture2DMultisampleEXT(GLenum t, GLenum att, GLenum tt, GLuint tex, GLint lvl, GLsizei s) { sc_batch_begin(SC_GL_glFramebufferTexture2DMultisampleEXT); sc_emit_u32(t); sc_emit_u32(att); sc_emit_u32(tt); sc_emit_u32(tex); sc_emit_i32(lvl); sc_emit_i32(s); sc_batch_end(); }
void glFramebufferTextureMultiviewOVR(GLenum t, GLenum att, GLuint tex, GLint lvl, GLint baseView, GLsizei numViews) { sc_batch_begin(SC_GL_glFramebufferTextureMultiviewOVR); sc_emit_u32(t); sc_emit_u32(att); sc_emit_u32(tex); sc_emit_i32(lvl); sc_emit_i32(baseView); sc_emit_i32(numViews); sc_batch_end(); }
void glTexStorage2DEXT(GLenum t, GLsizei lvls, GLenum ifmt, GLsizei w, GLsizei h) { glTexStorage2D(t, lvls, ifmt, w, h); }
void glTexStorage3DEXT(GLenum t, GLsizei lvls, GLenum ifmt, GLsizei w, GLsizei h, GLsizei d) { glTexStorage3D(t, lvls, ifmt, w, h, d); }
void glTextureStorage2DEXT(GLuint tex, GLenum t, GLsizei lvls, GLenum ifmt, GLsizei w, GLsizei h) { sc_batch_begin(SC_GL_glTextureStorage2DEXT); sc_emit_u32(tex); sc_emit_u32(t); sc_emit_i32(lvls); sc_emit_u32(ifmt); sc_emit_i32(w); sc_emit_i32(h); sc_batch_end(); }
void glTextureStorage3DEXT(GLuint tex, GLenum t, GLsizei lvls, GLenum ifmt, GLsizei w, GLsizei h, GLsizei d) { sc_batch_begin(SC_GL_glTextureStorage3DEXT); sc_emit_u32(tex); sc_emit_u32(t); sc_emit_i32(lvls); sc_emit_u32(ifmt); sc_emit_i32(w); sc_emit_i32(h); sc_emit_i32(d); sc_batch_end(); }
void glCopyImageSubDataEXT(GLuint s, GLenum st, GLint sl, GLint sx, GLint sy, GLint sz, GLuint d, GLenum dt, GLint dl, GLint dx, GLint dy, GLint dz, GLsizei w, GLsizei h, GLsizei dep) {
    glCopyImageSubData(s, st, sl, sx, sy, sz, d, dt, dl, dx, dy, dz, w, h, dep);
}
void glDiscardFramebufferEXT(GLenum t, GLsizei n, const GLenum *atts) { glInvalidateFramebuffer(t, n, atts); }
void glMultiDrawArraysEXT(GLenum mode, const GLint *first, const GLsizei *count, GLsizei n) {
    sc_batch_begin(SC_GL_glMultiDrawArraysEXT);
    sc_emit_u32(mode); sc_emit_i32(n);
    for (GLsizei i = 0; i < n; i++) { sc_emit_i32(first ? first[i] : 0); sc_emit_i32(count ? count[i] : 0); }
    sc_batch_end();
}
void glMultiDrawElementsEXT(GLenum mode, const GLsizei *count, GLenum type, const void *const*indices, GLsizei n) {
    sc_batch_begin(SC_GL_glMultiDrawElementsEXT);
    sc_emit_u32(mode); sc_emit_i32(n);
    for (GLsizei i = 0; i < n; i++) {
        sc_emit_i32(count ? count[i] : 0);
        sc_emit_u32(type);
        emit_ptr(indices ? indices[i] : NULL);
    }
    sc_batch_end();
}
void glMultiDrawArraysIndirectEXT(GLenum mode, const void *indirect, GLsizei drawcount, GLsizei stride) { sc_batch_begin(SC_GL_glMultiDrawArraysIndirectEXT); sc_emit_u32(mode); emit_ptr(indirect); sc_emit_i32(drawcount); sc_emit_i32(stride); sc_batch_end(); }
void glMultiDrawElementsIndirectEXT(GLenum mode, GLenum type, const void *indirect, GLsizei drawcount, GLsizei stride) { sc_batch_begin(SC_GL_glMultiDrawElementsIndirectEXT); sc_emit_u32(mode); sc_emit_u32(type); emit_ptr(indirect); sc_emit_i32(drawcount); sc_emit_i32(stride); sc_batch_end(); }

void glDebugMessageCallbackKHR(GLDEBUGPROCKHR cb, const void *user) { (void)cb; (void)user; }
void glDebugMessageControlKHR(GLenum s, GLenum t, GLenum sev, GLsizei n, const GLuint *ids, GLboolean en) { glDebugMessageControl(s, t, sev, n, ids, en); }
void glDebugMessageInsertKHR(GLenum s, GLenum t, GLuint id, GLenum sev, GLsizei len, const GLchar *buf) { glDebugMessageInsert(s, t, id, sev, len, buf); }
GLuint glGetDebugMessageLogKHR(GLuint c, GLsizei b, GLenum *s, GLenum *t, GLuint *i, GLenum *sev, GLsizei *l, GLchar *log) { (void)c;(void)b;(void)s;(void)t;(void)i;(void)sev;(void)l;(void)log; return 0; }
void glPushDebugGroupKHR(GLenum s, GLuint id, GLsizei l, const GLchar *m) { glPushDebugGroup(s, id, l, m); }
void glPopDebugGroupKHR(void) { glPopDebugGroup(); }
void glObjectLabelKHR(GLenum id, GLuint n, GLsizei l, const GLchar *lb) { glObjectLabel(id, n, l, lb); }
void glGetObjectLabelKHR(GLenum id, GLuint n, GLsizei b, GLsizei *l, GLchar *lb) { glGetObjectLabel(id, n, b, l, lb); }
void glObjectPtrLabelKHR(const void *p, GLsizei l, const GLchar *lb) { glObjectPtrLabel(p, l, lb); }
void glGetObjectPtrLabelKHR(const void *p, GLsizei b, GLsizei *l, GLchar *lb) { glGetObjectPtrLabel(p, b, l, lb); }
void glGetPointervKHR(GLenum pname, void **params) { glGetPointerv(pname, params); }

void glQueryCounterEXT(GLuint id, GLenum target) { sc_batch_begin(SC_GL_glQueryCounterEXT); sc_emit_u32(id); sc_emit_u32(target); sc_batch_end(); }
void glGetQueryObjecti64vEXT(GLuint id, GLenum p, GLint64 *out) { glGetQueryObjecti64v(id, p, out); }
void glGetQueryObjectui64vEXT(GLuint id, GLenum p, GLuint64 *out) { glGetQueryObjectui64v(id, p, out); }
void glGetQueryObjectivEXT(GLuint id, GLenum p, GLint *out) { glGetQueryObjectiv(id, p, out); }
void glGetQueryObjectuivEXT(GLuint id, GLenum p, GLuint *out) { glGetQueryObjectuiv(id, p, out); }

void glBlendBarrierKHR(void) { glBlendBarrier(); }
void glBlendBarrierNV(void) { glBlendBarrier(); }

void glTexBufferEXT(GLenum t, GLenum ifmt, GLuint buf) { glTexBuffer(t, ifmt, buf); }
void glTexBufferOES(GLenum t, GLenum ifmt, GLuint buf) { glTexBuffer(t, ifmt, buf); }
void glTexBufferRangeEXT(GLenum t, GLenum ifmt, GLuint buf, GLintptr off, GLsizeiptr sz) { glTexBufferRange(t, ifmt, buf, off, sz); }
void glTexBufferRangeOES(GLenum t, GLenum ifmt, GLuint buf, GLintptr off, GLsizeiptr sz) { glTexBufferRange(t, ifmt, buf, off, sz); }

void glFramebufferTexture2DDownsampleIMG(GLenum t, GLenum att, GLenum tt, GLuint tex, GLint lvl, GLint x, GLint y) { sc_batch_begin(SC_GL_glFramebufferTexture2DDownsampleIMG); sc_emit_u32(t); sc_emit_u32(att); sc_emit_u32(tt); sc_emit_u32(tex); sc_emit_i32(lvl); sc_emit_i32(x); sc_emit_i32(y); sc_batch_end(); }
void glFramebufferTextureLayerDownsampleIMG(GLenum t, GLenum att, GLuint tex, GLint lvl, GLint layer, GLint x, GLint y) { sc_batch_begin(SC_GL_glFramebufferTextureLayerDownsampleIMG); sc_emit_u32(t); sc_emit_u32(att); sc_emit_u32(tex); sc_emit_i32(lvl); sc_emit_i32(layer); sc_emit_i32(x); sc_emit_i32(y); sc_batch_end(); }
void glFramebufferTextureMultisampleMultiviewOVR(GLenum t, GLenum att, GLuint tex, GLint lvl, GLsizei samples, GLint baseView, GLsizei numViews) { sc_batch_begin(SC_GL_glFramebufferTextureMultisampleMultiviewOVR); sc_emit_u32(t); sc_emit_u32(att); sc_emit_u32(tex); sc_emit_i32(lvl); sc_emit_i32(samples); sc_emit_i32(baseView); sc_emit_i32(numViews); sc_batch_end(); }

/* ============================================================ shim lookup */

extern EGLBoolean eglSwapBuffersWithDamageKHR(EGLDisplay, EGLSurface, const EGLint*, EGLint);
extern EGLDisplay eglGetPlatformDisplayEXT(EGLenum, void*, const EGLint*);

static void *lookup_egl(const char *name) {
    if (!strcmp(name, "eglSwapBuffersWithDamageKHR")) return (void*)eglSwapBuffersWithDamageKHR;
    if (!strcmp(name, "eglGetPlatformDisplayEXT"))    return (void*)eglGetPlatformDisplayEXT;
    return NULL;
}

static void *lookup_gl(const char *name) {
    if (!strcmp(name, "glEGLImageTargetTexture2DOES"))             return (void*)glEGLImageTargetTexture2DOES;
    if (!strcmp(name, "glEGLImageTargetRenderbufferStorageOES"))   return (void*)glEGLImageTargetRenderbufferStorageOES;
    if (!strcmp(name, "glBlendEquationOES"))                       return (void*)glBlendEquationOES;
    if (!strcmp(name, "glBlendEquationSeparateOES"))               return (void*)glBlendEquationSeparateOES;
    if (!strcmp(name, "glBlendFuncSeparateOES"))                   return (void*)glBlendFuncSeparateOES;
    if (!strcmp(name, "glBlendFuncSeparateiOES"))                  return (void*)glBlendFuncSeparateiOES;
    if (!strcmp(name, "glBlendEquationiOES"))                      return (void*)glBlendEquationiOES;
    if (!strcmp(name, "glBlendEquationSeparateiOES"))              return (void*)glBlendEquationSeparateiOES;
    if (!strcmp(name, "glBlendFunciOES"))                          return (void*)glBlendFunciOES;
    if (!strcmp(name, "glDrawArraysInstancedANGLE"))               return (void*)glDrawArraysInstancedANGLE;
    if (!strcmp(name, "glDrawElementsInstancedANGLE"))             return (void*)glDrawElementsInstancedANGLE;
    if (!strcmp(name, "glVertexAttribDivisorANGLE"))               return (void*)glVertexAttribDivisorANGLE;
    if (!strcmp(name, "glGenVertexArraysOES"))                     return (void*)glGenVertexArraysOES;
    if (!strcmp(name, "glBindVertexArrayOES"))                     return (void*)glBindVertexArrayOES;
    if (!strcmp(name, "glDeleteVertexArraysOES"))                  return (void*)glDeleteVertexArraysOES;
    if (!strcmp(name, "glIsVertexArrayOES"))                       return (void*)glIsVertexArrayOES;
    if (!strcmp(name, "glRenderbufferStorageMultisampleANGLE"))    return (void*)glRenderbufferStorageMultisampleANGLE;
    if (!strcmp(name, "glFramebufferTexture2DMultisampleEXT"))     return (void*)glFramebufferTexture2DMultisampleEXT;
    if (!strcmp(name, "glFramebufferTextureMultiviewOVR"))         return (void*)glFramebufferTextureMultiviewOVR;
    if (!strcmp(name, "glTexStorage2DEXT"))                        return (void*)glTexStorage2DEXT;
    if (!strcmp(name, "glTexStorage3DEXT"))                        return (void*)glTexStorage3DEXT;
    if (!strcmp(name, "glTextureStorage2DEXT"))                    return (void*)glTextureStorage2DEXT;
    if (!strcmp(name, "glTextureStorage3DEXT"))                    return (void*)glTextureStorage3DEXT;
    if (!strcmp(name, "glCopyImageSubDataEXT"))                    return (void*)glCopyImageSubDataEXT;
    if (!strcmp(name, "glDiscardFramebufferEXT"))                  return (void*)glDiscardFramebufferEXT;
    if (!strcmp(name, "glMultiDrawArraysEXT"))                     return (void*)glMultiDrawArraysEXT;
    if (!strcmp(name, "glMultiDrawElementsEXT"))                   return (void*)glMultiDrawElementsEXT;
    if (!strcmp(name, "glMultiDrawArraysIndirectEXT"))             return (void*)glMultiDrawArraysIndirectEXT;
    if (!strcmp(name, "glMultiDrawElementsIndirectEXT"))           return (void*)glMultiDrawElementsIndirectEXT;
    if (!strcmp(name, "glDebugMessageCallbackKHR"))                return (void*)glDebugMessageCallbackKHR;
    if (!strcmp(name, "glDebugMessageControlKHR"))                 return (void*)glDebugMessageControlKHR;
    if (!strcmp(name, "glDebugMessageInsertKHR"))                  return (void*)glDebugMessageInsertKHR;
    if (!strcmp(name, "glGetDebugMessageLogKHR"))                  return (void*)glGetDebugMessageLogKHR;
    if (!strcmp(name, "glPushDebugGroupKHR"))                      return (void*)glPushDebugGroupKHR;
    if (!strcmp(name, "glPopDebugGroupKHR"))                       return (void*)glPopDebugGroupKHR;
    if (!strcmp(name, "glObjectLabelKHR"))                         return (void*)glObjectLabelKHR;
    if (!strcmp(name, "glGetObjectLabelKHR"))                      return (void*)glGetObjectLabelKHR;
    if (!strcmp(name, "glObjectPtrLabelKHR"))                      return (void*)glObjectPtrLabelKHR;
    if (!strcmp(name, "glGetObjectPtrLabelKHR"))                   return (void*)glGetObjectPtrLabelKHR;
    if (!strcmp(name, "glGetPointervKHR"))                         return (void*)glGetPointervKHR;
    if (!strcmp(name, "glQueryCounterEXT"))                        return (void*)glQueryCounterEXT;
    if (!strcmp(name, "glGetQueryObjecti64vEXT"))                  return (void*)glGetQueryObjecti64vEXT;
    if (!strcmp(name, "glGetQueryObjectui64vEXT"))                 return (void*)glGetQueryObjectui64vEXT;
    if (!strcmp(name, "glGetQueryObjectivEXT"))                    return (void*)glGetQueryObjectivEXT;
    if (!strcmp(name, "glGetQueryObjectuivEXT"))                   return (void*)glGetQueryObjectuivEXT;
    if (!strcmp(name, "glBlendBarrierKHR"))                        return (void*)glBlendBarrierKHR;
    if (!strcmp(name, "glBlendBarrierNV"))                         return (void*)glBlendBarrierNV;
    if (!strcmp(name, "glTexBufferEXT"))                           return (void*)glTexBufferEXT;
    if (!strcmp(name, "glTexBufferOES"))                           return (void*)glTexBufferOES;
    if (!strcmp(name, "glTexBufferRangeEXT"))                      return (void*)glTexBufferRangeEXT;
    if (!strcmp(name, "glTexBufferRangeOES"))                      return (void*)glTexBufferRangeOES;
    if (!strcmp(name, "glFramebufferTexture2DDownsampleIMG"))      return (void*)glFramebufferTexture2DDownsampleIMG;
    if (!strcmp(name, "glFramebufferTextureLayerDownsampleIMG"))   return (void*)glFramebufferTextureLayerDownsampleIMG;
    if (!strcmp(name, "glFramebufferTextureMultisampleMultiviewOVR")) return (void*)glFramebufferTextureMultisampleMultiviewOVR;
    return NULL;
}

void *sc_shim_lookup(const char *name, uint32_t kind) {
    if (!name) return NULL;
    switch (kind) {
    case 1: return lookup_egl(name);
    case 2: return lookup_gl(name);
    default: return NULL;
    }
}
