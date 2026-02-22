#include "internal/stdio_impl.h"
#include "internal/__stdlib.h"
#include "FreeRTOS.h"
#include "task.h"
#include "cmd.h"
#include "../../drivers/psram/psram.h"
#include <string.h>

extern int printf(const char *restrict, ...);

static void* allocator(void) {
    return pvPortMalloc(sizeof(void*));
}

void* __new_ctx(void) {
    cmd_ctx_t* res = (cmd_ctx_t*)pvPortCalloc(1, sizeof(cmd_ctx_t));
    res->pallocs = new_list_v(allocator, (dealloc_fn_ptr_t)psram_free, 0);
    return res;
}

/// TODO: __delete_ctx

void* __malloc2(void* ctx, size_t sz) {
    if (!ctx || !((cmd_ctx_t*)ctx)->pallocs)
        return pvPortMalloc(sz);
    void* res = pvPortMalloc(sz);
    if (!res && psram_is_available())
        res = psram_alloc(sz);
    if (!res) return NULL;
    list_push_back(((cmd_ctx_t*)ctx)->pallocs, res);
    return res;
}

void* __calloc2(void* ctx, size_t n, size_t sz) {
    if (!ctx || !((cmd_ctx_t*)ctx)->pallocs)
        return pvPortCalloc(n, sz);
    void* res = pvPortCalloc(n, sz);
    if (!res && psram_is_available()) {
        res = psram_alloc(n * sz);
        if (res) memset(res, 0, n * sz);
    }
    if (!res) return NULL;
    list_push_back(((cmd_ctx_t*)ctx)->pallocs, res);
    return res;
}

void* __realloc2(void* ctx, void* p, size_t sz) {
    if (!p) return __malloc2(ctx, sz);
    if (!ctx || !((cmd_ctx_t*)ctx)->pallocs)
        return pvPortRealloc(p, sz);
    list_t* pa = ((cmd_ctx_t*)ctx)->pallocs;
    node_t* n = list_lookup(pa, p);
    bool is_psram = ((uintptr_t)p >= PSRAM_BASE && (uintptr_t)p < PSRAM_END);
    void* res;
    if (is_psram) {
        /* PSRAM has no realloc — allocate new, copy, free old.
         * Read old block size from PSRAM allocator header. */
        size_t old_sz = *(size_t *)((uint8_t *)p - sizeof(size_t) * 2);
        size_t copy_sz = old_sz < sz ? old_sz : sz;
        res = pvPortMalloc(sz);
        if (!res) res = psram_alloc(sz);
        if (!res) return NULL;
        memcpy(res, p, copy_sz);
        psram_free(p);
    } else {
        res = pvPortRealloc(p, sz);
        if (!res && psram_is_available()) {
            /* pvPortRealloc failed — try PSRAM.
             * Cannot easily get old block size from heap_4, so copy sz
             * bytes (SRAM is fully readable, so no fault risk). */
            res = psram_alloc(sz);
            if (!res) return NULL;
            memcpy(res, p, sz);
            vPortFree(p);
        }
        if (!res) return NULL;
    }
    if (!n) {
        list_push_back(((cmd_ctx_t*)ctx)->pallocs, res);
    } else {
        n->data = res;
    }
    return res;
}

void __free2(void* ctx, void* p) {
    if (!ctx || !((cmd_ctx_t*)ctx)->pallocs) {
        psram_free(p);
        return;
    }
    list_t* pa = ((cmd_ctx_t*)ctx)->pallocs;
    node_t* n = list_lookup(pa, p);
    if (!n) {
        psram_free(p);
    } else {
        list_erase_node(pa, n);
    }
}

void __free_ctx(void* ctx) {
    if (!ctx)
        return;
    cmd_ctx_t* c = (cmd_ctx_t*)ctx;
    if (c->pallocs) {
        printf("[__free_ctx] pallocs=%p size=%u\n", c->pallocs, (unsigned)c->pallocs->size);
        delete_list(c->pallocs);
        c->pallocs = 0;
        printf("[__free_ctx] delete_list done\n");
    }
    if (c->pid) {
        pids->p[c->pid] = 0;
        if (c->pid > 1) {
            for (size_t i = 0; i < pids->size; ++i) {
                cmd_ctx_t* c2 = (cmd_ctx_t*)pids->p[i];
                if (!c2) continue;
                if (c2->ppid == c->pid) {
                    c2->parent_task = 0; // like old "detached" style
                    c2->ppid = 1; // assign to init proc
                }
            }
        }
    }
    printf("[__free_ctx] done\n");
}

void* __malloc(size_t sz) {
    return __malloc2(get_cmd_ctx(), sz);
}

void* __calloc(size_t n, size_t sz) {
    return __calloc2(get_cmd_ctx(), n, sz);
}

void* __realloc(void* p, size_t sz) {
    return __realloc2(get_cmd_ctx(), p, sz);
}

void __free(void* p) {
    return __free2(get_cmd_ctx(), p);
}

char* __copy_str(const char* s) {
    char* res = (char*)__malloc(strlen(s) + 1);
    strcpy(res, s);
    return res;
}
