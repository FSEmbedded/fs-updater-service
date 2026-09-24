#include "service.h"

#include <fcntl.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <systemd/sd-journal.h>
#include <syslog.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

/* Stable Linux kernel ABI; provide locally in case <linux/ioprio.h>
 * is not on the include path (it isn't on every libc setup). */
#ifndef IOPRIO_WHO_PROCESS
#define IOPRIO_WHO_PROCESS 1
#endif
#ifndef IOPRIO_CLASS_BE
#define IOPRIO_CLASS_BE    2
#endif
#ifndef IOPRIO_CLASS_SHIFT
#define IOPRIO_CLASS_SHIFT 13
#endif

namespace {

/* Env knob reader: returns the parsed int, or `dflt` when the var is
 * unset, empty, or unparseable. Out-of-range values use `dflt`. */
int read_env_int_or(const char* name, int dflt, int lo, int hi)
{
    const char* raw = std::getenv(name);
    if (raw == nullptr || raw[0] == '\0') return dflt;
    char* end = nullptr;
    const long v = std::strtol(raw, &end, 10);
    if (end == raw || *end != '\0') return dflt;
    if (v < lo || v > hi)            return dflt;
    return static_cast<int>(v);
}

} // namespace

static constexpr const char* OBJ_PATH  = "/de/fsembedded/fsupdate1";
static constexpr const char* INTERFACE = "de.fsembedded.fsupdate1";

/* The interface's own version, independent of the daemon's package version --
 * a caller sees only the bus, and the two do not move together. Additive
 * changes raise it; the trailing 1 in the interface name is what a breaking
 * change would move instead. The rules are in the contract, and this number is
 * meaningless without them. */
static constexpr uint32_t FSUPDATE_INTERFACE_VERSION = 6;

/* Distinct D-Bus error name for "a conflicting operation is already in
 * flight". Every busy rejection uses this name + -EBUSY so the client can
 * tell busy apart from bad arguments (-EINVAL) or a missing updater
 * (-ENOSYS). */
static constexpr const char* ERROR_BUSY = "de.fsembedded.fsupdate1.Error.Busy";

/* Distinct D-Bus error name for "the updater backend is unavailable",
 * paired with -ENOSYS. A handler-return errno does not cross the bus once
 * ret_err carries a name, so without this name the client's no-updater
 * branch is unreachable and would fall through to a generic failure. */
static constexpr const char* ERROR_NO_UPDATER = "de.fsembedded.fsupdate1.Error.NoUpdater";

/* Reject with the busy contract: the ERROR_BUSY name plus -EBUSY. The errno
 * is returned explicitly rather than propagated from sd_bus_error_set, which
 * maps a custom (unregistered) name to -EIO — that would defeat the client's
 * name+errno busy detection. Routing every busy rejection through here makes
 * the name/errno pairing impossible to get wrong. */
static int reject_busy(sd_bus_error* e, const char* msg)
{
    sd_bus_error_set(e, ERROR_BUSY, msg);
    return -EBUSY;
}

/* Reject when the updater backend is missing: distinct name + -ENOSYS
 * (errno returned explicitly, same reason as reject_busy). */
static int reject_no_updater(sd_bus_error* e)
{
    sd_bus_error_set(e, ERROR_NO_UPDATER, "updater not available");
    return -ENOSYS;
}

/* One error name per library failure category, so a method rejection says what
 * kind of failure it was instead of collapsing every one into Error.Failed and
 * -EIO. The names mirror the categories the install outcome publishes, which is
 * what makes the two doors one vocabulary rather than two.
 *
 * "system" and anything unmapped stay on the generic name deliberately: they
 * are the categories that carry no promise about their cause, and inventing a
 * name for them would suggest one. */
static const char* error_name_for(const std::string& kind)
{
    if (kind == "generic")            { return "de.fsembedded.fsupdate1.Error.Generic"; }
    if (kind == "not_allowed_state")  { return "de.fsembedded.fsupdate1.Error.NotAllowedState"; }
    if (kind == "update_in_progress") { return "de.fsembedded.fsupdate1.Error.UpdateInProgress"; }
    if (kind == "internal")           { return "de.fsembedded.fsupdate1.Error.Internal"; }
    if (kind == "nothing_to_apply")   { return "de.fsembedded.fsupdate1.Error.NothingToApply"; }
    return SD_BUS_ERROR_FAILED;
}

/* Reject with the failure's own category. The NAME is what a bus caller acts
 * on -- the returned errno does not cross the wire, a client derives its own
 * from the name -- so the category names above are the contract and this errno
 * only serves an in-process caller.
 *
 * Guarded on > 0 rather than != 0: UpdaterError takes an unchecked int and the
 * interface exists to be driven by a double, so a negative value would negate
 * into a positive return and read as success. Where no errno was carried, -EIO
 * is returned. */
static int reject_updater(sd_bus_error* e, const UpdaterError& err)
{
    sd_bus_error_set(e, error_name_for(err.kind), err.what());
    return err.errorno > 0 ? -err.errorno : -EIO;
}

/* ── Constructor / Destructor ───────────────────────────────────────────── */

Service::Service(sd_bus* bus, sd_event* event, std::unique_ptr<IUpdater> updater)
    : m_bus(bus)
    , m_event(event)
    , m_updater(std::move(updater))
{
    m_wakeup_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (m_wakeup_fd < 0) {
        sd_journal_print(LOG_ERR, "fs-updater-service: eventfd: %s\n", strerror(errno));
        return;
    }

    int r = sd_event_add_io(m_event, &m_wakeup_src, m_wakeup_fd,
                             EPOLLIN, &Service::on_wakeup, this);
    if (r < 0)
        sd_journal_print(LOG_ERR, "fs-updater-service: sd_event_add_io: %s\n", strerror(-r));

    /* Worker-tuning knobs, env-driven. See service.h for rationale.
     * Read once at construction so a tester can setenv() before
     * Service is built; production picks up the values from the
     * systemd unit's Environment= lines. */
    m_progress_throttle_ms = read_env_int_or("FSUPDATE_PROGRESS_MS", 250,
                                              /*lo=*/0,  /*hi=*/60000);
    m_worker_nice          = read_env_int_or("FSUPDATE_WORKER_NICE",   5,
                                              /*lo=*/-20, /*hi=*/19);
    m_worker_ioprio        = read_env_int_or("FSUPDATE_WORKER_IOPRIO", 7,
                                              /*lo=*/0,  /*hi=*/7);
    m_inhibit_sleep        = read_env_int_or("FSUPDATE_INHIBIT_SLEEP", 1,
                                              /*lo=*/0,  /*hi=*/1);

    /* Only InstallState is reconstructed at startup: it is session-local
     * and a poll loop reads it between install and apply, so "idle" after a
     * restart would read as a failed install. The durable state proves a
     * completed install; everything else stays at its defaults.
     * Defensive: m_updater may be null and the query may throw; the service
     * then starts with the defaults. */
    if (m_updater != nullptr) {
        try {
            if (m_updater->check_update_available().available) {
                m_install_state = "finished";
                m_install_progress.store(100, std::memory_order_release);
            }
        } catch (const std::exception& e) {
            sd_journal_print(LOG_ERR,
                "fs-updater-service: install-state recovery skipped: %s\n",
                e.what());
        }
    }
}

