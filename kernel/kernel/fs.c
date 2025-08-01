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
    
    // Initialize directory system
    fs->next_entry_id = 1;  // Start from 1, 0 is reserved for root
    fs->current_dir_id = 0; // Start in root directory
    
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
        
        // Create root directory (entry_id = 0)
        file_entry_t* root = &fs->files[0];
        strcpy(root->name, "/");
        root->size = 0;
        root->data_offset = 0;
        root->parent_id = 0;  // Root is its own parent
        root->entry_id = 0;
        root->type = ENTRY_TYPE_DIRECTORY;
        root->used = 1;
        fs->header.file_count = 1;
        
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
        
        // Create root directory (entry_id = 0)
        file_entry_t* root = &fs->files[0];
        strcpy(root->name, "/");
        root->size = 0;
        root->data_offset = 0;
        root->parent_id = 0;  // Root is its own parent
        root->entry_id = 0;
        root->type = ENTRY_TYPE_DIRECTORY;
        root->used = 1;
        fs->header.file_count = 1;
    }
    
    printf("Filesystem initialized successfully\n");
}

// Validate filename - must be a single word (no spaces)
static int is_valid_filename(const char* name) {
    if (!name || *name == '\0') {
        return 0; // Empty name is invalid
    }
    
    // Check for spaces or other whitespace characters
    for (const char* p = name; *p; p++) {
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
            return 0; // Contains whitespace
        }
    }
    
    // Check for invalid characters (optional - you can add more)
    for (const char* p = name; *p; p++) {
        if (*p == '/' || *p == '\\' || *p == ':' || *p == '*' ||
            *p == '?' || *p == '"' || *p == '<' || *p == '>' || *p == '|') {
            return 0; // Contains invalid characters
        }
    }
    
    return 1; // Valid filename
}

// Find entry by ID
file_entry_t* fs_find_entry_by_id(uint32_t entry_id) {
    if (!fs) return NULL;
    
    for (uint32_t i = 0; i < fs->header.max_files; i++) {
        if (fs->files[i].used && fs->files[i].entry_id == entry_id) {
            return &fs->files[i];
        }
    }
    return NULL;
}

// Find entry by name in specific directory
static file_entry_t* find_entry_in_directory(const char* name, uint32_t parent_id) {
    if (!fs || !name) return NULL;
    
    for (uint32_t i = 0; i < fs->header.max_files; i++) {
        if (fs->files[i].used &&
            fs->files[i].parent_id == parent_id &&
            strcmp(fs->files[i].name, name) == 0) {
            return &fs->files[i];
        }
    }
    return NULL;
}

// Parse path and resolve to parent directory ID and filename
int fs_resolve_path(const char* path, uint32_t* parent_id, char* filename) {
    if (!fs || !path || !parent_id || !filename) return -1;
    
    // Handle absolute vs relative paths
    uint32_t current_dir = (path[0] == '/') ? 0 : fs->current_dir_id;
    
    // Copy path for parsing - allocate dynamically
    size_t path_len = strlen(path);
    char* path_copy = (char*)kmalloc(path_len + 1);
    if (!path_copy) {
        return -1;
    }
    strcpy(path_copy, path);
    
    // Skip leading slash for absolute paths
    char* start = (path_copy[0] == '/') ? path_copy + 1 : path_copy;
    
    // If empty path after removing slash, we're at root
    if (*start == '\0') {
        *parent_id = 0;
        strcpy(filename, "/");
        return 0;
    }
    
    // Parse path components
    char* token = start;
    char* next_slash;
    
    while ((next_slash = strchr((char*)token, '/')) != NULL) {
        *next_slash = '\0';
        
        // Handle special directories
        if (strcmp(token, ".") == 0) {
            // Current directory - do nothing
        } else if (strcmp(token, "..") == 0) {
            // Parent directory
            if (current_dir != 0) {
                file_entry_t* current = fs_find_entry_by_id(current_dir);
                if (current) {
                    current_dir = current->parent_id;
                }
            }
        } else {
            // Regular directory
            file_entry_t* entry = find_entry_in_directory(token, current_dir);
            if (!entry || entry->type != ENTRY_TYPE_DIRECTORY) {
                kfree(path_copy);
                return -1; // Directory not found
            }
            current_dir = entry->entry_id;
        }
        
        token = next_slash + 1;
    }
    
    // The remaining token is the filename
    *parent_id = current_dir;
    strcpy(filename, token);
    kfree(path_copy);
    return 0;
}

