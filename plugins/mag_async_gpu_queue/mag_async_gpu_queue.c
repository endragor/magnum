static struct tm_temp_allocator_api *tm_temp_allocator_api;
static struct tm_allocator_api *tm_allocator_api;
static struct tm_os_api *tm_os_api;

#include <foundation/allocator.h>
#include <foundation/api_registry.h>
#include <foundation/atomics.inl>
#include <foundation/carray.inl>
#include <foundation/os.h>

#include <plugins/renderer/render_backend.h>

#include "plugins/mag_async_gpu_queue/mag_async_gpu_queue.h"

#define THREAD_STACK_SIZE (256 * 1024)

#define task_t mag_async_gpu_queue_task_params_t

typedef struct executing_task_t
{
    uint64_t task_id;
    uint32_t *fences;
    void (*completion_callback)(void *data);
    void *data;
} executing_task_t;

typedef struct mag_async_gpu_queue_o
{
    tm_allocator_i allocator;
    tm_renderer_backend_i *backend;
    uint32_t max_simultaneous_tasks;
    uint32_t device_affinity_mask;

    tm_thread_o thread;
    atomic_uint_least32_t shutdown;

    atomic_uint_least64_t next_id;

    tm_critical_section_o tasks_cs;
    tm_semaphore_o sem;
    task_t *task_heap;
    // IDs match the heap order but are stored separately to make the
    // linear search faster by not polluting the CPU cache.
    uint64_t *task_ids;

    tm_critical_section_o executing_tasks_cs;
    executing_task_t *executing_tasks;

    tm_critical_section_o completed_tasks_cs;
    // completed but not yet confirmed (by is_task_done()) tasks
    uint64_t *completed_tasks;
    // canceled but currently executing tasks
    uint64_t *canceled_tasks;
} mag_async_gpu_queue_o;

// Returns the index of parent item of the heap item at index `i`.
static inline uint32_t heap__parent(uint32_t i)
{
    return i > 0 ? (i - 1) / 2 : UINT32_MAX;
}

// Returns the index of the first child of the heap item at index `i`.
static inline uint32_t heap__first_child(uint32_t i)
{
    return i * 2 + 1;
}

// Swaps the heap items at indices `a` and `b`.
static void heap__swap(task_t *heap, uint64_t *task_ids, uint32_t a, uint32_t b)
{
    const struct task_t tmp = heap[a];
    heap[a] = heap[b];
    heap[b] = tmp;

    const uint64_t tmp_id = task_ids[a];
    task_ids[a] = task_ids[b];
    task_ids[b] = tmp_id;
}

// Updates the heap so that the heap invariant is maintained after the heap value at index `i` has
// changed.
static void heap__update(struct task_t *heap, uint64_t *task_ids, uint32_t i)
{
    const uint32_t n = (uint32_t)tm_carray_size(heap);

    // As long as item has a parent and is smaller than its parent, swap.
    while (i) {
        const uint32_t p = heap__parent(i);
        if (i < n && heap[i].priority < heap[p].priority) {
            heap__swap(heap, task_ids, i, p);
            i = p;
        } else
            break;
    }

    // As long as item has a child that is smaller than the item, swap the item with its smallest
    // child.
    while (true) {
        const uint32_t l = heap__first_child(i);
        if (l >= n)
            break;
        const uint32_t r = l + 1;
        const uint32_t smallest = r >= n || heap[l].priority < heap[r].priority ? l : r;
        if (heap[smallest].priority < heap[i].priority) {
            heap__swap(heap, task_ids, i, smallest);
            i = smallest;
        } else
            break;
    }
}

// Pops the top item from the heap and returns it.
static task_t heap__pop(task_t *heap, uint64_t *task_ids, uint64_t *out_id)
{
    task_t res = heap[0];
    *out_id = task_ids[0];
    heap__swap(heap, task_ids, 0, (uint32_t)tm_carray_size(heap) - 1);
    tm_carray_pop(heap);
    tm_carray_pop(task_ids);
    heap__update(heap, task_ids, 0);
    return res;
}