/* ── Small state helpers ────────────────────────────────────────────────── */

bool Service::install_running() const
{
    return m_install_state == "in_progress";
}

void Service::reset_session_view()
{
    m_session_id        = 0;
    m_download_state    = "idle";
    m_download_progress = 0;
    m_install_state     = "idle";
    m_install_progress.store(0, std::memory_order_release);
    m_update_type       = {};
    m_update_version    = {};
    m_update_size       = 0;
    m_payload_path      = {};
}

Service::~Service()
{
    if (m_install_thread.joinable())
        m_install_thread.join();

    if (m_wakeup_src != nullptr)
        sd_event_source_unref(m_wakeup_src);

    if (m_wakeup_fd >= 0)
        close(m_wakeup_fd);
}

/* ── Published update view — derived on read, never cached ──────────────── */

bool Service::session_in_flight() const
{
    if (m_session_id == 0)
        return false;
    /* A terminal install ends the session's authority over the published
     * view; so does a failed download that never reached an install. */
    if (m_install_state == "finished" || m_install_state == "failed")
        return false;
    if (m_install_state == "idle" && m_download_state == "failed")
        return false;
    return true;
}

Service::PublishedUpdateView Service::published_update_view()
{
    PublishedUpdateView view;

    /* Loop-thread-only, like every sd-bus callback. While the worker
     * thread runs the library (install in progress), entering it from
     * here as well would break the library's single-threaded use; the
     * session values below answer for that window instead. */
    const bool worker_running = install_running();

    if (!worker_running && m_updater != nullptr) {
        try {
            const CheckUpdateInfo info = m_updater->check_update_available();
            view.update_available = info.available;
            view.update_type      = info.type;
            view.update_version   = info.version;
            view.update_size      = info.size;
            view.reboot_pending   = m_updater->reboot_pending();
        } catch (const std::exception& e) {
            /* A wedged durable query must not take down every property
             * read (one failing getter breaks GetAll wholesale); answer
             * with the empty view and leave the cause in the journal. */
            sd_journal_print(LOG_ERR,
                "fs-updater-service: durable state query failed: %s\n",
                e.what());
            view = PublishedUpdateView{};
        } catch (...) {
            /* This helper runs inside sd-bus C frames (property getters,
             * PropertiesChanged emission); a non-std throw unwinding
             * through them is std::terminate at best. Same containment
             * the install worker applies. */
            sd_journal_print(LOG_ERR,
                "fs-updater-service: durable state query failed: unknown exception\n");
            view = PublishedUpdateView{};
        }
    }

    if (session_in_flight()) {
        /* The session's announced metadata wins while it runs, so
         * subscribers never see empty strings mid-install; an update is
         * by definition announced. RebootPending keeps the durable
         * answer — mid-install it stays at its default false: the
         * durable state is being rewritten right then, and the settled
         * answer follows completion. */
        view.update_available = true;
        view.update_type      = m_update_type;
        view.update_version   = m_update_version;
        view.update_size      = m_update_size;
    }

    return view;
}

/* ── Wakeup handler (drains the eventfd the install thread signals) ─────── */

int Service::on_wakeup(sd_event_source* /*src*/, int fd, uint32_t /*revents*/, void* userdata)
{
    uint64_t v = 0;
    [[maybe_unused]] ssize_t n = ::read(fd, &v, sizeof(v));

    auto* svc = static_cast<Service*>(userdata);

    std::vector<PendingEvent> batch;
    {
        std::lock_guard<std::mutex> lk(svc->m_pending_mtx);
        batch.swap(svc->m_pending);
    }

    for (auto& ev : batch) {
        if (ev.type == PendingEvent::Progress) {
            if (!svc->install_running())
                continue;
            /* Release-store pairs with the acquire-load in
             * prop_install_progress and method_get_state. */
            svc->m_install_progress.store(ev.progress, std::memory_order_release);
            sd_bus_emit_properties_changed(svc->m_bus, OBJ_PATH, INTERFACE,
                "InstallProgress", nullptr);
        } else if (ev.type == PendingEvent::Completed) {
            /* Best-effort cancel: if CancelInstall fired while the
             * worker was running (or after it queued the completion),
             * override the outcome. The install may have actually
             * completed — we cannot know — but the API contract is
             * "you asked to cancel, we report cancelled". InstallProgress
             * is left at whatever the worker reached so observers can
             * still see how far it got. */
            const bool cancelled =
                svc->m_cancel_requested.load(std::memory_order_acquire);
            const bool ok = ev.success && !cancelled;
            if (ok) {
                svc->m_install_state = "finished";
                svc->m_install_progress.store(100, std::memory_order_release);
            } else {
                svc->m_install_state = "failed";
            }
            const std::string msg = cancelled
                ? std::string("cancelled by user (best-effort)")
                : ev.message;
            sd_bus_emit_properties_changed(svc->m_bus, OBJ_PATH, INTERFACE,
                "InstallState", "InstallProgress", nullptr);
            sd_bus_emit_signal(svc->m_bus, OBJ_PATH, INTERFACE, "InstallCompleted",
                "ubs", svc->m_session_id, static_cast<int>(ok), msg.c_str());
            /* The same outcome with the cause attached. A separate signal
             * rather than a wider InstallCompleted: extending a published
             * signature is invisible to a caller reading the leading fields
             * and fatal to a generated proxy that checks the whole thing.
             * Existing subscribers keep the signal they subscribed to. */
            sd_bus_emit_signal(svc->m_bus, OBJ_PATH, INTERFACE, "InstallOutcome",
                "ubis", svc->m_session_id, static_cast<int>(ok),
                cancelled ? 0 : ev.errorno,
                (cancelled ? "cancelled" : (ok ? "" : ev.kind.c_str())));
        } else {
            svc->m_install_state = "failed";
            sd_bus_emit_properties_changed(svc->m_bus, OBJ_PATH, INTERFACE,
                "InstallState", nullptr);
            sd_bus_emit_signal(svc->m_bus, OBJ_PATH, INTERFACE, "JobFailed",
                "us", svc->m_session_id, ev.message.c_str());
        }
    }

    return 0;
}

