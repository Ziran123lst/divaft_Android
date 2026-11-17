// LD_PRELOAD diagnostic shim: line-buffer stdout + host crash backtrace to file.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <unwind.h>
#include <sys/ucontext.h>

static int fd = -1;
static void wstr(const char* s){ if(fd>=0) write(fd, s, strlen(s)); }
static void whex(unsigned long v){ char b[32]; int n=snprintf(b,sizeof b,"0x%lx",v); if(fd>=0) write(fd,b,n); }

static _Unwind_Reason_Code bt_cb(struct _Unwind_Context* ctx, void* arg){
    unsigned long ip = _Unwind_GetIP(ctx);
    Dl_info info;
    wstr("  #"); whex(ip);
    if (dladdr((void*)ip, &info) && info.dli_fname){
        wstr(" "); wstr(info.dli_fname);
        if (info.dli_sname){ wstr(" "); wstr(info.dli_sname); }
        wstr("+"); whex((unsigned long)((char*)ip-(char*)info.dli_fbase));
    }
    wstr("\n");
    return _URC_NO_REASON;
}

static void handler(int sig, siginfo_t* si, void* uc){
    wstr("\n=== HOST CRASH sig="); whex(sig); wstr(" addr="); whex((unsigned long)si->si_addr); wstr(" ===\n");
    _Unwind_Backtrace(bt_cb, 0);
    if(fd>=0) fsync(fd);
    _exit(128+sig);
}

__attribute__((constructor(101)))
static void init(void){
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    const char* p = getenv("DIVA_DIAG_LOG");
    if (p) {
        fd = open(p, O_WRONLY|O_CREAT|O_TRUNC, 0644);
        // Android app subprocess env lacks HOME/XDG_*; shadps4 path_util static
        // init does strlen(getenv("HOME")) -> NULL crash. Provide POSIX-like env.
        static char dir[512];
        strncpy(dir, p, sizeof(dir)-1); dir[sizeof(dir)-1]=0;
        char* slash = strrchr(dir, '/');
        if (slash) *slash = 0;
        if (!getenv("HOME")) setenv("HOME", dir, 1);
        if (!getenv("XDG_DATA_HOME")) setenv("XDG_DATA_HOME", dir, 1);
        if (!getenv("XDG_CONFIG_HOME")) setenv("XDG_CONFIG_HOME", dir, 1);
        if (!getenv("XDG_CACHE_HOME")) setenv("XDG_CACHE_HOME", dir, 1);
        if (!getenv("TMPDIR")) setenv("TMPDIR", dir, 1);
        wstr("[divadiag] HOME="); wstr(getenv("HOME")); wstr("\n");
    }
    static char stack[64*1024];
    stack_t ss = { .ss_sp=stack, .ss_size=sizeof stack, .ss_flags=0 };
    sigaltstack(&ss, NULL);
    struct sigaction sa; memset(&sa,0,sizeof sa);
    sa.sa_sigaction = handler; sa.sa_flags = SA_ONSTACK|SA_SIGINFO;
    sigaction(SIGSEGV,&sa,NULL); sigaction(SIGBUS,&sa,NULL);
    sigaction(SIGABRT,&sa,NULL); sigaction(SIGFPE,&sa,NULL); sigaction(SIGILL,&sa,NULL);
    wstr("[divadiag] installed, stdout line-buffered\n");
}
