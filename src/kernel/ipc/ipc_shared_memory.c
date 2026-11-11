#include <kernel/ipc.h>
#include <kernel/process.h>
#include <kernel/smp.h>
#include <ipc.h>
#include <memory/address_space.h>
#include <memory/kmalloc.h>
#include <syscall.h>
#include <string.h>

#define IPC_INITIAL_SHM_CAPACITY 8u
#define IPC_INITIAL_SHM_CLIENT_CAPACITY 4u
#define IPC_SHM_GENERATION_MAX 0x000FFFFFu
#ifndef IPC_SHM_VIRTUAL_BASE
#define IPC_SHM_VIRTUAL_BASE 0xD0000000u
#endif
#define IPC_SHM_PAGE_SIZE 4096u

typedef struct {
    uint32_t pid;
    uint32_t rights;
    bool attached;
} ipc_shm_client_t;

typedef struct {
    bool active;
    bool destroyed;
    bool retired;
    uint32_t id;
    uint32_t generation;
    uint32_t owner_pid;
    uint32_t public_rights;
    uint32_t size;
    uint32_t virtual_base;
    char *name;
    uint8_t *memory;
    uint8_t *allocation;
    ipc_shm_client_t *clients;
    uint32_t client_capacity;
} ipc_shm_region_t;

static spinlock_t ipc_shm_lock;
static ipc_shm_region_t *ipc_shm_regions;
static uint32_t ipc_shm_capacity;
static bool ipc_shm_ready;

static ipc_shm_region_t *ipc_shm_find_id_locked(uint32_t id) {
    for (uint32_t i = 0u; i < ipc_shm_capacity; i++) {
        if (ipc_shm_regions[i].active && ipc_shm_regions[i].id == id)
            return &ipc_shm_regions[i];
    }
    return NULL;
}

static ipc_shm_region_t *ipc_shm_find_name_locked(const char *name) {
    for (uint32_t i = 0u; i < ipc_shm_capacity; i++) {
        ipc_shm_region_t *region = &ipc_shm_regions[i];
        if (region->active && !region->destroyed && region->name &&
            strcmp(region->name, name) == 0)
            return region;
    }
    return NULL;
}

static void ipc_shm_clear_locked(ipc_shm_region_t *region) {
    uint32_t generation = region->generation;
    bool retired = region->retired || generation >= IPC_SHM_GENERATION_MAX;
    uint8_t *allocation = region->allocation;
    ipc_shm_client_t *clients = region->clients;
    char *name = region->name;
    memset(region, 0, sizeof(*region));
    region->generation = generation;
    region->retired = retired;
    if (name) kfree(name);
    if (clients) kfree(clients);
    if (allocation) kfree(allocation);
}

static uint32_t ipc_shm_find_free_vaddr_locked(uint32_t size_aligned) {
    uint32_t candidate = IPC_SHM_VIRTUAL_BASE;
    bool collision;
    do {
        collision = false;
        if (candidate > 0xF0000000u - size_aligned) {
            return 0u;
        }
        for (uint32_t i = 0u; i < ipc_shm_capacity; i++) {
            ipc_shm_region_t *r = &ipc_shm_regions[i];
            if (!r->active || r->virtual_base == 0u) continue;
            uint32_t r_aligned = (r->size + IPC_SHM_PAGE_SIZE - 1u) &
                                 ~(IPC_SHM_PAGE_SIZE - 1u);
            if (candidate < r->virtual_base + r_aligned &&
                candidate + size_aligned > r->virtual_base) {
                candidate = (r->virtual_base + r_aligned + IPC_SHM_PAGE_SIZE - 1u) &
                            ~(IPC_SHM_PAGE_SIZE - 1u);
                collision = true;
                break;
            }
        }
    } while (collision);
    return candidate;
}

