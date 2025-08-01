#ifndef FS_H
#define FS_H

#include <stddef.h>
#include <stdint.h>

#define MAX_FILENAME_LENGTH 32
#define FS_SECTOR_START 1  // Start from sector 1 (sector 0 is MBR)
#define FS_MAGIC 0x46534950  // "FSIP" - filesystem magic number

// Entry types
#define ENTRY_TYPE_FILE 1
#define ENTRY_TYPE_DIRECTORY 2

typedef struct {
    char name[MAX_FILENAME_LENGTH];
    uint32_t size;
    uint32_t data_offset;
    uint32_t parent_id;  // ID of parent directory (0 for root)
    uint32_t entry_id;   // Unique entry ID
    uint8_t type;        // ENTRY_TYPE_FILE or ENTRY_TYPE_DIRECTORY
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
    uint32_t next_entry_id;  // Next available entry ID
    uint32_t current_dir_id; // Current working directory ID (0 for root)
    int disk_available;
} filesystem_t;

// Initialize filesystem
void fs_init(void);

// File operations
int fs_create_file(const char* path);
int fs_delete_file(const char* path);
int fs_rename_file(const char* old_path, const char* new_name);
int fs_move_entry(const char* source_path, const char* dest_path);
int fs_write_file(const char* path, const void* data, size_t size);
int fs_read_file(const char* path, void* buffer, size_t buffer_size);
int fs_get_file_size(const char* path);
int fs_file_exists(const char* path);

// Directory operations
int fs_create_directory(const char* path);
int fs_delete_directory(const char* path);
int fs_rename_directory(const char* old_path, const char* new_name);
int fs_change_directory(const char* path);
void fs_list_directory(const char* path);
void fs_list_files(void);  // List current directory
char* fs_get_current_path(void);

// Path utilities
int fs_resolve_path(const char* path, uint32_t* parent_id, char* filename);
file_entry_t* fs_find_entry_by_path(const char* path);
file_entry_t* fs_find_entry_by_id(uint32_t entry_id);

// Disk operations
int fs_save_to_disk(void);
int fs_load_from_disk(void);
int fs_sync(void);

#endif