/* ── Method handlers ────────────────────────────────────────────────────── */

int Service::method_check_update_available(sd_bus_message* msg, void* userdata, sd_bus_error* ret_err)
{
    auto* svc = static_cast<Service*>(userdata);

    if (svc->m_updater == nullptr)
        return reject_no_updater(ret_err);

    CheckUpdateInfo info;
    try {
        info = svc->m_updater->check_update_available();
    } catch (const UpdaterError& e) {
        return reject_updater(ret_err, e);
    } catch (const std::exception& e) {
        sd_bus_error_set(ret_err, SD_BUS_ERROR_FAILED, e.what());
        return -EIO;
    }

    return sd_bus_reply_method_return(msg, "bsst",
        static_cast<int>(info.available),
        info.type.c_str(),
        info.version.c_str(),
        info.size);
}

int Service::method_start_download(sd_bus_message* msg, void* userdata, sd_bus_error* ret_err)
{
    auto* svc = static_cast<Service*>(userdata);

    const char* type    = nullptr;
    const char* version = nullptr;
    uint64_t    size    = 0;
    int r = sd_bus_message_read(msg, "sst", &type, &version, &size);
    if (r < 0) {
        sd_bus_error_set(ret_err, SD_BUS_ERROR_FAILED, "failed to read arguments");
        return r;
    }

    /* Reject a new download session while one is active OR while an install
     * is running. Without the install guard, download_state reads "finished"
     * for the whole install phase, so a second StartDownload would mint a new
     * session over the live worker — the completion of the old install would
     * then be applied to the new session. */
    if ((svc->m_session_id != 0 &&
         svc->m_download_state != "finished" &&
         svc->m_download_state != "failed") ||
        svc->install_running())
    {
        return reject_busy(ret_err, "session already active");
    }

    if (++svc->m_session_counter == 0)
        ++svc->m_session_counter;

    svc->m_session_id        = svc->m_session_counter;
    svc->m_update_type       = type;
    svc->m_update_version    = version;
    svc->m_update_size       = size;
    svc->m_download_state    = "in_progress";
    svc->m_download_progress = 0;
    svc->m_install_state     = "idle";
    svc->m_install_progress  = 0;
    svc->m_payload_path.clear();

    sd_bus_emit_properties_changed(svc->m_bus, OBJ_PATH, INTERFACE,
        "SessionId", "UpdateAvailable", "UpdateType", "UpdateVersion",
        "UpdateSize", "DownloadState", "DownloadProgress",
        "InstallState", "InstallProgress", "RebootPending", nullptr);

    return sd_bus_reply_method_return(msg, "u", svc->m_session_id);
}

int Service::method_report_download_progress(sd_bus_message* msg, void* userdata, sd_bus_error* ret_err)
{
    auto* svc = static_cast<Service*>(userdata);

    uint32_t sid   = 0;
    uint64_t bytes = 0;
    int r = sd_bus_message_read(msg, "ut", &sid, &bytes);
    if (r < 0) {
        sd_bus_error_set(ret_err, SD_BUS_ERROR_FAILED, "failed to read arguments");
        return r;
    }

    if (sid != svc->m_session_id) {
        sd_bus_error_set(ret_err, SD_BUS_ERROR_FAILED, "invalid session id");
        return -EINVAL;
    }

    if (svc->m_download_state != "in_progress") {
        sd_bus_error_set(ret_err, SD_BUS_ERROR_FAILED, "download not in progress");
        return -EINVAL;
    }

    int32_t progress = svc->m_update_size > 0
        ? static_cast<int32_t>(std::min<uint64_t>(bytes * 100 / svc->m_update_size, 100))
        : 0;

    svc->m_download_progress = progress;

    sd_bus_emit_properties_changed(svc->m_bus, OBJ_PATH, INTERFACE,
        "DownloadProgress", nullptr);

    return sd_bus_reply_method_return(msg, nullptr);
}

int Service::method_finish_download(sd_bus_message* msg, void* userdata, sd_bus_error* ret_err)
{
    auto* svc = static_cast<Service*>(userdata);

    uint32_t    sid  = 0;
    int         ok   = 0;
    const char* path = nullptr;
    int r = sd_bus_message_read(msg, "ubs", &sid, &ok, &path);
    if (r < 0) {
        sd_bus_error_set(ret_err, SD_BUS_ERROR_FAILED, "failed to read arguments");
        return r;
    }

    if (sid != svc->m_session_id) {
        sd_bus_error_set(ret_err, SD_BUS_ERROR_FAILED, "invalid session id");
        return -EINVAL;
    }

    if (svc->m_download_state != "in_progress") {
        sd_bus_error_set(ret_err, SD_BUS_ERROR_FAILED, "download not in progress");
        return -EINVAL;
    }

    if (ok) {
        svc->m_download_state    = "finished";
        svc->m_payload_path      = path;
        svc->m_download_progress = 100;
    } else {
        svc->m_download_state = "failed";
    }

    sd_bus_emit_properties_changed(svc->m_bus, OBJ_PATH, INTERFACE,
        "DownloadState", "DownloadProgress", nullptr);

    return sd_bus_reply_method_return(msg, nullptr);
}

int Service::method_start_install(sd_bus_message* msg, void* userdata, sd_bus_error* ret_err)
{
    auto* svc = static_cast<Service*>(userdata);

    uint32_t    sid         = 0;
    const char* update_type = nullptr;
    int r = sd_bus_message_read(msg, "us", &sid, &update_type);
    if (r < 0) {
        sd_bus_error_set(ret_err, SD_BUS_ERROR_FAILED, "failed to read arguments");
        return r;
    }

    /* The interface reserves session_id 0 as the "no active session"
     * sentinel; it is never issued by StartDownload. Reject explicitly
     * so that a client passing 0 (e.g. against a freshly-restarted
     * service that recovered UpdateAvailable but has SessionId==0)
     * does not collide with m_session_id's default. */
    if (sid == 0) {
        sd_bus_error_set(ret_err, SD_BUS_ERROR_INVALID_ARGS,
                         "session_id 0 is reserved");
        return -EINVAL;
    }

    if (sid != svc->m_session_id) {
        sd_bus_error_set(ret_err, SD_BUS_ERROR_FAILED, "invalid session id");
        return -EINVAL;
    }

    if (svc->m_download_state != "finished") {
        sd_bus_error_set(ret_err, SD_BUS_ERROR_FAILED, "download not finished");
        return -EINVAL;
    }

    if (svc->install_running())
        return reject_busy(ret_err, "install already in progress");

    if (svc->m_install_state != "idle") {
        /* finished / failed are terminal for this session and will not clear
         * themselves — report a permanent FAILED/-EINVAL rather than the
         * transient busy contract, so a client does not retry-loop forever. */
        sd_bus_error_set(ret_err, SD_BUS_ERROR_FAILED, "install already done or failed");
        return -EINVAL;
    }

    if (std::string(update_type) != svc->m_update_type) {
        sd_bus_error_set(ret_err, SD_BUS_ERROR_FAILED, "update_type mismatch");
        return -EINVAL;
    }

    if (svc->m_updater == nullptr)
        return reject_no_updater(ret_err);

    svc->spawn_install_worker(svc->m_update_type, svc->m_payload_path);

    return sd_bus_reply_method_return(msg, nullptr);
}