static bool ipc_shm_map_locked(ipc_shm_region_t *region, process_t *proc,
                               uint32_t rights) {
    if (!region || !proc || !proc->address_space || !region->memory)
        return false;

    uint32_t pages = (region->size + IPC_SHM_PAGE_SIZE - 1u) /
                     IPC_SHM_PAGE_SIZE;
    uint32_t virtual_address = region->virtual_base;
    uint32_t physical_address = (uint32_t)(uintptr_t)region->memory;
    uint32_t flags = ADDRESS_SPACE_PAGE_USER;
    if ((rights & IPO_SHM_RIGHT_WRITE) != 0u) {
        flags |= ADDRESS_SPACE_PAGE_WRITABLE;
    }

    for (uint32_t page = 0u; page < pages; page++) {
        if (page > (UINT32_MAX - virtual_address) / IPC_SHM_PAGE_SIZE ||
            page > (UINT32_MAX - physical_address) / IPC_SHM_PAGE_SIZE ||
            !address_space_map(proc->address_space,
                               virtual_address + page * IPC_SHM_PAGE_SIZE,
                               physical_address + page * IPC_SHM_PAGE_SIZE,
                               flags)) {
            while (page > 0u) {
                page--;
                (void)address_space_unmap(
                    proc->address_space,
                    virtual_address + page * IPC_SHM_PAGE_SIZE);
            }
            return false;
        }
    }
    return true;
}

static bool ipc_shm_unmap_locked(ipc_shm_region_t *region, process_t *proc) {
    if (!region || !proc || !proc->address_space) return false;
    uint32_t pages = (region->size + IPC_SHM_PAGE_SIZE - 1u) /
                     IPC_SHM_PAGE_SIZE;
    uint32_t virtual_address = region->virtual_base;
    bool result = true;
    for (uint32_t page = 0u; page < pages; page++) {
        if (!address_space_unmap(proc->address_space,
                                 virtual_address + page * IPC_SHM_PAGE_SIZE))
            result = false;
    }
    return result;
}

static bool ipc_shm_has_clients(const ipc_shm_region_t *region) {
    if (!region || !region->clients) return false;
    for (uint32_t i = 0u; i < region->client_capacity; i++) {
        if (region->clients[i].pid != 0u) return true;
    }
    return false;
}

static ipc_shm_client_t *ipc_shm_find_client_locked(
    ipc_shm_region_t *region, uint32_t pid) {
    if (!region || !region->clients) return NULL;
    for (uint32_t i = 0u; i < region->client_capacity; i++) {
        if (region->clients[i].pid == pid) return &region->clients[i];
    }
    return NULL;
}

