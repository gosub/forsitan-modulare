// imber_worker.hpp — starting a background render from the audio thread.
//
// imber and sylla both kick a detached worker off inside process(), which
// on Linux runs on the audio callback thread. That thread is SCHED_FIFO
// under JACK, PipeWire or any RT-capable backend, and pthread_create's
// default attributes are PTHREAD_INHERIT_SCHED: the worker silently came
// up realtime too, then computed for hundreds of milliseconds without
// blocking. That is precisely what RLIMIT_RTTIME exists to stop. RTKit
// (and PipeWire and jackd2 through it) installs a 200 ms soft limit on any
// process it grants realtime to, so the kernel answered a full 50 MB bank
// render with SIGXCPU and killed Rack, mid-generation, every time the
// module was added from the browser. Nothing was wrong with the render
// itself, which is why it never reproduced with the audio device set to
// "no device" (no callback thread, no engine, no render) and why sylla,
// whose single-buffer renders finish inside the limit, survived.
//
// So the worker asks for SCHED_OTHER explicitly rather than inheriting.
// Only the scheduling policy changes; the render is the same work, and it
// can no longer starve the audio thread it was cloned from either.
#pragma once

#include <thread>

#if defined(__linux__) || defined(__APPLE__)
#define IMBER_WORKER_POSIX 1
#include <pthread.h>
#include <sched.h>
#endif

namespace imber_worker {

#ifdef IMBER_WORKER_POSIX
template <typename Fn>
inline void* trampoline(void* arg) {
    Fn* fn = static_cast<Fn*>(arg);
    (*fn)();
    delete fn;
    return NULL;
}
#endif

// Run fn on a fresh detached thread at ordinary (non-realtime) priority.
// Falls back to a plain std::thread wherever the explicit request cannot
// be made, which is no worse than what it replaces.
//
// Returns false if no thread could be started, and never throws: the
// caller is the audio thread, and starting a thread is a resource request
// like any other. std::thread reports failure by throwing, and under a
// jack client's mlockall a failed pthread_create took Rack down with
// "terminate called after throwing an instance of 'std::system_error'".
template <typename Fn>
inline bool startDetached(const Fn& fn) {
#ifdef IMBER_WORKER_POSIX
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) == 0) {
        // the schedparam left in the attributes by init() is the platform's
        // default for a normal thread, which is what we want
        bool ready =
            pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED) == 0
            && pthread_attr_setschedpolicy(&attr, SCHED_OTHER) == 0
            && pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) == 0;
        // Ask for a modest stack rather than taking the default, which on
        // glibc is whatever RLIMIT_STACK happens to be. A session that sets
        // that limit high hands every new thread an enormous mapping it
        // will never touch, and pthread_create fails for want of it. The
        // render's deepest frame measures under a kilobyte.
        size_t stackBytes = 1024 * 1024;
        if (stackBytes < (size_t)PTHREAD_STACK_MIN)
            stackBytes = (size_t)PTHREAD_STACK_MIN;
        pthread_attr_setstacksize(&attr, stackBytes);
        if (ready) {
            Fn* held = NULL;
            try {
                held = new Fn(fn);
            }
            catch (...) {
                pthread_attr_destroy(&attr);
                return false;
            }
            pthread_t th;
            if (pthread_create(&th, &attr, &trampoline<Fn>, held) == 0) {
                pthread_attr_destroy(&attr);
                return true;
            }
            delete held;
        }
        pthread_attr_destroy(&attr);
    }
#endif
    try {
        std::thread(fn).detach();
        return true;
    }
    catch (...) {
        return false;
    }
}

} // namespace imber_worker