// Find entry by full path
file_entry_t* fs_find_entry_by_path(const char* path) {
    if (!path) return NULL;
    
    uint32_t parent_id;
    char filename[MAX_FILENAME_LENGTH];
    
    if (fs_resolve_path(path, &parent_id, filename) != 0) {
        return NULL;
    }
    
    return find_entry_in_directory(filename, parent_id);
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

// Get current working directory path
char* fs_get_current_path(void) {
    static char* path = NULL;
    
    // Free previous path if exists
    if (path) {
        kfree(path);
        path = NULL;
    }
    
    if (!fs) {
        path = (char*)kmalloc(2);
        if (path) strcpy(path, "/");
        return path;
    }
    
    if (fs->current_dir_id == 0) {
        path = (char*)kmalloc(2);
        if (path) strcpy(path, "/");
        return path;
    }
    
    // Build path by traversing up to root
    char components[32][MAX_FILENAME_LENGTH]; // Max 32 directory levels
    int component_count = 0;
    uint32_t current_id = fs->current_dir_id;
    
    while (current_id != 0 && component_count < 32) {
        file_entry_t* entry = fs_find_entry_by_id(current_id);
        if (!entry) break;
        
        strcpy(components[component_count], entry->name);
        component_count++;
        current_id = entry->parent_id;
    }
    
    // Calculate required path length
    size_t path_len = 1; // Start with "/"
    for (int i = component_count - 1; i >= 0; i--) {
        if (path_len > 1) path_len++; // Add "/" separator
        path_len += strlen(components[i]);
    }
    path_len++; // Null terminator
    
    // Allocate and build path string
    path = (char*)kmalloc(path_len);
    if (!path) return NULL;
    
    strcpy(path, "/");
    for (int i = component_count - 1; i >= 0; i--) {
        if (strlen(path) > 1) {
            strcat(path, "/");
        }
        strcat(path, components[i]);
    }
    
    return path;
}

// Create directory
int fs_create_directory(const char* path) {
    if (!fs || !path) return -1;
    
    uint32_t parent_id;
    char dirname[MAX_FILENAME_LENGTH];
    
    if (fs_resolve_path(path, &parent_id, dirname) != 0) {
        printf("Invalid path\n");
        return -1;
    }
    
    // Check dirname length
    if (strlen(dirname) >= MAX_FILENAME_LENGTH) {
        printf("Directory name too long\n");
        return -1;
    }
    
    // Validate directory name
    if (!is_valid_filename(dirname)) {
        printf("Invalid directory name. Name must be a single word without spaces or special characters\n");
        return -1;
    }
    
    // Check if directory already exists
    if (find_entry_in_directory(dirname, parent_id)) {
        printf("Directory already exists\n");
        return -1;
    }
    
    // Find free slot for new directory
    file_entry_t* entry = find_free_slot();
    if (!entry) {
        printf("Failed to allocate directory slot\n");
        return -1;
    }
    
    // Create directory entry
    strcpy(entry->name, dirname);
    entry->size = 0;
    entry->data_offset = 0;
    entry->parent_id = parent_id;
    entry->entry_id = fs->next_entry_id++;
    entry->type = ENTRY_TYPE_DIRECTORY;
    entry->used = 1;
    
    fs->header.file_count++;
    
    // Save changes to disk
    fs_sync();
    
    return 0;
}

// Delete directory (must be empty)
int fs_delete_directory(const char* path) {
    if (!fs || !path) return -1;
    
    file_entry_t* entry = fs_find_entry_by_path(path);
    if (!entry) {
        printf("Directory not found\n");
        return -1;
    }
    
    if (entry->type != ENTRY_TYPE_DIRECTORY) {
        printf("Not a directory\n");
        return -1;
    }
    
    if (entry->entry_id == 0) {
        printf("Cannot delete root directory\n");
        return -1;
    }
    
    // Check if directory is empty
    for (uint32_t i = 0; i < fs->header.max_files; i++) {
        if (fs->files[i].used && fs->files[i].parent_id == entry->entry_id) {
            printf("Directory not empty\n");
            return -1;
        }
    }
    
    // If current directory is being deleted, change to parent
    if (fs->current_dir_id == entry->entry_id) {
        fs->current_dir_id = entry->parent_id;
    }
    
    // Clear directory entry
    memset(entry, 0, sizeof(file_entry_t));
    fs->header.file_count--;
    
    // Save changes to disk
    fs_sync();
    
    return 0;
}

// Rename directory
int fs_rename_directory(const char* old_path, const char* new_name) {
    if (!fs || !old_path || !new_name) return -1;
    
    // Check new name length
    if (strlen(new_name) >= MAX_FILENAME_LENGTH) {
        printf("Directory name too long\n");
        return -1;
    }
    
    // Validate new directory name
    if (!is_valid_filename(new_name)) {
        printf("Invalid directory name. Name must be a single word without spaces or special characters\n");
        return -1;
    }
    
    // Find the directory to rename
    file_entry_t* entry = fs_find_entry_by_path(old_path);
    if (!entry) {
        printf("Directory not found\n");
        return -1;
    }
    
    if (entry->type != ENTRY_TYPE_DIRECTORY) {
        printf("Not a directory\n");
        return -1;
    }
    
    if (entry->entry_id == 0) {
        printf("Cannot rename root directory\n");
        return -1;
    }
    
    // Check if a directory with the new name already exists in the same parent
    if (find_entry_in_directory(new_name, entry->parent_id)) {
        printf("Directory with name '%s' already exists\n", new_name);
        return -1;
    }
    
    // Rename the directory
    strcpy(entry->name, new_name);
    
    // Save changes to disk
    fs_sync();
    
    return 0;
}

// Rename file
int fs_rename_file(const char* old_path, const char* new_name) {
    if (!fs || !old_path || !new_name) return -1;
    
    // Check new name length
    if (strlen(new_name) >= MAX_FILENAME_LENGTH) {
        printf("Filename too long\n");
        return -1;
    }
    
    // Validate new filename
    if (!is_valid_filename(new_name)) {
        printf("Invalid filename. Name must be a single word without spaces or special characters\n");
        return -1;
    }
    
    // Find the file to rename
    file_entry_t* entry = fs_find_entry_by_path(old_path);
    if (!entry) {
        printf("File not found\n");
        return -1;
    }
    
    if (entry->type != ENTRY_TYPE_FILE) {
        printf("Not a file\n");
        return -1;
    }
    
    // Check if a file with the new name already exists in the same parent
    if (find_entry_in_directory(new_name, entry->parent_id)) {
        printf("File with name '%s' already exists\n", new_name);
        return -1;
    }
    
    // Rename the file
    strcpy(entry->name, new_name);
    
    // Save changes to disk
    fs_sync();
    
    return 0;
}

// Change current directory
int fs_change_directory(const char* path) {
    if (!fs) return -1;
    
    // Handle special cases
    if (!path) {
        // No path provided - go to root
        fs->current_dir_id = 0;
        return 0;
    }
    
    if (strcmp(path, ".") == 0) {
        // Current directory - do nothing
        return 0;
    }
    
    if (strcmp(path, "..") == 0) {
        // Parent directory
        if (fs->current_dir_id == 0) {
            // Already at root
            return 0;
        }
        file_entry_t* current = fs_find_entry_by_id(fs->current_dir_id);
        if (current) {
            fs->current_dir_id = current->parent_id;
            return 0;
        }
        return -1;
    }
    
    if (strcmp(path, "/") == 0) {
        // Root directory
        fs->current_dir_id = 0;
        return 0;
    }
    
    // Regular path resolution
    file_entry_t* entry = fs_find_entry_by_path(path);
    if (!entry) {
        printf("Directory not found\n");
        return -1;
    }
    
    if (entry->type != ENTRY_TYPE_DIRECTORY) {
        printf("Not a directory\n");
        return -1;
    }
    
    fs->current_dir_id = entry->entry_id;
    return 0;
}

// List directory contents
void fs_list_directory(const char* path) {
    if (!fs) {
        printf("Filesystem not initialized\n");
        return;
    }
    
    uint32_t dir_id;
    if (path) {
        file_entry_t* entry = fs_find_entry_by_path(path);
        if (!entry) {
            printf("Directory not found\n");
            return;
        }
        if (entry->type != ENTRY_TYPE_DIRECTORY) {
            printf("Not a directory\n");
            return;
        }
        dir_id = entry->entry_id;
    } else {
        dir_id = fs->current_dir_id;
    }
    
    printf("Contents of %s:\n", path ? path : fs_get_current_path());
    int entry_count = 0;
    
    // Show parent directory if not root
    if (dir_id != 0) {
        printf("  .. (parent directory)\n");
    }
    
    for (uint32_t i = 0; i < fs->header.max_files; i++) {
        if (fs->files[i].used && fs->files[i].parent_id == dir_id) {
            // Skip root directory entry (entry_id = 0) when listing
            if (fs->files[i].entry_id == 0) {
                continue;
            }
            
            if (fs->files[i].type == ENTRY_TYPE_DIRECTORY) {
                printf("  %s/ (directory)\n", fs->files[i].name);
            } else {
                printf("  %s (%d bytes)\n", fs->files[i].name, fs->files[i].size);
            }
            entry_count++;
        }
    }
    
    if (entry_count == 0 && dir_id != 0) {
        printf("  (empty)\n");
    }
}

int fs_create_file(const char* path) {
    if (!fs || !path) return -1;
    
    uint32_t parent_id;
    char filename[MAX_FILENAME_LENGTH];
    
    if (fs_resolve_path(path, &parent_id, filename) != 0) {
        printf("Invalid path\n");
        return -1;
    }
    
    // Check filename length
    if (strlen(filename) >= MAX_FILENAME_LENGTH) {
        printf("Filename too long\n");
        return -1;
    }
    
    // Validate filename
    if (!is_valid_filename(filename)) {
        printf("Invalid filename. Name must be a single word without spaces or special characters\n");
        return -1;
    }
    
    // Check if file already exists
    if (find_entry_in_directory(filename, parent_id)) {
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
    entry->parent_id = parent_id;
    entry->entry_id = fs->next_entry_id++;
    entry->type = ENTRY_TYPE_FILE;
    entry->used = 1;
    
    fs->header.file_count++;
    
    // Save changes to disk
    fs_sync();
    
    return 0;
}

int fs_delete_file(const char* path) {
    if (!fs || !path) return -1;
    
    file_entry_t* entry = fs_find_entry_by_path(path);
    if (!entry) {
        printf("File not found\n");
        return -1;
    }
    
    if (entry->type != ENTRY_TYPE_FILE) {
        printf("Not a file\n");
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

int fs_write_file(const char* path, const void* data, size_t size) {
    if (!fs || !path || !data || size == 0) return -1;
    
    file_entry_t* entry = fs_find_entry_by_path(path);
    if (!entry) {
        printf("File not found\n");
        return -1;
    }
    
    if (entry->type != ENTRY_TYPE_FILE) {
        printf("Not a file\n");
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

int fs_read_file(const char* path, void* buffer, size_t buffer_size) {
    if (!fs || !path || !buffer) return -1;
    
    file_entry_t* entry = fs_find_entry_by_path(path);
    if (!entry) {
        printf("File not found\n");
        return -1;
    }
    
    if (entry->type != ENTRY_TYPE_FILE) {
        printf("Not a file\n");
        return -1;
    }
    
    if (entry->size == 0) {
        return 0; // Empty file
    }
    
    size_t bytes_to_read = (entry->size < buffer_size) ? entry->size : buffer_size;
    memcpy(buffer, fs->data_area + entry->data_offset, bytes_to_read);
    
    return bytes_to_read;
}

int fs_get_file_size(const char* path) {
    if (!fs || !path) return -1;
    
    file_entry_t* entry = fs_find_entry_by_path(path);
    if (!entry) {
        return -1;
    }
    
    if (entry->type != ENTRY_TYPE_FILE) {
        return -1;
    }
    
    return entry->size;
}

void fs_list_files(void) {
    fs_list_directory(NULL);  // List current directory
}

int fs_file_exists(const char* path) {
    file_entry_t* entry = fs_find_entry_by_path(path);
    return (entry != NULL && entry->type == ENTRY_TYPE_FILE);
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
    
    // Initialize next_entry_id based on loaded entries
    fs->next_entry_id = 1;
    for (uint32_t i = 0; i < fs->header.max_files; i++) {
        if (fs->files && fs->files[i].used && fs->files[i].entry_id >= fs->next_entry_id) {
            fs->next_entry_id = fs->files[i].entry_id + 1;
        }
    }
    
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
        return -1;
    }
    
    if (!fs->disk_available) {
        // Disk not available - return success for memory-only mode
        return 0;
    }
    
    int result = fs_save_to_disk();
    if (result != 0) {
        // If synchronization fails, disconnect the disk
        fs->disk_available = 0;
    }
    
    return result;
}