// Pushes the item `h` to the heap.
static void heap__push(task_t **heap, uint64_t **task_ids, task_t task, uint64_t id, tm_allocator_i *a)
{
    tm_carray_push(*heap, task, a);
    tm_carray_push(*task_ids, id, a);
    heap__update(*heap, *task_ids, (uint32_t)tm_carray_size(*heap) - 1);
}

// Deletes element at index i from the heap
static void heap__remove(task_t *heap, uint64_t *task_ids, uint32_t i)
{
    uint64_t task_count = tm_carray_size(task_ids);
    task_ids[i] = tm_carray_pop(task_ids);
    heap[i] = tm_carray_pop(heap);
    if (i < task_count - 1) {
        heap__update(heap, task_ids, i);
    }
}

static void wait_for_fences(tm_renderer_backend_i *backend, uint32_t *fences, bool *signaled, uint32_t n, uint32_t device_affinity_mask)
{
    while (!backend->wait_for_reads(backend->inst, fences, signaled, n, UINT64_MAX, device_affinity_mask))
        ;
}

// Waits until at least one of the tasks is fully completed.
static uint64_t *wait_for_task(mag_async_gpu_queue_o *q, executing_task_t *wait_tasks, tm_allocator_i *a)
{
    TM_INIT_TEMP_ALLOCATOR_WITH_ADAPTER(ta, taa);

    uint64_t *completed_ids = NULL;
    uint32_t *fences = NULL;
    struct fence_index
    {
        uint32_t task_i;
        uint32_t fence_i;
    } *fence_indexes = NULL;
    bool *signaled = NULL;

    while (!tm_carray_size(completed_ids)) {
        tm_carray_shrink(fences, 0);
        tm_carray_shrink(fence_indexes, 0);
        for (uint32_t i = 0; i < tm_carray_size(wait_tasks); ++i) {
            tm_carray_push_array(fences, wait_tasks[i].fences, tm_carray_size(wait_tasks[i].fences), taa);
            for (uint32_t fi = 0; fi < tm_carray_size(wait_tasks[i].fences); ++fi) {
                struct fence_index index = { i, fi };
                tm_carray_temp_push(fence_indexes, index, ta);
            }
        }

        tm_carray_temp_resize(signaled, tm_carray_size(fences), ta);
        memset(signaled, 0, sizeof(*signaled) * tm_carray_size(signaled));
        wait_for_fences(q->backend, fences, signaled, (uint32_t)tm_carray_size(fences), q->device_affinity_mask);

        for (uint64_t i = 0; i < tm_carray_size(fences); ++i) {
            if (signaled[i]) {
                executing_task_t *task = wait_tasks + fence_indexes[i].task_i;
                // Cannot swap-remove yet, as it will invalidate the indexes.
                // Just mark the fence so that we can remove it later.
                task->fences[fence_indexes[i].fence_i] = 0;
            }
        }

        uint64_t task_i = 0;
        while (task_i < tm_carray_size(wait_tasks)) {
            executing_task_t *task = wait_tasks + task_i;
            uint64_t ri = 0;
            while (ri < tm_carray_size(task->fences)) {
                if (!task->fences[ri])
                    task->fences[ri] = tm_carray_pop(task->fences);
                else
                    ++ri;
            }
            if (!ri) {
                tm_carray_free(task->fences, &q->allocator);
                tm_carray_push(completed_ids, task->task_id, a);
                task->completion_callback(task->data);
                wait_tasks[task_i] = tm_carray_pop(wait_tasks);
            } else {
                ++task_i;
            }
        }
    }

    TM_SHUTDOWN_TEMP_ALLOCATOR(ta);

    return completed_ids;
}

