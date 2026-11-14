#ifndef IPO_SYSCALL_H

#define IPO_SYSCALL_H

#include <stdint.h>
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

#define IPO_SYSCALL_PROCESS_IS_BATCH_ACTIVE 0x1028u
#define IPO_SYSCALL_SET_RETURN_VALUE 0x1029u
#define IPO_SYSCALL_GET_RETURN_VALUE 0x102Au

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
#define IPO_SYSCALL_NET_SET_MAC     0x10FFu

#define IPO_SYSCALL_FREE         0x1047u

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

#define IPO_SYSCALL_FS_STATVFS        0x1064u

#define IPO_SYSCALL_VGA_GET_BOUNDS    0x1065u

#define IPO_SYSCALL_PORT_IN8          0x1066u
#define IPO_SYSCALL_PORT_OUT8         0x1067u
#define IPO_SYSCALL_PORT_IN16         0x1068u
#define IPO_SYSCALL_PORT_OUT16        0x1069u
#define IPO_SYSCALL_PORT_IN32         0x106Au
#define IPO_SYSCALL_PORT_OUT32        0x106Bu

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
#define IPO_SYSCALL_GETPID             0x107Cu
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
#define IPO_SYSCALL_MOUSE_GET_MOTION      0x10B1u

/* VGA Hardware Syscalls */
#define IPO_SYSCALL_VGA_SET_CURSOR        0x10C0u
#define IPO_SYSCALL_VGA_GET_CURSOR        0x10C1u
#define IPO_SYSCALL_VGA_SHOW_CURSOR       0x10C2u
#define IPO_SYSCALL_VGA_HIDE_CURSOR       0x10C3u
#define IPO_SYSCALL_VGA_SET_PALETTE       0x10C4u
#define IPO_SYSCALL_VGA_SET_OVERLAY_FLAGS 0x10C5u
#define IPO_SYSCALL_VGA_GET_OVERLAY_FLAGS 0x10C6u
#define IPO_SYSCALL_VGA_GET_TEXT_INFO     0x10C7u

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

#define IPO_SYSCALL_BINFMT_REGISTER   0x10E0u
#define IPO_SYSCALL_BINFMT_UNREGISTER 0x10E1u
#define IPO_SYSCALL_BINFMT_LIST       0x10E2u

#define IPO_SYSCALL_IPC_CREATE        0x10F0u
#define IPO_SYSCALL_IPC_OPEN          0x10F1u
#define IPO_SYSCALL_IPC_CLOSE         0x10F2u
#define IPO_SYSCALL_IPC_DESTROY       0x10F3u
#define IPO_SYSCALL_IPC_SEND          0x10F4u
#define IPO_SYSCALL_IPC_RECEIVE       0x10F5u
#define IPO_SYSCALL_IPC_PIPE_CREATE   0x10F6u
#define IPO_SYSCALL_IPC_PIPE_OPEN     0x10F7u
#define IPO_SYSCALL_IPC_PIPE_CLOSE    0x10F8u
#define IPO_SYSCALL_IPC_PIPE_DESTROY  0x10F9u
#define IPO_SYSCALL_IPC_PIPE_READ     0x10FAu
#define IPO_SYSCALL_IPC_PIPE_WRITE    0x10FBu
#define IPO_SYSCALL_IPC_NOTIFY_SEND   0x10FCu
#define IPO_SYSCALL_IPC_NOTIFY_WAIT   0x10FDu

#define IPO_SYSCALL_SHM_CREATE        0x1100u
#define IPO_SYSCALL_SHM_OPEN          0x1101u
#define IPO_SYSCALL_SHM_ATTACH        0x1102u
#define IPO_SYSCALL_SHM_DETACH        0x1103u
#define IPO_SYSCALL_SHM_DESTROY       0x1104u

#define IPO_SYSCALL_SIGACTION         0x1110u
#define IPO_SYSCALL_SIGPROCMASK       0x1111u
#define IPO_SYSCALL_KILL              0x1112u
#define IPO_SYSCALL_SIGALTSTACK       0x1113u
#define IPO_SYSCALL_THREAD_CREATE     0x1120u
#define IPO_SYSCALL_THREAD_EXIT       0x1121u
#define IPO_SYSCALL_THREAD_JOIN       0x1122u
#define IPO_SYSCALL_THREAD_SELF       0x1123u
#define IPO_SYSCALL_THREAD_SYNC       0x1130u

