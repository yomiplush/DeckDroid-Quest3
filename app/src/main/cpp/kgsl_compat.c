/*
 * Horizon OS permits KGSL's GPUOBJ ABI to untrusted apps but denies the older
 * GPUMEM_ALLOC_ID / GPUMEM_FREE_ID pair that Turnip still uses.  Translate only
 * an EACCES from those requests, leaving every normal Android driver untouched.
 *
 * This source builds twice: as an AdrenoTools hook for the app-process
 * compositor and as an LD_PRELOAD library for gamescope, Steam and Proton.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifdef __ANDROID__
#include <android/log.h>
#define KGSL_LOG(...) __android_log_print(ANDROID_LOG_INFO, "kgsl-compat", __VA_ARGS__)
#else
#include <stdio.h>
#define KGSL_LOG(...) do { fprintf(stderr, "kgsl-compat: " __VA_ARGS__); fputc('\n', stderr); } while (0)
#endif

// The UAPI annotates user pointers. Bionic defines this through its kernel headers; glibc's
// cross headers do not, and the annotation has no userspace meaning.
#ifndef __user
#define __user
#endif

#include "adrenotools/src/hook/kgsl.h"

#ifdef __GLIBC__
typedef unsigned long ioctl_request_t;
#else
typedef int ioctl_request_t;
#endif

#define EXPORT __attribute__((visibility("default")))

static int raw_ioctl(int fd, ioctl_request_t request, void *argument) {
    return (int) syscall(SYS_ioctl, fd, request, argument);
}

typedef int (*ioctl_fn)(int, ioctl_request_t, ...);

// Preserve every existing preload hook. Only the GPUOBJ ioctls below deliberately go straight
// to the kernel so this wrapper cannot recurse through itself.
static int forwarded_ioctl(int fd, ioctl_request_t request, void *argument) {
    static ioctl_fn next;
    if (next == NULL) next = (ioctl_fn) dlsym(RTLD_NEXT, "ioctl");
    return next ? next(fd, request, argument) : raw_ioctl(fd, request, argument);
}

/* Fill Turnip's legacy allocation result from the GPUOBJ allocation it replaces. */
static int gpuobj_allocate(int fd, struct kgsl_gpumem_alloc_id *legacy) {
    struct kgsl_gpuobj_alloc object = {
        .size = legacy->size,
        .flags = legacy->flags,
    };
    if (raw_ioctl(fd, IOCTL_KGSL_GPUOBJ_ALLOC, &object) < 0) return -1;

    struct kgsl_gpuobj_info info = { .id = object.id };
    if (raw_ioctl(fd, IOCTL_KGSL_GPUOBJ_INFO, &info) < 0) {
        const int saved_errno = errno;
        struct kgsl_gpuobj_free free_object = { .id = object.id };
        raw_ioctl(fd, IOCTL_KGSL_GPUOBJ_FREE, &free_object);
        errno = saved_errno;
        return -1;
    }

    legacy->id = object.id;
    legacy->flags = (unsigned int) info.flags;
    legacy->size = info.size;
    legacy->mmapsize = object.mmapsize;
    legacy->gpuaddr = info.gpuaddr;
    return 0;
}

static int gpuobj_free(int fd, const struct kgsl_gpumem_free_id *legacy) {
    struct kgsl_gpuobj_free object = { .id = legacy->id };
    return raw_ioctl(fd, IOCTL_KGSL_GPUOBJ_FREE, &object);
}

/*
 * Quest's KGSL policy denies the legacy timestamp query although the same context's commands and
 * WAITTIMESTAMP stay usable. Turnip uses the query only for a zero-timeout poll of "has timestamp
 * T retired", and it must never report a command done early: the compositor would present the
 * frame before the GPU wrote it. So every submitted timestamp is remembered per context and the
 * retired value is found by asking WAITTIMESTAMP, with no timeout, about them (submission order is
 * retirement order, hence a binary search).
 */
#define TS_CONTEXTS 32
#define TS_RING 512

struct ts_context {
    unsigned int id;
    bool used;
    unsigned int retired;
    unsigned int head;
    unsigned int count;
    unsigned int ring[TS_RING];
};

static struct ts_context ts_contexts[TS_CONTEXTS];
static unsigned int ts_evict;
static volatile int ts_lock;
static volatile int readtimestamp_denied;
static volatile int waittimestamp_denied;

static void ts_acquire(void) {
    while (__atomic_exchange_n(&ts_lock, 1, __ATOMIC_ACQUIRE)) syscall(SYS_sched_yield);
}

static void ts_release(void) {
    __atomic_store_n(&ts_lock, 0, __ATOMIC_RELEASE);
}

static struct ts_context *ts_find(unsigned int id) {
    for (int i = 0; i < TS_CONTEXTS; i++)
        if (ts_contexts[i].used && ts_contexts[i].id == id) return &ts_contexts[i];
    return NULL;
}

