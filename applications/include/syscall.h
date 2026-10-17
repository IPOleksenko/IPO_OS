#ifndef _IPO_SYSCALL_H
#define _IPO_SYSCALL_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define IPO_SYSCALL_ENOSYS       0xFFFFFFFFu
#define IPO_SYSCALL_OK           0x00000000u
#define IPO_SYSCALL_REGISTER     0x0001u
#define IPO_SYSCALL_CALL         0x0002u

#define IPO_SYSCALL_PRINT        0x1001u
#define IPO_SYSCALL_WRITE        0x1002u

#define IPO_SYSCALL_FS_CREATE    0x1010u
#define IPO_SYSCALL_FS_OPEN      0x1011u
#define IPO_SYSCALL_FS_READ      0x1012u
#define IPO_SYSCALL_FS_WRITE     0x1013u
#define IPO_SYSCALL_FS_DELETE    0x1014u
#define IPO_SYSCALL_FS_STAT      0x1015u
#define IPO_SYSCALL_FS_STATVFS   0x1064u
#define IPO_SYSCALL_FS_LIST      0x1016u
#define IPO_SYSCALL_FS_RENAME    0x1017u
#define IPO_SYSCALL_FS_CLOSE     0x1018u
#define IPO_SYSCALL_FS_SEEK      0x1019u

#define IPO_SYSCALL_EXEC         0x1020u
#define IPO_SYSCALL_READ         0x1021u
#define IPO_SYSCALL_TERMINAL_INPUT 0x1022u
#define IPO_SYSCALL_PROCESS_YIELD 0x1023u
#define IPO_SYSCALL_PROCESS_IS_FOREGROUND 0x1024u
#define IPO_SYSCALL_GETCWD       0x1025u
#define IPO_SYSCALL_CHDIR        0x1026u
#define IPO_SYSCALL_GET_EXIT_CODE 0x1027u

#define IPO_SYSCALL_ASYNC_START  0x1030u
#define IPO_SYSCALL_ASYNC_STOP   0x1031u
#define IPO_SYSCALL_STACK_GROW   0x1040u
#define IPO_SYSCALL_STACK_SHRINK 0x1041u
#define IPO_SYSCALL_VAR_SET      0x1042u
#define IPO_SYSCALL_VAR_GET      0x1043u
#define IPO_SYSCALL_VAR_DELETE   0x1044u
#define IPO_SYSCALL_SBRK         0x1045u
#define IPO_SYSCALL_TIME         0x1046u
#define IPO_SYSCALL_MILLIS       0x1048u
#define IPO_SYSCALL_ELAPSED_MS   0x1049u
#define IPO_SYSCALL_TIMER_AFTER  0x104Au
#define IPO_SYSCALL_NET_DNS_RESOLVE 0x1090u
#define IPO_SYSCALL_NET_ICMP_SEND   0x1091u
#define IPO_SYSCALL_NET_ICMP_POLL   0x1092u
#define IPO_SYSCALL_NET_SOCKET      0x1093u
#define IPO_SYSCALL_NET_BIND        0x1094u
#define IPO_SYSCALL_NET_LISTEN      0x1095u
#define IPO_SYSCALL_NET_ACCEPT      0x1096u
#define IPO_SYSCALL_NET_CONNECT     0x1097u
#define IPO_SYSCALL_NET_SEND        0x1098u
#define IPO_SYSCALL_NET_RECV        0x1099u
#define IPO_SYSCALL_NET_CLOSE       0x109Au
#define IPO_SYSCALL_NET_CONNECT_ENDPOINT 0x109Bu
#define IPO_SYSCALL_NET_POLL        0x109Cu
#define IPO_SYSCALL_NET_GET_INTERFACE 0x109Du
#define IPO_SYSCALL_NET_INIT        0x109Eu
#define IPO_SYSCALL_NET_SET_IP      0x109Fu
#define IPO_SYSCALL_FREE         0x1047u
#define IPO_SYSCALL_GETPID       0x107Cu

#define IPO_SYSCALL_PROCESS_IS_BATCH_ACTIVE 0x1028u
#define IPO_SYSCALL_SET_RETURN_VALUE 0x1029u
#define IPO_SYSCALL_GET_RETURN_VALUE 0x102Au

