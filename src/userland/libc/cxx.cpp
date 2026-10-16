#include <c++/iostream>
#include <stdlib.h>
#include <stdio.h>

namespace std {
    ostream cout(stdout);
    ostream cerr(stderr);
    istream cin(stdin);
}

void *operator new(size_t size) {
    void *p = malloc(size);
    return p;
}

void *operator new[](size_t size) {
    void *p = malloc(size);
    return p;
}

void operator delete(void *p) noexcept {
    free(p);
}

void operator delete[](void *p) noexcept {
    free(p);
}

void operator delete(void *p, size_t size) noexcept {
    (void)size;
    free(p);
}

void operator delete[](void *p, size_t size) noexcept {
    (void)size;
    free(p);
}

extern "C" void __cxa_pure_virtual() {
    fprintf(stderr, "Pure virtual function called!\n");
    abort();
}

extern "C" {
    void *__dso_handle = (void *)&__dso_handle;

    struct cxa_entry_t {
        void (*func)(void *);
        void *arg;
        void *dso_handle;
    };

    static struct cxa_entry_t *cxa_list = 0;
    static size_t cxa_count = 0;
    static size_t cxa_cap = 0;

    int __cxa_atexit(void (*func)(void *), void *arg, void *dso_handle) {
        if (!func) return -1;
        if (cxa_count >= cxa_cap) {
            size_t new_cap = (cxa_cap == 0) ? 16 : cxa_cap * 2;
            struct cxa_entry_t *new_list = (struct cxa_entry_t *)realloc(cxa_list, new_cap * sizeof(struct cxa_entry_t));
            if (!new_list) return -1;
            cxa_list = new_list;
            cxa_cap = new_cap;
        }
        cxa_list[cxa_count].func = func;
        cxa_list[cxa_count].arg = arg;
        cxa_list[cxa_count].dso_handle = dso_handle;
        cxa_count++;
        return 0;
    }

    void __cxa_finalize(void *dso_handle) {
        for (int i = (int)cxa_count - 1; i >= 0; i--) {
            if (cxa_list[i].func != 0) {
                if (dso_handle == 0 || cxa_list[i].dso_handle == dso_handle) {
                    void (*f)(void *) = cxa_list[i].func;
                    void *arg = cxa_list[i].arg;
                    cxa_list[i].func = 0;
                    f(arg);
                }
            }
        }
        if (dso_handle == 0 && cxa_list != 0) {
            free(cxa_list);
            cxa_list = 0;
            cxa_count = 0;
            cxa_cap = 0;
        }
    }

    int __gxx_personality_v0(int version, int actions, uint64_t exception_class,
                             void *exception_object, void *context) {
        (void)version; (void)actions; (void)exception_class; (void)exception_object; (void)context;
        return 0;
    }

    void _Unwind_Resume(void *exception) {
        (void)exception;
        abort();
    }
}