static void ts_record(unsigned int id, unsigned int timestamp) {
    ts_acquire();
    struct ts_context *context = ts_find(id);
    if (context == NULL) {
        for (int i = 0; i < TS_CONTEXTS && context == NULL; i++)
            if (!ts_contexts[i].used) context = &ts_contexts[i];
        if (context == NULL) context = &ts_contexts[ts_evict++ % TS_CONTEXTS];
        *context = (struct ts_context) { .id = id, .used = true };
    }
    if (context->count == TS_RING) {
        // Keep the newest bounded window without advancing the retired watermark on assumption.
        context->head = (context->head + 1) % TS_RING;
        context->count--;
    }
    context->ring[(context->head + context->count++) % TS_RING] = timestamp;
    ts_release();
}

static void ts_forget(unsigned int id) {
    ts_acquire();
    struct ts_context *context = ts_find(id);
    if (context != NULL) context->used = false;
    ts_release();
}

// The nonblocking wait ioctl succeeds only when the timestamp has retired. Errors are unknown,
// never evidence of completion; in particular, do not turn another SELinux denial into a frame.
static bool ts_retired(int fd, unsigned int id, unsigned int timestamp) {
    if (__atomic_load_n(&waittimestamp_denied, __ATOMIC_RELAXED)) return false;
    struct kgsl_device_waittimestamp_ctxtid wait = { .context_id = id, .timestamp = timestamp };
    if (raw_ioctl(fd, IOCTL_KGSL_DEVICE_WAITTIMESTAMP_CTXTID, &wait) == 0) return true;
    if (errno == EACCES) {
        __atomic_store_n(&waittimestamp_denied, 1, __ATOMIC_RELAXED);
        KGSL_LOG("KGSL WAITTIMESTAMP_CTXTID denied; timestamp polling remains conservative");
    }
    return false;
}

static int read_retired_timestamp(int fd, struct kgsl_cmdstream_readtimestamp_ctxtid *request) {
    ts_acquire();
    struct ts_context *context = ts_find(request->context_id);
    if (context != NULL && context->count > 0) {
        int low = -1;
        int high = (int) context->count - 1;
        while (low < high) {
            const int middle = (low + high + 1) / 2;
            if (ts_retired(fd, context->id, context->ring[(context->head + middle) % TS_RING]))
                low = middle;
            else
                high = middle - 1;
        }
        if (low >= 0) {
            context->retired = context->ring[(context->head + low) % TS_RING];
            context->head = (context->head + low + 1) % TS_RING;
            context->count -= low + 1;
        }
    }
    request->timestamp = context != NULL ? context->retired : 0;
    ts_release();
    return 0;
}

EXPORT int ioctl(int fd, ioctl_request_t request, ...) {
    va_list arguments;
    va_start(arguments, request);
    void *argument = va_arg(arguments, void *);
    va_end(arguments);

    if (argument == NULL || _IOC_TYPE(request) != KGSL_IOC_TYPE)
        return forwarded_ioctl(fd, request, argument);
    if (__atomic_load_n(&readtimestamp_denied, __ATOMIC_RELAXED) &&
        request == (ioctl_request_t) IOCTL_KGSL_CMDSTREAM_READTIMESTAMP_CTXTID)
        return read_retired_timestamp(fd, argument);

    const int result = forwarded_ioctl(fd, request, argument);
    if (result == 0) {
        if (request == (ioctl_request_t) IOCTL_KGSL_GPU_COMMAND) {
            const struct kgsl_gpu_command *command = argument;
            ts_record(command->context_id, command->timestamp);
        } else if (request == (ioctl_request_t) IOCTL_KGSL_GPU_AUX_COMMAND) {
            const struct kgsl_gpu_aux_command *command = argument;
            ts_record(command->context_id, command->timestamp);
        } else if (request == (ioctl_request_t) IOCTL_KGSL_DRAWCTXT_DESTROY) {
            ts_forget(((const struct kgsl_drawctxt_destroy *) argument)->drawctxt_id);
        }
        return result;
    }
    const int failure = errno;
    if (failure != EACCES) {
        // A bounded trace of every other KGSL failure: it names the call a policy blocks.
        static int traced;
        if (traced < 32) {
            traced++;
            KGSL_LOG("KGSL ioctl %#lx failed: errno %d", (unsigned long) request, failure);
            errno = failure;
        }
        return result;
    }
    if (request == (ioctl_request_t) IOCTL_KGSL_GPUMEM_ALLOC_ID) {
        static int logged;
        if (!logged) {
            logged = 1;
            KGSL_LOG("translating GPUMEM_ALLOC_ID after EACCES");
        }
        return gpuobj_allocate(fd, argument);
    }
    if (request == (ioctl_request_t) IOCTL_KGSL_GPUMEM_FREE_ID) return gpuobj_free(fd, argument);
    if (request == (ioctl_request_t) IOCTL_KGSL_CMDSTREAM_READTIMESTAMP_CTXTID) {
        __atomic_store_n(&readtimestamp_denied, 1, __ATOMIC_RELAXED);
        KGSL_LOG("emulating CMDSTREAM_READTIMESTAMP_CTXTID from WAITTIMESTAMP after EACCES");
        return read_retired_timestamp(fd, argument);
    }
    KGSL_LOG("unhandled KGSL ioctl %#lx denied with EACCES", (unsigned long) request);
    errno = failure;
    return result;
}
