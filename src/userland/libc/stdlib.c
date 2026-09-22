#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <syscall.h>
#include <stdio.h>
#include <stdbool.h>

/* -------------------------------------------------------------
 * Memory allocator over sbrk
 * ------------------------------------------------------------- */

#define BLOCK_MAGIC 0x4D414C43 /* "MALC" */

typedef struct block_hdr {
    size_t size;               /* Payload size in bytes */
    uint32_t magic;
    int is_free;
    struct block_hdr *next;
    struct block_hdr *prev;
} block_hdr_t;

#define HDR_SIZE sizeof(block_hdr_t)
#define ALIGN8(s) (((s) + 7u) & ~7u)

static block_hdr_t *heap_head = NULL;

void *malloc(size_t size) {
    if (size == 0) return NULL;
    size = ALIGN8(size);

    /* Search free list */
    block_hdr_t *curr = heap_head;
    while (curr) {
        if (curr->magic == BLOCK_MAGIC && curr->is_free && curr->size >= size) {
            curr->is_free = 0;
            return (void *)((uint8_t *)curr + HDR_SIZE);
        }
        curr = curr->next;
    }

    /* Request memory from kernel */
    size_t alloc_size = ALIGN8(size + HDR_SIZE);
    if (alloc_size < 65536) {
        alloc_size = 65536; /* Allocate in 64KB chunks to reduce syscall overhead */
    }

    void *p = sbrk((intptr_t)alloc_size);
    if (p == (void *)-1 || p == NULL) {
        errno = ENOMEM;
        return NULL;
    }

    block_hdr_t *new_block = (block_hdr_t *)p;
    new_block->magic = BLOCK_MAGIC;
    new_block->size = size;
    new_block->is_free = 0;
    new_block->next = NULL;
    new_block->prev = NULL;

    /* If chunk is bigger than requested, create a free remainder block */
    size_t leftover = alloc_size - (size + HDR_SIZE);
    if (leftover >= HDR_SIZE + 16) {
        block_hdr_t *rem = (block_hdr_t *)((uint8_t *)p + HDR_SIZE + size);
        rem->magic = BLOCK_MAGIC;
        rem->size = leftover - HDR_SIZE;
        rem->is_free = 1;
        rem->next = NULL;
        rem->prev = new_block;
        new_block->next = rem;
    }

    /* Append to heap list */
    if (!heap_head) {
        heap_head = new_block;
    } else {
        block_hdr_t *tail = heap_head;
        while (tail->next) tail = tail->next;
        tail->next = new_block;
        new_block->prev = tail;
    }

    return (void *)((uint8_t *)new_block + HDR_SIZE);
}

void free(void *ptr) {
    if (!ptr) return;
    block_hdr_t *block = (block_hdr_t *)((uint8_t *)ptr - HDR_SIZE);
    if (block->magic != BLOCK_MAGIC) return;
    block->is_free = 1;

    /* Coalesce with next if free */
    if (block->next && block->next->magic == BLOCK_MAGIC && block->next->is_free) {
        block->size += HDR_SIZE + block->next->size;
        block->next = block->next->next;
        if (block->next) block->next->prev = block;
    }

    /* Coalesce with prev if free */
    if (block->prev && block->prev->magic == BLOCK_MAGIC && block->prev->is_free) {
        block->prev->size += HDR_SIZE + block->size;
        block->prev->next = block->next;
        if (block->next) block->next->prev = block->prev;
    }
}

void *calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *realloc(void *ptr, size_t new_size) {
    if (!ptr) return malloc(new_size);
    if (new_size == 0) {
        free(ptr);
        return NULL;
    }
    block_hdr_t *block = (block_hdr_t *)((uint8_t *)ptr - HDR_SIZE);
    if (block->magic != BLOCK_MAGIC) return NULL;

    if (block->size >= new_size) {
        return ptr;
    }

    void *new_ptr = malloc(new_size);
    if (!new_ptr) return NULL;
    memcpy(new_ptr, ptr, block->size);
    free(ptr);
    return new_ptr;
}

/* -------------------------------------------------------------
 * Process Lifecycle & atexit
 * ------------------------------------------------------------- */

