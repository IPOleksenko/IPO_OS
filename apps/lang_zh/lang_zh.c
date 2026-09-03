#include <stdio.h>
#include <syscall.h>
#include <driver/input/keymap/dynamic_keymap.h>

static const keymap_entry_t zh_keymap[] = {
    { KEY_1, "一", "!" }, { KEY_2, "二", "@" }, { KEY_3, "三", "#" },
    { KEY_4, "四", "$" }, { KEY_5, "五", "%" }, { KEY_6, "六", "^" },
    { KEY_7, "七", "&" }, { KEY_8, "八", "*" }, { KEY_9, "九", "(" },
    { KEY_0, "十", ")" }, { KEY_MINUS, "-", "_" }, { KEY_EQUAL, "=", "+" },
    { KEY_Q, "七", "期" }, { KEY_W, "我", "们" }, { KEY_E, "二", "儿" },
    { KEY_R, "人", "日" }, { KEY_T, "他", "她" }, { KEY_Y, "一", "有" },
    { KEY_U, "你", "您" }, { KEY_I, "在", "这" }, { KEY_O, "和", "好" },
    { KEY_P, "平", "朋" }, { KEY_A, "啊", "爱" }, { KEY_S, "是", "上" },
    { KEY_D, "的", "大" }, { KEY_F, "发", "分" }, { KEY_G, "个", "国" },
    { KEY_H, "好", "很" }, { KEY_J, "见", "家" }, { KEY_K, "可", "看" },
    { KEY_L, "了", "来" }, { KEY_Z, "中", "子" }, { KEY_X, "小", "想" },
    { KEY_C, "出", "吃" }, { KEY_V, "为", "问" }, { KEY_B, "不", "把" },
    { KEY_N, "那", "年" }, { KEY_M, "么", "门" }, { KEY_SPACE, " ", " " }
};

int main(int argc, char **argv) {
    if (argc > 1 && argv && argv[1]) {
        if (strcmp(argv[1], "off") == 0 || strcmp(argv[1], "disable") == 0 ||
            strcmp(argv[1], "-d") == 0) {
            int res = ipo_keymap_disable("Chinese (中文汉字)");
            if (res == 0) {
                printf("Chinese layout disabled.\n");
            } else {
                printf("Failed to disable Chinese layout.\n");
            }
            return res;
        }
        if (strcmp(argv[1], "rm") == 0 || strcmp(argv[1], "remove") == 0) {
            int res = ipo_keymap_remove("Chinese (中文汉字)");
            if (res == 0) {
                printf("Chinese layout removed.\n");
            } else {
                printf("Failed to remove Chinese layout.\n");
            }
            return res;
        }
    }
    const char *font_path = (argc > 1 && argv && argv[1]) ? argv[1] : "/system/fonts.bin";
    ipo_font_load_cyrillic(font_path);
    return ipo_keymap_set("Chinese (中文汉字)", zh_keymap, sizeof(zh_keymap) / sizeof(zh_keymap[0]));
}