static void process_one_task(mag_async_gpu_queue_o *q)
{
    task_t task = { 0 };
    uint64_t task_id = 0;
    {
        TM_OS_ENTER_CRITICAL_SECTION(&q->tasks_cs);
        if (tm_carray_size(q->task_ids)) {
            task = heap__pop(q->task_heap, q->task_ids, &task_id);
        }
        TM_OS_LEAVE_CRITICAL_SECTION(&q->tasks_cs);
    }

    if (!task_id)
        return;

    mag_async_gpu_queue_task_args_t args = {
        .data = task.data,
        .fences_allocator = &q->allocator,
        .task_id = task_id,
    };
    task.f(&args);
    executing_task_t active_task = { .task_id = task_id, .fences = args.out_fences, .completion_callback = task.completion_callback, .data = task.data };

    TM_INIT_TEMP_ALLOCATOR_WITH_ADAPTER(ta, a);

    executing_task_t *wait_tasks = NULL;
    {
        TM_OS_ENTER_CRITICAL_SECTION(&q->executing_tasks_cs);
        tm_carray_push(q->executing_tasks, active_task, &q->allocator);
        if (tm_carray_size(q->executing_tasks) >= q->max_simultaneous_tasks) {
            wait_tasks = tm_carray_from(q->executing_tasks, tm_carray_size(q->executing_tasks), a);
            tm_carray_shrink(q->executing_tasks, 0);
        }
        TM_OS_LEAVE_CRITICAL_SECTION(&q->executing_tasks_cs);
    }

    if (wait_tasks) {
        // Reached simultaneous task limit. Time to block until something completes.
        uint64_t *completed_ids = wait_for_task(q, wait_tasks, a);

        {
            TM_OS_ENTER_CRITICAL_SECTION(&q->completed_tasks_cs);
            for (uint64_t ir = 0; ir < tm_carray_size(completed_ids); ++ir) {
                uint64_t completed_id = completed_ids[ir];
                bool is_canceled = false;
                for (uint64_t ic = 0; ic < tm_carray_size(q->canceled_tasks); ++ic) {
                    if (q->canceled_tasks[ic] == completed_id) {
                        is_canceled = true;
                        q->canceled_tasks[ic] = tm_carray_pop(q->canceled_tasks);
                        break;
                    }
                }

                if (!is_canceled)
                    tm_carray_push(q->completed_tasks, completed_id, &q->allocator);
            }
            TM_OS_LEAVE_CRITICAL_SECTION(&q->completed_tasks_cs);
        }

        if (tm_carray_size(wait_tasks)) {
            TM_OS_ENTER_CRITICAL_SECTION(&q->executing_tasks_cs);
            tm_carray_push_array(q->executing_tasks, wait_tasks, tm_carray_size(wait_tasks), &q->allocator);
            TM_OS_LEAVE_CRITICAL_SECTION(&q->executing_tasks_cs);
        }
    }

    TM_SHUTDOWN_TEMP_ALLOCATOR(ta);
}

static void wait_for_all_executing_tasks(mag_async_gpu_queue_o *q)
{
    TM_INIT_TEMP_ALLOCATOR_WITH_ADAPTER(ta, a);
    executing_task_t *wait_tasks = NULL;

    TM_OS_ENTER_CRITICAL_SECTION(&q->executing_tasks_cs);
    if (tm_carray_size(q->executing_tasks)) {
        wait_tasks = tm_carray_from(q->executing_tasks, tm_carray_size(q->executing_tasks), a);
        tm_carray_shrink(q->executing_tasks, 0);
    }
    TM_OS_LEAVE_CRITICAL_SECTION(&q->executing_tasks_cs);

    while (tm_carray_size(wait_tasks)) {
        wait_for_task(q, wait_tasks, a);
    }

    TM_SHUTDOWN_TEMP_ALLOCATOR(ta);
}

static void thread_entry(void *user_data)
{
    mag_async_gpu_queue_o *q = (mag_async_gpu_queue_o *)user_data;
    while (true) {
        tm_os_api->thread->semaphore_wait(q->sem);
        if (q->shutdown) {
            // need to wait for in-flight tasks so that the fences are not leaked
            wait_for_all_executing_tasks(q);
            break;
        }
        process_one_task(q);
    }
}

