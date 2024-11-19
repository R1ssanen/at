/**
 * @file at.h
 * @brief Asynchronous Tasks - multithreaded task scheduling library
 * @author github.com/R1ssanen
 */

#ifndef _AT_H
#define _AT_H

#include <stdio.h>
#include <stdlib.h>
#include <threads.h>
#include <time.h>

typedef _Bool         b8;
typedef int           i32;
typedef long          i64;
typedef unsigned int  u32;
typedef unsigned long u64;
typedef double        f64;

#define true  1
#define false 0
#define _AT_FORWARD_DECL

typedef mtx_t _at_mutex_t;
#define _AT_MUTEX_CREATE(mtx)  mtx_init(&mtx, mtx_plain)
#define _AT_MUTEX_DESTROY(mtx) mtx_destroy(&mtx)
#define _AT_LOCK(mtx)          mtx_lock(&mtx)
#define _AT_UNLOCK(mtx)        mtx_unlock(&mtx)

typedef thrd_t _at_pid_t;
#define _AT_THREAD_CREATE(pid, fn, args) thrd_create(&pid, fn, args)
#define _AT_THREAD_JOIN(pid)             thrd_join(pid, NULL)
#define _AT_THREAD_SLEEP(duration)       thrd_sleep(&duration, NULL)
#define _AT_THREAD_WHICH()               thrd_current()

typedef enum {
    AT_SUCCESS = 0,
    AT_NULL_INPUT,
    AT_SYS_ERROR,
} at_error_t;

/**
 * @brief Get brief explanation string of error code.
 * @param err Error to get information of.
 * @return
 */
static inline const char* atGetErrorString(at_error_t err) {
    switch (err) {
    case AT_SUCCESS: return "(AT) No errors.";
    case AT_NULL_INPUT: return "(AT) Invalid null function argument.";
    case AT_SYS_ERROR: return "(AT) C-standard library function returned an error.";
    default: return "(AT) Unknown error code.";
    }
}

typedef struct atTask   atTask;
typedef struct atThread atThread;
typedef struct atPool   atPool;
typedef struct _atQNode _atQNode;
typedef struct _atQueue _atQueue;
typedef at_error_t (*at_task_func_t)(void*);

/**
 * @brief Convenience function to create a timespec from seconds.
 * @param seconds Seconds to create the timespec from.
 * @return
 */
static inline struct timespec _atCreateTime(f64 seconds) {
    struct timespec time;
    time.tv_sec  = (i64)(seconds);
    time.tv_nsec = (i64)(seconds - time.tv_sec) * 1E9L;
    return time;
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
    _atQueue queue = { .front = NULL, .back = NULL };
    _AT_MUTEX_CREATE(queue.lock);
    return queue;
}

atThread* _AT_FORWARD_DECL _atQDequeue(_atQueue*);

/**
 * (internal)
 * @brief Destroy thread queue.
 * @param queue Queue to be destroyed.
 * @return
 */
static inline at_error_t _atDestroyQueue(_atQueue* queue) {
    if (!queue) { return AT_NULL_INPUT; }

    while (_atQDequeue(queue));

    _AT_MUTEX_DESTROY(queue->lock);
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

    _AT_LOCK(queue->lock);
    b8 is_empty = (!queue->front) && (!queue->back);
    _AT_UNLOCK(queue->lock);

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

    _AT_LOCK(queue->lock);
    {
        if (!queue->back) {
            queue->front = node;
        } else {
            queue->back->next = node;
        }

        queue->back = node;
    }
    _AT_UNLOCK(queue->lock);

    return AT_SUCCESS;
}

/**
 * (internal)
 * @brief Dequeue thread from queue.
 * @param queue Queue to dequeue from.
 * @return Null for empty queue or error.
 */
atThread* _atQDequeue(_atQueue* queue) {
    if (!queue || _atQIsEmpty(queue, NULL)) { return NULL; }

    _atQNode* old_front = NULL;

    _AT_LOCK(queue->lock);
    {
        old_front    = queue->front;
        queue->front = queue->front->next;
        if (!queue->front) { queue->back = NULL; }
    }
    _AT_UNLOCK(queue->lock);

    atThread* old_front_thread = old_front->thread;
    free(old_front);
    return old_front_thread;
}

/*
 * TASK
 * */

/**
 * @class atTask
 * @brief Thread task return type.
 */
struct atTask {
    _at_mutex_t     lock;
    struct timespec update_freq;
    at_task_func_t  fn;
    void*           args;
    at_error_t      err;
    b8              done;
};

/**
 * (internal)
 * @brief Create a task.
 * @param fn Function to execute.
 * @param args Arguments to the function.
 * @param update_freq Frequency in which task checks whether it's
 *                    been completed when awaiting, in seconds.
 * @return
 */
