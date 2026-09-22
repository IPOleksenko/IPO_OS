#include <file_system/ipo_fs.h>
#include <string.h>
#include <memory/kmalloc.h>

struct ipo_superblock sb;
uint64_t fs_start_lba = 0;
bool fs_mounted = false;
struct ipo_fd *fds = NULL;
uint32_t fds_capacity = 0;

void ipo_fs_init(void) {
    memset(&sb, 0, sizeof(sb));
    fs_mounted = false;
    if (!fds) {
        fds_capacity = 32;
        fds = (struct ipo_fd *)kmalloc(fds_capacity * sizeof(struct ipo_fd));
    }
    if (fds) {
        for (uint32_t i = 0; i < fds_capacity; i++) {
            fds[i].used = (i < 3) ? 1 : 0;
            fds[i].inode = 0;
            fds[i].offset = 0;
            fds[i].flags = 0;
        }
    }
}
