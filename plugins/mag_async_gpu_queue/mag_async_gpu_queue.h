#pragma once

struct tm_temp_allocator_i;
struct tm_allocator_i;
struct tm_renderer_backend_i;

#include <foundation/api_types.h>

typedef struct mag_async_gpu_queue_o mag_async_gpu_queue_o;

typedef struct mag_async_gpu_queue_task_args_t
{
    uint64_t task_id;
    void *data;

    // carray of fences that denote completion of the task
    uint32_t *out_fences;
    // the allocator to use when pushing to the out_fences carray
    struct tm_allocator_i *fences_allocator;
} mag_async_gpu_queue_task_args_t;

typedef struct mag_async_gpu_queue_params_t
{
    uint32_t max_simultaneous_tasks;
    uint32_t device_affinity_mask;
} mag_async_gpu_queue_params_t;

typedef struct mag_async_gpu_queue_task_params_t
{
    void (*f)(mag_async_gpu_queue_task_args_t *args);
    void *data;
    void (*cancel_callback)(void *data);
    void (*completion_callback)(void *data);
    uint64_t priority;
} mag_async_gpu_queue_task_params_t;

// Priority queue for submitting GPU tasks. The tasks execute asynchronously with a limit
// on the number of simultaneously executing tasks.
// Notice that either cancel_task() or a positive is_task_done() is expected to be called for every
// submitted task.
struct mag_async_gpu_queue_api
{
    mag_async_gpu_queue_o *(*create)(tm_allocator_i *a, struct tm_renderer_backend_i *backend, const mag_async_gpu_queue_params_t *params);

    // Destroys the queue and the associated resources.
    // Blocks until the executing tasks are completed to avoid leaking the read fences.
    void (*destroy)(mag_async_gpu_queue_o *q);

    // Lower priority values are executed first. 0 - top priority.
    // If the data_allocator is not NULL, it will be used to free the data pointer
    uint64_t (*submit_task)(mag_async_gpu_queue_o *q, const mag_async_gpu_queue_task_params_t *params);

    // Returns false if the task was not in the queue. This usually means the task is executing/done.
    bool (*update_task_priority)(mag_async_gpu_queue_o *q, uint64_t task_id, uint64_t new_priority);

    void (*cancel_task)(mag_async_gpu_queue_o *q, uint64_t task_id);

    bool (*is_task_done)(mag_async_gpu_queue_o *q, uint64_t task_id);
};

#define mag_async_gpu_queue_api_version TM_VERSION(1, 0, 0)