/**
 * @file at.h
 * @brief Asynchronous Tasks - multithreaded task scheduling library
 * @author github.com/R1ssanen
 */

#ifndef AT_H
#define AT_H

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

typedef _Bool    b8;
typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;
typedef int64_t  i64;
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef float    f32;
typedef double   f64;

#define true  1
#define false 0
#define _at_FORWARD_DECL

#define _at_LOCK(mtx)   pthread_mutex_lock(&mtx)
#define _at_UNLOCK(mtx) pthread_mutex_unlock(&mtx)

typedef pthread_mutex_t _at_mutex_t;
typedef pthread_t       _at_pid_t;

typedef struct atTask   atTask;
typedef struct atThread atThread;
typedef struct atPool   atPool;
typedef struct _atQNode _atQNode;
typedef struct _atQueue _atQueue;
typedef void* (*at_task_func_t)(void*);

typedef enum {
    AT_SUCCESS = 0,
    AT_NULL_INPUT,
    AT_SYS_ERROR,
    AT_LIB_ERROR,
} at_error_t;

static inline const char* atGetErrorString(at_error_t err) {
    switch (err) {
    case AT_SUCCESS: return "(AT) No errors.";
    case AT_NULL_INPUT: return "(AT) Invalid null function argument.";
    case AT_SYS_ERROR: return "(AT) C-standard function returned an error.";
    case AT_LIB_ERROR: return "(AT) External library function returned an error.";
    default: return "(AT) Unknown error code.";
    }
}

/*
 * QUEUE (internal)
 * */

/**
 * (internal)
 * @class _atQNode
 * @brief Thread queue node.
 */
struct _atQNode {
    atThread* thread;
    _atQNode* next;
};

/**
 * (internal)
 * @class _atQueue
 * @brief Thread queue.
 */
struct _atQueue {
    _at_mutex_t lock; //!  NOTE: All queue operations are thread-safe
    _atQNode*   front;
    _atQNode*   back;
};

/**
 * (internal)
 * @brief Create thread queue.
 * @return
 */
static inline _atQueue _atCreateQueue(void) {
    return (_atQueue){ .lock = PTHREAD_MUTEX_INITIALIZER, .front = NULL, .back = NULL };
}

atThread* _at_FORWARD_DECL _atQDequeue(_atQueue*);

/**
 * (internal)
 * @brief Destroy thread queue.
 * @param queue Queue to be destroyed.
 * @return
 */
static inline at_error_t _atDestroyQueue(_atQueue* queue) {
    if (!queue) { return AT_NULL_INPUT; }

    while (_atQDequeue(queue));

    if (pthread_mutex_destroy(&queue->lock)) { return AT_LIB_ERROR; }

    return AT_SUCCESS;
}

/**
 * (internal)
 * @brief Check if thread queue is empty.
 * @param queue Queue to be checked.
 * @param err Optional error code output. Nullable.
 * @return
 */
static inline b8 _atQIsEmpty(_atQueue* queue, at_error_t* err) {
    if (!queue) {
        if (err) { *err = AT_NULL_INPUT; }
        return false;
    }

    _at_LOCK(queue->lock);
    b8 is_empty = (!queue->front) && (!queue->back);
    _at_UNLOCK(queue->lock);

    if (err) { *err = AT_SUCCESS; }
    return is_empty;
}

/**
 * (internal)
 * @brief Enqueue thread to queue.
 * @param queue Queue to enqueue to.
 * @param thread Thread to enqueue.
 * @return
 */
static inline at_error_t _atQEnqueue(_atQueue* queue, atThread* thread) {
    if (!queue || !thread) { return AT_NULL_INPUT; }

    _atQNode* node = malloc(sizeof(_atQNode));
    if (!node) { return AT_SYS_ERROR; }

    node->thread = thread;
    node->next   = NULL;

    _at_LOCK(queue->lock);
    {
        if (!queue->back) {
            queue->front = node;
            queue->back  = node;
        } else {
            queue->back->next = node;
            queue->back       = node;
        }
    }
    _at_UNLOCK(queue->lock);

    return AT_SUCCESS;
}

/**
 * (internal)
 * @brief Dequeue thread from queue.
 * @param queue Queue to dequeue from.
 * @return
 */
atThread* _atQDequeue(_atQueue* queue) {
    if (!queue || _atQIsEmpty(queue, NULL)) { return NULL; }

    _atQNode* old_front = NULL;

    _at_LOCK(queue->lock);
    {
        old_front    = queue->front;
        queue->front = queue->front->next;
        if (!queue->front) { queue->back = NULL; }
    }
    _at_UNLOCK(queue->lock);

    atThread* old_front_data = old_front->thread;
    free(old_front);
    return old_front_data;
}

