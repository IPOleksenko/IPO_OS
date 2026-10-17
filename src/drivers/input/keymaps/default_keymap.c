/**
 * Default System Keyboard Layout: English (US)
 * Registered as standard dynamic keymap entries with full scancode dictionary.
 */

#include <driver/input/keymap/dynamic_keymap.h>

const char default_keymap_name[] = "English (US)";

const keymap_entry_t default_keymap_entries[] = {
    { KEY_ESC,        "\x1B", "\x1B" },
    { KEY_1,          "1",    "!"    },
    { KEY_2,          "2",    "@"    },
    { KEY_3,          "3",    "#"    },
    { KEY_4,          "4",    "$"    },
    { KEY_5,          "5",    "%"    },
    { KEY_6,          "6",    "^"    },
    { KEY_7,          "7",    "&"    },
    { KEY_8,          "8",    "*"    },
    { KEY_9,          "9",    "("    },
    { KEY_0,          "0",    ")"    },
    { KEY_MINUS,      "-",    "_"    },
    { KEY_EQUAL,      "=",    "+"    },
    { KEY_BACKSPACE,  "\b",   "\b"   },
    { KEY_TAB,        "\t",   "\t"   },
    { KEY_Q,          "q",    "Q"    },
    { KEY_W,          "w",    "W"    },
    { KEY_E,          "e",    "E"    },
    { KEY_R,          "r",    "R"    },
    { KEY_T,          "t",    "T"    },
    { KEY_Y,          "y",    "Y"    },
    { KEY_U,          "u",    "U"    },
    { KEY_I,          "i",    "I"    },
    { KEY_O,          "o",    "O"    },
    { KEY_P,          "p",    "P"    },
    { KEY_LBRACKET,   "[",    "{"    },
    { KEY_RBRACKET,   "]",    "}"    },
    { KEY_ENTER,      "\n",   "\n"   },
    { KEY_A,          "a",    "A"    },
    { KEY_S,          "s",    "S"    },
    { KEY_D,          "d",    "D"    },
    { KEY_F,          "f",    "F"    },
    { KEY_G,          "g",    "G"    },
    { KEY_H,          "h",    "H"    },
    { KEY_J,          "j",    "J"    },
    { KEY_K,          "k",    "K"    },
    { KEY_L,          "l",    "L"    },
    { KEY_SEMICOLON,  ";",    ":"    },
    { KEY_QUOTE,      "'",    "\""   },
    { KEY_BACKTICK,   "`",    "~"    },
    { KEY_BACKSLASH,  "\\",   "|"    },
    { KEY_Z,          "z",    "Z"    },
    { KEY_X,          "x",    "X"    },
    { KEY_C,          "c",    "C"    },
    { KEY_V,          "v",    "V"    },
    { KEY_B,          "b",    "B"    },
    { KEY_N,          "n",    "N"    },
    { KEY_M,          "m",    "M"    },
    { KEY_COMMA,      ",",    "<"    },
    { KEY_PERIOD,     ".",    ">"    },
    { KEY_SLASH,      "/",    "?"    },
    { KEY_SPACE,      " ",    " "    },
    { 0x37,           "*",    "*"    },
};

const uint32_t default_keymap_entries_count = sizeof(default_keymap_entries) / sizeof(default_keymap_entries[0]);