/* ── Worker spawn helper — shared by StartInstall (cloud) and
 *    InstallLocal (local). Caller has already validated state under
 *    the loop thread and stamped session_id / update_type / payload_path.
 *    This routine only handles the worker transition and dispatch.
 *
 *    Single-writer contract: the worker thread writes to m_pending under
 *    m_pending_mtx and signals via the wakeup eventfd. It never touches
 *    state-record fields directly — the loop thread applies all changes
 *    in on_wakeup, preserving the single-writer-per-resource invariant.
 */
void Service::spawn_install_worker(const std::string& update_type,
                                   const std::string& payload_path)
{
    /* Cheap join of any previously-completed worker; not a wait on a
     * still-running install (gated by the state check in the caller). */
    if (m_install_thread.joinable())
        m_install_thread.join();

    m_cancel_requested.store(false, std::memory_order_release);
    m_install_state    = "in_progress";
    m_install_progress.store(0, std::memory_order_release);

    sd_bus_emit_properties_changed(m_bus, OBJ_PATH, INTERFACE,
        "InstallState", "InstallProgress", nullptr);

    /* Progress callback: single-producer (worker) → SPSC queue → loop.
     * No sd_bus calls here — those are loop-thread-only by contract.
     *
     * Throttle: emit on value change AND when at least
     * m_progress_throttle_ms have elapsed since the last emission.
     * 100 % always emits so the bar reaches the end. 0 ms disables. */
    Service* svc = this;
    using clock = std::chrono::steady_clock;
    /* Seed last_t to "throttle_window + 1ms in the past" so the very
     * first event always passes the elapsed-time gate. Using
     * time_point::min() here would wrap the int64 ns subtraction. */
    const auto seed_past = clock::now() -
        std::chrono::milliseconds(m_progress_throttle_ms + 1);
    m_updater->setProgressCallback(
        [svc, last_pct = int{-1}, last_t = seed_past](int pct) mutable {
            const bool   final_tick = (pct >= 100);
            const auto   now        = clock::now();
            const auto   elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                          now - last_t).count();
            const bool   should_emit =
                (pct != last_pct) &&
                (final_tick ||
                 svc->m_progress_throttle_ms <= 0 ||
                 elapsed_ms >= svc->m_progress_throttle_ms);
            if (!should_emit) return;

            last_pct = pct;
            last_t   = now;

            PendingEvent ev{PendingEvent::Progress, pct, false, {}, 0, {}};
            {
                std::lock_guard<std::mutex> lk(svc->m_pending_mtx);
                svc->m_pending.push_back(std::move(ev));
            }
            const uint64_t one = 1U;
            [[maybe_unused]] auto _ = ::write(svc->m_wakeup_fd, &one, sizeof(one));
        });

    /* Acquire the sleep/shutdown inhibit on the loop thread before
     * spawning so sd_bus_call_method runs single-threaded against
     * m_bus (sd-bus is not thread-safe). The worker just holds and
     * closes the fd. -1 means disabled or unavailable. */
    const int inhibit_fd = acquire_sleep_inhibit();

    /* Capture by value: payload_path / utype are caller-owned strings;
     * the worker takes its own copies so the caller can mutate or
     * reset the state record without racing the worker. */
    m_install_thread = std::thread([svc, payload_path, update_type, inhibit_fd]() {
        /* Drop priority before touching the lib so we don't preempt
         * the loop thread for the whole install duration. Failures
         * are diagnostic — many container/test environments do not
         * permit unprivileged nice/ioprio changes; install continues
         * either way. */
        if (svc->m_worker_nice != 0) {
            if (::setpriority(PRIO_PROCESS, 0, svc->m_worker_nice) != 0) {
                sd_journal_print(LOG_ERR, "fs-updater-service: setpriority(nice=%d): %s\n",
                        svc->m_worker_nice, strerror(errno));
            }
        }
        /* ioprio_set is a thin wrapper around the syscall; libc does
         * not expose it on all targets. Class 2 = BE, data = priority
         * within class (0 high .. 7 low). */
        {
            const int ioprio =
                (IOPRIO_CLASS_BE << IOPRIO_CLASS_SHIFT) | svc->m_worker_ioprio;
            if (::syscall(SYS_ioprio_set, IOPRIO_WHO_PROCESS, 0, ioprio) != 0) {
                sd_journal_print(LOG_ERR, "fs-updater-service: ioprio_set(BE-%d): %s\n",
                        svc->m_worker_ioprio, strerror(errno));
            }
        }

        PendingEvent result;
        /* UpdaterError first: it is the arm that carries a cause a caller can
         * act on. The adapter translates the library's kinds into it, so this
         * loop stays free of library types and a test double can express a
         * failure without linking the library. */
        try {
            svc->m_updater->update_image(update_type, payload_path);
            result = PendingEvent{PendingEvent::Completed, 100, true, {}, 0, {}};
        } catch (const UpdaterError& e) {
            result = PendingEvent{PendingEvent::Completed, 0, false,
                                  std::string(e.what()), e.errorno, e.kind};
        } catch (const std::exception& e) {
            result = PendingEvent{PendingEvent::Completed, 0, false,
                                  std::string(e.what()), 0, "system"};
        } catch (...) {
            /* A non-std exception escaping the thread would call
             * std::terminate and take the whole service down without
             * ever emitting InstallCompleted. */
            result = PendingEvent{PendingEvent::Completed, 0, false,
                                  "install failed: unknown exception", 0, "unknown"};
        }
        {
            std::lock_guard<std::mutex> lk(svc->m_pending_mtx);
            svc->m_pending.push_back(std::move(result));
        }
        const uint64_t one = 1U;
        [[maybe_unused]] auto _ = ::write(svc->m_wakeup_fd, &one, sizeof(one));

        /* Release the sleep/shutdown inhibit. logind sees the fd close
         * and re-enables suspend immediately. */
        if (inhibit_fd >= 0) ::close(inhibit_fd);
    });
}

