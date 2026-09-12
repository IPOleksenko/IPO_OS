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

    int __cxa_atexit(void (*func)(void *), void *arg, void *dso_handle) {
        (void)func; (void)arg; (void)dso_handle;
        return 0;
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
