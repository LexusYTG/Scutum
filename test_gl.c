#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdio.h>
int main(void) {
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(d, NULL, NULL)) { puts("init fail"); return 1; }
    eglBindAPI(EGL_OPENGL_ES_API);

    EGLint ca[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_NONE
    };
    EGLConfig cfg; EGLint n = 0;
    if (!eglChooseConfig(d, ca, &cfg, 1, &n) || n == 0) {
        printf("chooseConfig fail err=0x%x\n", eglGetError()); return 1;
    }

    EGLint pa[] = { EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE };
    EGLSurface s = eglCreatePbufferSurface(d, cfg, pa);

    EGLint xa[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext c = eglCreateContext(d, cfg, EGL_NO_CONTEXT, xa);

    if (!eglMakeCurrent(d, s, s, c)) { puts("makeCurrent fail"); return 1; }

    printf("VENDOR   = %s\n", glGetString(GL_VENDOR));
    printf("RENDERER = %s\n", glGetString(GL_RENDERER));
    printf("VERSION  = %s\n", glGetString(GL_VERSION));
    printf("err pre  = 0x%x\n", glGetError());

    glClearColor(1,0,0,1);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();
    printf("err post = 0x%x\n", glGetError());

    return 0;
}