static void (**atexit_handlers)(void) = NULL;
static size_t atexit_count = 0;
static size_t atexit_cap = 0;

int atexit(void (*func)(void)) {
    if (!func) return -1;
    if (atexit_count >= atexit_cap) {
        size_t new_cap = (atexit_cap == 0) ? 16 : atexit_cap * 2;
        void (**new_handlers)(void) = (void (**)(void))realloc(atexit_handlers, new_cap * sizeof(void (*)(void)));
        if (!new_handlers) return -1;
        atexit_handlers = new_handlers;
        atexit_cap = new_cap;
    }
    atexit_handlers[atexit_count++] = func;
    return 0;
}

void __libc_fini_array(void);

void exit(int status) {
    /* Run atexit handlers in LIFO order */
    for (int i = (int)atexit_count - 1; i >= 0; i--) {
        if (atexit_handlers[i]) {
            atexit_handlers[i]();
        }
    }
    if (atexit_handlers) {
        free(atexit_handlers);
        atexit_handlers = NULL;
        atexit_count = atexit_cap = 0;
    }
    /* Run global destructors */
    __libc_fini_array();
    /* Flush stdio streams */
    fflush(NULL);
    /* Exit via kernel */
    ipo_exit(status);
    __builtin_unreachable();
}

void _exit(int status) {
    ipo_exit(status);
    __builtin_unreachable();
}

void abort(void) {
    fputs("Aborted\n", stderr);
    exit(134);
}

void __assert_fail(const char *assertion, const char *file, unsigned int line, const char *function) {
    fprintf(stderr, "Assertion failed: %s (%s: %s: %u)\n",
            assertion ? assertion : "unknown",
            file ? file : "unknown",
            function ? function : "unknown",
            line);
    abort();
}

/* -------------------------------------------------------------
 * Environment variables (Dynamically expandable, zero artificial limits)
 * ------------------------------------------------------------- */

static char *env[] = {
    NULL
};
extern char **environ;

static bool env_is_allocated = false;
static size_t env_cap = 0;

char *getenv(const char *name) {
    if (!name || !*name) return NULL;
    char **cur_env = environ ? environ : env;
    size_t nlen = strlen(name);
    for (char **ep = cur_env; *ep != NULL; ep++) {
        if (strncmp(*ep, name, nlen) == 0 && (*ep)[nlen] == '=') {
            return *ep + nlen + 1;
        }
    }
    return NULL;
}

int setenv(const char *name, const char *value, int overwrite) {
    if (!name || !*name || strchr(name, '=') != NULL) {
        errno = EINVAL;
        return -1;
    }

    if (!value) value = "";
    size_t nlen = strlen(name);
    size_t vlen = strlen(value);
    size_t entry_len = nlen + 1 + vlen + 1;

    char **cur_env = environ ? environ : env;
    size_t count = 0;
    int found_idx = -1;

    while (cur_env[count] != NULL) {
        if (strncmp(cur_env[count], name, nlen) == 0 && cur_env[count][nlen] == '=') {
            found_idx = (int)count;
        }
        count++;
    }

    /* If variable exists and we are not allowed to overwrite, return success */
    if (found_idx >= 0 && !overwrite) {
        return 0;
    }

    /* If variable exists and we can overwrite */
    if (found_idx >= 0) {
        char *new_entry = (char *)malloc(entry_len);
        if (!new_entry) {
            errno = ENOMEM;
            return -1;
        }
        memcpy(new_entry, name, nlen);
        new_entry[nlen] = '=';
        memcpy(new_entry + nlen + 1, value, vlen + 1);

        if (env_is_allocated) {
            free(cur_env[found_idx]);
        }
        cur_env[found_idx] = new_entry;
        return 0;
    }

    /* Variable does not exist: need to append */
    char *new_entry = (char *)malloc(entry_len);
    if (!new_entry) {
        errno = ENOMEM;
        return -1;
    }
    memcpy(new_entry, name, nlen);
    new_entry[nlen] = '=';
    memcpy(new_entry + nlen + 1, value, vlen + 1);

    if (!env_is_allocated) {
        size_t new_cap = count + 8;
        char **new_env = (char **)malloc(new_cap * sizeof(char *));
        if (!new_env) {
            free(new_entry);
            errno = ENOMEM;
            return -1;
        }
        for (size_t i = 0; i < count; i++) {
            new_env[i] = cur_env[i];
        }
        new_env[count] = new_entry;
        new_env[count + 1] = NULL;
        environ = new_env;
        env_is_allocated = true;
        env_cap = new_cap;
    } else {
        if (count + 2 > env_cap) {
            size_t new_cap = env_cap ? env_cap * 2 : (count + 8);
            char **new_env = (char **)realloc(environ, new_cap * sizeof(char *));
            if (!new_env) {
                free(new_entry);
                errno = ENOMEM;
                return -1;
            }
            environ = new_env;
            env_cap = new_cap;
        }
        environ[count] = new_entry;
        environ[count + 1] = NULL;
    }

    return 0;
}