/* Extended Input, Custom Shortcuts & Generic Input Devices */
#define IPO_SYSCALL_INPUT_POLL_KEY_EXT    0x1140u
#define IPO_SYSCALL_INPUT_GET_KEY_STATE   0x1141u
#define IPO_SYSCALL_INPUT_GET_MODIFIERS   0x1142u
#define IPO_SYSCALL_SHORTCUT_REGISTER     0x1143u
#define IPO_SYSCALL_SHORTCUT_UNREGISTER   0x1144u
#define IPO_SYSCALL_SHORTCUT_POLL         0x1145u
#define IPO_SYSCALL_INPUT_DEV_REGISTER    0x1146u
#define IPO_SYSCALL_INPUT_DEV_UNREGISTER  0x1147u
#define IPO_SYSCALL_INPUT_DEV_EMIT        0x1148u
#define IPO_SYSCALL_INPUT_DEV_LIST        0x1149u
#define IPO_SYSCALL_INPUT_DEV_POLL_EVENT  0x114Au
#define IPO_SYSCALL_INPUT_DEV_GET_STATE   0x114Bu
#define IPO_SYSCALL_MOUSE_INJECT          0x114Cu

/* Low-Level PS/2 Hooks & Transactions */
#define IPO_SYSCALL_PS2_HOOK_REGISTER     0x1150u
#define IPO_SYSCALL_PS2_HOOK_UNREGISTER   0x1151u
#define IPO_SYSCALL_PS2_HOOK_POLL         0x1152u
#define IPO_SYSCALL_PS2_TRANSACT          0x1153u

/* Low-Level Hardware IRQ Primitives */
#define IPO_SYSCALL_IRQ_SUBSCRIBE         0x1160u
#define IPO_SYSCALL_IRQ_UNSUBSCRIBE       0x1161u
#define IPO_SYSCALL_IRQ_POLL              0x1162u
#define IPO_SYSCALL_IRQ_GET_COUNT         0x1163u
#define IPO_SYSCALL_IRQ_REGISTER_CB       0x1164u
#define IPO_SYSCALL_IRQ_UNREGISTER_CB     0x1165u

/* Low-Level MMIO / DMA Hardware Memory Primitives */
#define IPO_SYSCALL_HW_MAP_MMIO           0x1170u
#define IPO_SYSCALL_HW_UNMAP_MMIO         0x1171u
#define IPO_SYSCALL_HW_ALLOC_DMA          0x1172u
#define IPO_SYSCALL_HW_FREE_DMA           0x1173u
#define IPO_SYSCALL_HW_VIRT_TO_PHYS       0x1174u

/* PCI Enumeration & Configuration Primitives */
#define IPO_SYSCALL_PCI_GET_COUNT         0x1178u
#define IPO_SYSCALL_PCI_GET_DEVICE        0x1179u
#define IPO_SYSCALL_PCI_CONFIG_READ       0x117Au
#define IPO_SYSCALL_PCI_CONFIG_WRITE      0x117Bu
#define IPO_SYSCALL_PCI_ENABLE_BUS_MASTER 0x117Cu

/* Device Registry Primitives */
#define IPO_SYSCALL_DEV_GET_COUNT         0x1180u
#define IPO_SYSCALL_DEV_GET_INFO          0x1181u
#define IPO_SYSCALL_DEV_REGISTER          0x1182u
#define IPO_SYSCALL_DEV_UNREGISTER        0x1183u

#define IPO_SYSCALL_EXIT         0xFFFFu

typedef uint32_t (*ipo_syscall_handler_t)(uint32_t, uint32_t, uint32_t *);

void syscall_init(void);
void fpu_init(void);
bool syscall_prepare_cpu_exceptions(uint32_t cpu_count);
bool syscall_init_cpu(uint32_t cpu_id);
void syscall_update_exception_page_directory(void);

void ipo_register_syscall(uint32_t num, ipo_syscall_handler_t handler);

int ipo_syscall(uint32_t num, uint32_t argc, uint32_t *argv);

