#ifndef FS_H
#define FS_H

#include <stddef.h>
#include <stdint.h>

#define MAX_FILENAME_LENGTH 32
#define MAX_FILE_SIZE 4096
#define FS_SECTOR_START 1  // Start from sector 1 (sector 0 is MBR)
#define FS_MAGIC 0x46534950  // "FSIP" - filesystem magic number

typedef struct {
    char name[MAX_FILENAME_LENGTH];
    uint32_t size;
    uint32_t data_offset;
    uint8_t used;
} file_entry_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t file_count;
    uint32_t data_used;
    uint32_t max_files;
} fs_header_t;

typedef struct {
    fs_header_t header;
    file_entry_t* files;
    uint8_t* data_area;
    uint32_t data_area_size;
    int disk_available;
} filesystem_t;

// Initialize filesystem
void fs_init(void);

// Create file
int fs_create_file(const char* filename);

// Delete file
int fs_delete_file(const char* filename);

// Write data to file
int fs_write_file(const char* filename, const void* data, size_t size);

// Read data from file
int fs_read_file(const char* filename, void* buffer, size_t buffer_size);

// Get file size
int fs_get_file_size(const char* filename);

// List files
void fs_list_files(void);

// Check if file exists
int fs_file_exists(const char* filename);

// Save filesystem to disk
int fs_save_to_disk(void);

// Load filesystem from disk
int fs_load_from_disk(void);

// Synchronize with disk
int fs_sync(void);

#endif