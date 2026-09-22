#include <file_system/ipo_fs.h>
#include <string.h>

struct ipo_superblock sb;
uint64_t fs_start_lba = 0;
bool fs_mounted = false;
struct ipo_fd fds[IPO_MAX_FDS];

void ipo_fs_init(void) {
    memset(&sb, 0, sizeof(sb));
    fs_mounted = false;
    for (int i = 0; i < IPO_MAX_FDS; i++) {
        fds[i].used = (i < 3) ? 1 : 0;
        fds[i].inode = 0;
        fds[i].offset = 0;
        fds[i].flags = 0;
    }
}
