#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

#define ARMAG  "!<arch>\n"
#define SARMAG 8
#define ARFMAG "`\n"

struct ar_hdr {
    char ar_name[16];
    char ar_date[12];
    char ar_uid[6];
    char ar_gid[6];
    char ar_mode[8];
    char ar_size[10];
    char ar_fmag[2];
};

typedef struct ar_member {
    char name[64];
    uint32_t date;
    uint32_t uid;
    uint32_t gid;
    uint32_t mode;
    uint32_t size;
    uint8_t *data;
    struct ar_member *next;
} ar_member_t;

static ar_member_t *members = NULL;

static void add_member(const char *name, uint32_t date, uint32_t uid, uint32_t gid,
                       uint32_t mode, uint32_t size, uint8_t *data) {
    ar_member_t **curr = &members;
    while (*curr) {
        if (strcmp((*curr)->name, name) == 0) {
            // Replace existing
            free((*curr)->data);
            (*curr)->date = date;
            (*curr)->uid = uid;
            (*curr)->gid = gid;
            (*curr)->mode = mode;
            (*curr)->size = size;
            (*curr)->data = data;
            return;
        }
        curr = &(*curr)->next;
    }
    ar_member_t *m = (ar_member_t *)malloc(sizeof(ar_member_t));
    strncpy(m->name, name, sizeof(m->name) - 1);
    m->name[sizeof(m->name) - 1] = '\0';
    m->date = date;
    m->uid = uid;
    m->gid = gid;
    m->mode = mode;
    m->size = size;
    m->data = data;
    m->next = NULL;
    *curr = m;
}

static int read_archive(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    char magic[SARMAG];
    if (fread(magic, 1, SARMAG, f) != SARMAG || memcmp(magic, ARMAG, SARMAG) != 0) {
        fclose(f);
        return -1;
    }

    struct ar_hdr hdr;
    while (fread(&hdr, 1, sizeof(hdr), f) == sizeof(hdr)) {
        if (memcmp(hdr.ar_fmag, ARFMAG, 2) != 0) break;

        char name[17];
        memcpy(name, hdr.ar_name, 16);
        name[16] = '\0';
        char *slash = strchr(name, '/');
        if (slash) *slash = '\0';
        else {
            char *end = name + strlen(name) - 1;
            while (end >= name && *end == ' ') *end-- = '\0';
        }

        uint32_t size = (uint32_t)strtoul(hdr.ar_size, NULL, 10);
        uint32_t date = (uint32_t)strtoul(hdr.ar_date, NULL, 10);
        uint32_t uid = (uint32_t)strtoul(hdr.ar_uid, NULL, 10);
        uint32_t gid = (uint32_t)strtoul(hdr.ar_gid, NULL, 10);
        uint32_t mode = (uint32_t)strtoul(hdr.ar_mode, NULL, 8);

        uint8_t *data = (uint8_t *)malloc(size);
        if (fread(data, 1, size, f) != size) {
            free(data);
            break;
        }
        if (size & 1) (void)fgetc(f); // skip padding

        // Ignore existing symbol table symbol '/'
        if (strcmp(name, "") != 0 && strcmp(name, "/") != 0 && strcmp(name, "__.SYMDEF") != 0) {
            add_member(name, date, uid, gid, mode, size, data);
        } else {
            free(data);
        }
    }

    fclose(f);
    return 0;
}

static int add_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "ar: %s: No such file\n", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *data = (uint8_t *)malloc(size > 0 ? size : 1);
    if (size > 0 && fread(data, 1, size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return -1;
    }
    fclose(f);

    const char *basename = strrchr(path, '/');
    basename = basename ? basename + 1 : path;

    add_member(basename, (uint32_t)time(NULL), 0, 0, 0644, (uint32_t)size, data);
    return 0;
}

static int write_archive(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "ar: %s: Cannot open for writing\n", path);
        return -1;
    }

    fwrite(ARMAG, 1, SARMAG, f);

    for (ar_member_t *m = members; m != NULL; m = m->next) {
        struct ar_hdr hdr;
        memset(&hdr, ' ', sizeof(hdr));

        char name_slash[17];
        snprintf(name_slash, sizeof(name_slash), "%s/", m->name);
        size_t nlen = strlen(name_slash);
        memcpy(hdr.ar_name, name_slash, nlen > 16 ? 16 : nlen);

        char buf[32];
        snprintf(buf, sizeof(buf), "%-12u", m->date);
        memcpy(hdr.ar_date, buf, 12);
        snprintf(buf, sizeof(buf), "%-6u", m->uid);
        memcpy(hdr.ar_uid, buf, 6);
        snprintf(buf, sizeof(buf), "%-6u", m->gid);
        memcpy(hdr.ar_gid, buf, 6);
        snprintf(buf, sizeof(buf), "%-8o", m->mode);
        memcpy(hdr.ar_mode, buf, 8);
        snprintf(buf, sizeof(buf), "%-10u", m->size);
        memcpy(hdr.ar_size, buf, 10);
        memcpy(hdr.ar_fmag, ARFMAG, 2);

        fwrite(&hdr, 1, sizeof(hdr), f);
        if (m->size > 0) {
            fwrite(m->data, 1, m->size, f);
        }
        if (m->size & 1) {
            fputc('\n', f);
        }
    }

    fclose(f);
    return 0;
}

static void list_archive(void) {
    for (ar_member_t *m = members; m != NULL; m = m->next) {
        printf("%s\n", m->name);
    }
}

static void extract_archive(void) {
    for (ar_member_t *m = members; m != NULL; m = m->next) {
        FILE *f = fopen(m->name, "wb");
        if (f) {
            if (m->size > 0) fwrite(m->data, 1, m->size, f);
            fclose(f);
            printf("x - %s\n", m->name);
        }
    }
}

int main(int argc, char **argv) {
    const char *prog = strrchr(argv[0], '/');
    prog = prog ? prog + 1 : argv[0];

    if (strcmp(prog, "ranlib") == 0) {
        if (argc < 2) {
            fprintf(stderr, "usage: ranlib archive\n");
            return 1;
        }
        read_archive(argv[1]);
        return write_archive(argv[1]);
    }

    if (argc < 3) {
        fprintf(stderr, "usage: %s [rcstvx] archive [files...]\n", prog);
        return 1;
    }

    const char *opts = argv[1];
    if (*opts == '-') opts++;
    const char *arch_path = argv[2];

    read_archive(arch_path);

    if (strchr(opts, 't')) {
        list_archive();
        return 0;
    }

    if (strchr(opts, 'x')) {
        extract_archive();
        return 0;
    }

    if (strchr(opts, 'r') || strchr(opts, 'c')) {
        for (int i = 3; i < argc; i++) {
            if (add_file(argv[i]) != 0) {
                return 1;
            }
        }
        return write_archive(arch_path);
    }

    return 0;
}
