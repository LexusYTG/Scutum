#include <EGL/egl.h>
#include <stdio.h>
int main(void) {
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    printf("display = %p\n", (void*)d);
    EGLint maj=0, min=0;
    if (!eglInitialize(d, &maj, &min)) {
        printf("eglInitialize FALLO (err=0x%x)\n", eglGetError());
        return 1;
    }
    printf("EGL %d.%d\n", maj, min);
    printf("VENDOR  = %s\n", eglQueryString(d, EGL_VENDOR));
    printf("VERSION = %s\n", eglQueryString(d, EGL_VERSION));
    eglTerminate(d);
    return 0;
}