static inline atTask _atCreateTask(at_task_func_t fn, void* args, f64 update_freq) {
    atTask task = {
        .update_freq = _atCreateTime(update_freq), .fn = fn, .args = args, .done = false
    };

    _AT_MUTEX_CREATE(task.lock);
    return task;
}

at_error_t _AT_FORWARD_DECL atAwaitTask(atTask*);

/**
 * (internal)
 * @brief Destroy a task.
 * @param task Task to be destroyed.
 * @return
 */
static inline at_error_t _atDestroyTask(atTask* task) {
    if (!task) { return AT_NULL_INPUT; }

    at_error_t err = atAwaitTask(task);
    if (err) { return err; }

    _AT_MUTEX_DESTROY(task->lock);
    return AT_SUCCESS;
}

atTask* _AT_FORWARD_DECL atLaunchTask(at_task_func_t, void*, atThread*);

atTask* _AT_FORWARD_DECL atScheduleTask(at_task_func_t, void*, atPool*);

/**
 * @brief Await a task. You must always await a launched or
 *        scheduled task before accessing its values to avoid a race condition.
 * @param task Task to be awaited.
 * @return Error code returned from the task.
 */
at_error_t atAwaitTask(atTask* task) {
    if (!task) { return AT_NULL_INPUT; }

    _AT_LOCK(task->lock);
    {
        while (!task->done) {
            _AT_UNLOCK(task->lock);
            _AT_THREAD_SLEEP(task->update_freq);
            _AT_LOCK(task->lock);
        }
    }
    _AT_UNLOCK(task->lock);

    return task->err;
}

/*
 * THREAD
 * */

/**
 * @class atThread
 * @brief Thread structure.
 */
struct atThread {
    atTask          task;
    _at_mutex_t     lock;
    struct timespec update_freq;
    atPool*         pool;
    _at_pid_t       pid;
    b8              exit;
    b8              working;
};

i32 _AT_FORWARD_DECL _atThreadLoop(void*);

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

    *thread = (atThread){ .task        = _atCreateTask(NULL, NULL, update_freq),
                          .pool        = pool,
                          .update_freq = _atCreateTime(update_freq),
                          .exit        = false,
                          .working     = false };

    if (_AT_MUTEX_CREATE(thread->lock) != thrd_success) { return AT_SYS_ERROR; }
    if (_AT_THREAD_CREATE(thread->pid, _atThreadLoop, thread) != thrd_success) {
        return AT_SYS_ERROR;
    }

    return AT_SUCCESS;
}

/**
 * @brief Awaits the threads current task (if it has one), and destroys it.
 * @param thread Thread to be killed.
 * @return
 */
static inline at_error_t atKillThread(atThread* thread) {
    if (!thread) { return AT_NULL_INPUT; }

    at_error_t err = _atDestroyTask(&thread->task);
    if (err) { return err; }

    _AT_LOCK(thread->lock);
    thread->exit = true;
    _AT_UNLOCK(thread->lock);

    if (_AT_THREAD_JOIN(thread->pid) != thrd_success) { return AT_SYS_ERROR; }
    _AT_MUTEX_DESTROY(thread->lock);

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
    _atQueue        free;
    struct timespec update_freq;
    atThread*       pool;
    u32             thread_count;
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

    pool->update_freq  = _atCreateTime(update_freq);
    pool->thread_count = thread_count;
    pool->free         = _atCreateQueue();

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

    _AT_LOCK(thread->lock);
    {
        if (thread->working) {
            _AT_UNLOCK(thread->lock);
            return NULL;
        }

        thread->task.fn   = fn;
        thread->task.args = args;
        thread->task.done = false;
        thread->working   = true;
    }
    _AT_UNLOCK(thread->lock);

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

    while (_atQIsEmpty(&pool->free, NULL)) { _AT_THREAD_SLEEP(pool->update_freq); }

    atThread* free_thread = _atQDequeue(&pool->free);
    return atLaunchTask(fn, args, free_thread);
}

/*
 * THREAD
 * */

/**
 * (internal)
 * @brief Thread running loop.
 * @param arg Thread handle.
 */
i32 _atThreadLoop(void* arg) {
    atThread* thread = (atThread*)arg;
    atTask*   task   = &thread->task;

    _AT_LOCK(thread->lock);
    {
        while (!thread->exit) {
            if (!thread->working) {
                _AT_UNLOCK(thread->lock);
                _AT_THREAD_SLEEP(thread->update_freq);
            } else {
                thread->working = false;
                _AT_UNLOCK(thread->lock);

                _AT_LOCK(task->lock);
                task->err  = task->fn(task->args);
                task->done = true;
                _AT_UNLOCK(task->lock);

                _atQEnqueue(&thread->pool->free, thread);
            }

            _AT_LOCK(thread->lock);
        }
    }
    _AT_UNLOCK(thread->lock);

    return 0;
}

#endif
