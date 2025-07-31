#include <string.h>
#include <stdio.h>
#include <kernel/sys/fs.h>
#include <kernel/sys/kheap.h>
#include <kernel/drv/ata.h>

#define INITIAL_MAX_FILES 16

static filesystem_t* fs = NULL;

void fs_init(void) {
    // Allocate memory for filesystem structure
    fs = (filesystem_t*)kmalloc(sizeof(filesystem_t));
    if (!fs) {
        printf("Failed to initialize filesystem: out of memory\n");
        return;
    }
    
    // Initialize filesystem structure
    memset(fs, 0, sizeof(filesystem_t));
    
    // Allocate data area for file contents
    fs->data_area_size = 1024 * 1024;
    fs->data_area = (uint8_t*)kmalloc(fs->data_area_size);
    if (!fs->data_area) {
        printf("Failed to allocate data area for filesystem\n");
        kfree(fs);
        fs = NULL;
        return;
    }
    
    // Allocate initial file table
    fs->header.max_files = INITIAL_MAX_FILES;
    fs->files = (file_entry_t*)kmalloc(fs->header.max_files * sizeof(file_entry_t));
    if (!fs->files) {
        printf("Failed to allocate file table\n");
        kfree(fs->data_area);
        kfree(fs);
        fs = NULL;
        return;
    }
    memset(fs->files, 0, fs->header.max_files * sizeof(file_entry_t));
    
    // Check disk availability by attempting a simple read operation
    fs->disk_available = 0;
    
    // Try to read first sector to test disk availability
    uint8_t* test_buffer = (uint8_t*)kmalloc(SECTOR_SIZE);
    if (test_buffer) {
        if (ata_read_sectors(0, 1, test_buffer) == 0) {
            fs->disk_available = 1;
            printf("Disk available, attempting to load filesystem...\n");
        } else {
            printf("Disk not available, using memory-only mode\n");
        }
        kfree(test_buffer);
    }
    
    // Try to load filesystem from disk if available
    if (fs->disk_available && fs_load_from_disk() != 0) {
        // If loading failed, create new filesystem
        printf("Creating new filesystem...\n");
        fs->header.magic = FS_MAGIC;
        fs->header.version = 1;
        fs->header.file_count = 0;
        fs->header.data_used = 0;
        // max_files is already set during allocation
        
        // Save new filesystem to disk
        if (fs_save_to_disk() != 0) {
            printf("Warning: Failed to save filesystem to disk, continuing in memory-only mode\n");
            fs->disk_available = 0;
        }
    } else if (!fs->disk_available) {
        // Create memory-only filesystem
        printf("Creating memory-only filesystem...\n");
        fs->header.magic = FS_MAGIC;
        fs->header.version = 1;
        fs->header.file_count = 0;
        fs->header.data_used = 0;
        // max_files is already set during allocation
    }
    
    printf("Filesystem initialized successfully\n");
}

static file_entry_t* find_file(const char* filename) {
    if (!fs || !filename) return NULL;
    
    for (uint32_t i = 0; i < fs->header.max_files; i++) {
        if (fs->files[i].used && strcmp(fs->files[i].name, filename) == 0) {
            return &fs->files[i];
        }
    }
    return NULL;
}

static file_entry_t* find_free_slot(void) {
    if (!fs) return NULL;
    
    // First try to find existing free slot
    for (uint32_t i = 0; i < fs->header.max_files; i++) {
        if (!fs->files[i].used) {
            return &fs->files[i];
        }
    }
    
    // If no free slot, expand file table
    uint32_t old_max_files = fs->header.max_files;
    uint32_t new_max_files = (old_max_files == 0) ? INITIAL_MAX_FILES : old_max_files * 2;
    
    file_entry_t* new_files = (file_entry_t*)kmalloc(new_max_files * sizeof(file_entry_t));
    if (!new_files) {
        printf("Failed to expand file table\n");
        return NULL;
    }
    
    // Initialize all new slots
    memset(new_files, 0, new_max_files * sizeof(file_entry_t));
    
    // Copy existing files if any
    if (fs->files && old_max_files > 0) {
        memcpy(new_files, fs->files, old_max_files * sizeof(file_entry_t));
        kfree(fs->files);
    }
    
    // Update filesystem
    fs->files = new_files;
    fs->header.max_files = new_max_files;
    
    printf("Expanded file table to %d files\n", fs->header.max_files);
    
    // Return first new slot
    return &fs->files[old_max_files];
}