/* ── logind Inhibit helper ──────────────────────────────────────────────── */

int Service::acquire_sleep_inhibit()
{
    if (m_inhibit_sleep == 0) return -1;
    if (m_bus == nullptr)     return -1;

    sd_bus_error    err   = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;
    int r = sd_bus_call_method(m_bus,
        "org.freedesktop.login1",
        "/org/freedesktop/login1",
        "org.freedesktop.login1.Manager",
        "Inhibit",
        &err, &reply,
        "ssss",
        "shutdown:sleep:idle",
        "fs-updater-service",
        "Firmware install in progress",
        "block");
    if (r < 0) {
        /* Common in test environments (no logind on the user bus) and
         * on minimal rescue images. Install still proceeds. */
        sd_journal_print(LOG_ERR,
            "fs-updater-service: logind Inhibit failed: %s (install will proceed without)\n",
            err.message != nullptr ? err.message : strerror(-r));
        sd_bus_error_free(&err);
        return -1;
    }

    int reply_fd = -1;
    r = sd_bus_message_read(reply, "h", &reply_fd);
    if (r < 0 || reply_fd < 0) {
        sd_journal_print(LOG_ERR,
            "fs-updater-service: Inhibit reply read failed: %s\n",
            strerror(r < 0 ? -r : EBADF));
        sd_bus_message_unref(reply);
        return -1;
    }

    /* fd is owned by the reply — duplicate so the inhibit survives
     * the reply being freed. CLOEXEC keeps it from leaking into any
     * future fork(). */
    const int owned = ::fcntl(reply_fd, F_DUPFD_CLOEXEC, 3);
    sd_bus_message_unref(reply);
    if (owned < 0) {
        sd_journal_print(LOG_ERR,
            "fs-updater-service: dup inhibit fd: %s\n", strerror(errno));
        return -1;
    }
    return owned;
}

int Service::method_install_local(sd_bus_message* msg, void* userdata, sd_bus_error* ret_err)
{
    auto* svc = static_cast<Service*>(userdata);

    const char* path = nullptr;
    int r = sd_bus_message_read(msg, "s", &path);
    if (r < 0) {
        sd_bus_error_set(ret_err, SD_BUS_ERROR_FAILED, "failed to read arguments");
        return r;
    }

    if (path == nullptr || path[0] == '\0') {
        sd_bus_error_set(ret_err, SD_BUS_ERROR_INVALID_ARGS, "path must be non-empty");
        return -EINVAL;
    }

    /* Existence + readability via POSIX (POSIX, no std::filesystem). */
    if (::access(path, R_OK) != 0) {
        sd_bus_error_set(ret_err, SD_BUS_ERROR_FILE_NOT_FOUND,
                         "path does not exist or is unreadable");
        return -ENOENT;
    }

    if (svc->m_updater == nullptr)
        return reject_no_updater(ret_err);

    /* Reject if any phase is in flight. Mirrors method_start_download's
     * "session already active" gate plus the StartInstall "install
     * already in progress" gate, collapsed into one check since a
     * local install opens a fresh session. */
    if (svc->install_running() ||
        svc->m_download_state == "in_progress")
    {
        return reject_busy(ret_err, "another install or download is in progress");
    }

    /* Drain a previously-completed worker before reassigning state.
     * spawn_install_worker also joins, but doing it here keeps the
     * subsequent property writes consistent under tight test loops. */
    if (svc->m_install_thread.joinable())
        svc->m_install_thread.join();

    /* Introspect the bundle to populate UpdateType/Version/Size so
     * D-Bus subscribers see meaningful values during the install
     * rather than empty strings. Implementations without
     * introspection return available=false or empty type — we still proceed and the
     * worker passes whatever type we got into update_image(), where
     * the lib determines the real type from the container header. */
    CheckUpdateInfo info;
    try {
        info = svc->m_updater->inspect_bundle(path);
    } catch (const UpdaterError& e) {
        return reject_updater(ret_err, e);
    } catch (const std::exception& e) {
        sd_bus_error_set(ret_err, SD_BUS_ERROR_FAILED, e.what());
        return -EIO;
    }

    /* Mint a fresh session id only after introspection succeeds (mirror
     * method_start_download). Doing it here rather than before the try means a
     * throwing inspect_bundle cannot advance the counter and strand a session
     * id with no matching metadata or PropertiesChanged. Wrap to 1 if the
     * counter ever hits 0 — the interface reserves 0. */
    if (++svc->m_session_counter == 0)
        ++svc->m_session_counter;
    svc->m_session_id = svc->m_session_counter;

    svc->m_update_type       = info.type;
    svc->m_update_version    = info.version;
    svc->m_update_size       = info.size;
    svc->m_payload_path      = path;
    svc->m_download_state    = "finished";
    svc->m_download_progress = 100;

    sd_bus_emit_properties_changed(svc->m_bus, OBJ_PATH, INTERFACE,
        "SessionId", "UpdateAvailable", "UpdateType", "UpdateVersion",
        "UpdateSize", "DownloadState", "DownloadProgress",
        "RebootPending", nullptr);

    svc->spawn_install_worker(svc->m_update_type, svc->m_payload_path);

    return sd_bus_reply_method_return(msg, "u", svc->m_session_id);
}

int Service::method_cancel_install(sd_bus_message* msg, void* userdata, sd_bus_error* ret_err)
{
    auto* svc = static_cast<Service*>(userdata);

    uint32_t sid = 0;
    int r = sd_bus_message_read(msg, "u", &sid);
    if (r < 0) {
        sd_bus_error_set(ret_err, SD_BUS_ERROR_FAILED, "failed to read arguments");
        return r;
    }

    if (sid == 0) {
        sd_bus_error_set(ret_err, SD_BUS_ERROR_INVALID_ARGS,
                         "session_id 0 is reserved");
        return -EINVAL;
    }

    if (sid != svc->m_session_id) {
        sd_bus_error_set(ret_err, SD_BUS_ERROR_INVALID_ARGS,
                         "session_id does not match active session");
        return -EINVAL;
    }

    if (!svc->install_running()) {
        sd_bus_error_set(ret_err, SD_BUS_ERROR_FILE_NOT_FOUND,
                         "no install in progress");
        return -ENOENT;
    }

    /* Best-effort cancel: the lib has no poll points inside update_image
     * today, so the flag is checked by on_wakeup when the worker's
     * Completion event arrives. Whether the install actually stopped
     * early or completed before the cancel landed is observable only
     * via the InstallCompleted message — we always report cancellation
     * once the flag is set, per the XML contract. */
    svc->m_cancel_requested.store(true, std::memory_order_release);

    /* Nudge the loop so any pending progress event drains promptly;
     * harmless if there is none. */
    const uint64_t one = 1U;
    [[maybe_unused]] auto _ = ::write(svc->m_wakeup_fd, &one, sizeof(one));

    return sd_bus_reply_method_return(msg, nullptr);
}

