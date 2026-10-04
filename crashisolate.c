#define _GNU_SOURCE
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

static pthread_t g_main;

/* Imprime "label: modulo+offset (simbolo)" — el offset sirve directo con addr2line. */
static void print_addr(const char *label, void *addr) {
    Dl_info di;
    char buf[512];
    int n;
    if (dladdr(addr, &di) && di.dli_fname) {
        unsigned long off = (unsigned long)addr - (unsigned long)di.dli_fbase;
        n = snprintf(buf, sizeof buf, "[crashisolate] %s: %s+0x%lx (%s)\n",
                     label, di.dli_fname, off, di.dli_sname ? di.dli_sname : "?");
    } else {
        n = snprintf(buf, sizeof buf, "[crashisolate] %s: %p (sin modulo)\n", label, addr);
    }
    if (n > 0) write(2, buf, (size_t)n);
}

static void handler(int sig, siginfo_t *si, void *uctx) {
    pthread_t self = pthread_self();
    long tid = syscall(SYS_gettid);
    ucontext_t *uc = (ucontext_t *)uctx;
    char buf[256];
    int n = snprintf(buf, sizeof buf,
        "\n[crashisolate] sig=%d tid=%ld addr=%p main=%d\n",
        sig, tid, si->si_addr, pthread_equal(self, g_main) ? 1 : 0);
    if (n > 0) write(2, buf, (size_t)n);

    print_addr("pc", (void *)(uintptr_t)uc->uc_mcontext.pc);
    print_addr("lr", (void *)(uintptr_t)uc->uc_mcontext.regs[30]);

    void *bt[32];
    int nb = backtrace(bt, 32);
    for (int i = 0; i < nb; i++) {
        char lbl[16];
        snprintf(lbl, sizeof lbl, "bt%d", i);
        print_addr(lbl, bt[i]);
    }

    if (!pthread_equal(self, g_main)) {
        write(2, "[crashisolate] worker aislado\n", 30);
        syscall(SYS_exit, 0);
    }
    _exit(128 + sig);
}

__attribute__((constructor))
static void init(void) {
    g_main = pthread_self();
    struct sigaction sa = {0};
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);
}
