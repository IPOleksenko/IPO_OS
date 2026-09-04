#include <stdio.h>
#include <string.h>
#include <syscall.h>

static void print_usage(void) {
    printf("Usage: setfont [options] [font_file]\n");
    printf("Manage and switch system text console fonts.\n\n");
    printf("Options / Commands:\n");
    printf("  setfont              Display active font and list available fonts\n");
    printf("  setfont <name/path>  Switch system font (e.g. terminus, bold, default)\n");
    printf("  setfont default      Switch back to default system font\n");
    printf("  setfont -r           Reset system font\n");
    printf("  setfont -h, --help   Show this help message\n\n");
    printf("Known fonts in /fonts:\n");
    printf("  default   - Standard console font (Fixed16 + Hanzi)\n");
    printf("  terminus  - Terminus clean console font\n");
    printf("  bold      - Terminus Bold VGA font\n");
}

int main(int argc, char **argv) {
    char current_name[64];
    uint32_t glyph_count = 0;
    current_name[0] = '\0';
    ipo_font_get_info(current_name, sizeof(current_name), &glyph_count);

    if (argc < 2) {
        printf("Current system font: %s (%u glyphs)\n\n",
               current_name[0] ? current_name : "default", glyph_count);
        printf("Available system fonts in /fonts:\n");
        printf("  - /fonts/default.fnt   (Fixed16 + Hanzi)\n");
        printf("  - /fonts/terminus.fnt  (Terminus regular)\n");
        printf("  - /fonts/bold.fnt      (Terminus bold)\n\n");
        printf("Usage:\n");
        printf("  setfont terminus\n");
        printf("  setfont bold\n");
        printf("  setfont default\n");
        return 0;
    }

    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        print_usage();
        return 0;
    }

    const char *target = argv[1];
    char resolved_path[128];

    if (strcmp(target, "default") == 0 || strcmp(target, "-r") == 0 || strcmp(target, "--reset") == 0) {
        strncpy(resolved_path, "/fonts/default.fnt", sizeof(resolved_path) - 1);
    } else if (strcmp(target, "terminus") == 0) {
        strncpy(resolved_path, "/fonts/terminus.fnt", sizeof(resolved_path) - 1);
    } else if (strcmp(target, "bold") == 0) {
        strncpy(resolved_path, "/fonts/bold.fnt", sizeof(resolved_path) - 1);
    } else if (target[0] == '/') {
        strncpy(resolved_path, target, sizeof(resolved_path) - 1);
    } else {
        snprintf(resolved_path, sizeof(resolved_path), "/fonts/%s.fnt", target);
    }
    resolved_path[sizeof(resolved_path) - 1] = '\0';

    printf("Loading system font from: %s...\n", resolved_path);

    int res = ipo_font_load(resolved_path);
    if (res != 0) {
        /* If .fnt failed and didn't have leading /fonts/, try exact path */
        if (target[0] != '/') {
            snprintf(resolved_path, sizeof(resolved_path), "/fonts/%s", target);
            res = ipo_font_load(resolved_path);
        }
    }

    if (res != 0) {
        printf("Error: Failed to load font '%s' (code %d)\n", target, res);
        return 1;
    }

    /* Query updated info */
    glyph_count = 0;
    current_name[0] = '\0';
    ipo_font_get_info(current_name, sizeof(current_name), &glyph_count);

    printf("Success! System font updated.\n");
    printf("Active Font: %s (%u glyphs)\n",
           current_name[0] ? current_name : resolved_path, glyph_count);
    printf("Preview: ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz 0123456789\n");
    return 0;
}