int fs_create_file(const char* filename) {
    if (!fs || !filename) return -1;
    
    // Check filename length
    if (strlen(filename) >= MAX_FILENAME_LENGTH) {
        printf("Filename too long\n");
        return -1;
    }
    
    // Check if file already exists
    if (find_file(filename)) {
        printf("File already exists\n");
        return -1;
    }
    
    // Find free slot for new file
    file_entry_t* entry = find_free_slot();
    if (!entry) {
        printf("Failed to allocate file slot\n");
        return -1;
    }
    
    // Create file entry
    strcpy(entry->name, filename);
    entry->size = 0;
    entry->data_offset = 0;
    entry->used = 1;
    
    fs->header.file_count++;
    
    // Save changes to disk
    fs_sync();
    
    return 0;
}

int fs_delete_file(const char* filename) {
    if (!fs || !filename) return -1;
    
    file_entry_t* entry = find_file(filename);
    if (!entry) {
        printf("File not found\n");
        return -1;
    }
    
    // Free space in data area
    if (entry->size > 0) {
        // Shift data of other files
        uint32_t move_start = entry->data_offset + entry->size;
        uint32_t move_size = fs->header.data_used - move_start;
        
        if (move_size > 0) {
            memmove(fs->data_area + entry->data_offset,
                   fs->data_area + move_start,
                   move_size);
            
            // Update offsets of other files
            for (uint32_t i = 0; i < fs->header.max_files; i++) {
                if (fs->files[i].used && fs->files[i].data_offset > entry->data_offset) {
                    fs->files[i].data_offset -= entry->size;
                }
            }
        }
        
        fs->header.data_used -= entry->size;
    }
    
    // Clear file entry
    memset(entry, 0, sizeof(file_entry_t));
    fs->header.file_count--;
    
    // Save changes to disk
    fs_sync();
    
    return 0;
}

int fs_write_file(const char* filename, const void* data, size_t size) {
    if (!fs || !filename || !data || size == 0) return -1;
    
    if (size > MAX_FILE_SIZE) {
        printf("File size too large\n");
        return -1;
    }
    
    file_entry_t* entry = find_file(filename);
    if (!entry) {
        printf("File not found\n");
        return -1;
    }
    
    // Check if there's enough space
    uint32_t space_needed = size;
    uint32_t space_freed = entry->size;
    
    if (fs->header.data_used - space_freed + space_needed > fs->data_area_size) {
        printf("Not enough space\n");
        return -1;
    }
    
    // If file already contains data, free old space
    if (entry->size > 0) {
        uint32_t move_start = entry->data_offset + entry->size;
        uint32_t move_size = fs->header.data_used - move_start;
        
        if (move_size > 0) {
            memmove(fs->data_area + entry->data_offset,
                   fs->data_area + move_start,
                   move_size);
            
            // Update offsets of other files
            for (uint32_t i = 0; i < fs->header.max_files; i++) {
                if (fs->files[i].used && fs->files[i].data_offset > entry->data_offset) {
                    fs->files[i].data_offset -= entry->size;
                }
            }
        }
        
        fs->header.data_used -= entry->size;
    }
    
    // Write new data to end of data area
    entry->data_offset = fs->header.data_used;
    entry->size = size;
    
    memcpy(fs->data_area + entry->data_offset, data, size);
    fs->header.data_used += size;
    
    // Save changes to disk
    fs_sync();
    
    return 0;
}

int fs_read_file(const char* filename, void* buffer, size_t buffer_size) {
    if (!fs || !filename || !buffer) return -1;
    
    file_entry_t* entry = find_file(filename);
    if (!entry) {
        printf("File not found\n");
        return -1;
    }
    
    if (entry->size == 0) {
        return 0; // Empty file
    }
    
    size_t bytes_to_read = (entry->size < buffer_size) ? entry->size : buffer_size;
    memcpy(buffer, fs->data_area + entry->data_offset, bytes_to_read);
    
    return bytes_to_read;
}