int Service::method_apply(sd_bus_message* msg, void* userdata, sd_bus_error* ret_err)
{
    auto* svc = static_cast<Service*>(userdata);

    if (svc->m_updater == nullptr)
        return reject_no_updater(ret_err);

    if (svc->install_running())
        return reject_busy(ret_err, "install in progress");

    /* Apply's own authorization, never the published view. Two shapes
     * qualify:
     *  - this session finished its install ("finished" is terminal and
     *    only an install sets it), or
     *  - no session exists and the durable state holds a pending update.
     * Any other session is refused: deriving a type there would bridge a
     * failed session onto an unrelated durable update.
     * The type argument is informational; a restart passes it empty. */
    std::string apply_type = svc->m_update_type;
    if (svc->m_install_state != "finished") {
        if (svc->m_session_id != 0 || svc->m_install_state != "idle") {
            sd_bus_error_set(ret_err, SD_BUS_ERROR_FAILED, "install not finished");
            return -EINVAL;
        }
        CheckUpdateInfo info{false, "", "", 0};
        try {
            info = svc->m_updater->check_update_available();
        } catch (const UpdaterError& e) {
            return reject_updater(ret_err, e);
        } catch (const std::exception& e) {
            sd_bus_error_set(ret_err, SD_BUS_ERROR_FAILED, e.what());
            return -EIO;
        }
        if (!info.available) {
            sd_bus_error_set(ret_err, SD_BUS_ERROR_FAILED, "install not finished");
            return -EINVAL;
        }
        apply_type = info.type;
    }

    /* Apply performs no durable transition of its own: the install already
     * put the target at the head of the boot order, so all this asks the lib
     * is whether the pending state needs a reboot to take effect. Throwing
     * leaves the state machine intact so the caller can retry safely. */
    bool reboot_req = false;
    try {
        reboot_req = svc->m_updater->apply_pending_update(apply_type);
    } catch (const UpdaterError& e) {
        return reject_updater(ret_err, e);
    } catch (const std::exception& e) {
        sd_bus_error_set(ret_err, SD_BUS_ERROR_FAILED, e.what());
        return -EIO;
    }

    /* No RebootPending write: the property derives from the durable state,
     * which the install already moved — the caller additionally gets the
     * lib's answer in the reply. Only the session view is torn down. */
    svc->reset_session_view();

    sd_bus_emit_properties_changed(svc->m_bus, OBJ_PATH, INTERFACE,
        "RebootPending", "SessionId", "DownloadState", "DownloadProgress",
        "InstallState", "InstallProgress",
        "UpdateAvailable", "UpdateType", "UpdateVersion", "UpdateSize", nullptr);

    return sd_bus_reply_method_return(msg, "b", static_cast<int>(reboot_req));
}

int Service::method_rollback(sd_bus_message* msg, void* userdata, sd_bus_error* ret_err)
{
    auto* svc = static_cast<Service*>(userdata);

    if (svc->m_updater == nullptr)
        return reject_no_updater(ret_err);

    if (svc->install_running())
        return reject_busy(ret_err, "install in progress");

    /* The type handed to the lib is the DURABLE rollback dimension, never
     * the session mirror: the mirror holds whatever a session announced —
     * possibly a download that failed and installed nothing — and the
     * library's firmware rollback on a device with nothing pending is not
     * a no-op, it switches the boot order to the other committed slot.
     * When the durable state names no dimension, refuse: the bus call
     * carries no type argument, so there is nothing the door could act
     * on, and the library called with an empty type does nothing and
     * throws nothing. */
    std::string rollback_type;
    try {
        rollback_type = svc->m_updater->rollback_candidate_type();
    } catch (const UpdaterError& e) {
        return reject_updater(ret_err, e);
    } catch (const std::exception& e) {
        sd_bus_error_set(ret_err, SD_BUS_ERROR_FAILED, e.what());
        return -EIO;
    }

    if (rollback_type.empty()) {
        sd_bus_error_set(ret_err, error_name_for("nothing_to_apply"),
                         "nothing to roll back");
        return -EIO;
    }

    try {
        svc->m_updater->rollback(rollback_type);
    } catch (const UpdaterError& e) {
        return reject_updater(ret_err, e);
    } catch (const std::exception& e) {
        sd_bus_error_set(ret_err, SD_BUS_ERROR_FAILED, e.what());
        return -EIO;
    }

    svc->reset_session_view();

    sd_bus_emit_properties_changed(svc->m_bus, OBJ_PATH, INTERFACE,
        "RebootPending", "SessionId", "DownloadState", "DownloadProgress",
        "InstallState", "InstallProgress",
        "UpdateAvailable", "UpdateType", "UpdateVersion", "UpdateSize", nullptr);

    return sd_bus_reply_method_return(msg, nullptr);
}

/* Commit is the D-Bus entry point for runtime clients to render the health
 * verdict through the daemon. It deliberately does NOT replace the CLI's local
 * `fs-updater --commit_update`, which keeps calling fs::FSUpdate directly: the
 * boot-time confirm runs before this daemon is necessarily up, so routing it
 * through D-Bus would add a fragile boot ordering dependency. Both paths
 * converge on the same U-Boot state.
 *
 * The boot-time path settles only what has nothing left to decide -- a taken
 * rollback, a failed install, a bootloader fallback. A healthy pending update
 * is left for this verdict, and the mark-good service renders none: it
 * restores a boot counter. */
int Service::method_commit(sd_bus_message* msg, void* userdata, sd_bus_error* ret_err)
{
    auto* svc = static_cast<Service*>(userdata);

    if (svc->m_updater == nullptr)
        return reject_no_updater(ret_err);

    if (svc->install_running())
        return reject_busy(ret_err, "install in progress");

    bool committed = false;
    try {
        committed = svc->m_updater->commit_update();
    } catch (const UpdaterError& e) {
        return reject_updater(ret_err, e);
    } catch (const std::exception& e) {
        sd_bus_error_set(ret_err, SD_BUS_ERROR_FAILED, e.what());
        return -EIO;
    }

    /* Teardown only records a confirmation that happened: with
     * committed == false the library confirmed nothing, and clearing the
     * session view would assert a settlement the device contradicts —
     * the same class of false reply the rollback guard closes. */
    if (committed) {
        svc->reset_session_view();

        sd_bus_emit_properties_changed(svc->m_bus, OBJ_PATH, INTERFACE,
            "RebootPending", "SessionId", "DownloadState", "DownloadProgress",
            "InstallState", "InstallProgress",
            "UpdateAvailable", "UpdateType", "UpdateVersion", "UpdateSize", nullptr);
    }

    return sd_bus_reply_method_return(msg, "b", committed ? 1 : 0);
}

