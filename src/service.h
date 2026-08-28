#pragma once

#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/* ── IUpdater — pure-abstract interface for testability ────────────────── */

struct CheckUpdateInfo {
    bool        available;
    std::string type;
    std::string version;
    uint64_t    size;
};

class IUpdater {
public:
    virtual ~IUpdater() = default;

    virtual CheckUpdateInfo check_update_available() = 0;
    /* Inspect a local update bundle on disk and return its declared
     * type / version / size, without installing anything. Used by
     * InstallLocal to populate the UpdateType/Version/Size properties
     * before the worker starts, so D-Bus subscribers see meaningful
     * values during the install rather than empty strings. Implementations
     * may return CheckUpdateInfo{false, "", "", 0} when introspection
     * is unsupported; the install still proceeds. */
    virtual CheckUpdateInfo inspect_bundle(const std::string& path) = 0;
    virtual void            update_image(std::string type, std::string path) = 0;
    /* Perform the pre-reboot Apply transition: for fw / fw+app
     * updates, ask RAUC to mark the inactive slot good (swaps
     * BOOT_ORDER so the next reboot lands on the new firmware); for
     * app-only, no slot swap. Returns true when a reboot is required
     * to make the update take effect — currently all three
     * INCOMPLETE_* states (preinit must re-run for the new app
     * squashfs). Throws when no install is pending apply. */
    virtual bool            apply_pending_update(const std::string& type) = 0;
    virtual void            rollback(std::string type) = 0;
    /* Post-reboot confirmation: transition the pending update / rollback to
     * IDLE (lib commit_update). Returns true when a commit was performed. */
    virtual bool            commit_update() = 0;
    virtual void            setProgressCallback(std::function<void(int)> cb) = 0;
};

/* ── Service ────────────────────────────────────────────────────────────── */

class Service {
public:
    Service(sd_bus* bus, sd_event* event, std::unique_ptr<IUpdater> updater);
    ~Service();

    Service(const Service&)            = delete;
    Service& operator=(const Service&) = delete;
    Service(Service&&)                 = delete;
    Service& operator=(Service&&)      = delete;

    struct PendingEvent {
        enum Type { Progress, Completed, Failed } type;
        int         progress;
        bool        success;
        std::string message;
    };

    /* ── State fields (read by property getters) ─────────────────────── */
    uint32_t    m_session_id       = 0;
    uint32_t    m_session_counter  = 0;
    bool        m_update_available = false;
    std::string m_update_type;
    std::string m_update_version;
    uint64_t    m_update_size      = 0;
    std::string m_download_state   = "idle";
    int32_t     m_download_progress = 0;
    std::string m_install_state    = "idle";
    /* Hot read path: property getter loads this on every D-Bus call.
     * Worker thread writes through on_wakeup (loop thread). std::atomic
     * with explicit memory_order_acquire / memory_order_release gives a
     * torn-read-free property reader without taking m_state_mtx. */
    alignas(64) std::atomic<int32_t> m_install_progress{0};
    bool        m_reboot_pending   = false;

    std::string m_payload_path;

    /* ── Thread communication ─────────────────────────────────────────── */
    std::mutex                m_pending_mtx;
    std::vector<PendingEvent> m_pending;
    std::thread               m_install_thread;
    /* Set by CancelInstall handler (loop thread); read by worker thread
     * at phase boundaries. Cooperative cancel — no preemption. */
    std::atomic<bool>         m_cancel_requested{false};

    /* ── Worker-tuning knobs (env-driven, read once at construction) ──
     * FSUPDATE_PROGRESS_MS   min interval between PropertiesChanged
     *                        emissions on InstallProgress; the value
     *                        100 always fires regardless. 0 disables
     *                        throttling. Cuts dbus-daemon load on
     *                        single-core targets where RAUC ticks
     *                        faster than humans/UIs care to see.
     * FSUPDATE_WORKER_NICE   nice() applied on worker thread entry;
     *                        keeps the loop thread responsive for
     *                        property reads while the worker churns.
     * FSUPDATE_WORKER_IOPRIO best-effort I/O priority (0..7, higher
     *                        is lower priority); lets the rest of the
     *                        system co-exist with eMMC/NAND writes. */
    int                       m_progress_throttle_ms = 250;
    int                       m_worker_nice          = 5;
    int                       m_worker_ioprio        = 7;
    /* FSUPDATE_INHIBIT_SLEEP: non-zero (default) takes a
     * logind Inhibit fd of "shutdown:sleep:idle" for the duration of
     * an install. Prevents a low-power target from suspending or
     * shutting down mid-write, which would leave the slot half-flashed
     * and force a rollback. Failures (no logind, no permission) are
     * non-fatal — install proceeds without the inhibit, with a warning
     * to stderr/journal. */
    int                       m_inhibit_sleep        = 1;

private:
    sd_bus*          m_bus        = nullptr;
    sd_event*        m_event      = nullptr;
    sd_event_source* m_wakeup_src = nullptr;
    int              m_wakeup_fd  = -1;

