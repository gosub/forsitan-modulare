// smoke_oom - imber and sylla must survive a render they cannot allocate.
//
// Both start a background render from process(), i.e. from the audio
// thread. A jack client runs with its memory locked (libjack calls
// mlockall), so every allocation the process makes afterwards is locked
// memory that the kernel can refuse. std::thread and operator new report
// that by throwing, and an exception escaping process() terminates Rack
// with nothing written to the log: exactly the crash reported on Ubuntu
// 24.04 (community thread 26023), which survives the module being added
// with the audio device set to "no device" because that path runs the
// engine on Rack's own fallback thread instead of a jack client's.
//
// RLIMIT_AS stands in for the locked-memory ceiling here: it needs no
// privileges and fails the same way on any Linux box. The module is given
// room to exist but not room for a 50 MB bank.
#include "smoke_harness.hpp"
#include "../src/imber.cpp"
#include "../src/sylla.cpp"

#include <sys/resource.h>
#include <unistd.h>
#include <thread>
#include <chrono>

// current address-space size in bytes, from /proc/self/statm (pages)
static size_t vmSize() {
    FILE* f = fopen("/proc/self/statm", "r");
    if (!f)
        return 0;
    unsigned long pages = 0;
    if (fscanf(f, "%lu", &pages) != 1)
        pages = 0;
    fclose(f);
    return (size_t)pages * (size_t)sysconf(_SC_PAGESIZE);
}

static bool capAddressSpace(size_t headroom) {
    size_t vm = vmSize();
    if (!vm)
        return false;
    struct rlimit rl;
    if (getrlimit(RLIMIT_AS, &rl) != 0)
        return false;
    rl.rlim_cur = vm + headroom;
    return setrlimit(RLIMIT_AS, &rl) == 0;
}

int main(int argc, char** argv) {
    bool header = true;
    for (int i = 1; i < argc; i++)
        if (!std::strcmp(argv[i], "--no-header")) header = false;
    rack::random::init();
    if (header) printf("module,check,value,pass\n");

    // build the modules first: they must be allowed to exist, it is the
    // bank that must not fit
    Imber* im = new Imber();
    Sylla* sy = new Sylla();
    if (!capAddressSpace(24u * 1024 * 1024)) {
        report("oom", "rlimit_as_set", 0, true);   // cannot test here
        return 0;
    }

    long frame = 0;
    // imber: the first frame starts the bank, which cannot be allocated
    for (int i = 0; i < 4096; i++)
        im->process(makeArgs(frame++));
    for (int w = 0; w < 400 && im->bankJob; w++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        im->process(makeArgs(frame++));
    }
    for (int i = 0; i < (int)(0.5f * SR); i++)
        im->process(makeArgs(frame++));
    report("oom", "imber_survives_failed_bank", im->bankFailed ? 1 : 0,
           im->bankFailed && !im->eng.bank);
    // and it must not thrash: one refusal, then quiet until asked again
    bool retried = false;
    for (int i = 0; i < 4096; i++) {
        im->process(makeArgs(frame++));
        if (im->bankJob) retried = true;
    }
    report("oom", "imber_no_retry_storm", retried ? 1 : 0, !retried);
    // audio stays finite and silent rather than reading a bank it never got
    Stats s;
    for (int i = 0; i < (int)(0.5f * SR); i++) {
        im->process(makeArgs(frame++));
        s.add(im->outputs[Imber::LEFT_OUTPUT].getVoltage());
    }
    report("oom", "imber_silent_not_nan", s.nans, s.nans == 0 && s.peak < 1e-3f);

    // sylla renders one buffer, which is small enough that it may well
    // succeed under the cap; either way it must not take the process down
    long sf = 0;
    for (int i = 0; i < 4096; i++)
        sy->process(makeArgs(sf++));
    for (int w = 0; w < 200 && sy->job; w++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        sy->process(makeArgs(sf++));
    }
    for (int i = 0; i < (int)(0.5f * SR); i++)
        sy->process(makeArgs(sf++));
    report("oom", "sylla_survives", sy->renderFailed ? 1 : 0, true);

    return failures ? 1 : 0;
}
