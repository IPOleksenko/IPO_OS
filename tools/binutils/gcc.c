#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    char **items;
    size_t count;
    size_t cap;
} str_list_t;

static void str_list_init(str_list_t *l) {
    l->items = NULL;
    l->count = 0;
    l->cap = 0;
}

static void str_list_push(str_list_t *l, const char *item) {
    if (l->count >= l->cap) {
        size_t new_cap = (l->cap == 0) ? 16 : l->cap * 2;
        char **new_items = (char **)realloc(l->items, new_cap * sizeof(char *));
        if (!new_items) {
            fprintf(stderr, "gcc: out of memory\n");
            exit(1);
        }
        l->items = new_items;
        l->cap = new_cap;
    }
    l->items[l->count++] = strdup(item);
}

static void str_list_free(str_list_t *l) {
    for (size_t i = 0; i < l->count; i++) {
        free(l->items[i]);
    }
    free(l->items);
    l->items = NULL;
    l->count = l->cap = 0;
}

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} dyn_str_t;

static void dyn_str_init(dyn_str_t *ds) {
    ds->data = NULL;
    ds->len = 0;
    ds->cap = 0;
}

static void dyn_str_append(dyn_str_t *ds, const char *str) {
    if (!str) return;
    size_t slen = strlen(str);
    if (ds->len + slen + 1 > ds->cap) {
        size_t new_cap = ds->cap ? ds->cap * 2 : 256;
        while (ds->len + slen + 1 > new_cap) new_cap *= 2;
        char *new_data = (char *)realloc(ds->data, new_cap);
        if (!new_data) {
            fprintf(stderr, "gcc: out of memory\n");
            exit(1);
        }
        ds->data = new_data;
        ds->cap = new_cap;
    }
    memcpy(ds->data + ds->len, str, slen);
    ds->len += slen;
    ds->data[ds->len] = '\0';
}

static void dyn_str_free(dyn_str_t *ds) {
    free(ds->data);
    ds->data = NULL;
    ds->len = ds->cap = 0;
}

static int flag_compile_only = 0;   // -c
static int flag_preprocess_only = 0;// -E
static int flag_asm_only = 0;       // -S
static int flag_verbose = 0;        // -v
static int flag_nostdlib = 0;
static int flag_nostartfiles = 0;
static int is_cxx = 0;

static const char *outfile = NULL;

static void print_version(void) {
    printf("%s (IPO_OS GCC) 13.2.0\n", is_cxx ? "g++" : "gcc");
    printf("Copyright (C) 2026 Free Software Foundation, Inc.\n");
    printf("This is free software; see the source for copying conditions.  There is NO\n");
    printf("warranty; not even for MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.\n");
    printf("Target: i386-pc-ipo_os\n");
    printf("Thread model: single\n");
}

static void print_verbose_info(void) {
    printf("Using built-in specs.\n");
    printf("COLLECT_GCC=%s\n", is_cxx ? "g++" : "gcc");
    printf("Target: i386-pc-ipo_os\n");
    printf("Configured with: --target=i386-pc-ipo_os --enable-languages=c,c++,objc,fortran,ada\n");
    printf("Thread model: single\n");
    printf("gcc version 13.2.0 (IPO_OS)\n");
}

static char *replace_extension(const char *path, const char *new_ext) {
    char *res = (char *)malloc(strlen(path) + strlen(new_ext) + 2);
    strcpy(res, path);
    char *dot = strrchr(res, '.');
    if (dot) *dot = '\0';
    strcat(res, new_ext);
    return res;
}