/*
 * TASK
 * */

/**
 * @class atTask
 * @brief Thread task return type.
 */
struct atTask {
    _at_mutex_t    lock;
    at_task_func_t fn;
    void*          args;
    void*          ret;
    u32            update_freq_ms;
    _at_pid_t      pid; //! Thread associated with the task.
    b8             done;
};

/**
 * (internal)
 * @brief Create a task.
 * @param fn Function to execute.
 * @param args Arguments to the function.
 * @param update_freq_ms Frequence in which task checks whether it's
 *                       been completed when awaiting, in seconds.
 * @return
 */
static inline atTask _atCreateTask(at_task_func_t fn, void* args, f64 update_freq) {
    return (atTask){ .lock           = PTHREAD_MUTEX_INITIALIZER,
                     .fn             = fn,
                     .args           = args,
                     .ret            = NULL,
                     .update_freq_ms = (u32)(update_freq * 1000.0),
                     .done           = false };
}

/**
 * (internal)
 * @brief Destroy a task.
 * @param task Task to be destroyed.
 * @return
 */
static inline at_error_t _atDestroyTask(atTask* task) {
    if (!task) { return AT_NULL_INPUT; }

    if (pthread_mutex_destroy(&task->lock)) { return AT_LIB_ERROR; }

    task->fn   = NULL;
    task->args = NULL;
    task->ret  = NULL;
    task->pid  = 0;

    return AT_SUCCESS;
}

atTask* _at_FORWARD_DECL atLaunchTask(at_task_func_t, void*, atThread*);

atTask* _at_FORWARD_DECL atScheduleTask(at_task_func_t, void*, atPool*);

/**
 * @brief Await a task. You must always await a launched or
 *        scheduled task before accessing its values to avoid a race condition.
 * @param task Task to be awaited.
 * @param err Optional error code output. Nullable.
 */
static inline void* atAwaitTask(atTask* task, at_error_t* err) {
    if (!task) {
        if (err) { *err = AT_NULL_INPUT; }
        return NULL;
    }

    _at_LOCK(task->lock);
    {
        while (!task->done) {
            _at_UNLOCK(task->lock);
            usleep(task->update_freq_ms);
            _at_LOCK(task->lock);
        }
    }
    _at_UNLOCK(task->lock);

    if (err) { *err = AT_SUCCESS; }
    return task->ret;
}

/*
 * THREAD
 * */

/**
 * @class atThread
 * @brief Thread structure.
 */
struct atThread {
    atTask      task;
    _at_mutex_t lock;
    atPool*     pool;
    _at_pid_t   pid;
    u32         update_freq_ms;
    b8          exit;
    b8          working;
};

void* _at_FORWARD_DECL _atThreadLoop(void*);

/**
 * @brief Create a thread.
 * @param pool Pool to associate the thread with.
 *             Can be null, in which case the thread is single.
 * @param update_freq Frequency in which thread checks if it has work, in seconds.
 *                    Is passed down to thread task as well.
 * @param thread Output to the thread to be created.
 * @return
 */
static inline at_error_t atCreateThread(atPool* pool, f64 update_freq, atThread* thread) {
    if (!thread) { return AT_NULL_INPUT; }

    *thread = (atThread){ .task           = _atCreateTask(NULL, NULL, update_freq),
                          .lock           = PTHREAD_MUTEX_INITIALIZER,
                          .pool           = pool,
                          .update_freq_ms = (u32)(update_freq * 1000.0),
                          .exit           = false,
                          .working        = false };

    if (pthread_create(&thread->pid, NULL, _atThreadLoop, thread)) { return AT_LIB_ERROR; }

    thread->task.pid = thread->pid;
    return AT_SUCCESS;
}

/**
 * @brief Awaits the threads current task (if it has one), and destroys it.
 * @param thread Thread to be killed.
 * @return
 */
static inline at_error_t atKillThread(atThread* thread) {
    if (!thread) { return AT_NULL_INPUT; }

    _at_LOCK(thread->lock);
    thread->exit = true;
    _at_UNLOCK(thread->lock);

    if (pthread_join(thread->pid, NULL)) { return AT_LIB_ERROR; }
    if (pthread_mutex_destroy(&thread->lock)) { return AT_LIB_ERROR; }

    at_error_t err = _atDestroyTask(&thread->task);
    if (err) { return err; }

    return AT_SUCCESS;
}

