#include <kernel/async.h>

#include <kernel/process.h>
#include <memory/kmalloc.h>
#include <system/timer.h>

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>


typedef struct async_task {

    char name[32];

    async_task_fn_t fn;

    uint32_t interval_ms;
    uint32_t last_run_ms;

    bool active;

    process_t *owner;

    struct async_task *next;

} async_task_t;


static async_task_t *async_task_list = NULL;
static uint32_t async_ticks = 0;
static bool async_initialized = false;


void async_scheduler_init(void)
{
    async_task_list = NULL;
    async_ticks = 0;
    async_initialized = true;
}


int async_start_task(
    const char *name,
    uint32_t interval_ms,
    async_task_fn_t fn
)
{
    if (!async_initialized) {
        async_scheduler_init();
    }

    if (!name || name[0] == '\0' || fn == NULL) {
        return -1;
    }

    process_t *owner = process_get_current();

    if (owner != NULL) {
        process_set_keep_alive(owner, 1);
    }

    if (interval_ms == 0) {
        interval_ms = 10000u;
    }

    uint32_t now_ms = timer_millis();

    /*
     * Check whether task with this name already exists.
     */
    async_task_t *node = async_task_list;

    while (node != NULL) {

        if (node->active && strcmp(node->name, name) == 0) {

            node->interval_ms = interval_ms;
            node->last_run_ms = now_ms;
            node->fn = fn;

            return 0;
        }

        node = node->next;
    }

    /*
     * Allocate new task.
     */
    async_task_t *new_task =
        kmalloc(sizeof(async_task_t));

    if (new_task == NULL) {
        return -1;
    }

    memset(new_task, 0, sizeof(async_task_t));

    strncpy(
        new_task->name,
        name,
        sizeof(new_task->name) - 1
    );

    new_task->name[sizeof(new_task->name) - 1] = '\0';

    new_task->fn = fn;
    new_task->interval_ms = interval_ms;
    new_task->last_run_ms = now_ms;
    new_task->active = true;
    new_task->owner = owner;

    new_task->next = async_task_list;
    async_task_list = new_task;

    printf(
        "[async] registered task '%s' interval=%u ms owner_pid=%u\n",
        new_task->name,
        new_task->interval_ms,
        new_task->owner ? new_task->owner->pid : 0u
    );

    return 0;
}


int async_stop_task(const char *name)
{
    if (!name) {
        return -1;
    }

    async_task_t **it = &async_task_list;

    while (*it != NULL) {

        async_task_t *node = *it;

        if (node->active &&
            strcmp(node->name, name) == 0) {

            process_t *owner = node->owner;

            *it = node->next;

            kfree(node);

            if (owner != NULL) {

                process_set_keep_alive(owner, 0);

                if (owner->async_task_count == 0) {
                    process_cleanup(owner);
                }
            }

            return 0;
        }

        it = &node->next;
    }

    return -1;
}


void async_scheduler_tick(void)
{
    if (!async_initialized ||
        async_task_list == NULL) {
        return;
    }

    uint32_t now_ms = timer_millis();

    if (now_ms == 0) {
        return;
    }

    async_task_t *node = async_task_list;

    while (node != NULL) {

        if (node->active && node->fn != NULL) {

            uint32_t elapsed =
                now_ms - node->last_run_ms;

            if (elapsed >= node->interval_ms) {

                node->last_run_ms = now_ms;

                node->fn();
            }
        }

        node = node->next;
    }
}