#define IPO_SYSCALL_KEYMAP_SET      0x1050u
#define IPO_SYSCALL_KEYMAP_GET      0x1051u
#define IPO_SYSCALL_FONT_LOAD       0x1052u
#define IPO_SYSCALL_KEYMAP_DISABLE  0x1053u
#define IPO_SYSCALL_KEYMAP_ENABLE   0x1054u
#define IPO_SYSCALL_KEYMAP_REMOVE   0x1055u
#define IPO_SYSCALL_KEYMAP_TRANSLATE 0x1056u
#define IPO_SYSCALL_KEYMAP_IS_ACTIVE 0x1057u
#define IPO_SYSCALL_KEYMAP_CYCLE_NEXT 0x1058u
#define IPO_SYSCALL_KEYMAP_CYCLE_PREV 0x1059u
#define IPO_SYSCALL_KEYMAP_GET_NAME  0x105Au
#define IPO_SYSCALL_FONT_GET_INFO    0x105Bu
#define IPO_SYSCALL_VGA_GLYPH        0x105Cu
#define IPO_SYSCALL_VGA_SET_MODE     0x105Du
#define IPO_SYSCALL_VGA_GET_MODE     0x105Eu
#define IPO_SYSCALL_SYSTEM_INTERRUPT 0x105Fu

#define IPO_SYSCALL_DRIVER_REGISTER   0x1060u
#define IPO_SYSCALL_DRIVER_UNREGISTER 0x1061u
#define IPO_SYSCALL_DRIVER_LIST       0x1062u
#define IPO_SYSCALL_DRIVER_SET_DESC   0x1063u

#define IPO_SYSCALL_WM_CREATE_WINDOW   0x1070u
#define IPO_SYSCALL_WM_DESTROY_WINDOW  0x1071u
#define IPO_SYSCALL_WM_SESSION_START   0x1072u
#define IPO_SYSCALL_WM_SESSION_STOP    0x1073u
#define IPO_SYSCALL_WM_SESSION_ACTIVE  0x1074u
#define IPO_SYSCALL_WM_GET_COUNT       0x1075u
#define IPO_SYSCALL_WM_GET_FOCUSED     0x1076u
#define IPO_SYSCALL_WM_SET_FOCUS       0x1077u
#define IPO_SYSCALL_WM_FOCUS_NEXT      0x1078u
#define IPO_SYSCALL_WM_FOCUS_PREV      0x1079u
#define IPO_SYSCALL_WM_INVALIDATE      0x107Au
#define IPO_SYSCALL_WM_IS_VALID        0x107Bu
#define IPO_SYSCALL_WM_TOGGLE_MAXIMIZE 0x107Du
#define IPO_SYSCALL_WM_RESIZE          0x107Eu
#define IPO_SYSCALL_WM_GET_LIST        0x107Fu
#define IPO_SYSCALL_PROCESS_SWITCH_FG  0x1080u
#define IPO_SYSCALL_WM_INIT            0x1081u
#define IPO_SYSCALL_WM_INVALIDATE_ALL  0x1082u
#define IPO_SYSCALL_WM_SET_FULLSCREEN  0x1083u
#define IPO_SYSCALL_WM_MINIMIZE        0x1084u
#define IPO_SYSCALL_WM_RESTORE         0x1085u
#define IPO_SYSCALL_WM_MOVE            0x1086u
#define IPO_SYSCALL_WM_SET_TITLE       0x1087u
#define IPO_SYSCALL_WM_SET_SHAPE       0x1088u
#define IPO_SYSCALL_WM_DISPATCH_KEY    0x1089u
#define IPO_SYSCALL_WM_COMPOSE         0x108Au
#define IPO_SYSCALL_WM_COMPOSITOR_TICK 0x108Bu
#define IPO_SYSCALL_WM_SESSION_RUN     0x108Cu
#define IPO_SYSCALL_WM_GET_COUNT_PID   0x108Du
#define IPO_SYSCALL_WM_HAS_WINDOWS_PID 0x108Eu
#define IPO_SYSCALL_WM_FOCUS_PID       0x108Fu

/* Keyboard & Input Syscalls */
#define IPO_SYSCALL_KBD_GET_SCANCODE      0x10A0u
#define IPO_SYSCALL_KBD_WAIT_SCANCODE     0x10A1u
#define IPO_SYSCALL_KBD_SET_APP_MODE      0x10A2u
#define IPO_SYSCALL_KBD_FLUSH             0x10A3u
#define IPO_SYSCALL_KBD_GET_MODIFIERS     0x10A4u
#define IPO_SYSCALL_KBD_POLL              0x10A5u
#define IPO_SYSCALL_KBD_GET_KEY_STRING    0x10A6u