int fs_get_file_size(const char* filename) {
    if (!fs || !filename) return -1;
    
    file_entry_t* entry = find_file(filename);
    if (!entry) {
        return -1;
    }
    
    return entry->size;
}

void fs_list_files(void) {
    if (!fs) {
        printf("Filesystem not initialized\n");
        return;
    }
    
    printf("Files:\n");
    int file_count = 0;
    
    for (uint32_t i = 0; i < fs->header.max_files; i++) {
        if (fs->files[i].used) {
            printf("  %s (%d bytes)\n", fs->files[i].name, fs->files[i].size);
            file_count++;
        }
    }
    
    if (file_count == 0) {
        printf("  No files found\n");
    }
    
    printf("Total files: %d (max: %d)\n", fs->header.file_count, fs->header.max_files);
    printf("Data used: %d/%d bytes\n", fs->header.data_used, fs->data_area_size);
    printf("Disk available: %s\n", fs->disk_available ? "Yes" : "No");
}

int fs_file_exists(const char* filename) {
    return find_file(filename) != NULL;
}
int fs_save_to_disk(void) {
    if (!fs || !fs->disk_available) {
        return -1;
    }
    
    printf("fs_save_to_disk: saving %d files, %d bytes of data\n", fs->header.file_count, fs->header.data_used);
    
    // Prepare buffer for writing
    uint8_t* sector_buffer = (uint8_t*)kmalloc(SECTOR_SIZE);
    if (!sector_buffer) {
        printf("Failed to allocate sector buffer\n");
        return -1;
    }
    
    // Write filesystem header
    memset(sector_buffer, 0, SECTOR_SIZE);
    memcpy(sector_buffer, &fs->header, sizeof(fs_header_t));
    
    printf("fs_save_to_disk: writing header to sector %d\n", FS_SECTOR_START);
    if (ata_write_sectors(FS_SECTOR_START, 1, sector_buffer) != 0) {
        printf("Failed to write filesystem header to disk\n");
        kfree(sector_buffer);
        return -1;
    }
    printf("fs_save_to_disk: header written successfully\n");
    
    // Write file table
    uint32_t file_table_size = fs->header.max_files * sizeof(file_entry_t);
    uint32_t file_sectors_needed = (file_table_size + SECTOR_SIZE - 1) / SECTOR_SIZE;
    
    uint8_t* file_buffer = (uint8_t*)kmalloc(file_sectors_needed * SECTOR_SIZE);
    if (!file_buffer) {
        printf("Failed to allocate file table buffer\n");
        kfree(sector_buffer);
        return -1;
    }
    
    memset(file_buffer, 0, file_sectors_needed * SECTOR_SIZE);
    memcpy(file_buffer, fs->files, file_table_size);
    
    printf("fs_save_to_disk: writing %d sectors of file table starting at sector %d\n", file_sectors_needed, FS_SECTOR_START + 1);
    if (ata_write_sectors(FS_SECTOR_START + 1, file_sectors_needed, file_buffer) != 0) {
        printf("Failed to write file table to disk\n");
        kfree(file_buffer);
        kfree(sector_buffer);
        return -1;
    }
    printf("fs_save_to_disk: file table written successfully\n");
    kfree(file_buffer);
    
    // Write file data
    uint32_t data_sectors_needed = (fs->header.data_used + SECTOR_SIZE - 1) / SECTOR_SIZE;
    if (data_sectors_needed > 0) {
        // Copy data to sector-aligned buffer
        uint8_t* data_buffer = (uint8_t*)kmalloc(data_sectors_needed * SECTOR_SIZE);
        if (!data_buffer) {
            printf("Failed to allocate data buffer\n");
            kfree(sector_buffer);
            return -1;
        }
        
        memset(data_buffer, 0, data_sectors_needed * SECTOR_SIZE);
        memcpy(data_buffer, fs->data_area, fs->header.data_used);
        
        uint32_t data_start_sector = FS_SECTOR_START + 1 + file_sectors_needed;
        printf("fs_save_to_disk: writing %d sectors of data starting at sector %d\n", data_sectors_needed, data_start_sector);
        if (ata_write_sectors(data_start_sector, data_sectors_needed, data_buffer) != 0) {
            printf("Failed to write filesystem data to disk\n");
            kfree(data_buffer);
            kfree(sector_buffer);
            return -1;
        }
        printf("fs_save_to_disk: data written successfully\n");
        
        kfree(data_buffer);
    }
    
    kfree(sector_buffer);
    printf("fs_save_to_disk: filesystem saved successfully\n");
    return 0;
}