int putenv(char *string) {
    if (!string || !*string) {
        errno = EINVAL;
        return -1;
    }
    char *eq = strchr(string, '=');
    if (!eq) {
        return unsetenv(string);
    }
    size_t nlen = (size_t)(eq - string);
    char *name_buf = (char *)malloc(nlen + 1);
    if (!name_buf) {
        errno = ENOMEM;
        return -1;
    }
    memcpy(name_buf, string, nlen);
    name_buf[nlen] = '\0';
    int res = setenv(name_buf, eq + 1, 1);
    free(name_buf);
    return res;
}

int unsetenv(const char *name) {
    if (!name || !*name || strchr(name, '=') != NULL) {
        errno = EINVAL;
        return -1;
    }

    char **cur_env = environ ? environ : env;
    size_t nlen = strlen(name);
    size_t i = 0;

    while (cur_env[i] != NULL) {
        if (strncmp(cur_env[i], name, nlen) == 0 && cur_env[i][nlen] == '=') {
            if (env_is_allocated) {
                free(cur_env[i]);
            }
            /* Shift remaining entries down */
            size_t j = i;
            while (cur_env[j] != NULL) {
                cur_env[j] = cur_env[j + 1];
                j++;
            }
            /* Don't increment i; check the shifted element at index i */
            continue;
        }
        i++;
    }

    return 0;
}

int clearenv(void) {
    char **cur_env = environ ? environ : env;
    if (env_is_allocated) {
        for (size_t i = 0; cur_env[i] != NULL; i++) {
            free(cur_env[i]);
            cur_env[i] = NULL;
        }
    }
    if (environ) {
        environ[0] = NULL;
    }
    return 0;
}

int system(const char *command) {
    if (!command || !*command) return 0;
    char *buf = strdup(command);
    if (!buf) return -1;

    size_t argv_cap = 16;
    char **argv = (char **)malloc(argv_cap * sizeof(char *));
    if (!argv) {
        free(buf);
        return -1;
    }

    int argc = 0;
    char *token = strtok(buf, " \t\r\n");
    while (token) {
        if ((size_t)argc + 2 >= argv_cap) {
            argv_cap *= 2;
            char **new_argv = (char **)realloc(argv, argv_cap * sizeof(char *));
            if (!new_argv) {
                free(argv);
                free(buf);
                return -1;
            }
            argv = new_argv;
        }
        argv[argc++] = token;
        token = strtok(NULL, " \t\r\n");
    }
    argv[argc] = NULL;
    if (argc == 0) {
        free(argv);
        free(buf);
        return 0;
    }
    int pid = ipo_exec(argv[0], argc, argv);
    int code = (pid < 0) ? -1 : ipo_get_exit_code();
    free(argv);
    free(buf);
    return code;
}

/* -------------------------------------------------------------
 * Conversion functions
 * ------------------------------------------------------------- */

int atoi(const char *nptr) {
    return (int)strtol(nptr, NULL, 10);
}

long atol(const char *nptr) {
    return strtol(nptr, NULL, 10);
}

long long atoll(const char *nptr) {
    return strtoll(nptr, NULL, 10);
}

double atof(const char *nptr) {
    return strtod(nptr, NULL);
}