static mag_async_gpu_queue_o *create(tm_allocator_i *parent, tm_renderer_backend_i *backend, const mag_async_gpu_queue_params_t *params)
{
    tm_os_thread_api *thread_api = tm_os_api->thread;

    tm_allocator_i a = tm_allocator_api->create_child(parent, "mag_async_gpu_queue");
    mag_async_gpu_queue_o *q;
    q = tm_alloc(&a, sizeof(*q));
    *q = (mag_async_gpu_queue_o) {
        .allocator = a,
        .backend = backend,
        .next_id = 1,
        .sem = thread_api->create_semaphore(0),
        .max_simultaneous_tasks = params->max_simultaneous_tasks,
        .device_affinity_mask = params->device_affinity_mask,
    };
    thread_api->create_critical_section(&q->tasks_cs);
    thread_api->create_critical_section(&q->completed_tasks_cs);
    thread_api->create_critical_section(&q->executing_tasks_cs);
    q->thread = thread_api->create_thread(thread_entry, q, THREAD_STACK_SIZE, "mag_async_gpu_queue");
    return q;
}

static void destroy(mag_async_gpu_queue_o *q)
{
    tm_os_thread_api *thread_api = tm_os_api->thread;

    atomic_fetch_add_uint32_t(&q->shutdown, 1);
    thread_api->semaphore_add(q->sem, 1);

    thread_api->wait_for_thread(q->thread);

    thread_api->destroy_semaphore(q->sem);
    thread_api->destroy_critical_section(&q->tasks_cs);
    thread_api->destroy_critical_section(&q->executing_tasks_cs);
    thread_api->destroy_critical_section(&q->completed_tasks_cs);

    for (uint64_t i = 0; i < tm_carray_size(q->task_heap); ++i) {
        q->task_heap[i].cancel_callback(q->task_heap[i].data);
    }

    tm_carray_free(q->task_heap, &q->allocator);
    tm_carray_free(q->task_ids, &q->allocator);
    tm_carray_free(q->executing_tasks, &q->allocator);
    tm_carray_free(q->completed_tasks, &q->allocator);
    tm_carray_free(q->canceled_tasks, &q->allocator);

    tm_allocator_i a = q->allocator;
    tm_free(&a, q, sizeof(*q));
    tm_allocator_api->destroy_child(&a);
}

static uint64_t submit_task(mag_async_gpu_queue_o *q, const mag_async_gpu_queue_task_params_t *params)
{
    const uint64_t id = atomic_fetch_add_uint64_t(&q->next_id, 1);

    TM_OS_ENTER_CRITICAL_SECTION(&q->tasks_cs);
    heap__push(&q->task_heap, &q->task_ids, *params, id, &q->allocator);
    TM_OS_LEAVE_CRITICAL_SECTION(&q->tasks_cs);

    tm_os_api->thread->semaphore_add(q->sem, 1);
    return id;
}

static bool update_task_priority(mag_async_gpu_queue_o *q, uint64_t task_id, uint64_t new_priority)
{
    bool found = false;
    TM_OS_ENTER_CRITICAL_SECTION(&q->tasks_cs);
    for (uint32_t i = 0; i < tm_carray_size(q->task_ids); ++i) {
        if (q->task_ids[i] == task_id) {
            found = true;
            if (new_priority != q->task_heap[i].priority) {
                q->task_heap[i].priority = new_priority;
                heap__update(q->task_heap, q->task_ids, i);
            }
            break;
        }
    }
    TM_OS_LEAVE_CRITICAL_SECTION(&q->tasks_cs);
    return found;
}

