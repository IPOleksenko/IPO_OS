#ifndef _DLFCN_H
#define _DLFCN_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Relocation / Binding flags for dlopen() */
#define RTLD_LAZY    0x0001
#define RTLD_NOW     0x0002
#define RTLD_BINDING_MASK 0x0003

#define RTLD_NOLOAD  0x0004
#define RTLD_DEEPBIND 0x0008

/* Visibility flags */
#define RTLD_LOCAL   0x0000
#define RTLD_GLOBAL  0x0100

/* Special pseudo-handles */
#define RTLD_DEFAULT ((void *)0)
#define RTLD_NEXT    ((void *)-1)

/* Structure filled by dladdr() */
typedef struct {
    const char *dli_fname; /* Pathname of shared object that contains address */
    void       *dli_fbase; /* Base address at which shared object is loaded */
    const char *dli_sname; /* Name of nearest symbol with address <= addr */
    void       *dli_saddr; /* Exact address of symbol named in dli_sname */
} Dl_info;

/**
 * dlopen - Dynamically load an executable library or module from disk.
 *
 * Supported formats:
 *   - 32-bit ELF Relocatable Objects (ET_REL, e.g. compiled via gcc -c / tcc -c)
 *   - 32-bit ELF Shared Objects (ET_DYN, e.g. compiled via gcc -shared -fPIC)
 *
 * Path / Extension agnostic:
 *   Files can have ANY extension (.so, .dll, .bin, .dat, .o) or NO extension at all.
 *   Paths can be absolute ("/applications/my_plugin") or relative ("./plugin.dat").
 *   If filename does not contain '/', search paths (/applications/, /lib/) are probed.
 *
 * @param filename  Path to dynamic module file, or NULL for main process/global symbols.
 * @param flags     RTLD_NOW, RTLD_LAZY, RTLD_GLOBAL, RTLD_LOCAL.
 * @return          Opaque module handle, or NULL on error (check dlerror()).
 */
void *dlopen(const char *filename, int flags);

/**
 * dlsym - Look up an exported symbol by name from a loaded library.
 *
 * @param handle  Module handle returned by dlopen(), or RTLD_DEFAULT.
 * @param symbol  Null-terminated symbol name (function or variable).
 * @return        Pointer to symbol in memory, or NULL if not found.
 */
void *dlsym(void *handle, const char *symbol);

/**
 * dlclose - Decrement module reference count and unload when 0.
 *
 * @param handle  Module handle returned by dlopen().
 * @return        0 on success, non-zero on error.
 */
int dlclose(void *handle);

/**
 * dlerror - Return human-readable string for the most recent dynamic linking error.
 *
 * Once called, the error is cleared (subsequent call returns NULL).
 *
 * @return  Error message string, or NULL if no error has occurred.
 */
char *dlerror(void);

/**
 * dladdr - Translate an address into symbolic information.
 */
int dladdr(const void *addr, Dl_info *info);

/**
 * dlregister_symbol - IPO_OS extension to register host application symbols
 * so dynamic plugins can resolve host functions and callbacks.
 *
 * @param name  Symbol name (e.g. "my_app_callback").
 * @param addr  Pointer to function or data.
 * @return      0 on success, -1 on failure.
 */
int dlregister_symbol(const char *name, void *addr);

#ifdef __cplusplus
}
#endif

#endif /* _DLFCN_H */