    std::unique_ptr<IUpdater> m_updater;

    friend class ServiceTest;
    /* gtest FRIEND_TEST expansion — grants the standalone ctor-recovery
     * Apply regression access to method_apply without a gtest include. */
    friend class ServiceCtorRecoveryTest_ApplyWorksOnRecoveredPendingUpdate_Test;

    static int on_wakeup(sd_event_source* src, int fd, uint32_t revents, void* userdata);

    /* Spawn the install worker thread for the current session.
     * Pre: caller is on the loop thread, payload_path is non-empty,
     * install_state was just transitioned to "in_progress" and the
     * corresponding PropertiesChanged emitted.
     * Joins any previously-terminated worker first (cheap when the
     * prior install already completed; pathological case otherwise). */
    void spawn_install_worker(const std::string& update_type,
                              const std::string& payload_path);

    /* Acquire a logind sleep/shutdown inhibit fd for the duration of
     * an install. Returns the duplicated fd on success, or -1 if the
     * knob is disabled, logind is unavailable, or the call failed
     * for any other reason. Caller owns the fd; closing it releases
     * the inhibit. Must run on the loop thread (uses m_bus). */
    int acquire_sleep_inhibit();

    /* ── Method handlers ─────────────────────────────────────────────── */
    static int method_check_update_available(sd_bus_message* msg, void* userdata, sd_bus_error* ret_err);
    static int method_start_download        (sd_bus_message* msg, void* userdata, sd_bus_error* ret_err);
    static int method_report_download_progress(sd_bus_message* msg, void* userdata, sd_bus_error* ret_err);
    static int method_finish_download       (sd_bus_message* msg, void* userdata, sd_bus_error* ret_err);
    static int method_start_install         (sd_bus_message* msg, void* userdata, sd_bus_error* ret_err);
    static int method_install_local         (sd_bus_message* msg, void* userdata, sd_bus_error* ret_err);
    static int method_cancel_install        (sd_bus_message* msg, void* userdata, sd_bus_error* ret_err);
    static int method_apply                 (sd_bus_message* msg, void* userdata, sd_bus_error* ret_err);
    static int method_rollback              (sd_bus_message* msg, void* userdata, sd_bus_error* ret_err);
    static int method_commit                (sd_bus_message* msg, void* userdata, sd_bus_error* ret_err);
    static int method_get_state             (sd_bus_message* msg, void* userdata, sd_bus_error* ret_err);

    /* ── Property getters ────────────────────────────────────────────── */
    static int prop_interface_version (sd_bus* bus, const char* path, const char* iface, const char* prop, sd_bus_message* reply, void* userdata, sd_bus_error* err);
    static int prop_session_id        (sd_bus* bus, const char* path, const char* iface, const char* prop, sd_bus_message* reply, void* userdata, sd_bus_error* err);
    static int prop_update_available  (sd_bus* bus, const char* path, const char* iface, const char* prop, sd_bus_message* reply, void* userdata, sd_bus_error* err);
    static int prop_update_type       (sd_bus* bus, const char* path, const char* iface, const char* prop, sd_bus_message* reply, void* userdata, sd_bus_error* err);
    static int prop_update_version    (sd_bus* bus, const char* path, const char* iface, const char* prop, sd_bus_message* reply, void* userdata, sd_bus_error* err);
    static int prop_update_size       (sd_bus* bus, const char* path, const char* iface, const char* prop, sd_bus_message* reply, void* userdata, sd_bus_error* err);
    static int prop_download_state    (sd_bus* bus, const char* path, const char* iface, const char* prop, sd_bus_message* reply, void* userdata, sd_bus_error* err);
    static int prop_download_progress (sd_bus* bus, const char* path, const char* iface, const char* prop, sd_bus_message* reply, void* userdata, sd_bus_error* err);
    static int prop_install_state     (sd_bus* bus, const char* path, const char* iface, const char* prop, sd_bus_message* reply, void* userdata, sd_bus_error* err);
    static int prop_install_progress  (sd_bus* bus, const char* path, const char* iface, const char* prop, sd_bus_message* reply, void* userdata, sd_bus_error* err);
    static int prop_reboot_pending    (sd_bus* bus, const char* path, const char* iface, const char* prop, sd_bus_message* reply, void* userdata, sd_bus_error* err);

public:
    static const sd_bus_vtable service_vtable[];
};