long strtol(const char *nptr, char **endptr, int base) {
    if (!nptr) return 0;
    const char *s = nptr;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') { s++; }

    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    } else if (base == 0) {
        base = (s[0] == '0') ? 8 : 10;
    }

    long val = 0;
    while (*s) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'z') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') d = *s - 'A' + 10;
        else break;
        if (d >= base) break;
        val = val * base + d;
        s++;
    }
    if (endptr) *endptr = (char *)s;
    return neg ? -val : val;
}

unsigned long strtoul(const char *nptr, char **endptr, int base) {
    return (unsigned long)strtol(nptr, endptr, base);
}

long long strtoll(const char *nptr, char **endptr, int base) {
    return (long long)strtol(nptr, endptr, base);
}

unsigned long long strtoull(const char *nptr, char **endptr, int base) {
    return (unsigned long long)strtol(nptr, endptr, base);
}

double strtod(const char *nptr, char **endptr) {
    if (!nptr) return 0.0;
    const char *s = nptr;
    while (*s == ' ' || *s == '\t' || *s == '\n') s++;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') { s++; }

    double val = 0.0;
    while (*s >= '0' && *s <= '9') {
        val = val * 10.0 + (*s - '0');
        s++;
    }
    if (*s == '.') {
        s++;
        double frac = 0.1;
        while (*s >= '0' && *s <= '9') {
            val += (*s - '0') * frac;
            frac *= 0.1;
            s++;
        }
    }
    if (*s == 'e' || *s == 'E') {
        s++;
        int exp_neg = 0;
        if (*s == '-') { exp_neg = 1; s++; }
        else if (*s == '+') { s++; }
        int exp = 0;
        while (*s >= '0' && *s <= '9') {
            exp = exp * 10 + (*s - '0');
            s++;
        }
        double factor = 1.0;
        for (int i = 0; i < exp; i++) factor *= 10.0;
        if (exp_neg) val /= factor;
        else val *= factor;
    }
    if (endptr) *endptr = (char *)s;
    return neg ? -val : val;
}

float strtof(const char *nptr, char **endptr) {
    return (float)strtod(nptr, endptr);
}

long double strtold(const char *nptr, char **endptr) {
    return (long double)strtod(nptr, endptr);
}

/* -------------------------------------------------------------
 * Math & utilities
 * ------------------------------------------------------------- */

int abs(int j) {
    return (j < 0) ? -j : j;
}

long labs(long j) {
    return (j < 0) ? -j : j;
}

div_t div(int numer, int denom) {
    div_t r;
    r.quot = numer / denom;
    r.rem = numer % denom;
    return r;
}

ldiv_t ldiv(long numer, long denom) {
    ldiv_t r;
    r.quot = numer / denom;
    r.rem = numer % denom;
    return r;
}

static unsigned long rand_next = 1;
int rand(void) {
    rand_next = rand_next * 1103515245 + 12345;
    return (unsigned int)(rand_next / 65536) % 32768;
}

void srand(unsigned int seed) {
    rand_next = seed;
}

void qsort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *)) {
    if (nmemb <= 1 || !base || size == 0) return;
    char *arr = (char *)base;
    for (size_t i = 0; i < nmemb - 1; i++) {
        for (size_t j = i + 1; j < nmemb; j++) {
            if (compar(arr + i * size, arr + j * size) > 0) {
                for (size_t k = 0; k < size; k++) {
                    char tmp = arr[i * size + k];
                    arr[i * size + k] = arr[j * size + k];
                    arr[j * size + k] = tmp;
                }
            }
        }
    }
}

void *bsearch(const void *key, const void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *)) {
    size_t l = 0;
    size_t r = nmemb;
    while (l < r) {
        size_t m = l + (r - l) / 2;
        const void *elem = (const char *)base + m * size;
        int cmp = compar(key, elem);
        if (cmp == 0) return (void *)elem;
        if (cmp < 0) r = m;
        else l = m + 1;
    }
    return NULL;
}

char *realpath(const char *path, char *resolved_path) {
    if (!path) return NULL;
    if (!resolved_path) resolved_path = (char *)malloc(1024);
    if (!resolved_path) return NULL;
    strncpy(resolved_path, path, 1023);
    resolved_path[1023] = '\0';
    return resolved_path;
}
