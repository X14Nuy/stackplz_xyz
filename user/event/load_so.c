#include "load_so.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

typedef const char* (*FPTR)(char*, void*, void*, void*);
typedef const char* (*FPTRV2)(int, void*, void*, void*);

static void*  g_handle = NULL;
static FPTR   g_fptr = NULL;
static FPTRV2 g_fptrv2 = NULL;

int init_stack_unwinder(const char* dl_path) {
    if (g_handle != NULL) {
        // 已经初始化
        return 0;
    }

    char full_path[256];
    snprintf(full_path, sizeof(full_path), "%s/%s", dl_path, "libstackplz.so");

    g_handle = dlopen(full_path, RTLD_NOW);
    if (g_handle == NULL) {
        fprintf(stderr, "Failed to dlopen %s: %s\n", full_path, dlerror());
        return -1;
    }

    // 清除旧的dlerror信息
    dlerror(); 
    g_fptr = (FPTR)dlsym(g_handle, "StackPlz");
    const char* dlsym_error = dlerror();
    if (dlsym_error != NULL) {
        fprintf(stderr, "Failed to dlsym StackPlz: %s\n", dlsym_error);
        dlclose(g_handle);
        g_handle = NULL;
        return -1;
    }

    dlerror();
    g_fptrv2 = (FPTRV2)dlsym(g_handle, "StackPlzV2");
    dlsym_error = dlerror();
    if (dlsym_error != NULL) {
        fprintf(stderr, "Failed to dlsym StackPlzV2: %s\n", dlsym_error);
        dlclose(g_handle);
        g_handle = NULL;
        g_fptr = NULL;
        return -1;
    }

    return 0;
}

void close_stack_unwinder() {
    if (g_handle != NULL) {
        dlclose(g_handle);
        g_handle = NULL;
        g_fptr = NULL;
        g_fptrv2 = NULL;
    }
}

const char* get_stack(char* map_buffer, void* opt, void* regs_buf, void* stack_buf) {
    if (g_fptr == NULL) {
        // 如果未初始化，返回NULL，让上层处理
        return NULL;
    }
    return (*g_fptr)(map_buffer, opt, regs_buf, stack_buf);
}

const char* get_stackv2(int pid, void* opt, void* regs_buf, void* stack_buf) {
    if (g_fptrv2 == NULL) {
        return NULL;
    }
    return (*g_fptrv2)(pid, opt, regs_buf, stack_buf);
}

void free_stack_str(char* str) {
    if (str != NULL) {
        free(str);
    }
}