static int ipc_shm_create(const char *user_name, uint32_t size,
                          uint32_t public_rights) {
    process_t *proc = process_get_current();
    if (!proc || size == 0u || size > 0x10000000u ||
        (public_rights & ~(IPO_SHM_RIGHT_READ | IPO_SHM_RIGHT_WRITE)) != 0u ||
        size > UINT32_MAX - (IPC_SHM_PAGE_SIZE - 1u))
        return IPO_IPC_ERR_INVALID;
    char *name = ipc_copy_name_alloc(user_name);
    if (!name) return IPO_IPC_ERR_INVALID;

    uint32_t aligned_size = (size + IPC_SHM_PAGE_SIZE - 1u) &
                            ~(IPC_SHM_PAGE_SIZE - 1u);
    uint8_t *allocation = (uint8_t *)kmalloc(aligned_size + IPC_SHM_PAGE_SIZE);
    if (!allocation) {
        kfree(name);
        return IPO_IPC_ERR_NO_SPACE;
    }
    uint8_t *memory = (uint8_t *)(((uintptr_t)allocation +
                                  IPC_SHM_PAGE_SIZE - 1u) &
                                 ~(uintptr_t)(IPC_SHM_PAGE_SIZE - 1u));
    memset(memory, 0, size);

    spin_lock(&ipc_shm_lock);
    if (ipc_shm_find_name_locked(name)) {
        spin_unlock(&ipc_shm_lock);
        kfree(name);
        kfree(allocation);
        return IPO_IPC_ERR_EXISTS;
    }

    uint32_t vbase = ipc_shm_find_free_vaddr_locked(aligned_size);
    if (vbase == 0u) {
        spin_unlock(&ipc_shm_lock);
        kfree(name);
        kfree(allocation);
        return IPO_IPC_ERR_NO_SPACE;
    }

    uint32_t slot = UINT32_MAX;
    for (uint32_t i = 0u; i < ipc_shm_capacity; i++) {
        if (!ipc_shm_regions[i].active && !ipc_shm_regions[i].retired) {
            slot = i;
            break;
        }
    }
    if (slot == UINT32_MAX) {
        uint32_t new_cap = ipc_shm_capacity ? (ipc_shm_capacity * 2u)
                                            : IPC_INITIAL_SHM_CAPACITY;
        if (new_cap <= ipc_shm_capacity ||
            new_cap > (SIZE_MAX / sizeof(*ipc_shm_regions))) {
            spin_unlock(&ipc_shm_lock);
            kfree(name);
            kfree(allocation);
            return IPO_IPC_ERR_NO_SPACE;
        }
        ipc_shm_region_t *grown = (ipc_shm_region_t *)krealloc(
            ipc_shm_regions, new_cap * sizeof(*ipc_shm_regions));
        if (!grown) {
            spin_unlock(&ipc_shm_lock);
            kfree(name);
            kfree(allocation);
            return IPO_IPC_ERR_NO_SPACE;
        }
        memset(grown + ipc_shm_capacity, 0,
               (new_cap - ipc_shm_capacity) * sizeof(*ipc_shm_regions));
        ipc_shm_regions = grown;
        slot = ipc_shm_capacity;
        ipc_shm_capacity = new_cap;
    }

    ipc_shm_region_t *region = &ipc_shm_regions[slot];
    uint32_t generation = region->generation + 1u;
    if (generation == 0u || generation > IPC_SHM_GENERATION_MAX) {
        region->retired = true;
        spin_unlock(&ipc_shm_lock);
        kfree(name);
        kfree(allocation);
        return IPO_IPC_ERR_NO_SPACE;
    }
    ipc_shm_client_t *clients = (ipc_shm_client_t *)kmalloc(
        IPC_INITIAL_SHM_CLIENT_CAPACITY * sizeof(ipc_shm_client_t));
    if (!clients) {
        spin_unlock(&ipc_shm_lock);
        kfree(name);
        kfree(allocation);
        return IPO_IPC_ERR_NO_SPACE;
    }
    memset(clients, 0, IPC_INITIAL_SHM_CLIENT_CAPACITY * sizeof(ipc_shm_client_t));
    memset(region, 0, sizeof(*region));
    region->active = true;
    region->id = ((generation - 1u) << 11) + slot + 1u;
    region->generation = generation;
    region->owner_pid = proc->pid;
    region->public_rights = public_rights;
    region->size = size;
    region->virtual_base = vbase;
    region->memory = memory;
    region->allocation = allocation;
    region->clients = clients;
    region->client_capacity = IPC_INITIAL_SHM_CLIENT_CAPACITY;
    region->name = name;
    region->clients[0].pid = proc->pid;
    region->clients[0].rights = IPO_SHM_RIGHT_READ | IPO_SHM_RIGHT_WRITE;
    region->clients[0].attached = false;
    uint32_t id = region->id;
    spin_unlock(&ipc_shm_lock);
    return (int)id;
}

static int ipc_shm_open(const char *user_name, uint32_t rights) {
    process_t *proc = process_get_current();
    if (!proc || rights == 0u ||
        (rights & ~(IPO_SHM_RIGHT_READ | IPO_SHM_RIGHT_WRITE)) != 0u)
        return IPO_IPC_ERR_INVALID;
    char *name = ipc_copy_name_alloc(user_name);
    if (!name) return IPO_IPC_ERR_INVALID;

    spin_lock(&ipc_shm_lock);
    ipc_shm_region_t *region = ipc_shm_find_name_locked(name);
    kfree(name);
    if (!region) {
        spin_unlock(&ipc_shm_lock);
        return IPO_IPC_ERR_NOT_FOUND;
    }
    if ((rights & region->public_rights) != rights) {
        spin_unlock(&ipc_shm_lock);
        return IPO_IPC_ERR_PERMISSION;
    }

    ipc_shm_client_t *client = ipc_shm_find_client_locked(region, proc->pid);
    if (client) {
        client->rights |= rights;
        uint32_t id = region->id;
        spin_unlock(&ipc_shm_lock);
        return (int)id;
    }

    ipc_shm_client_t *free_client = NULL;
    for (uint32_t i = 0u; i < region->client_capacity; i++) {
        if (region->clients[i].pid == 0u) {
            free_client = &region->clients[i];
            break;
        }
    }
    if (!free_client) {
        uint32_t new_cap = region->client_capacity ? (region->client_capacity * 2u)
                                                   : IPC_INITIAL_SHM_CLIENT_CAPACITY;
        if (new_cap <= region->client_capacity ||
            new_cap > (SIZE_MAX / sizeof(*region->clients))) {
            spin_unlock(&ipc_shm_lock);
            return IPO_IPC_ERR_NO_SPACE;
        }
        ipc_shm_client_t *grown = (ipc_shm_client_t *)krealloc(
            region->clients, new_cap * sizeof(ipc_shm_client_t));
        if (!grown) {
            spin_unlock(&ipc_shm_lock);
            return IPO_IPC_ERR_NO_SPACE;
        }
        memset(grown + region->client_capacity, 0,
               (new_cap - region->client_capacity) * sizeof(ipc_shm_client_t));
        free_client = &grown[region->client_capacity];
        region->clients = grown;
        region->client_capacity = new_cap;
    }

    free_client->pid = proc->pid;
    free_client->rights = rights;
    free_client->attached = false;
    uint32_t id = region->id;
    spin_unlock(&ipc_shm_lock);
    return (int)id;
}

