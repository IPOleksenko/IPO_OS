#ifndef _DLFCN_H
#define _DLFCN_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RTLD_LAZY    0x0001
#define RTLD_NOW     0x0002
#define RTLD_BINDING_MASK 0x0003

#define RTLD_NOLOAD  0x0004
#define RTLD_DEEPBIND 0x0008

#define RTLD_LOCAL   0x0000
#define RTLD_GLOBAL  0x0100

#define RTLD_DEFAULT ((void *)0)
#define RTLD_NEXT    ((void *)-1)

typedef struct {
    const char *dli_fname;
    void       *dli_fbase;
    const char *dli_sname;
    void       *dli_saddr;
} Dl_info;

void *dlopen(const char *filename, int flags);
void *dlsym(void *handle, const char *symbol);
int dlclose(void *handle);
char *dlerror(void);
int dladdr(const void *addr, Dl_info *info);
int dlregister_symbol(const char *name, void *addr);

#ifdef __cplusplus
}
#endif

#endif /* _DLFCN_H */