int main(int argc, char **argv) {
    char *prog = strrchr(argv[0], '/');
    prog = prog ? prog + 1 : argv[0];
    if (strstr(prog, "g++") != NULL || strstr(prog, "c++") != NULL) {
        is_cxx = 1;
    }

    str_list_t in_files, include_dirs, lib_dirs, libs, cflags, obj_files;
    str_list_init(&in_files);
    str_list_init(&include_dirs);
    str_list_init(&lib_dirs);
    str_list_init(&libs);
    str_list_init(&cflags);
    str_list_init(&obj_files);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            flag_verbose = 1;
        } else if (strcmp(argv[i], "--version") == 0) {
            print_version();
            return 0;
        } else if (strcmp(argv[i], "-c") == 0) {
            flag_compile_only = 1;
        } else if (strcmp(argv[i], "-E") == 0) {
            flag_preprocess_only = 1;
        } else if (strcmp(argv[i], "-S") == 0) {
            flag_asm_only = 1;
        } else if (strcmp(argv[i], "-nostdlib") == 0) {
            flag_nostdlib = 1;
        } else if (strcmp(argv[i], "-nostartfiles") == 0) {
            flag_nostartfiles = 1;
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            outfile = argv[++i];
        } else if (strncmp(argv[i], "-I", 2) == 0) {
            str_list_push(&include_dirs, argv[i]);
        } else if (strncmp(argv[i], "-L", 2) == 0) {
            str_list_push(&lib_dirs, argv[i]);
        } else if (strncmp(argv[i], "-l", 2) == 0) {
            str_list_push(&libs, argv[i]);
        } else if (argv[i][0] == '-') {
            str_list_push(&cflags, argv[i]);
        } else {
            str_list_push(&in_files, argv[i]);
        }
    }

    if (flag_verbose && in_files.count == 0) {
        print_verbose_info();
        return 0;
    }

    if (in_files.count == 0) {
        fprintf(stderr, "%s: fatal error: no input files\ncompilation terminated.\n", is_cxx ? "g++" : "gcc");
        return 1;
    }

    // Default include directories
    dyn_str_t inc_str;
    dyn_str_init(&inc_str);
    dyn_str_append(&inc_str, "-I/usr/include -I/include");
    if (is_cxx) {
        dyn_str_append(&inc_str, " -I/usr/include/c++ -I/include/c++");
    }
    for (size_t i = 0; i < include_dirs.count; i++) {
        dyn_str_append(&inc_str, " ");
        dyn_str_append(&inc_str, include_dirs.items[i]);
    }

    for (size_t f = 0; f < in_files.count; f++) {
        const char *in = in_files.items[f];
        const char *ext = strrchr(in, '.');

        if (ext && (strcmp(ext, ".o") == 0 || strcmp(ext, ".a") == 0)) {
            str_list_push(&obj_files, in);
            continue;
        }

        char *target_obj = (flag_compile_only && outfile) ? (char *)outfile : replace_extension(in, ".o");

        dyn_str_t cmd;
        dyn_str_init(&cmd);

        if (ext && (strcmp(ext, ".s") == 0 || strcmp(ext, ".S") == 0 || strcmp(ext, ".asm") == 0)) {
            // Assembly file
            dyn_str_append(&cmd, "as ");
            dyn_str_append(&cmd, in);
            dyn_str_append(&cmd, " -o ");
            dyn_str_append(&cmd, target_obj);

            if (flag_verbose) printf("%s\n", cmd.data);
            int ret = system(cmd.data);
            dyn_str_free(&cmd);
            if (ret != 0) return ret;
        } else {
            // C or C++ source file
            dyn_str_append(&cmd, "tcc -c ");
            dyn_str_append(&cmd, inc_str.data);
            dyn_str_append(&cmd, " ");
            dyn_str_append(&cmd, in);
            dyn_str_append(&cmd, " -o ");
            dyn_str_append(&cmd, target_obj);

            if (flag_verbose) printf("%s\n", cmd.data);
            int ret = system(cmd.data);
            if (ret != 0) {
                // Try /usr/bin/tcc or /app/tcc
                dyn_str_t fallback;
                dyn_str_init(&fallback);
                dyn_str_append(&fallback, "/app/tcc -c ");
                dyn_str_append(&fallback, inc_str.data);
                dyn_str_append(&fallback, " ");
                dyn_str_append(&fallback, in);
                dyn_str_append(&fallback, " -o ");
                dyn_str_append(&fallback, target_obj);
                ret = system(fallback.data);
                dyn_str_free(&fallback);
                if (ret != 0) {
                    dyn_str_free(&cmd);
                    return ret;
                }
            }
            dyn_str_free(&cmd);
        }

        str_list_push(&obj_files, target_obj);
    }
    dyn_str_free(&inc_str);

    if (flag_compile_only) {
        return 0;
    }

    // Linking stage
    const char *final_out = outfile ? outfile : "a.out";
    dyn_str_t link_cmd;
    dyn_str_init(&link_cmd);
    dyn_str_append(&link_cmd, "ld -m elf_i386 -o ");
    dyn_str_append(&link_cmd, final_out);

    if (!flag_nostdlib && !flag_nostartfiles) {
        FILE *f = fopen("/lib/crt0.o", "rb");
        if (f) {
            fclose(f);
            dyn_str_append(&link_cmd, " /lib/crt0.o /lib/crti.o");
        } else {
            dyn_str_append(&link_cmd, " /usr/lib/crt0.o /usr/lib/crti.o");
        }
    }

    for (size_t i = 0; i < obj_files.count; i++) {
        dyn_str_append(&link_cmd, " ");
        dyn_str_append(&link_cmd, obj_files.items[i]);
    }

    dyn_str_append(&link_cmd, " -L/lib -L/usr/lib");
    for (size_t i = 0; i < lib_dirs.count; i++) {
        dyn_str_append(&link_cmd, " ");
        dyn_str_append(&link_cmd, lib_dirs.items[i]);
    }

    if (is_cxx) {
        dyn_str_append(&link_cmd, " -lstdc++");
    }

    for (size_t i = 0; i < libs.count; i++) {
        dyn_str_append(&link_cmd, " ");
        dyn_str_append(&link_cmd, libs.items[i]);
    }

    if (!flag_nostdlib) {
        dyn_str_append(&link_cmd, " -lc -lm -lgcc");
    }

    if (!flag_nostdlib && !flag_nostartfiles) {
        FILE *f = fopen("/lib/crtn.o", "rb");
        if (f) {
            fclose(f);
            dyn_str_append(&link_cmd, " /lib/crtn.o");
        } else {
            dyn_str_append(&link_cmd, " /usr/lib/crtn.o");
        }
    }

    if (flag_verbose) printf("%s\n", link_cmd.data);
    int ret = system(link_cmd.data);
    if (ret != 0) {
        // Fallback to /usr/bin/ld
        dyn_str_t fallback_link;
        dyn_str_init(&fallback_link);
        dyn_str_append(&fallback_link, "/usr/bin/");
        dyn_str_append(&fallback_link, link_cmd.data);
        ret = system(fallback_link.data);
        dyn_str_free(&fallback_link);
    }
    dyn_str_free(&link_cmd);

    str_list_free(&in_files);
    str_list_free(&include_dirs);
    str_list_free(&lib_dirs);
    str_list_free(&libs);
    str_list_free(&cflags);
    str_list_free(&obj_files);

    return ret;
}