int Service::method_get_state(sd_bus_message* msg, void* userdata, sd_bus_error* /*ret_err*/)
{
    auto* svc = static_cast<Service*>(userdata);

    sd_bus_message* reply = nullptr;
    sd_bus_message_new_method_return(msg, &reply);

    /* The same derivation the property getters use — two doors, one
     * answer. Computed once for the whole snapshot. */
    const PublishedUpdateView view = svc->published_update_view();

    sd_bus_message_open_container(reply, 'a', "{sv}");

    sd_bus_message_open_container(reply, 'e', "sv");
    sd_bus_message_append_basic(reply, 's', "SessionId");
    sd_bus_message_open_container(reply, 'v', "u");
    sd_bus_message_append_basic(reply, 'u', &svc->m_session_id);
    sd_bus_message_close_container(reply);
    sd_bus_message_close_container(reply);

    {
        int v = static_cast<int>(view.update_available);
        sd_bus_message_open_container(reply, 'e', "sv");
        sd_bus_message_append_basic(reply, 's', "UpdateAvailable");
        sd_bus_message_open_container(reply, 'v', "b");
        sd_bus_message_append_basic(reply, 'b', &v);
        sd_bus_message_close_container(reply);
        sd_bus_message_close_container(reply);
    }

    sd_bus_message_open_container(reply, 'e', "sv");
    sd_bus_message_append_basic(reply, 's', "UpdateType");
    sd_bus_message_open_container(reply, 'v', "s");
    sd_bus_message_append_basic(reply, 's', view.update_type.c_str());
    sd_bus_message_close_container(reply);
    sd_bus_message_close_container(reply);

    sd_bus_message_open_container(reply, 'e', "sv");
    sd_bus_message_append_basic(reply, 's', "UpdateVersion");
    sd_bus_message_open_container(reply, 'v', "s");
    sd_bus_message_append_basic(reply, 's', view.update_version.c_str());
    sd_bus_message_close_container(reply);
    sd_bus_message_close_container(reply);

    sd_bus_message_open_container(reply, 'e', "sv");
    sd_bus_message_append_basic(reply, 's', "UpdateSize");
    sd_bus_message_open_container(reply, 'v', "t");
    sd_bus_message_append_basic(reply, 't', &view.update_size);
    sd_bus_message_close_container(reply);
    sd_bus_message_close_container(reply);

    sd_bus_message_open_container(reply, 'e', "sv");
    sd_bus_message_append_basic(reply, 's', "DownloadState");
    sd_bus_message_open_container(reply, 'v', "s");
    sd_bus_message_append_basic(reply, 's', svc->m_download_state.c_str());
    sd_bus_message_close_container(reply);
    sd_bus_message_close_container(reply);

    sd_bus_message_open_container(reply, 'e', "sv");
    sd_bus_message_append_basic(reply, 's', "DownloadProgress");
    sd_bus_message_open_container(reply, 'v', "i");
    sd_bus_message_append_basic(reply, 'i', &svc->m_download_progress);
    sd_bus_message_close_container(reply);
    sd_bus_message_close_container(reply);

    sd_bus_message_open_container(reply, 'e', "sv");
    sd_bus_message_append_basic(reply, 's', "InstallState");
    sd_bus_message_open_container(reply, 'v', "s");
    sd_bus_message_append_basic(reply, 's', svc->m_install_state.c_str());
    sd_bus_message_close_container(reply);
    sd_bus_message_close_container(reply);

    /* InstallProgress — i.
     * sd_bus_message_append_basic takes a pointer to the basic type; the
     * atomic is not layout-compatible with the underlying int32_t, so load
     * to a stack copy first. */
    {
        const int32_t install_progress =
            svc->m_install_progress.load(std::memory_order_acquire);
        sd_bus_message_open_container(reply, 'e', "sv");
        sd_bus_message_append_basic(reply, 's', "InstallProgress");
        sd_bus_message_open_container(reply, 'v', "i");
        sd_bus_message_append_basic(reply, 'i', &install_progress);
        sd_bus_message_close_container(reply);
        sd_bus_message_close_container(reply);
    }

    {
        int v = static_cast<int>(view.reboot_pending);
        sd_bus_message_open_container(reply, 'e', "sv");
        sd_bus_message_append_basic(reply, 's', "RebootPending");
        sd_bus_message_open_container(reply, 'v', "b");
        sd_bus_message_append_basic(reply, 'b', &v);
        sd_bus_message_close_container(reply);
        sd_bus_message_close_container(reply);
    }

    sd_bus_message_close_container(reply);

    /* sd_bus_send does not take ownership of the reply — unref it so the
     * per-call reply message is not leaked. */
    const int sent = sd_bus_send(nullptr, reply, nullptr);
    sd_bus_message_unref(reply);
    return sent;
}

/* ── Property getters — read live from Service state fields ─────────────── */

int Service::prop_session_id(sd_bus* /*bus*/, const char* /*path*/,
                              const char* /*iface*/, const char* /*prop*/,
                              sd_bus_message* reply, void* userdata, sd_bus_error* /*err*/)
{
    auto* svc = static_cast<Service*>(userdata);
    return sd_bus_message_append(reply, "u", svc->m_session_id);
}

int Service::prop_update_available(sd_bus* /*bus*/, const char* /*path*/,
                                    const char* /*iface*/, const char* /*prop*/,
                                    sd_bus_message* reply, void* userdata, sd_bus_error* /*err*/)
{
    auto* svc = static_cast<Service*>(userdata);
    return sd_bus_message_append(reply, "b",
        (int)svc->published_update_view().update_available);
}

/* What a caller may rely on, asked at runtime rather than guessed from the
 * daemon's package version -- the two move independently, and a caller can only
 * see the bus. Read the section "Interface version and stability" in the
 * contract before changing this number: it says which changes leave it alone
 * and which one requires a new interface name. */
int Service::prop_interface_version(sd_bus* /*bus*/, const char* /*path*/,
                                     const char* /*iface*/, const char* /*prop*/,
                                     sd_bus_message* reply, void* /*userdata*/, sd_bus_error* /*err*/)
{
    return sd_bus_message_append(reply, "u", (uint32_t)FSUPDATE_INTERFACE_VERSION);
}