static int ipc_shm_attach(uint32_t id, void **user_address,
                          uint32_t *user_size) {
    process_t *proc = process_get_current();
    if (!proc || !ipc_user_range(user_address, sizeof(*user_address)) ||
        !ipc_user_range(user_size, sizeof(*user_size)))
        return IPO_IPC_ERR_INVALID;

    spin_lock(&ipc_shm_lock);
    ipc_shm_region_t *region = ipc_shm_find_id_locked(id);
    if (!region || region->destroyed) {
        spin_unlock(&ipc_shm_lock);
        return IPO_IPC_ERR_NOT_FOUND;
    }
    ipc_shm_client_t *client = ipc_shm_find_client_locked(region, proc->pid);
    if (!client) {
        spin_unlock(&ipc_shm_lock);
        return IPO_IPC_ERR_PERMISSION;
    }
    if (!client->attached) {
        if (!ipc_shm_map_locked(region, proc, client->rights)) {
            spin_unlock(&ipc_shm_lock);
            return IPO_IPC_ERR_NO_SPACE;
        }
        client->attached = true;
    }
    *user_address = (void *)(uintptr_t)region->virtual_base;
    *user_size = region->size;
    spin_unlock(&ipc_shm_lock);
    return IPO_IPC_OK;
}

static int ipc_shm_detach(uint32_t id) {
    process_t *proc = process_get_current();
    if (!proc) return IPO_IPC_ERR_INVALID;
    spin_lock(&ipc_shm_lock);
    ipc_shm_region_t *region = ipc_shm_find_id_locked(id);
    if (!region) {
        spin_unlock(&ipc_shm_lock);
        return IPO_IPC_ERR_NOT_FOUND;
    }
    ipc_shm_client_t *client = ipc_shm_find_client_locked(region, proc->pid);
    if (!client) {
        spin_unlock(&ipc_shm_lock);
        return IPO_IPC_ERR_PERMISSION;
    }
    if (client->attached) {
        ipc_shm_unmap_locked(region, proc);
        client->attached = false;
    }
    memset(client, 0, sizeof(*client));
    if (region->destroyed && !ipc_shm_has_clients(region))
        ipc_shm_clear_locked(region);
    spin_unlock(&ipc_shm_lock);
    return IPO_IPC_OK;
}

static int ipc_shm_destroy(uint32_t id) {
    process_t *proc = process_get_current();
    if (!proc) return IPO_IPC_ERR_INVALID;
    spin_lock(&ipc_shm_lock);
    ipc_shm_region_t *region = ipc_shm_find_id_locked(id);
    if (!region || region->destroyed) {
        spin_unlock(&ipc_shm_lock);
        return IPO_IPC_ERR_NOT_FOUND;
    }
    if (region->owner_pid != proc->pid) {
        spin_unlock(&ipc_shm_lock);
        return IPO_IPC_ERR_PERMISSION;
    }
    region->destroyed = true;
    if (!ipc_shm_has_clients(region))
        ipc_shm_clear_locked(region);
    spin_unlock(&ipc_shm_lock);
    return IPO_IPC_OK;
}

bool ipc_shared_memory_init(void) {
    spinlock_init(&ipc_shm_lock);
    ipc_shm_regions = NULL;
    ipc_shm_capacity = 0u;
    ipc_shm_ready = true;
    return true;
}

