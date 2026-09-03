#include <stdio.h>
#include <string.h>
#include <syscall.h>
#include <driver/input/keymap/dynamic_keymap.h>

static const keymap_entry_t ua_keymap[] = {
    { KEY_1, "1", "!" }, { KEY_2, "2", "\"" }, { KEY_3, "3", "№" },
    { KEY_4, "4", ";" }, { KEY_5, "5", "%" }, { KEY_6, "6", ":" },
    { KEY_7, "7", "?" }, { KEY_8, "8", "*" }, { KEY_9, "9", "(" },
    { KEY_0, "0", ")" }, { KEY_MINUS, "-", "_" }, { KEY_EQUAL, "=", "+" },
    { KEY_Q, "й", "Й" }, { KEY_W, "ц", "Ц" }, { KEY_E, "у", "У" },
    { KEY_R, "к", "К" }, { KEY_T, "е", "Е" }, { KEY_Y, "н", "Н" },
    { KEY_U, "г", "Г" }, { KEY_I, "ш", "Ш" }, { KEY_O, "щ", "Щ" },
    { KEY_P, "з", "З" }, { KEY_LBRACKET, "х", "Х" }, { KEY_RBRACKET, "ї", "Ї" },
    { KEY_BACKSLASH, "ґ", "Ґ" }, { KEY_A, "ф", "Ф" }, { KEY_S, "і", "І" },
    { KEY_D, "в", "В" }, { KEY_F, "а", "А" }, { KEY_G, "п", "П" },
    { KEY_H, "р", "Р" }, { KEY_J, "о", "О" }, { KEY_K, "л", "Л" },
    { KEY_L, "д", "Д" }, { KEY_SEMICOLON, "ж", "Ж" }, { KEY_QUOTE, "є", "Є" },
    { KEY_GRAVE, "'", "~" }, { KEY_Z, "я", "Я" }, { KEY_X, "ч", "Ч" },
    { KEY_C, "с", "С" }, { KEY_V, "м", "М" }, { KEY_B, "и", "И" },
    { KEY_N, "т", "Т" }, { KEY_M, "ь", "Ь" }, { KEY_COMMA, "б", "Б" },
    { KEY_PERIOD, "ю", "Ю" }, { KEY_SLASH, ".", "," }, { KEY_SPACE, " ", " " }
};

int main(int argc, char **argv) {
    if (argc > 1 && argv && argv[1]) {
        if (strcmp(argv[1], "off") == 0 || strcmp(argv[1], "disable") == 0 ||
            strcmp(argv[1], "-d") == 0) {
            int res = ipo_keymap_disable("Ukrainian (Українська)");
            if (res == 0) {
                printf("Ukrainian layout disabled.\n");
            } else {
                printf("Failed to disable Ukrainian layout.\n");
            }
            return res;
        }
        if (strcmp(argv[1], "rm") == 0 || strcmp(argv[1], "remove") == 0) {
            int res = ipo_keymap_remove("Ukrainian (Українська)");
            if (res == 0) {
                printf("Ukrainian layout removed.\n");
            } else {
                printf("Failed to remove Ukrainian layout.\n");
            }
            return res;
        }
    }
    const char *font_path = (argc > 1 && argv && argv[1]) ? argv[1] : "/system/fonts.bin";
    ipo_font_load_cyrillic(font_path);
    return ipo_keymap_set("Ukrainian (Українська)", ua_keymap, sizeof(ua_keymap) / sizeof(ua_keymap[0]));
}