int fs_load_from_disk(void) {
    if (!fs || !fs->disk_available) {
        return -1;
    }
    
    // Prepare buffer for reading
    uint8_t* sector_buffer = (uint8_t*)kmalloc(SECTOR_SIZE);
    if (!sector_buffer) {
        printf("Failed to allocate sector buffer\n");
        return -1;
    }
    
    // Read filesystem header
    if (ata_read_sectors(FS_SECTOR_START, 1, sector_buffer) != 0) {
        printf("Failed to read filesystem header from disk\n");
        kfree(sector_buffer);
        return -1;
    }
    
    // Check magic number
    fs_header_t* header = (fs_header_t*)sector_buffer;
    if (header->magic != FS_MAGIC) {
        printf("Invalid filesystem magic number\n");
        kfree(sector_buffer);
        return -1;
    }
    
    // Copy header
    memcpy(&fs->header, header, sizeof(fs_header_t));
    
    // Allocate file table based on loaded max_files
    if (fs->files) {
        kfree(fs->files);
    }
    fs->files = (file_entry_t*)kmalloc(fs->header.max_files * sizeof(file_entry_t));
    if (!fs->files) {
        printf("Failed to allocate file table\n");
        kfree(sector_buffer);
        return -1;
    }
    
    // Read file table
    uint32_t file_table_size = fs->header.max_files * sizeof(file_entry_t);
    uint32_t file_sectors_needed = (file_table_size + SECTOR_SIZE - 1) / SECTOR_SIZE;
    
    uint8_t* file_buffer = (uint8_t*)kmalloc(file_sectors_needed * SECTOR_SIZE);
    if (!file_buffer) {
        printf("Failed to allocate file table buffer\n");
        kfree(sector_buffer);
        return -1;
    }
    
    if (ata_read_sectors(FS_SECTOR_START + 1, file_sectors_needed, file_buffer) != 0) {
        printf("Failed to read file table from disk\n");
        kfree(file_buffer);
        kfree(sector_buffer);
        return -1;
    }
    
    memcpy(fs->files, file_buffer, file_table_size);
    kfree(file_buffer);
    
    // Read file data
    if (fs->header.data_used > 0) {
        uint32_t data_sectors_needed = (fs->header.data_used + SECTOR_SIZE - 1) / SECTOR_SIZE;
        uint8_t* data_buffer = (uint8_t*)kmalloc(data_sectors_needed * SECTOR_SIZE);
        if (!data_buffer) {
            printf("Failed to allocate data buffer\n");
            kfree(sector_buffer);
            return -1;
        }
        
        uint32_t data_start_sector = FS_SECTOR_START + 1 + file_sectors_needed;
        if (ata_read_sectors(data_start_sector, data_sectors_needed, data_buffer) != 0) {
            printf("Failed to read filesystem data from disk\n");
            kfree(data_buffer);
            kfree(sector_buffer);
            return -1;
        }
        
        // Copy data to filesystem data area
        memcpy(fs->data_area, data_buffer, fs->header.data_used);
        
        kfree(data_buffer);
    }
    
    kfree(sector_buffer);
    printf("Filesystem loaded from disk successfully\n");
    return 0;
}

int fs_sync(void) {
    if (!fs) {
        printf("fs_sync: filesystem not initialized\n");
        return -1;
    }
    
    if (!fs->disk_available) {
        printf("fs_sync: disk not available, skipping sync\n");
        return -1;
    }
    
    printf("fs_sync: syncing filesystem to disk...\n");
    int result = fs_save_to_disk();
    if (result == 0) {
        printf("fs_sync: filesystem synced successfully\n");
    } else {
        printf("fs_sync: failed to sync filesystem\n");
        // If synchronization fails, disconnect the disk
        fs->disk_available = 0;
    }
    
    return result;
}