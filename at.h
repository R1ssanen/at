/**
 * @file at.h
 * @brief Asynchronous Tasks - multithreaded task scheduling library
 * @author github.com/R1ssanen
 */

#ifndef AT_H
#define AT_H

#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define true  1
#define false 0

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

static inline void
_at_LogAssertMsg(const char* expr, const char* file, i32 line, const char* fmt, ...) {
    char    msg[1024];
    va_list args;
    va_start(args, fmt);
    vsprintf(msg, fmt, args);
    va_end(args);

    fprintf(
        stderr, "(AT) Assertion [ %s ] failed\n\tat line %i\n\tof %s\n\twith message '%s'.\n", expr,
        line, file, msg
    );
}

#ifndef NDEBUG
#    define _at_ASSERT_MSG(expr, ...)                                                              \
        do {                                                                                       \
            if (!(expr)) {                                                                         \
                _at_LogAssertMsg(#expr, __FILE__, __LINE__, __VA_ARGS__);                          \
                abort();                                                                           \
            }                                                                                      \
        } while (false);
#else
#    define _at_ASSERT                                                                             \
        do {                                                                                       \
        } while (false);
#endif

#define _at_FORWARD_DECL

typedef enum {
    AT_SUCCESS     = 0,
    AT_FAILURE     = 0x000001,
    AT_NULL_INPUT  = 0x000011,
    AT_NULL_OUTPUT = 0x000101,
    AT_SYS_ERROR   = 0x001001,
} at_return_t;

typedef void* (*at_task_func_t)(void*);

typedef pthread_mutex_t _at_mutex_t;
typedef pthread_cond_t  _at_cond_t;
typedef pthread_t       _at_pid_t;

#define _at_LOCK(mtx)       pthread_mutex_lock(&mtx)
#define _at_UNLOCK(mtx)     pthread_mutex_unlock(&mtx)
#define _at_SIGNAL(cond)    pthread_cond_signal(&cond)
#define _at_WAIT(cond, mtx) pthread_cond_wait(&cond, &mtx)

typedef struct atTask   atTask;
typedef struct atThread atThread;
typedef struct atPool   atPool;

typedef struct _atQNode _atQNode;
typedef struct _atQueue _atQueue;

/*
 * QUEUE (internal)
 * */

struct _atQNode {
    atThread* data;
    _atQNode* next;
};

// NOTE: all queue operations are thread-safe
struct _atQueue {
    _at_mutex_t lock;
    _atQNode*   front;
    _atQNode*   back;
};

static inline _atQueue _atCreateQueue(void) {
    return (_atQueue){ .lock = PTHREAD_MUTEX_INITIALIZER, .front = NULL, .back = NULL };
}

b8 _at_FORWARD_DECL        _atQIsEmpty(_atQueue*);

atThread* _at_FORWARD_DECL _atQDequeue(_atQueue*);

static inline at_return_t  _atDestroyQueue(_atQueue* queue) {
    if (!queue) { return AT_NULL_INPUT; }

    while (!_atQIsEmpty(queue)) {
        if (!_atQDequeue(queue)) { return AT_FAILURE; }
    }

    return AT_SUCCESS;
}

static inline at_return_t _atQEnqueue(_atQueue* queue, atThread* thread) {
    if (!queue) { return AT_NULL_INPUT; }

    _atQNode* node = malloc(sizeof(_atQNode));
    if (!node) { return AT_SYS_ERROR; }

    *node = (_atQNode){ .data = thread, .next = NULL };

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

b8 _atQIsEmpty(_atQueue* queue) {
    _at_ASSERT_MSG(queue != NULL, "Cannot confirm if null queue is empty.");

    _at_LOCK(queue->lock);
    b8 is_empty = (!queue->front) && (!queue->back);
    _at_UNLOCK(queue->lock);

    return is_empty;
}

atThread* _atQDequeue(_atQueue* queue) {
    _at_ASSERT_MSG(queue != NULL, "Cannot dequeue from a null queue.");
    if (_atQIsEmpty(queue)) { return NULL; }

    _atQNode* tmp       = NULL;
    atThread* old_front = NULL;

    _at_LOCK(queue->lock);
    {
        tmp          = queue->front;
        old_front    = tmp->data;

        queue->front = queue->front->next;
        if (!queue->front) { queue->back = NULL; }
    }
    _at_UNLOCK(queue->lock);

    free(tmp);
    return old_front;
}

/*
 * TASK
 * */

struct atTask {
    _at_cond_t     ret_cond;
    _at_mutex_t    lock;
    at_task_func_t fn;
    void*          args;
    void*          ret;
};

static inline atTask _atCreateTask(at_task_func_t fn, void* args) {
    _at_ASSERT_MSG(fn != NULL, "Cannot create task with null function.");

    return (atTask){ .ret_cond = PTHREAD_COND_INITIALIZER,
                     .lock     = PTHREAD_MUTEX_INITIALIZER,
                     .fn       = fn,
                     .args     = args,
                     .ret      = NULL };
}

atTask* _at_FORWARD_DECL atLaunchTask(at_task_func_t, void*, atThread*);

atTask* _at_FORWARD_DECL atScheduleTask(at_task_func_t, void*, atPool*);

static inline void*      atAwaitTask(atTask* task) {
    _at_ASSERT_MSG(task != NULL, "Cannot await a null task.");

    _at_LOCK(task->lock);
    _at_WAIT(task->ret_cond, task->lock);
    _at_UNLOCK(task->lock);

    fputs("awaiting complete\n", stderr);

    return task->ret;
}

/*
 * THREAD
 * */

struct atThread {
    atTask      task;
    _at_cond_t  work_cond;
    _at_mutex_t lock;
    atPool*     pool;
    _at_pid_t   pid;
    u32         update_freq_ms;
    b8          activated;
    b8          exit;
};

void* _at_FORWARD_DECL    _atThreadLoop(void*);

static inline at_return_t atCreateThread(atPool* pool, f64 update_freq, atThread* thread) {
    if (!thread) { return AT_NULL_OUTPUT; }

    *thread = (atThread){ .task           = (atTask){},
                          .work_cond      = PTHREAD_COND_INITIALIZER,
                          .lock           = PTHREAD_MUTEX_INITIALIZER,
                          .pool           = pool,
                          .pid            = 0,
                          .update_freq_ms = (u32)(update_freq * 1000.f),
                          .activated      = false,
                          .exit           = false };

    if (pthread_create(&thread->pid, NULL, _atThreadLoop, thread) != 0) { return AT_SYS_ERROR; }
    return AT_SUCCESS;
}

static inline at_return_t atKillThread(atThread* thread) {
    if (!thread) { return AT_NULL_INPUT; }

    _at_LOCK(thread->lock);
    thread->exit = true;
    _at_UNLOCK(thread->lock);

    if (pthread_join(thread->pid, NULL) != 0) { return AT_SYS_ERROR; }

    fprintf(stderr, "%lu: killed\n", thread->pid);
    return AT_SUCCESS;
}

/*
 * POOL
 * */

struct atPool {
    _atQueue  free;
    atThread* pool;
    u32       update_freq_ms;
    u32       thread_count;
};

static inline at_return_t atCreatePool(u32 thread_count, f64 update_freq, atPool* pool) {
    _at_ASSERT_MSG(pool != NULL, "Cannot create a pool to null output parameter.");
    _at_ASSERT_MSG(update_freq >= 0.f, "Update frequency cannot be negative.");

    pool->pool = malloc(sizeof(atThread) * thread_count);
    if (!pool->pool) { return AT_SYS_ERROR; }

    pool->update_freq_ms = (u32)(update_freq * 1000.f);
    pool->thread_count   = thread_count;
    pool->free           = _atCreateQueue();

    at_return_t err;
    for (u64 i = 0; i < thread_count; ++i) {
        if ((err = atCreateThread(pool, update_freq, &pool->pool[i]))) { return err; }
        if ((err = _atQEnqueue(&pool->free, &pool->pool[i]))) { return err; }
    }

    return AT_SUCCESS;
}

static inline at_return_t atDestroyPool(atPool* pool) {
    _at_ASSERT_MSG(pool != NULL, "Cannot destroy null pool.");

    at_return_t err;
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

atTask* atLaunchTask(at_task_func_t fn, void* args, atThread* thread) {
    _at_ASSERT_MSG(thread != NULL, "Cannot launch task with null thread.");

    _at_LOCK(thread->lock);
    {
        thread->task      = _atCreateTask(fn, args);
        thread->task.args = &thread->pid; // TODO: < remove
        thread->activated = true;
    }
    _at_UNLOCK(thread->lock);

    return &thread->task;
}

atTask* atScheduleTask(at_task_func_t fn, void* args, atPool* pool) {
    _at_ASSERT_MSG(pool != NULL, "Cannot schedule task with null pool.");

    while (_atQIsEmpty(&pool->free)) { usleep(pool->update_freq_ms); }

    atThread* free_thread = _atQDequeue(&pool->free);
    return atLaunchTask(fn, args, free_thread);
}

/*
 * THREAD
 * */

void* _atThreadLoop(void* _arg) {
    atThread* thread = (atThread*)_arg;
    atTask*   task   = &thread->task;

    _at_LOCK(thread->lock);
    {
        while (!thread->exit) {
            if (!thread->activated) {
                _at_UNLOCK(thread->lock);
                usleep(thread->update_freq_ms);
            } else {
                thread->activated = false;
                _at_UNLOCK(thread->lock);

                task->ret = task->fn(task->args);
                fprintf(stderr, "%lu: done\n", thread->pid);
                _at_SIGNAL(task->ret_cond);
                _atQEnqueue(&thread->pool->free, thread);
            }

            _at_LOCK(thread->lock);
        }
    }
    _at_UNLOCK(thread->lock);

    fprintf(stderr, "%lu: exiting\n", thread->pid);
    pthread_exit(NULL); // return NULL;
}

#endif
