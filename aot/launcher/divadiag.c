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
#include <sys/stat.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/syscall.h>
static int fd;
static int bt_done;
static void wstr(const char* s);
static _Unwind_Reason_Code bt_cb(struct _Unwind_Context* ctx, void* arg);

static void bt_handler(int sig, siginfo_t* si, void* uc){
    (void)sig;(void)si;(void)uc;
    wstr("  BACKTRACE:");
    _Unwind_Backtrace(bt_cb, 0);
}

static void* watchdog(void* arg){
    (void)arg;
    for(;;){
        sleep(5);
        DIR* d=opendir("/proc/self/task");
        if(!d) continue;
        struct dirent* e;
        wstr("WATCHDOG tasks:");
        while((e=readdir(d))){
            if(e->d_name[0]=='.') continue;
            char path[160],comm[64]={0},state='?',wchan[64]={0},line[256];
            snprintf(path,sizeof path,"/proc/self/task/%s/stat",e->d_name);
            FILE* f=fopen(path,"r");
            if(f){ int pid; if(fscanf(f,"%d %63s %c",&pid,comm,&state)==3){} fclose(f); }
            snprintf(path,sizeof path,"/proc/self/task/%s/wchan",e->d_name);
            f=fopen(path,"r"); if(f){ if(fscanf(f,"%63s",wchan)==1){} fclose(f); }
            snprintf(line,sizeof line,"  tid=%s comm=%s state=%c wchan=%s",e->d_name,comm,state,wchan);
            wstr(line);
            if(!bt_done && state=='S' && (strstr(comm,"libbbprobe")||strstr(comm,"SceFiosIO")||strstr(comm,"renderer_worker")||strstr(comm,"SoundCue")))
                syscall(SYS_tgkill, getpid(), atoi(e->d_name), SIGUSR1);
        }
        closedir(d);
        bt_done=1;
    }
    return NULL;
}

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
    setenv("BB_TRACE_SEMA", "1", 1);
    setenv("BB_COND_BREAKER", "1", 1);
    setenv("BB_WAIT_PROFILE", "1", 1);
    { struct sigaction sa; memset(&sa,0,sizeof sa); sa.sa_sigaction=bt_handler; sa.sa_flags=SA_SIGINFO;
      sigaction(SIGUSR1,&sa,NULL); }
    { pthread_t wd; if (pthread_create(&wd, NULL, watchdog, NULL)==0) pthread_detach(wd); }
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
        // Turnip: if the user placed libvulkan_freedreno.so in the data dir, stage it
        // into the app private dir (/sdcard is not linker-permitted) and point the
        // runtime's driver loader at it (BB_VULKAN_DRIVER, read by vk_driver_loader).
        static char tsrc[600], tdst[600];
        snprintf(tsrc, sizeof tsrc, "%s/libvulkan_freedreno.so", dir);
        struct stat st;
        if (stat(tsrc, &st) == 0 && st.st_size > 0) {
            snprintf(tdst, sizeof tdst, "/data/data/com.divaft.launcher/files/libvulkan_freedreno.so");
            struct stat st2;
            if (stat(tdst, &st2) != 0 || st2.st_size != st.st_size) {
                int in = open(tsrc, O_RDONLY);
                int out = open(tdst, O_WRONLY|O_CREAT|O_TRUNC, 0644);
                if (in >= 0 && out >= 0) {
                    static char cb[1<<20]; ssize_t n;
                    while ((n = read(in, cb, sizeof cb)) > 0) {
                        ssize_t off = 0;
                        while (off < n) { ssize_t w = write(out, cb+off, n-off); if (w <= 0) break; off += w; }
                    }
                    wstr("[divadiag] turnip staged to private dir\n");
                } else wstr("[divadiag] turnip copy failed\n");
                if (in >= 0) close(in);
                if (out >= 0) close(out);
            }
            if (!getenv("BB_VULKAN_DRIVER")) {
                setenv("BB_VULKAN_DRIVER", tdst, 1);
                wstr("[divadiag] BB_VULKAN_DRIVER="); wstr(tdst); wstr("\n");
            }
        }
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