/* Mouse Syscalls */
#define IPO_SYSCALL_MOUSE_GET_STATE       0x10B0u

/* VGA Hardware Syscalls */
#define IPO_SYSCALL_VGA_SET_CURSOR        0x10C0u
#define IPO_SYSCALL_VGA_GET_CURSOR        0x10C1u
#define IPO_SYSCALL_VGA_SHOW_CURSOR       0x10C2u
#define IPO_SYSCALL_VGA_HIDE_CURSOR       0x10C3u
#define IPO_SYSCALL_VGA_SET_PALETTE       0x10C4u

/* Audio Core Syscalls */
#define IPO_SYSCALL_AUDIO_INIT            0x10D0u
#define IPO_SYSCALL_AUDIO_TICK            0x10D1u
#define IPO_SYSCALL_AUDIO_STOP_ALL        0x10D2u
#define IPO_SYSCALL_AUDIO_STREAM_CREATE   0x10D3u
#define IPO_SYSCALL_AUDIO_STREAM_CREATE_RING 0x10D4u
#define IPO_SYSCALL_AUDIO_STREAM_DESTROY  0x10D5u
#define IPO_SYSCALL_AUDIO_STREAM_PLAY     0x10D6u
#define IPO_SYSCALL_AUDIO_STREAM_PAUSE    0x10D7u
#define IPO_SYSCALL_AUDIO_STREAM_STOP     0x10D8u
#define IPO_SYSCALL_AUDIO_STREAM_SEEK     0x10D9u
#define IPO_SYSCALL_AUDIO_STREAM_SET_VOL  0x10DAu
#define IPO_SYSCALL_AUDIO_STREAM_PROGRESS 0x10DBu
#define IPO_SYSCALL_AUDIO_STREAM_UPDATE   0x10DCu

#define IPO_SYSCALL_EXIT         0xFFFFu

#include <file_system/ipo_fs.h>

