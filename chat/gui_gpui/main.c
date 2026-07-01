#include <stdio.h>
#include <dlfcn.h>
#include <signal.h>
#include <unistd.h>

typedef void* (*create_fn)(const char*);
typedef int   (*run_fn)(void*);
typedef void  (*status_fn)(void*, const char*);
typedef void  (*delta_fn)(void*, const char*);
typedef void  (*free_fn)(void*);

static void* g_app = NULL;
static volatile int g_running = 1;

void handler(int sig) { g_running = 0; }

int main() {
    signal(SIGINT, handler);
    signal(SIGTERM, handler);

    void* h = dlopen("/opt/friday/chat/gui_gpui/target/release/libfriday_gui.so", RTLD_NOW);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }

    create_fn create = (create_fn)dlsym(h, "gui_app_create");
    run_fn run = (run_fn)dlsym(h, "gui_run");
    status_fn status = (status_fn)dlsym(h, "gui_set_status");
    delta_fn delta = (delta_fn)dlsym(h, "gui_stream_delta");
    free_fn app_free = (free_fn)dlsym(h, "gui_app_free");

    if (!create || !run) { fprintf(stderr, "no symbols\n"); return 1; }

    g_app = create("");
    if (!g_app) { fprintf(stderr, "create failed\n"); return 1; }

    if (status) status(g_app, "就绪");

    fprintf(stderr, "calling run...\n");
    fflush(stderr);
    int ret = run(g_app);
    fprintf(stderr, "gui_run returned %d\n", ret);

    if (app_free) app_free(g_app);
    dlclose(h);
    return 0;
}