static inline uint64_t ipo_syscall64(uint32_t num, uint32_t argc,
                                     uint32_t *argv) {
    uint32_t low;
    uint32_t high;
    __asm__ volatile(
        "int $0x80"
        : "=a"(low), "=d"(high)
        : "a"(num),
          "b"(argc),
          "c"(argv)
        : "memory"
    );
    return ((uint64_t)high << 32) | low;
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

static inline int ipo_var_set_float(const char *name, float value) {
    return ipo_var_set(name, &value, sizeof(value));
}

static inline int ipo_var_get_float(const char *name, float *out_value) {
    return ipo_var_get(name, out_value, sizeof(*out_value));
}

static inline int ipo_var_set_bool(const char *name, int value) {
    return ipo_var_set(name, &value, sizeof(value));
}

static inline int ipo_var_get_bool(const char *name, int *out_value) {
    return ipo_var_get(name, out_value, sizeof(*out_value));
}

static inline int ipo_var_set_str(const char *name, const char *value) {
    if (value == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }
    return ipo_var_set(name, value, (uint32_t)strlen(value) + 1u);
}

static inline int ipo_var_get_str(const char *name, char *buffer, uint32_t buffer_size) {
    if (buffer == NULL || buffer_size == 0u) {
        return IPO_SYSCALL_ENOSYS;
    }
    return ipo_var_get(name, buffer, buffer_size);
}

#define IPO_VAR_SET(name, value) \
    __extension__ ({ __typeof__(value) _ipo_var_tmp = (value); ipo_var_set((name), &_ipo_var_tmp, sizeof(_ipo_var_tmp)); })

#define IPO_VAR_GET(name, out_var) \
    __extension__ ({ \
        __typeof__(*(out_var)) _ipo_var_tmp; \
        memset(&_ipo_var_tmp, 0, sizeof(_ipo_var_tmp)); \
        int _ipo_var_rc = ipo_var_get((name), &_ipo_var_tmp, sizeof(_ipo_var_tmp)); \
        if (_ipo_var_rc >= 0) { *(out_var) = _ipo_var_tmp; } \
        _ipo_var_rc; \
    })

#define IPO_VAR_SET_STR(name, value) ipo_var_set_str((name), (value))
#define IPO_VAR_GET_STR(name, buffer, buffer_size) ipo_var_get_str((name), (buffer), (buffer_size))

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

static inline int64_t ipo_create(const char *path, uint8_t type) {
    uint32_t args[2];
    args[0] = (uint32_t)(uintptr_t)path;
    args[1] = (uint32_t)type;
    return (int64_t)ipo_syscall64(IPO_SYSCALL_FS_CREATE, 2u, args);
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

static inline int ipo_rename(const char *oldpath, const char *newpath) {
    uint32_t args[2];
    args[0] = (uint32_t)(uintptr_t)oldpath;
    args[1] = (uint32_t)(uintptr_t)newpath;
    return ipo_syscall(IPO_SYSCALL_FS_RENAME, 2u, args);
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

static inline int ipo_getpid(void) {
    return ipo_syscall(IPO_SYSCALL_GETPID, 0u, NULL);
}

struct ipo_inode;
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

static inline int ipo_vga_get_screen_bounds(int32_t *width, int32_t *height) {
    if (width == NULL || height == NULL) return -1;
    uint32_t args[2];
    args[0] = (uint32_t)(uintptr_t)width;
    args[1] = (uint32_t)(uintptr_t)height;
    return ipo_syscall(IPO_SYSCALL_VGA_GET_BOUNDS, 2u, args);
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

static inline uint8_t ipo_port_in8(uint16_t port) {
    uint32_t args[1] = {port};
    return (uint8_t)ipo_syscall(IPO_SYSCALL_PORT_IN8, 1u, args);
}

static inline int ipo_port_out8(uint16_t port, uint8_t value) {
    uint32_t args[2] = {port, value};
    return (int)ipo_syscall(IPO_SYSCALL_PORT_OUT8, 2u, args);
}

static inline uint16_t ipo_port_in16(uint16_t port) {
    uint32_t args[1] = {port};
    return (uint16_t)ipo_syscall(IPO_SYSCALL_PORT_IN16, 1u, args);
}

static inline int ipo_port_out16(uint16_t port, uint16_t value) {
    uint32_t args[2] = {port, value};
    return (int)ipo_syscall(IPO_SYSCALL_PORT_OUT16, 2u, args);
}

static inline uint32_t ipo_port_in32(uint16_t port) {
    uint32_t args[1] = {port};
    return ipo_syscall(IPO_SYSCALL_PORT_IN32, 1u, args);
}

static inline int ipo_port_out32(uint16_t port, uint32_t value) {
    uint32_t args[2] = {port, value};
    return (int)ipo_syscall(IPO_SYSCALL_PORT_OUT32, 2u, args);
}

static inline int ipo_binfmt_register(const char *name, const void *magic, uint32_t magic_len, uint32_t offset, const char *runner_path) {
    uint32_t args[5];
    args[0] = (uint32_t)(uintptr_t)name;
    args[1] = (uint32_t)(uintptr_t)magic;
    args[2] = magic_len;
    args[3] = offset;
    args[4] = (uint32_t)(uintptr_t)runner_path;
    return ipo_syscall(IPO_SYSCALL_BINFMT_REGISTER, 5u, args);
}

static inline int ipo_binfmt_unregister(const char *name) {
    uint32_t args[1];
    args[0] = (uint32_t)(uintptr_t)name;
    return ipo_syscall(IPO_SYSCALL_BINFMT_UNREGISTER, 1u, args);
}

static inline int ipo_binfmt_list(void) {
    return ipo_syscall(IPO_SYSCALL_BINFMT_LIST, 0u, NULL);
}

static inline int ipo_vga_get_text_info(uint16_t *out_w, uint16_t *out_h, uint32_t *out_vram_addr) {
    uint32_t args[3] = {
        (uint32_t)(uintptr_t)out_w,
        (uint32_t)(uintptr_t)out_h,
        (uint32_t)(uintptr_t)out_vram_addr
    };
    return ipo_syscall(IPO_SYSCALL_VGA_GET_TEXT_INFO, 3u, args);
}

static inline int ipo_sys_input_poll_key_ext(void *out_ev) {
    uint32_t args[1] = { (uint32_t)(uintptr_t)out_ev };
    return ipo_syscall(IPO_SYSCALL_INPUT_POLL_KEY_EXT, 1u, args);
}

static inline int ipo_sys_input_get_key_state(uint32_t keycode) {
    uint32_t args[1] = { keycode };
    return ipo_syscall(IPO_SYSCALL_INPUT_GET_KEY_STATE, 1u, args);
}

static inline uint32_t ipo_sys_input_get_modifiers(void) {
    return (uint32_t)ipo_syscall(IPO_SYSCALL_INPUT_GET_MODIFIERS, 0u, NULL);
}

static inline uint32_t ipo_sys_shortcut_register(const void *spec) {
    uint32_t args[1] = { (uint32_t)(uintptr_t)spec };
    return (uint32_t)ipo_syscall(IPO_SYSCALL_SHORTCUT_REGISTER, 1u, args);
}

static inline int ipo_sys_shortcut_unregister(uint32_t shortcut_id) {
    uint32_t args[1] = { shortcut_id };
    return ipo_syscall(IPO_SYSCALL_SHORTCUT_UNREGISTER, 1u, args);
}

static inline int ipo_sys_shortcut_poll(void *out_ev) {
    uint32_t args[1] = { (uint32_t)(uintptr_t)out_ev };
    return ipo_syscall(IPO_SYSCALL_SHORTCUT_POLL, 1u, args);
}

static inline uint32_t ipo_sys_input_dev_register(const void *desc) {
    uint32_t args[1] = { (uint32_t)(uintptr_t)desc };
    return (uint32_t)ipo_syscall(IPO_SYSCALL_INPUT_DEV_REGISTER, 1u, args);
}

static inline int ipo_sys_input_dev_unregister(uint32_t dev_id) {
    uint32_t args[1] = { dev_id };
    return ipo_syscall(IPO_SYSCALL_INPUT_DEV_UNREGISTER, 1u, args);
}

static inline int ipo_sys_input_dev_emit(uint32_t dev_id, const void *ev) {
    uint32_t args[2] = { dev_id, (uint32_t)(uintptr_t)ev };
    return ipo_syscall(IPO_SYSCALL_INPUT_DEV_EMIT, 2u, args);
}

static inline uint32_t ipo_sys_input_dev_list(void *out_arr, uint32_t max_count) {
    uint32_t args[2] = { (uint32_t)(uintptr_t)out_arr, max_count };
    return (uint32_t)ipo_syscall(IPO_SYSCALL_INPUT_DEV_LIST, 2u, args);
}

static inline int ipo_sys_input_dev_poll_event(uint32_t dev_filter_id, void *out_ev) {
    uint32_t args[2] = { dev_filter_id, (uint32_t)(uintptr_t)out_ev };
    return ipo_syscall(IPO_SYSCALL_INPUT_DEV_POLL_EVENT, 2u, args);
}

static inline int ipo_sys_input_dev_get_state(uint32_t dev_id, uint16_t type, uint32_t code, int32_t *out_val) {
    uint32_t args[4] = { dev_id, (uint32_t)type, code, (uint32_t)(uintptr_t)out_val };
    return ipo_syscall(IPO_SYSCALL_INPUT_DEV_GET_STATE, 4u, args);
}

static inline int ipo_sys_mouse_inject(int32_t dx_or_x, int32_t dy_or_y, int32_t dz,
                                       uint32_t buttons_mask, bool is_absolute) {
    uint32_t args[5] = {
        (uint32_t)dx_or_x, (uint32_t)dy_or_y, (uint32_t)dz,
        buttons_mask, is_absolute ? 1u : 0u
    };
    return ipo_syscall(IPO_SYSCALL_MOUSE_INJECT, 5u, args);
}

static inline uint32_t ipo_sys_ps2_hook_register(uint32_t chan_mask, uint32_t flags) {
    uint32_t args[2] = { chan_mask, flags };
    return (uint32_t)ipo_syscall(IPO_SYSCALL_PS2_HOOK_REGISTER, 2u, args);
}

static inline int ipo_sys_ps2_hook_unregister(uint32_t handle) {
    uint32_t args[1] = { handle };
    return ipo_syscall(IPO_SYSCALL_PS2_HOOK_UNREGISTER, 1u, args);
}

static inline uint32_t ipo_sys_ps2_hook_poll(uint32_t handle, void *out_events, uint32_t max_count) {
    uint32_t args[3] = { handle, (uint32_t)(uintptr_t)out_events, max_count };
    return (uint32_t)ipo_syscall(IPO_SYSCALL_PS2_HOOK_POLL, 3u, args);
}

static inline int ipo_sys_ps2_transact(const void *req) {
    uint32_t args[1] = { (uint32_t)(uintptr_t)req };
    return ipo_syscall(IPO_SYSCALL_PS2_TRANSACT, 1u, args);
}

static inline uint32_t ipo_sys_irq_subscribe(uint8_t irq) {
    uint32_t args[1] = { (uint32_t)irq };
    return (uint32_t)ipo_syscall(IPO_SYSCALL_IRQ_SUBSCRIBE, 1u, args);
}

static inline int ipo_sys_irq_unsubscribe(uint32_t handle) {
    uint32_t args[1] = { handle };
    return ipo_syscall(IPO_SYSCALL_IRQ_UNSUBSCRIBE, 1u, args);
}

static inline uint32_t ipo_sys_irq_poll(uint32_t handle, uint32_t *out_total, uint64_t *out_last_ms) {
    uint32_t args[3] = { handle, (uint32_t)(uintptr_t)out_total, (uint32_t)(uintptr_t)out_last_ms };
    return (uint32_t)ipo_syscall(IPO_SYSCALL_IRQ_POLL, 3u, args);
}

static inline uint32_t ipo_sys_irq_get_count(uint8_t irq) {
    uint32_t args[1] = { (uint32_t)irq };
    return (uint32_t)ipo_syscall(IPO_SYSCALL_IRQ_GET_COUNT, 1u, args);
}

static inline uint32_t ipo_sys_irq_register_cb(uint8_t irq, void (*fn)(uint8_t, void *), void *ctx) {
    uint32_t args[3] = { (uint32_t)irq, (uint32_t)(uintptr_t)fn, (uint32_t)(uintptr_t)ctx };
    return (uint32_t)ipo_syscall(IPO_SYSCALL_IRQ_REGISTER_CB, 3u, args);
}

static inline int ipo_sys_irq_unregister_cb(uint32_t handle) {
    uint32_t args[1] = { handle };
    return ipo_syscall(IPO_SYSCALL_IRQ_UNREGISTER_CB, 1u, args);
}

static inline void *ipo_sys_hw_map_mmio(uint32_t phys_addr, uint32_t size, uint32_t flags) {
    uint32_t args[3] = { phys_addr, size, flags };
    return (void *)(uintptr_t)(uint32_t)ipo_syscall(IPO_SYSCALL_HW_MAP_MMIO, 3u, args);
}

static inline int ipo_sys_hw_unmap_mmio(void *virt_addr, uint32_t size) {
    uint32_t args[2] = { (uint32_t)(uintptr_t)virt_addr, size };
    return ipo_syscall(IPO_SYSCALL_HW_UNMAP_MMIO, 2u, args);
}

static inline void *ipo_sys_hw_alloc_dma(uint32_t size, uint32_t alignment, uint32_t *out_phys) {
    uint32_t args[3] = { size, alignment, (uint32_t)(uintptr_t)out_phys };
    return (void *)(uintptr_t)(uint32_t)ipo_syscall(IPO_SYSCALL_HW_ALLOC_DMA, 3u, args);
}

static inline int ipo_sys_hw_free_dma(void *virt_addr) {
    uint32_t args[1] = { (uint32_t)(uintptr_t)virt_addr };
    return ipo_syscall(IPO_SYSCALL_HW_FREE_DMA, 1u, args);
}

static inline uint32_t ipo_sys_hw_virt_to_phys(const void *virt_addr) {
    uint32_t args[1] = { (uint32_t)(uintptr_t)virt_addr };
    return (uint32_t)ipo_syscall(IPO_SYSCALL_HW_VIRT_TO_PHYS, 1u, args);
}

static inline uint32_t ipo_sys_pci_get_count(void) {
    return (uint32_t)ipo_syscall(IPO_SYSCALL_PCI_GET_COUNT, 0u, NULL);
}

static inline int ipo_sys_pci_get_device(uint32_t index, void *out_info) {
    uint32_t args[2] = { index, (uint32_t)(uintptr_t)out_info };
    return ipo_syscall(IPO_SYSCALL_PCI_GET_DEVICE, 2u, args);
}

static inline uint32_t ipo_sys_pci_config_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint8_t width) {
    uint32_t args[5] = { bus, slot, func, offset, width };
    return (uint32_t)ipo_syscall(IPO_SYSCALL_PCI_CONFIG_READ, 5u, args);
}

static inline int ipo_sys_pci_config_write(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint8_t width, uint32_t value) {
    uint32_t args[6] = { bus, slot, func, offset, width, value };
    return ipo_syscall(IPO_SYSCALL_PCI_CONFIG_WRITE, 6u, args);
}

static inline int ipo_sys_pci_enable_bus_master(uint8_t bus, uint8_t slot, uint8_t func) {
    uint32_t args[3] = { bus, slot, func };
    return ipo_syscall(IPO_SYSCALL_PCI_ENABLE_BUS_MASTER, 3u, args);
}

static inline uint32_t ipo_sys_device_get_count(void) {
    return (uint32_t)ipo_syscall(IPO_SYSCALL_DEV_GET_COUNT, 0u, NULL);
}

static inline int ipo_sys_device_get_info(uint32_t index, void *out_info) {
    uint32_t args[2] = { index, (uint32_t)(uintptr_t)out_info };
    return ipo_syscall(IPO_SYSCALL_DEV_GET_INFO, 2u, args);
}

static inline uint32_t ipo_sys_device_register(const void *info) {
    uint32_t args[1] = { (uint32_t)(uintptr_t)info };
    return (uint32_t)ipo_syscall(IPO_SYSCALL_DEV_REGISTER, 1u, args);
}

static inline int ipo_sys_device_unregister(uint32_t dev_id) {
    uint32_t args[1] = { dev_id };
    return ipo_syscall(IPO_SYSCALL_DEV_UNREGISTER, 1u, args);
}

void syscall_reset_user_heap(void);

#endif