static void cancel_task(mag_async_gpu_queue_o *q, uint64_t task_id)
{
    bool found = false;
    {
        void (*callback)(void *data) = 0;
        void *data = 0;
        TM_OS_ENTER_CRITICAL_SECTION(&q->tasks_cs);
        for (uint32_t i = 0; i < tm_carray_size(q->task_ids); ++i) {
            if (q->task_ids[i] == task_id) {
                found = true;
                callback = q->task_heap[i].cancel_callback;
                data = q->task_heap[i].data;
                heap__remove(q->task_heap, q->task_ids, i);
                break;
            }
        }
        TM_OS_LEAVE_CRITICAL_SECTION(&q->tasks_cs);
        if (callback)
            callback(data);
    }

    if (!found) {
        TM_OS_ENTER_CRITICAL_SECTION(&q->completed_tasks_cs);
        for (uint64_t i = 0; i < tm_carray_size(q->completed_tasks); ++i) {
            if (q->completed_tasks[i] == task_id) {
                q->completed_tasks[i] = tm_carray_pop(q->completed_tasks);
                found = true;
                break;
            }
        }
        if (!found)
            tm_carray_push(q->canceled_tasks, task_id, &q->allocator);
        TM_OS_LEAVE_CRITICAL_SECTION(&q->completed_tasks_cs);
    }
}

static bool is_task_done(mag_async_gpu_queue_o *q, uint64_t task_id)
{
    bool completed = false;
    {
        TM_OS_ENTER_CRITICAL_SECTION(&q->completed_tasks_cs);
        for (uint64_t i = 0; i < tm_carray_size(q->completed_tasks); ++i) {
            if (q->completed_tasks[i] == task_id) {
                q->completed_tasks[i] = tm_carray_pop(q->completed_tasks);
                completed = true;
                break;
            }
        }
        TM_OS_LEAVE_CRITICAL_SECTION(&q->completed_tasks_cs);
        if (completed)
            return true;
    }

    uint64_t completed_id = 0;
    void (*callback)(void *data) = 0;
    void *data = 0;
    {
        TM_OS_ENTER_CRITICAL_SECTION(&q->executing_tasks_cs);
        uint64_t executing_task_count = tm_carray_size(q->executing_tasks);
        for (uint64_t i = 0; i < executing_task_count; ++i) {
            executing_task_t *task = q->executing_tasks + i;
            if (task->task_id == task_id || i == executing_task_count - 1) {
                uint64_t ri = 0;
                while (ri < tm_carray_size(task->fences)) {
                    if (q->backend->read_complete(q->backend->inst, task->fences[ri], q->device_affinity_mask))
                        task->fences[ri] = tm_carray_pop(task->fences);
                    else
                        ++ri;
                }
                if (!ri) {
                    // all fences are done
                    tm_carray_free(task->fences, &q->allocator);
                    completed = task->task_id == task_id;
                    completed_id = task->task_id;
                    callback = task->completion_callback;
                    data = task->data;
                    q->executing_tasks[i] = tm_carray_pop(q->executing_tasks);
                }
                break;
            }
        }
        TM_OS_LEAVE_CRITICAL_SECTION(&q->executing_tasks_cs);
    }

    if (callback)
        callback(data);

    if (completed_id) {
        // we might've freed resources for a queued task
        tm_os_api->thread->semaphore_add(q->sem, 1);
        if (!completed) {
            TM_OS_ENTER_CRITICAL_SECTION(&q->completed_tasks_cs);
            tm_carray_push(q->completed_tasks, completed_id, &q->allocator);
            TM_OS_LEAVE_CRITICAL_SECTION(&q->completed_tasks_cs);
        }
    }

    return completed;
}

static struct mag_async_gpu_queue_api queue_api = {
    .create = create,
    .destroy = destroy,
    .submit_task = submit_task,
    .update_task_priority = update_task_priority,
    .cancel_task = cancel_task,
    .is_task_done = is_task_done,
};

TM_DLL_EXPORT void tm_load_plugin(struct tm_api_registry_api *reg, bool load)
{
    tm_temp_allocator_api = tm_get_api(reg, tm_temp_allocator_api);
    tm_allocator_api = tm_get_api(reg, tm_allocator_api);
    tm_os_api = tm_get_api(reg, tm_os_api);

    tm_set_or_remove_api(reg, load, mag_async_gpu_queue_api, &queue_api);
}