/**
 * @brief Forcefully kills a thread. Not as safe as 'atKillThread'.
 * @param thread Thread to be killed.
 * @return
 */
static inline at_error_t atForceKillThread(atThread* thread) {
    if (!thread) { return AT_NULL_INPUT; }

    if (pthread_cancel(thread->pid)) { return AT_LIB_ERROR; }
    if (pthread_mutex_destroy(&thread->lock)) { return AT_LIB_ERROR; }

    at_error_t err = _atDestroyTask(&thread->task);
    if (err) { return err; }

    return AT_SUCCESS;
}

/*
 * POOL
 * */

/**
 * @class atPool
 * @brief Thread pool structure.
 */
struct atPool {
    _atQueue  free;
    atThread* pool;
    u32       update_freq_ms;
    u32       thread_count;
};

/**
 * @brief Create a thread pool.
 * @param thread_count Amount of threads to create.
 * @param update_freq Frequency in which pool checks for free threads, in seconds.
 *                    Is passed down to each thread as well.
 * @param pool Output to the pool to be created.
 * @return
 */
static inline at_error_t atCreatePool(u32 thread_count, f64 update_freq, atPool* pool) {
    if (!pool) { return AT_NULL_INPUT; }

    pool->pool = malloc(sizeof(atThread) * thread_count);
    if (!pool->pool) { return AT_SYS_ERROR; }

    pool->update_freq_ms = (u32)(update_freq * 1000.0);
    pool->thread_count   = thread_count;
    pool->free           = _atCreateQueue();

    at_error_t err;
    for (u64 i = 0; i < thread_count; ++i) {
        if ((err = atCreateThread(pool, update_freq, &pool->pool[i]))) { return err; }
        if ((err = _atQEnqueue(&pool->free, &pool->pool[i]))) { return err; }
    }

    return AT_SUCCESS;
}

/**
 * @brief Destroy a thread pool.
 * @param pool Thread pool to be destroyed.
 * @return
 */
static inline at_error_t atDestroyPool(atPool* pool) {
    if (!pool) { return AT_NULL_INPUT; }

    at_error_t err;
    for (u64 i = 0; i < pool->thread_count; ++i) {
        if ((err = atKillThread(&pool->pool[i]))) { return err; }
    }

    if ((err = _atDestroyQueue(&pool->free))) { return err; }

    free(pool->pool);
    return AT_SUCCESS;
}

/*
 * TASK
 * */

/**
 * @brief Launch a task with a specific thread.
 * @param fn Task function.
 * @param args Task function arguments.
 * @param thread Thread to be given the task.
 * @return Null for error.
 */
atTask* atLaunchTask(at_task_func_t fn, void* args, atThread* thread) {
    if (!fn || !thread) { return NULL; }

    _at_LOCK(thread->lock);
    {
        if (thread->working) {
            _at_UNLOCK(thread->lock);
            return NULL;
        }

        thread->task.fn   = fn;
        thread->task.args = args;
        thread->task.done = false;
        thread->working   = true;
    }
    _at_UNLOCK(thread->lock);

    return &thread->task;
}

/**
 * @brief Schedule a task to be executed once a free thread is found.
 * @param fn Task function.
 * @param args Task function arguments.
 * @param pool Pool to schedule the task with.
 * @return Null for error.
 */
atTask* atScheduleTask(at_task_func_t fn, void* args, atPool* pool) {
    if (!pool) { return NULL; }

    while (_atQIsEmpty(&pool->free, NULL)) { usleep(pool->update_freq_ms); }

    atThread* free_thread = _atQDequeue(&pool->free);
    return atLaunchTask(fn, args, free_thread);
}

/*
 * THREAD
 * */

/**
 * (internal)
 * @brief Thread running loop.
 * @param _arg
 */
void* _atThreadLoop(void* _arg) {
    atThread* thread = (atThread*)_arg;
    atTask*   task   = &thread->task;

    _at_LOCK(thread->lock);
    {
        while (!thread->exit) {
            if (!thread->working) {
                _at_UNLOCK(thread->lock);
                usleep(thread->update_freq_ms);
            } else {
                thread->working = false;
                _at_UNLOCK(thread->lock);

                task->ret = task->fn(task->args);

                _at_LOCK(task->lock);
                task->done = true;
                _at_UNLOCK(task->lock);

                _atQEnqueue(&thread->pool->free, thread);
            }

            _at_LOCK(thread->lock);
        }
    }
    _at_UNLOCK(thread->lock);

    return NULL;
}

#endif
