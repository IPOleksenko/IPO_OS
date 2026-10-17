#ifndef _IOPORT_H
#define _IOPORT_H

#include <stdint.h>

static inline void outb(uint16_t port, uint8_t value) {
#ifdef __i386__
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
#else
    (void)port; (void)value;
#endif
}

static inline uint8_t inb(uint16_t port) {
#ifdef __i386__
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
#else
    (void)port; return 0;
#endif
}

static inline void outw(uint16_t port, uint16_t value) {
#ifdef __i386__
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
#else
    (void)port; (void)value;
#endif
}

static inline uint16_t inw(uint16_t port) {
#ifdef __i386__
    uint16_t ret;
    __asm__ volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
#else
    (void)port; return 0;
#endif
}

static inline void outl(uint16_t port, uint32_t value) {
#ifdef __i386__
    __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port));
#else
    (void)port; (void)value;
#endif
}

static inline uint32_t inl(uint16_t port) {
#ifdef __i386__
    uint32_t ret;
    __asm__ volatile("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
#else
    (void)port; return 0;
#endif
}

static inline void io_wait(void) {
    outb(0x80, 0);
}

static inline void insw(uint16_t port, void *addr, uint32_t count) {
#ifdef __i386__
    __asm__ volatile("cld; rep insw" : "+D"(addr), "+c"(count) : "d"(port) : "memory");
#else
    (void)port; (void)addr; (void)count;
#endif
}

static inline void outsw(uint16_t port, const void *addr, uint32_t count) {
#ifdef __i386__
    __asm__ volatile("cld; rep outsw" : "+S"(addr), "+c"(count) : "d"(port));
#else
    (void)port; (void)addr; (void)count;
#endif
}

#endif
