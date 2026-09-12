#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} dyn_str_t;

static void dyn_str_init(dyn_str_t *ds) {
    ds->data = NULL;
    ds->len = ds->cap = 0;
}

static void dyn_str_append(dyn_str_t *ds, const char *str) {
    if (!str) return;
    size_t slen = strlen(str);
    if (ds->len + slen + 1 > ds->cap) {
        size_t new_cap = ds->cap ? ds->cap * 2 : 256;
        while (ds->len + slen + 1 > new_cap) new_cap *= 2;
        char *new_data = (char *)realloc(ds->data, new_cap);
        if (!new_data) {
            fprintf(stderr, "as: out of memory\n");
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

static void print_version(void) {
    printf("GNU assembler (IPO_OS Binutils) 2.42\n");
    printf("Copyright (C) 2026 Free Software Foundation, Inc.\n");
    printf("This program is free software; you may redistribute it under the terms of the GNU GPL.\n");
    printf("This assembler was configured for a target of `i386-pc-ipo_os'.\n");
}

int main(int argc, char **argv) {
    const char *infile = NULL;
    const char *outfile = "a.out";
    dyn_str_t extra_args;
    dyn_str_init(&extra_args);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            print_version();
            dyn_str_free(&extra_args);
            return 0;
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            outfile = argv[++i];
        } else if (strcmp(argv[i], "-m32") == 0 || strcmp(argv[i], "--32") == 0) {
            // target 32-bit
        } else if (strncmp(argv[i], "-I", 2) == 0) {
            dyn_str_append(&extra_args, " ");
            dyn_str_append(&extra_args, argv[i]);
        } else if (strcmp(argv[i], "-g") == 0 || strcmp(argv[i], "-g0") == 0) {
            // debug info flag
        } else if (argv[i][0] == '-') {
            // ignore other flags
        } else {
            infile = argv[i];
        }
    }

    if (!infile) {
        fprintf(stderr, "as: no input files\n");
        dyn_str_free(&extra_args);
        return 1;
    }

    // Call nasm to assemble into elf32
    dyn_str_t cmd;
    dyn_str_init(&cmd);
    dyn_str_append(&cmd, "nasm -f elf32");
    if (extra_args.data) dyn_str_append(&cmd, extra_args.data);
    dyn_str_append(&cmd, " ");
    dyn_str_append(&cmd, infile);
    dyn_str_append(&cmd, " -o ");
    dyn_str_append(&cmd, outfile);

    int ret = system(cmd.data);
    if (ret != 0) {
        // If nasm failed, try /usr/bin/nasm directly
        dyn_str_t fallback;
        dyn_str_init(&fallback);
        dyn_str_append(&fallback, "/usr/bin/");
        dyn_str_append(&fallback, cmd.data);
        ret = system(fallback.data);
        dyn_str_free(&fallback);
    }

    dyn_str_free(&cmd);
    dyn_str_free(&extra_args);
    return ret;
}