int Service::prop_update_type(sd_bus* /*bus*/, const char* /*path*/,
                               const char* /*iface*/, const char* /*prop*/,
                               sd_bus_message* reply, void* userdata, sd_bus_error* /*err*/)
{
    auto* svc = static_cast<Service*>(userdata);
    return sd_bus_message_append(reply, "s",
        svc->published_update_view().update_type.c_str());
}

int Service::prop_update_version(sd_bus* /*bus*/, const char* /*path*/,
                                  const char* /*iface*/, const char* /*prop*/,
                                  sd_bus_message* reply, void* userdata, sd_bus_error* /*err*/)
{
    auto* svc = static_cast<Service*>(userdata);
    return sd_bus_message_append(reply, "s",
        svc->published_update_view().update_version.c_str());
}

int Service::prop_update_size(sd_bus* /*bus*/, const char* /*path*/,
                               const char* /*iface*/, const char* /*prop*/,
                               sd_bus_message* reply, void* userdata, sd_bus_error* /*err*/)
{
    auto* svc = static_cast<Service*>(userdata);
    return sd_bus_message_append(reply, "t",
        svc->published_update_view().update_size);
}

int Service::prop_download_state(sd_bus* /*bus*/, const char* /*path*/,
                                  const char* /*iface*/, const char* /*prop*/,
                                  sd_bus_message* reply, void* userdata, sd_bus_error* /*err*/)
{
    auto* svc = static_cast<Service*>(userdata);
    return sd_bus_message_append(reply, "s", svc->m_download_state.c_str());
}

int Service::prop_download_progress(sd_bus* /*bus*/, const char* /*path*/,
                                     const char* /*iface*/, const char* /*prop*/,
                                     sd_bus_message* reply, void* userdata, sd_bus_error* /*err*/)
{
    auto* svc = static_cast<Service*>(userdata);
    return sd_bus_message_append(reply, "i", svc->m_download_progress);
}

int Service::prop_install_state(sd_bus* /*bus*/, const char* /*path*/,
                                 const char* /*iface*/, const char* /*prop*/,
                                 sd_bus_message* reply, void* userdata, sd_bus_error* /*err*/)
{
    auto* svc = static_cast<Service*>(userdata);
    return sd_bus_message_append(reply, "s", svc->m_install_state.c_str());
}

int Service::prop_install_progress(sd_bus* /*bus*/, const char* /*path*/,
                                    const char* /*iface*/, const char* /*prop*/,
                                    sd_bus_message* reply, void* userdata, sd_bus_error* /*err*/)
{
    auto* svc = static_cast<Service*>(userdata);
    /* Explicit acquire-load: pairs with the release-store in on_wakeup
     * when a worker-pushed progress event is applied. Hot path — kept
     * outside m_state_mtx so a stream of property reads doesn't
     * contend with the state-record lock. */
    const int32_t progress =
        svc->m_install_progress.load(std::memory_order_acquire);
    return sd_bus_message_append(reply, "i", progress);
}

int Service::prop_reboot_pending(sd_bus* /*bus*/, const char* /*path*/,
                                  const char* /*iface*/, const char* /*prop*/,
                                  sd_bus_message* reply, void* userdata, sd_bus_error* /*err*/)
{
    auto* svc = static_cast<Service*>(userdata);
    return sd_bus_message_append(reply, "b",
        (int)svc->published_update_view().reboot_pending);
}

/* ── vtable ─────────────────────────────────────────────────────────────── */

const sd_bus_vtable Service::service_vtable[] = {
    SD_BUS_VTABLE_START(0),

    SD_BUS_METHOD("CheckUpdateAvailable",    "",    "bsst", method_check_update_available,    SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("StartDownload",           "sst", "u",    method_start_download,            0),
    SD_BUS_METHOD("ReportDownloadProgress",  "ut",  "",     method_report_download_progress,  0),
    SD_BUS_METHOD("FinishDownload",          "ubs", "",     method_finish_download,           0),
    SD_BUS_METHOD("StartInstall",            "us",  "",     method_start_install,             0),
    SD_BUS_METHOD("InstallLocal",            "s",   "u",    method_install_local,             0),
    SD_BUS_METHOD("CancelInstall",           "u",   "",     method_cancel_install,            0),
    SD_BUS_METHOD("Apply",                   "",    "b",    method_apply,                     0),
    SD_BUS_METHOD("Rollback",               "",    "",     method_rollback,                  0),
    SD_BUS_METHOD("Commit",                  "",    "b",    method_commit,                    0),
    SD_BUS_METHOD("GetState",               "",    "a{sv}",method_get_state,                 SD_BUS_VTABLE_UNPRIVILEGED),

    SD_BUS_PROPERTY("SessionId",        "u", prop_session_id,        0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    /* CONST, not EMITS_CHANGE: it cannot move while the daemon is running, so
     * a caller may read it once and cache it. */
    SD_BUS_PROPERTY("InterfaceVersion", "u", prop_interface_version, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    /* The five derived properties announce changes as INVALIDATION, not
     * as EMITS_CHANGE: a value the daemon does not own must not license a
     * proxy's cache. The daemon still signals at its own transitions, but
     * the local door can move the durable state without any signal — a
     * caching proxy holding a "current" value would serve a stale view.
     * Invalidation tells a
     * spec-conforming proxy to re-read; out-of-band changes still require
     * polling, which the contract states at the properties. */
    SD_BUS_PROPERTY("UpdateAvailable",  "b", prop_update_available,  0, SD_BUS_VTABLE_PROPERTY_EMITS_INVALIDATION),
    SD_BUS_PROPERTY("UpdateType",       "s", prop_update_type,       0, SD_BUS_VTABLE_PROPERTY_EMITS_INVALIDATION),
    SD_BUS_PROPERTY("UpdateVersion",    "s", prop_update_version,    0, SD_BUS_VTABLE_PROPERTY_EMITS_INVALIDATION),
    SD_BUS_PROPERTY("UpdateSize",       "t", prop_update_size,       0, SD_BUS_VTABLE_PROPERTY_EMITS_INVALIDATION),
    SD_BUS_PROPERTY("DownloadState",    "s", prop_download_state,    0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("DownloadProgress", "i", prop_download_progress, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("InstallState",     "s", prop_install_state,     0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("InstallProgress",  "i", prop_install_progress,  0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("RebootPending",    "b", prop_reboot_pending,    0, SD_BUS_VTABLE_PROPERTY_EMITS_INVALIDATION),

    SD_BUS_SIGNAL("InstallCompleted", "ubs", 0),
    /* The machine-readable half of the same event. Emitted alongside, never
     * instead of, InstallCompleted. */
    SD_BUS_SIGNAL("InstallOutcome",   "ubis", 0),
    SD_BUS_SIGNAL("JobFailed",        "us",  0),

    SD_BUS_VTABLE_END
};