void ipc_shared_memory_process_terminated(uint32_t pid) {
    if (!ipc_shm_ready || pid == 0u) return;
    process_t *proc = process_find_by_pid(pid);
    spin_lock(&ipc_shm_lock);
    for (uint32_t i = 0u; i < ipc_shm_capacity; i++) {
        ipc_shm_region_t *region = &ipc_shm_regions[i];
        if (!region->active) continue;
        ipc_shm_client_t *client = ipc_shm_find_client_locked(region, pid);
        if (client) {
            if (client->attached && proc) {
                ipc_shm_unmap_locked(region, proc);
                client->attached = false;
            }
            memset(client, 0, sizeof(*client));
        }
        if (region->owner_pid == pid) {
            region->destroyed = true;
        }
        if (region->destroyed && !ipc_shm_has_clients(region)) {
            ipc_shm_clear_locked(region);
        }
    }
    spin_unlock(&ipc_shm_lock);
}

bool ipc_shared_memory_user_range(uint32_t pid, const void *ptr,
                                  uint32_t length) {
    if (!ipc_shm_ready || !ptr || length == 0u) return false;
    uintptr_t start = (uintptr_t)ptr;
    if (start > UINTPTR_MAX - length) return false;
    uintptr_t end = start + length;

    spin_lock(&ipc_shm_lock);
    for (uint32_t i = 0u; i < ipc_shm_capacity; i++) {
        ipc_shm_region_t *region = &ipc_shm_regions[i];
        if (!region->active || region->virtual_base == 0u) continue;
        ipc_shm_client_t *client = ipc_shm_find_client_locked(region, pid);
        if (client && client->attached) {
            uintptr_t r_start = region->virtual_base;
            uintptr_t r_end = r_start + region->size;
            if (start >= r_start && end <= r_end) {
                spin_unlock(&ipc_shm_lock);
                return true;
            }
        }
    }
    spin_unlock(&ipc_shm_lock);
    return false;
}

static bool ipc_shm_copy_args(uint32_t argc, uint32_t *argv,
                              uint32_t required, uint32_t *out) {
    if (argc < required ||
        !ipc_user_range(argv, required * sizeof(uint32_t)))
        return false;
    memcpy(out, argv, required * sizeof(uint32_t));
    return true;
}

uint32_t ipc_shared_memory_syscall_handler(uint32_t num, uint32_t argc,
                                           uint32_t *argv) {
    if (!ipc_shm_ready) return (uint32_t)IPO_IPC_ERR_NO_SPACE;
    uint32_t args[3] = {0};
    switch (num) {
        case IPO_SYSCALL_SHM_CREATE:
            if (!ipc_shm_copy_args(argc, argv, 3u, args))
                return (uint32_t)IPO_IPC_ERR_INVALID;
            return (uint32_t)ipc_shm_create(
                (const char *)(uintptr_t)args[0], args[1], args[2]);
        case IPO_SYSCALL_SHM_OPEN:
            if (!ipc_shm_copy_args(argc, argv, 2u, args))
                return (uint32_t)IPO_IPC_ERR_INVALID;
            return (uint32_t)ipc_shm_open(
                (const char *)(uintptr_t)args[0], args[1]);
        case IPO_SYSCALL_SHM_ATTACH:
            if (!ipc_shm_copy_args(argc, argv, 3u, args))
                return (uint32_t)IPO_IPC_ERR_INVALID;
            return (uint32_t)ipc_shm_attach(
                args[0], (void **)(uintptr_t)args[1],
                (uint32_t *)(uintptr_t)args[2]);
        case IPO_SYSCALL_SHM_DETACH:
            if (!ipc_shm_copy_args(argc, argv, 1u, args))
                return (uint32_t)IPO_IPC_ERR_INVALID;
            return (uint32_t)ipc_shm_detach(args[0]);
        case IPO_SYSCALL_SHM_DESTROY:
            if (!ipc_shm_copy_args(argc, argv, 1u, args))
                return (uint32_t)IPO_IPC_ERR_INVALID;
            return (uint32_t)ipc_shm_destroy(args[0]);
        default:
            return (uint32_t)IPO_IPC_ERR_INVALID;
    }
}