#ifdef __cplusplus
extern "C" {
#endif

int ipo_syscall(uint32_t num, uint32_t argc, uint32_t *argv);

static inline int ipo_print(const char *text) {
    uint32_t args[1];
    args[0] = (uint32_t)(uintptr_t)text;
    return ipo_syscall(IPO_SYSCALL_WRITE, 1u, args);
}

static inline int ipo_exec(const char *path, int argc, char **argv) {
    uint32_t args[3];
    args[0] = (uint32_t)(uintptr_t)path;
    args[1] = (uint32_t)argc;
    args[2] = (uint32_t)(uintptr_t)argv;
    return ipo_syscall(IPO_SYSCALL_EXEC, 3u, args);
}

static inline int ipo_terminal_input(const char *text, int auto_execute) {
    uint32_t args[2];
    args[0] = (uint32_t)(uintptr_t)text;
    args[1] = (uint32_t)auto_execute;
    return ipo_syscall(IPO_SYSCALL_TERMINAL_INPUT, 2u, args);
}

static inline int ipo_var_set(const char *name, const void *value, uint32_t value_size) {
    uint32_t args[3];
    args[0] = (uint32_t)(uintptr_t)name;
    args[1] = (uint32_t)(uintptr_t)value;
    args[2] = value_size;
    return ipo_syscall(IPO_SYSCALL_VAR_SET, 3u, args);
}

static inline int ipo_var_get(const char *name, void *buffer, uint32_t buffer_size) {
    uint32_t args[3];
    args[0] = (uint32_t)(uintptr_t)name;
    args[1] = (uint32_t)(uintptr_t)buffer;
    args[2] = buffer_size;
    return ipo_syscall(IPO_SYSCALL_VAR_GET, 3u, args);
}

static inline int ipo_var_delete(const char *name) {
    uint32_t args[1];
    args[0] = (uint32_t)(uintptr_t)name;
    return ipo_syscall(IPO_SYSCALL_VAR_DELETE, 1u, args);
}

static inline int ipo_var_set_int(const char *name, int value) {
    return ipo_var_set(name, &value, sizeof(value));
}

static inline int ipo_var_get_int(const char *name, int *out_value) {
    return ipo_var_get(name, out_value, sizeof(*out_value));
}

static inline int ipo_var_set_str(const char *name, const char *value) {
    if (value == NULL) return IPO_SYSCALL_ENOSYS;
    return ipo_var_set(name, value, (uint32_t)strlen(value) + 1u);
}

static inline int ipo_var_get_str(const char *name, char *buffer, uint32_t buffer_size) {
    if (buffer == NULL || buffer_size == 0u) return IPO_SYSCALL_ENOSYS;
    return ipo_var_get(name, buffer, buffer_size);
}

static inline int ipo_keymap_set_with_font(const char *name,
                                           const void *entries,
                                           uint32_t count,
                                           const void *glyphs,
                                           uint32_t glyph_count) {
    uint32_t args[5];
    args[0] = (uint32_t)(uintptr_t)name;
    args[1] = (uint32_t)(uintptr_t)entries;
    args[2] = count;
    args[3] = (uint32_t)(uintptr_t)glyphs;
    args[4] = glyph_count;
    return ipo_syscall(IPO_SYSCALL_KEYMAP_SET, 5u, args);
}

static inline int ipo_keymap_set(const char *name, const void *entries, uint32_t count) {
    return ipo_keymap_set_with_font(name, entries, count, NULL, 0u);
}

static inline int ipo_rename(const char *oldpath, const char *newpath) {
    uint32_t args[2];
    args[0] = (uint32_t)(uintptr_t)oldpath;
    args[1] = (uint32_t)(uintptr_t)newpath;
    return ipo_syscall(IPO_SYSCALL_FS_RENAME, 2u, args);
}


static inline int ipo_open(const char *path) {
    uint32_t args[1];
    args[0] = (uint32_t)(uintptr_t)path;
    return ipo_syscall(IPO_SYSCALL_FS_OPEN, 1u, args);
}

static inline int ipo_close(int fd) {
    uint32_t args[1];
    args[0] = (uint32_t)fd;
    return ipo_syscall(IPO_SYSCALL_FS_CLOSE, 1u, args);
}

static inline int ipo_read(int fd, void *buf, uint32_t count, uint32_t offset) {
    uint32_t args[4];
    args[0] = (uint32_t)fd;
    args[1] = (uint32_t)(uintptr_t)buf;
    args[2] = count;
    args[3] = offset;
    return ipo_syscall(IPO_SYSCALL_FS_READ, 4u, args);
}

static inline int ipo_create(const char *path, uint8_t type) {
    uint32_t args[2];
    args[0] = (uint32_t)(uintptr_t)path;
    args[1] = (uint32_t)type;
    return ipo_syscall(IPO_SYSCALL_FS_CREATE, 2u, args);
}

static inline int ipo_write(int fd, const void *buf, uint32_t count, uint32_t offset) {
    uint32_t args[4];
    args[0] = (uint32_t)fd;
    args[1] = (uint32_t)(uintptr_t)buf;
    args[2] = count;
    args[3] = offset;
    return ipo_syscall(IPO_SYSCALL_FS_WRITE, 4u, args);
}

static inline int ipo_delete(const char *path) {
    uint32_t args[1];
    args[0] = (uint32_t)(uintptr_t)path;
    return ipo_syscall(IPO_SYSCALL_FS_DELETE, 1u, args);
}

static inline int ipo_seek(int fd, int32_t offset, int whence) {
    uint32_t args[3];
    args[0] = (uint32_t)fd;
    args[1] = (uint32_t)offset;
    args[2] = (uint32_t)whence;
    return ipo_syscall(IPO_SYSCALL_FS_SEEK, 3u, args);
}

static inline void *ipo_sbrk(int32_t increment) {
    uint32_t args[1];
    args[0] = (uint32_t)increment;
    int res = ipo_syscall(IPO_SYSCALL_SBRK, 1u, args);
    return (void *)(intptr_t)res;
}

static inline uint32_t ipo_time(uint32_t *tloc) {
    uint32_t args[1];
    args[0] = (uint32_t)(uintptr_t)tloc;
    return (uint32_t)ipo_syscall(IPO_SYSCALL_TIME, 1u, args);
}

static inline void ipo_exit(int status) {
    uint32_t args[1];
    args[0] = (uint32_t)status;
    ipo_syscall(IPO_SYSCALL_EXIT, 1u, args);
    while (1) { }
}

static inline int ipo_getcwd(char *buf, uint32_t size) {
    uint32_t args[2];
    args[0] = (uint32_t)(uintptr_t)buf;
    args[1] = size;
    return ipo_syscall(IPO_SYSCALL_GETCWD, 2u, args);
}

static inline int ipo_chdir(const char *path) {
    uint32_t args[1];
    args[0] = (uint32_t)(uintptr_t)path;
    return ipo_syscall(IPO_SYSCALL_CHDIR, 1u, args);
}

static inline int ipo_get_exit_code(void) {
    return ipo_syscall(IPO_SYSCALL_GET_EXIT_CODE, 0u, NULL);
}

static inline int ipo_stat(const char *path, struct ipo_inode *st) {
    uint32_t args[2];
    args[0] = (uint32_t)(uintptr_t)path;
    args[1] = (uint32_t)(uintptr_t)st;
    return ipo_syscall(IPO_SYSCALL_FS_STAT, 2u, args);
}

static inline int ipo_list_dir(const char *path, char *buf, int size) {
    uint32_t args[3];
    args[0] = (uint32_t)(uintptr_t)path;
    args[1] = (uint32_t)(uintptr_t)buf;
    args[2] = (uint32_t)size;
    return (int)ipo_syscall(IPO_SYSCALL_FS_LIST, 3u, args);
}

static inline int ipo_read_line(char *buf, uint32_t max_len) {
    if (buf == NULL || max_len == 0u) return -1;
    uint32_t args[2];
    args[0] = (uint32_t)(uintptr_t)buf;
    args[1] = max_len;
    return ipo_syscall(IPO_SYSCALL_READ, 2u, args);
}

static inline void ipo_kfree(void *ptr) {
    if (ptr == NULL) return;
    uint32_t args[1];
    args[0] = (uint32_t)(uintptr_t)ptr;
    ipo_syscall(IPO_SYSCALL_FREE, 1u, args);
}

static inline int ipo_read_line_dynamic(char **out_ptr) {
    if (out_ptr == NULL) return -1;
    uint32_t args[2];
    args[0] = (uint32_t)(uintptr_t)out_ptr;
    args[1] = 0u;
    return ipo_syscall(IPO_SYSCALL_READ, 2u, args);
}

static inline int ipo_getpid(void) {
    return ipo_syscall(IPO_SYSCALL_GETPID, 0u, NULL);
}

static inline int ipo_font_load(const char *path) {
    if (path != NULL) {
        uint32_t args[1];
        args[0] = (uint32_t)(uintptr_t)path;
        return ipo_syscall(IPO_SYSCALL_FONT_LOAD, 1u, args);
    }
    return ipo_syscall(IPO_SYSCALL_FONT_LOAD, 0u, NULL);
}

static inline int ipo_font_get_info(char *name_buf, uint32_t max_name_len, uint32_t *out_glyph_count) {
    uint32_t args[3];
    args[0] = (uint32_t)(uintptr_t)name_buf;
    args[1] = max_name_len;
    args[2] = (uint32_t)(uintptr_t)out_glyph_count;
    return ipo_syscall(IPO_SYSCALL_FONT_GET_INFO, 3u, args);
}

static inline int ipo_vga_glyph(uint32_t codepoint) {
    uint32_t args[1];
    args[0] = codepoint;
    return ipo_syscall(IPO_SYSCALL_VGA_GLYPH, 1u, args);
}

static inline int ipo_font_load_cyrillic(const char *path) {
    return ipo_font_load(path);
}

static inline int ipo_keymap_disable(const char *name) {
    if (name == NULL) return -1;
    uint32_t args[1];
    args[0] = (uint32_t)(uintptr_t)name;
    return ipo_syscall(IPO_SYSCALL_KEYMAP_DISABLE, 1u, args);
}

static inline int ipo_keymap_enable(const char *name) {
    if (name == NULL) return -1;
    uint32_t args[1];
    args[0] = (uint32_t)(uintptr_t)name;
    return ipo_syscall(IPO_SYSCALL_KEYMAP_ENABLE, 1u, args);
}

static inline int ipo_keymap_remove(const char *name) {
    if (name == NULL) return -1;
    uint32_t args[1];
    args[0] = (uint32_t)(uintptr_t)name;
    return ipo_syscall(IPO_SYSCALL_KEYMAP_REMOVE, 1u, args);
}

static inline int ipo_driver_register(void *drv) {
    uint32_t args[1];
    args[0] = (uint32_t)(uintptr_t)drv;
    return ipo_syscall(IPO_SYSCALL_DRIVER_REGISTER, 1u, args);
}

static inline int ipo_driver_unregister(const char *name) {
    uint32_t args[1];
    args[0] = (uint32_t)(uintptr_t)name;
    return ipo_syscall(IPO_SYSCALL_DRIVER_UNREGISTER, 1u, args);
}

static inline int ipo_driver_set_description(const char *name, const char *desc) {
    uint32_t args[2];
    args[0] = (uint32_t)(uintptr_t)name;
    args[1] = (uint32_t)(uintptr_t)desc;
    return ipo_syscall(IPO_SYSCALL_DRIVER_SET_DESC, 2u, args);
}

static inline int ipo_driver_list(void) {
    return ipo_syscall(IPO_SYSCALL_DRIVER_LIST, 0u, (void *)0);
}

static inline int ipo_set_return_int(int val) {
    uint32_t args[3];
    args[0] = 0; /* PROC_RET_INT */
    args[1] = (uint32_t)val;
    args[2] = 0;
    return ipo_syscall(IPO_SYSCALL_SET_RETURN_VALUE, 3u, args);
}

static inline int ipo_set_return_char(char c) {
    uint32_t args[3];
    args[0] = 1; /* PROC_RET_CHAR */
    args[1] = (uint32_t)(uint8_t)c;
    args[2] = 0;
    return ipo_syscall(IPO_SYSCALL_SET_RETURN_VALUE, 3u, args);
}

static inline int ipo_set_return_string(const char *str) {
    uint32_t args[3];
    args[0] = 2; /* PROC_RET_STRING */
    args[1] = (uint32_t)(uintptr_t)str;
    args[2] = 0;
    return ipo_syscall(IPO_SYSCALL_SET_RETURN_VALUE, 3u, args);
}

static inline int ipo_set_return_custom(const char *type_name, const void *data, uint32_t size) {
    uint32_t args[4];
    args[0] = 3; /* PROC_RET_STRUCT */
    args[1] = (uint32_t)(uintptr_t)data;
    args[2] = size;
    args[3] = (uint32_t)(uintptr_t)type_name;
    return ipo_syscall(IPO_SYSCALL_SET_RETURN_VALUE, 4u, args);
}

static inline int ipo_set_return_struct(const char *fmt, const void *data, uint32_t size) {
    return ipo_set_return_custom(fmt ? fmt : "struct", data, size);
}

#ifndef _IPO_RETURN_FORMAT_DESC_T_DEFINED
#define _IPO_RETURN_FORMAT_DESC_T_DEFINED
typedef struct ipo_return_format_desc {
    uint32_t magic;
    const char *fmt;
} ipo_return_format_desc_t;
#endif

#ifndef IPO_RETURN_FORMAT
#define IPO_RETURN_FORMAT(fmt_str) \
    __attribute__((used)) static const ipo_return_format_desc_t __ipo_ret_desc = { 0x524F5049, (fmt_str) }
#endif

static inline int ipo_get_return_string(char *buf, uint32_t max_len) {
    uint32_t args[2];
    args[0] = (uint32_t)(uintptr_t)buf;
    args[1] = max_len;
    return ipo_syscall(IPO_SYSCALL_GET_RETURN_VALUE, 2u, args);
}

static inline int ipo_keymap_get_table(char *table, uint32_t max_len, bool shift) {
    uint32_t args[4];
    args[0] = 1u;
    args[1] = (uint32_t)(uintptr_t)table;
    args[2] = max_len;
    args[3] = (uint32_t)shift;
    return ipo_syscall(IPO_SYSCALL_KEYMAP_GET, 4u, args);
}

static inline uint32_t ipo_keymap_get_count(void) {
    uint32_t args[1];
    args[0] = 3u;
    return ipo_syscall(IPO_SYSCALL_KEYMAP_GET, 1u, args);
}

static inline uint32_t ipo_keymap_get_entries(uint32_t slot_idx, void *out_entries, uint32_t max_count) {
    uint32_t args[4];
    args[0] = 2u;
    args[1] = slot_idx;
    args[2] = (uint32_t)(uintptr_t)out_entries;
    args[3] = max_count;
    return ipo_syscall(IPO_SYSCALL_KEYMAP_GET, 4u, args);
}

#ifdef __cplusplus
}
#endif

#endif
