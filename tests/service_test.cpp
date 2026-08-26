#include <gtest/gtest.h>
#include "service.h"

#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>

#include <unistd.h>     /* mkstemp, close, unlink */

#include <atomic>
#include <condition_variable>
#include <cstdlib>      /* mkstemp */
#include <cstring>      /* strncpy */
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

/* ── BlockingGate ─────────────────────────────────────────────────────────── */

class BlockingGate {
    std::mutex              m_;
    std::condition_variable cv_;
    bool                    open_{true};
public:
    void arm() {
        std::lock_guard<std::mutex> lk(m_);
        open_ = false;
    }
    void release() {
        std::lock_guard<std::mutex> lk(m_);
        open_ = true;
        cv_.notify_all();
    }
    void wait() {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [this] { return open_; });
    }
};

/* ── MockUpdater ─────────────────────────────────────────────────────────── */

class MockUpdater : public IUpdater {
public:
    CheckUpdateInfo check_update_available() override {
        ++check_calls_;
        return check_result_;
    }

    CheckUpdateInfo inspect_bundle(const std::string& path) override {
        ++inspect_calls_;
        last_inspect_path_ = path;
        return inspect_result_;
    }

    void update_image(std::string type, std::string path) override {
        last_update_type_ = type;
        last_update_path_ = path;
        ++update_calls_;
        install_gate_.wait();
        if (update_should_throw_)
            throw std::runtime_error("mock install error");
        if (update_should_throw_nonstd_)
            throw 42;
    }

    bool apply_pending_update(const std::string& type) override {
        last_apply_type_ = type;
        ++apply_calls_;
        if (apply_should_throw_)
            throw std::runtime_error("mock apply error");
        return apply_returns_reboot_;
    }

    void rollback(std::string type) override {
        last_rollback_type_ = type;
        ++rollback_calls_;
    }

    bool commit_update() override {
        ++commit_calls_;
        return commit_returns_;
    }

    void setProgressCallback(std::function<void(int)> cb) override {
        progress_cb_ = std::move(cb);
    }

    void arm_install()     { install_gate_.arm(); }
    void release_install() { install_gate_.release(); }

    CheckUpdateInfo          check_result_{false, "", "", 0};
    CheckUpdateInfo          inspect_result_{false, "", "", 0};
    bool                     update_should_throw_  = false;
    bool                     update_should_throw_nonstd_ = false;
    bool                     apply_should_throw_   = false;
    bool                     apply_returns_reboot_ = true;
    bool                     commit_returns_       = true;
    std::atomic<int>         check_calls_{0};
    std::atomic<int>         inspect_calls_{0};
    std::atomic<int>         update_calls_{0};
    std::atomic<int>         apply_calls_{0};
    std::atomic<int>         rollback_calls_{0};
    std::atomic<int>         commit_calls_{0};
    std::string              last_update_type_;
    std::string              last_update_path_;
    std::string              last_inspect_path_;
    std::string              last_apply_type_;
    std::string              last_rollback_type_;
    std::function<void(int)> progress_cb_;

private:
    BlockingGate install_gate_;
};

/* Busless environments skip the bus-backed tests; under FSUP_REQUIRE_BUS
 * (set by CI) the skip turns into a failure so a hollow "green" run cannot
 * pass unnoticed. */
static bool require_bus()
{
    return ::getenv("FSUP_REQUIRE_BUS") != nullptr;
}

/* ── ServiceTest fixture ─────────────────────────────────────────────────── */

class ServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        int r = sd_bus_open_user(&bus_);
        if (r < 0) {
            r = sd_bus_open_system(&bus_);
            if (r < 0) {
                if (require_bus())
                    FAIL() << "no D-Bus available but FSUP_REQUIRE_BUS is set (rc " << r << ")";
                GTEST_SKIP() << "no D-Bus available (user and system both failed)";
                return;
            }
        }

        r = sd_event_default(&event_);
        ASSERT_GE(r, 0) << "sd_event_default failed: " << r;

        r = sd_bus_attach_event(bus_, event_, SD_EVENT_PRIORITY_NORMAL);
        ASSERT_GE(r, 0) << "sd_bus_attach_event failed: " << r;

        auto mock = std::make_unique<MockUpdater>();
        mock_ = mock.get();
        svc_  = std::make_unique<Service>(bus_, event_, std::move(mock));

        /* Service ctor invokes check_update_available() for stale-state
         * recovery; reset the call counter so each test asserts from a
         * clean baseline. (check_result_ default {false,...} means the
         * recovery sets no state fields, so test bodies that prime
         * state are unaffected.) */
        mock_->check_calls_ = 0;

        /* Own the registration slot: the vtable's userdata must never
         * outlive svc_ (rebuild helpers re-register with the new one). */
        r = sd_bus_add_object_vtable(bus_, &slot_,
            "/de/fsembedded/fsupdate1",
            "de.fsembedded.fsupdate1",
            Service::service_vtable, svc_.get());
        ASSERT_GE(r, 0) << "sd_bus_add_object_vtable failed: " << r;

        sd_event_run(event_, 0);
    }

    void TearDown() override {
        if (mock_) mock_->release_install();    /* unblock any in-flight thread first */
        if (slot_) { sd_bus_slot_unref(slot_); slot_ = nullptr; }  /* before the userdata dies */
        svc_.reset();                            /* ~Service joins m_install_thread */
        if (event_) { sd_event_unref(event_); event_ = nullptr; }
        if (bus_)   { sd_bus_unref(bus_);     bus_   = nullptr; }
        mock_ = nullptr;
    }

    /* ── message construction ─────────────────────────────────────────── */

    sd_bus_message* make_msg(const char* member) {
        sd_bus_message* msg = nullptr;
        sd_bus_message_new_method_call(bus_, &msg,
            "de.fsembedded.fsupdate1",
            "/de/fsembedded/fsupdate1",
            "de.fsembedded.fsupdate1",
            member);
        return msg;
    }

    static void seal(sd_bus_message* msg) {
        sd_bus_message_seal(msg, 1, 0);
        sd_bus_message_rewind(msg, 1);
    }

    /* ── handler wrappers ─────────────────────────────────────────────── */

    /* Seal a message, dispatch one method handler directly (bypassing the
     * bus), capture the D-Bus error name, free the error, unref the message,
     * and return the handler rc. name_out receives the error name so a test
     * can pin the exact contract (e.g. Error.Busy / Error.NoUpdater), not just
     * the errno. One helper backs every per-method wrapper below, so every
     * method is name-pinnable and the seal/dispatch/free body lives once. */
    using MethodFn = int (*)(sd_bus_message*, void*, sd_bus_error*);
    int call(MethodFn fn, sd_bus_message* msg, std::string* name_out = nullptr) {
        seal(msg);
        sd_bus_error err = SD_BUS_ERROR_NULL;
        int r = fn(msg, svc_.get(), &err);
        if (name_out != nullptr) *name_out = err.name ? err.name : "";
        sd_bus_error_free(&err);
        sd_bus_message_unref(msg);
        return r;
    }

    int call_check_update_available(sd_bus_message* m, std::string* n = nullptr) {
        return call(&Service::method_check_update_available, m, n);
    }
    int call_start_download(sd_bus_message* m, std::string* n = nullptr) {
        return call(&Service::method_start_download, m, n);
    }
    int call_report_download_progress(sd_bus_message* m, std::string* n = nullptr) {
        return call(&Service::method_report_download_progress, m, n);
    }
    int call_finish_download(sd_bus_message* m, std::string* n = nullptr) {
        return call(&Service::method_finish_download, m, n);
    }
    int call_apply(sd_bus_message* m, std::string* n = nullptr) {
        return call(&Service::method_apply, m, n);
    }
    int call_rollback(sd_bus_message* m, std::string* n = nullptr) {
        return call(&Service::method_rollback, m, n);
    }
    int call_commit(sd_bus_message* m, std::string* n = nullptr) {
        return call(&Service::method_commit, m, n);
    }
    int call_start_install(sd_bus_message* m, std::string* n = nullptr) {
        return call(&Service::method_start_install, m, n);
    }
    int call_install_local(sd_bus_message* m, std::string* n = nullptr) {
        return call(&Service::method_install_local, m, n);
    }
    int call_cancel_install(sd_bus_message* m, std::string* n = nullptr) {
        return call(&Service::method_cancel_install, m, n);
    }

    /* Prime service into a state where StartInstall can succeed:
     * session 1, fw download finished at /tmp/test.raucb. */
    void prime_for_install() {
        svc_->m_session_id      = 1;
        svc_->m_session_counter = 1;
        svc_->m_update_type     = "fw";
        svc_->m_download_state  = "finished";
        svc_->m_payload_path    = "/tmp/test.raucb";
    }

    /* Real GetState round-trip through the registered vtable. Exercises the
     * method's a{sv} dict serialization (11 properties, nested variants),
     * which every other test bypasses by reading m_ fields directly — a
     * serialization bug there would ship unseen. */
    struct GetStateResult {
        bool        ok = false;
        uint32_t    session_id = 0;
        std::string install_state;
        std::string download_state;
        int32_t     install_progress = -1;
    };

    static int capture_reply(sd_bus_message* m, void* userdata, sd_bus_error*) {
        *static_cast<sd_bus_message**>(userdata) = sd_bus_message_ref(m);
        return 0;
    }

    GetStateResult get_state() {
        GetStateResult out;
        const char* unique = nullptr;
        if (sd_bus_get_unique_name(bus_, &unique) < 0 || unique == nullptr) {
            return out;
        }
        sd_bus_message* call = nullptr;
        if (sd_bus_message_new_method_call(bus_, &call, unique,
                "/de/fsembedded/fsupdate1", "de.fsembedded.fsupdate1", "GetState") < 0) {
            return out;
        }
        sd_bus_message* reply = nullptr;
        sd_bus_slot*    slot  = nullptr;
        int r = sd_bus_call_async(bus_, &slot, call, &capture_reply, &reply, 5000000);
        sd_bus_message_unref(call);
        if (r < 0) {
            return out;
        }
        for (int i = 0; i < 500 && reply == nullptr; ++i) {
            if (sd_event_run(event_, 20000) < 0) {
                break;
            }
        }
        if (slot != nullptr) {
            sd_bus_slot_unref(slot);
        }
        if (reply == nullptr) {
            return out;
        }

        if (sd_bus_message_enter_container(reply, 'a', "{sv}") >= 0) {
            while (sd_bus_message_enter_container(reply, 'e', "sv") > 0) {
                const char* key = nullptr;
                sd_bus_message_read_basic(reply, 's', &key);
                char        vtype = 0;
                const char* contents = nullptr;
                sd_bus_message_peek_type(reply, &vtype, &contents);
                sd_bus_message_enter_container(reply, 'v', contents);
                const std::string k = key != nullptr ? key : "";
                if (k == "InstallState" || k == "DownloadState") {
                    const char* val = nullptr;
                    sd_bus_message_read_basic(reply, 's', &val);
                    (k == "InstallState" ? out.install_state : out.download_state) =
                        val != nullptr ? val : "";
                } else if (k == "SessionId") {
                    sd_bus_message_read_basic(reply, 'u', &out.session_id);
                } else if (k == "InstallProgress") {
                    sd_bus_message_read_basic(reply, 'i', &out.install_progress);
                } else {
                    sd_bus_message_skip(reply, contents);
                }
                sd_bus_message_exit_container(reply);
                sd_bus_message_exit_container(reply);
            }
            sd_bus_message_exit_container(reply);
            out.ok = true;
        }
        sd_bus_message_unref(reply);
        return out;
    }

    /* Drain all ready events from the sd-event loop without blocking. */
    void wait_until_quiescent() {
        while (sd_event_run(event_, 0) > 0)
            ;
    }

    sd_bus*                  bus_   = nullptr;
    sd_bus_slot*             slot_  = nullptr;
    sd_event*                event_ = nullptr;
    std::unique_ptr<Service> svc_;
    MockUpdater*             mock_  = nullptr;
};

/* ═══════════════════════════════════════════════════════════════════════════
 * Unit tests — individual method behaviour
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 1. StartDownload sets state fields correctly */
TEST_F(ServiceTest, StartDownload_SetsState)
{
    sd_bus_message* msg = make_msg("StartDownload");
    sd_bus_message_append(msg, "sst", "fw", "1.0.0", static_cast<uint64_t>(1024));

    call_start_download(msg);

    EXPECT_NE(svc_->m_session_id, 0u);
    EXPECT_EQ(svc_->m_session_id, svc_->m_session_counter);
    EXPECT_EQ(svc_->m_update_type, "fw");
    EXPECT_EQ(svc_->m_update_version, "1.0.0");
    EXPECT_EQ(svc_->m_update_size, 1024u);
    EXPECT_EQ(svc_->m_download_state, "in_progress");
    EXPECT_EQ(svc_->m_download_progress, 0);
}

/* 2. StartDownload rejects when session is already active */
TEST_F(ServiceTest, StartDownload_RejectsActiveSession)
{
    {
        sd_bus_message* msg = make_msg("StartDownload");
        sd_bus_message_append(msg, "sst", "fw", "1.0", static_cast<uint64_t>(512));
        call_start_download(msg);
    }

    uint32_t first_id = svc_->m_session_id;
    ASSERT_NE(first_id, 0u);

    sd_bus_message* msg2 = make_msg("StartDownload");
    sd_bus_message_append(msg2, "sst", "app", "2.0", static_cast<uint64_t>(256));
    int r = call_start_download(msg2);

    EXPECT_LT(r, 0);
    EXPECT_EQ(svc_->m_session_id, first_id);
}

/* 3. StartDownload allows new session after terminal (finished) state */
TEST_F(ServiceTest, StartDownload_AllowsNewSessionAfterFinished)
{
    svc_->m_session_id      = 1;
    svc_->m_session_counter = 1;
    svc_->m_download_state  = "finished";

    sd_bus_message* msg = make_msg("StartDownload");
    sd_bus_message_append(msg, "sst", "app", "2.0", static_cast<uint64_t>(200));
    call_start_download(msg);

    EXPECT_EQ(svc_->m_download_state, "in_progress");
    EXPECT_NE(svc_->m_session_id, 0u);
    EXPECT_GT(svc_->m_session_id, 1u);
}

/* 4. StartDownload allows new session after terminal (failed) state */
TEST_F(ServiceTest, StartDownload_AllowsNewSessionAfterFailed)
{
    svc_->m_session_id      = 5;
    svc_->m_session_counter = 5;
    svc_->m_download_state  = "failed";

    sd_bus_message* msg = make_msg("StartDownload");
    sd_bus_message_append(msg, "sst", "fw", "3.0", static_cast<uint64_t>(100));
    call_start_download(msg);

    EXPECT_EQ(svc_->m_download_state, "in_progress");
    EXPECT_NE(svc_->m_session_id, 0u);
}

/* 5. ReportDownloadProgress updates progress correctly */
TEST_F(ServiceTest, ReportDownloadProgress_UpdatesProgress)
{
    svc_->m_session_id      = 1;
    svc_->m_session_counter = 1;
    svc_->m_download_state  = "in_progress";
    svc_->m_update_size     = 1000;

    sd_bus_message* msg = make_msg("ReportDownloadProgress");
    sd_bus_message_append(msg, "ut", static_cast<uint32_t>(1), static_cast<uint64_t>(500));
    call_report_download_progress(msg);

    EXPECT_EQ(svc_->m_download_progress, 50);
}

/* 6. ReportDownloadProgress rejects wrong session id */
TEST_F(ServiceTest, ReportDownloadProgress_RejectsWrongSession)
{
    svc_->m_session_id      = 1;
    svc_->m_session_counter = 1;
    svc_->m_download_state  = "in_progress";
    svc_->m_update_size     = 1000;

    sd_bus_message* msg = make_msg("ReportDownloadProgress");
    sd_bus_message_append(msg, "ut", static_cast<uint32_t>(99), static_cast<uint64_t>(500));
    int r = call_report_download_progress(msg);

    EXPECT_LT(r, 0);
    EXPECT_EQ(svc_->m_download_progress, 0);
}

/* 7. ReportDownloadProgress rejects when not in_progress */
TEST_F(ServiceTest, ReportDownloadProgress_RejectsWhenNotInProgress)
{
    svc_->m_session_id      = 1;
    svc_->m_session_counter = 1;
    svc_->m_download_state  = "finished";
    svc_->m_update_size     = 1000;

    sd_bus_message* msg = make_msg("ReportDownloadProgress");
    sd_bus_message_append(msg, "ut", static_cast<uint32_t>(1), static_cast<uint64_t>(500));
    int r = call_report_download_progress(msg);

    EXPECT_LT(r, 0);
}

/* 8. FinishDownload success sets finished + payload path + progress 100 */
TEST_F(ServiceTest, FinishDownload_SetsFinished)
{
    svc_->m_session_id      = 1;
    svc_->m_session_counter = 1;
    svc_->m_download_state  = "in_progress";

    sd_bus_message* msg = make_msg("FinishDownload");
    sd_bus_message_append(msg, "ubs",
        static_cast<uint32_t>(1), static_cast<int>(1), "/tmp/payload.raucb");
    call_finish_download(msg);

    EXPECT_EQ(svc_->m_download_state, "finished");
    EXPECT_EQ(svc_->m_payload_path, "/tmp/payload.raucb");
    EXPECT_EQ(svc_->m_download_progress, 100);
}

/* 9. FinishDownload failure sets failed state */
TEST_F(ServiceTest, FinishDownload_SetsFailed)
{
    svc_->m_session_id      = 1;
    svc_->m_session_counter = 1;
    svc_->m_download_state  = "in_progress";

    sd_bus_message* msg = make_msg("FinishDownload");
    sd_bus_message_append(msg, "ubs",
        static_cast<uint32_t>(1), static_cast<int>(0), "");
    call_finish_download(msg);

    EXPECT_EQ(svc_->m_download_state, "failed");
}

/* 10. FinishDownload rejects wrong session id */
TEST_F(ServiceTest, FinishDownload_RejectsWrongSession)
{
    svc_->m_session_id      = 1;
    svc_->m_session_counter = 1;
    svc_->m_download_state  = "in_progress";

    sd_bus_message* msg = make_msg("FinishDownload");
    sd_bus_message_append(msg, "ubs",
        static_cast<uint32_t>(99), static_cast<int>(1), "/tmp/payload.raucb");
    int r = call_finish_download(msg);

    EXPECT_LT(r, 0);
    EXPECT_EQ(svc_->m_download_state, "in_progress");
}

/* 11. Apply rejects when install state is not finished */
TEST_F(ServiceTest, Apply_RejectsWhenInstallNotFinished)
{
    svc_->m_install_state = "in_progress";
    svc_->m_session_id    = 1;

    sd_bus_message* msg = make_msg("Apply");
    int r = call_apply(msg);

    EXPECT_LT(r, 0);
    EXPECT_EQ(mock_->apply_calls_, 0);
}

/* 12. Apply forwards to the lib and resets session — reboot required
 *     for firmware-bearing updates (the common case). */
TEST_F(ServiceTest, Apply_FwForwardsAndResetsSession)
{
    svc_->m_install_state    = "finished";
    svc_->m_install_progress = 100;
    svc_->m_update_type      = "fw";
    svc_->m_update_version   = "2.0";
    svc_->m_update_size      = 1024;
    svc_->m_update_available = true;
    svc_->m_session_id       = 1;
    mock_->apply_returns_reboot_ = true;

    sd_bus_message* msg = make_msg("Apply");
    call_apply(msg);

    EXPECT_EQ(mock_->apply_calls_, 1);
    EXPECT_EQ(mock_->last_apply_type_, "fw");
    EXPECT_EQ(svc_->m_session_id, 0u);
    EXPECT_TRUE(svc_->m_reboot_pending);
    EXPECT_EQ(svc_->m_install_state, "idle");
    EXPECT_EQ(svc_->m_install_progress, 0);
    EXPECT_FALSE(svc_->m_update_available);
    EXPECT_EQ(svc_->m_update_type, "");
    EXPECT_EQ(svc_->m_update_version, "");
    EXPECT_EQ(svc_->m_update_size, 0u);
}

/* 13. Apply propagates the lib's no-reboot signal: when the lib
 *     reports no reboot needed, m_reboot_pending stays false. The lib
 *     decides — service just relays. */
TEST_F(ServiceTest, Apply_NoRebootPropagatesFromLib)
{
    svc_->m_install_state    = "finished";
    svc_->m_install_progress = 100;
    svc_->m_update_type      = "app";
    svc_->m_update_version   = "1.5";
    svc_->m_update_size      = 512;
    svc_->m_update_available = true;
    svc_->m_session_id       = 1;
    mock_->apply_returns_reboot_ = false;

    sd_bus_message* msg = make_msg("Apply");
    call_apply(msg);

    EXPECT_EQ(mock_->apply_calls_, 1);
    EXPECT_EQ(mock_->last_apply_type_, "app");
    EXPECT_EQ(svc_->m_session_id, 0u);
    EXPECT_FALSE(svc_->m_reboot_pending);
    EXPECT_EQ(svc_->m_install_state, "idle");
    EXPECT_EQ(svc_->m_install_progress, 0);
    EXPECT_FALSE(svc_->m_update_available);
    EXPECT_EQ(svc_->m_update_type, "");
}

/* 13b. Apply maps lib exceptions to a bus error and leaves session
 *      fields untouched so a retry is safe. */
TEST_F(ServiceTest, Apply_LibThrowReportsErrorAndKeepsSession)
{
    svc_->m_install_state    = "finished";
    svc_->m_install_progress = 100;
    svc_->m_update_type      = "fw";
    svc_->m_session_id       = 7;
    mock_->apply_should_throw_ = true;

    sd_bus_message* msg = make_msg("Apply");
    int r = call_apply(msg);

    EXPECT_LT(r, 0);
    EXPECT_EQ(mock_->apply_calls_, 1);
    /* Session fields preserved so the caller can retry. */
    EXPECT_EQ(svc_->m_session_id, 7u);
    EXPECT_EQ(svc_->m_install_state, "finished");
    EXPECT_FALSE(svc_->m_reboot_pending);
}

/* 14. Rollback resets session and clears states */
TEST_F(ServiceTest, Rollback_ResetsSession)
{
    svc_->m_session_id        = 1;
    svc_->m_session_counter   = 1;
    svc_->m_update_type       = "fw";
    svc_->m_update_version    = "2.0";
    svc_->m_update_size       = 2048;
    svc_->m_update_available  = true;
    svc_->m_download_state    = "in_progress";
    svc_->m_download_progress = 40;
    svc_->m_install_state     = "idle";
    svc_->m_install_progress  = 0;

    sd_bus_message* msg = make_msg("Rollback");
    call_rollback(msg);

    EXPECT_EQ(mock_->rollback_calls_, 1);
    EXPECT_EQ(svc_->m_session_id, 0u);
    EXPECT_EQ(svc_->m_download_state, "idle");
    EXPECT_EQ(svc_->m_install_state, "idle");
    EXPECT_EQ(svc_->m_download_progress, 0);
    EXPECT_EQ(svc_->m_install_progress, 0);
    EXPECT_FALSE(svc_->m_update_available);
    EXPECT_EQ(svc_->m_update_type, "");
    EXPECT_EQ(svc_->m_update_version, "");
    EXPECT_EQ(svc_->m_update_size, 0u);
}

/* ── D-Bus surface (GetState serialization) ───────────────────────────────── */

TEST_F(ServiceTest, GetState_SerializesIdleStateOverTheBus)
{
    const GetStateResult s = get_state();
    ASSERT_TRUE(s.ok) << "GetState round-trip did not complete";
    EXPECT_EQ(s.session_id, 0u);
    EXPECT_EQ(s.install_state, "idle");
    EXPECT_EQ(s.download_state, "idle");
    EXPECT_EQ(s.install_progress, 0);
}

TEST_F(ServiceTest, GetState_ReflectsLifecycleFieldsOverTheBus)
{
    /* Drive the backing fields to a mid-install view, then confirm the
     * dict the method actually serializes matches — not just the fields. */
    svc_->m_session_id       = 1;
    svc_->m_install_state    = "in_progress";
    svc_->m_install_progress = 42;
    svc_->m_download_state   = "finished";

    const GetStateResult s = get_state();
    ASSERT_TRUE(s.ok) << "GetState round-trip did not complete";
    EXPECT_EQ(s.session_id, 1u);
    EXPECT_EQ(s.install_state, "in_progress");
    EXPECT_EQ(s.download_state, "finished");
    EXPECT_EQ(s.install_progress, 42);
}

/* Stale-state recovery: Service ctor reflects lib-reported pending update
 * into the state record so D-Bus subscribers see the right view after a
 * post-crash restart. SessionId stays at 0 (process-local token is gone);
 * DownloadState stays "idle" because the payload_path was process-local
 * too — clients must Apply()/Rollback() or start a fresh StartDownload
 * rather than StartInstall against a phantom download. InstallState
 * recovers to "finished" so that Apply's gate accepts the pending
 * update (the restart contract). */
TEST(ServiceCtorRecoveryTest, ReflectsPendingUpdateFromUpdater)
{
    sd_bus*   bus   = nullptr;
    sd_event* event = nullptr;
    int r = sd_bus_open_user(&bus);
    if (r < 0) r = sd_bus_open_system(&bus);
    if (r < 0) {
        if (require_bus())
            FAIL() << "no D-Bus available but FSUP_REQUIRE_BUS is set (rc " << r << ")";
        GTEST_SKIP() << "no D-Bus available";
    }
    ASSERT_GE(sd_event_default(&event), 0);
    ASSERT_GE(sd_bus_attach_event(bus, event, SD_EVENT_PRIORITY_NORMAL), 0);

    auto mock = std::make_unique<MockUpdater>();
    mock->check_result_ = {true, "fw+app", "3.2.1", 4096};
    auto* mock_raw = mock.get();

    Service svc(bus, event, std::move(mock));

    EXPECT_EQ(mock_raw->check_calls_, 1);
    EXPECT_TRUE(svc.m_update_available);
    EXPECT_EQ(svc.m_update_type,    "fw+app");
    EXPECT_EQ(svc.m_update_version, "3.2.1");
    EXPECT_EQ(svc.m_update_size,    4096u);
    EXPECT_EQ(svc.m_download_state, "idle");
    EXPECT_EQ(svc.m_download_progress, 0);
    EXPECT_EQ(svc.m_session_id, 0u);     /* prior session is gone */
    EXPECT_EQ(svc.m_install_state, "finished");   /* Apply gate accepts */
    EXPECT_EQ(svc.m_install_progress, 100);

    sd_event_unref(event);
    sd_bus_unref(bus);
}

/* Apply must work on a pending update recovered at construction — the
 * restart contract: an install finished before a crash/restart is still
 * applicable over D-Bus (durable lib state, no session needed). One
 * Service per event loop: reusing the fixture's loop for a second
 * Service leaves a floating wakeup source behind. */
TEST(ServiceCtorRecoveryTest, ApplyWorksOnRecoveredPendingUpdate)
{
    sd_bus*   bus   = nullptr;
    sd_event* event = nullptr;
    int r = sd_bus_open_user(&bus);
    if (r < 0) r = sd_bus_open_system(&bus);
    if (r < 0) {
        if (require_bus())
            FAIL() << "no D-Bus available but FSUP_REQUIRE_BUS is set (rc " << r << ")";
        GTEST_SKIP() << "no D-Bus available";
    }
    ASSERT_GE(sd_event_default(&event), 0);
    ASSERT_GE(sd_bus_attach_event(bus, event, SD_EVENT_PRIORITY_NORMAL), 0);

    auto mock = std::make_unique<MockUpdater>();
    mock->check_result_ = {true, "fw", "3.2.1", 4096};
    auto* mock_raw = mock.get();

    {
        Service svc(bus, event, std::move(mock));
        EXPECT_EQ(svc.m_install_state, "finished");

        sd_bus_message* msg = nullptr;
        sd_bus_message_new_method_call(bus, &msg,
            "de.fsembedded.fsupdate1",
            "/de/fsembedded/fsupdate1",
            "de.fsembedded.fsupdate1",
            "Apply");
        sd_bus_message_seal(msg, 1, 0);
        sd_bus_message_rewind(msg, 1);
        sd_bus_error err = SD_BUS_ERROR_NULL;
        (void)Service::method_apply(msg, &svc, &err);
        sd_bus_error_free(&err);
        sd_bus_message_unref(msg);

        EXPECT_EQ(mock_raw->apply_calls_, 1);
        EXPECT_EQ(mock_raw->last_apply_type_, "fw");
    }

    sd_event_unref(event);
    sd_bus_unref(bus);
}

/* Stale-state recovery is a no-op when the lib reports nothing pending. */
TEST(ServiceCtorRecoveryTest, NoOpWhenNothingPending)
{
    sd_bus*   bus   = nullptr;
    sd_event* event = nullptr;
    int r = sd_bus_open_user(&bus);
    if (r < 0) r = sd_bus_open_system(&bus);
    if (r < 0) {
        if (require_bus())
            FAIL() << "no D-Bus available but FSUP_REQUIRE_BUS is set (rc " << r << ")";
        GTEST_SKIP() << "no D-Bus available";
    }
    ASSERT_GE(sd_event_default(&event), 0);
    ASSERT_GE(sd_bus_attach_event(bus, event, SD_EVENT_PRIORITY_NORMAL), 0);

    auto mock = std::make_unique<MockUpdater>();
    /* default check_result_ = {false, "", "", 0} */
    Service svc(bus, event, std::move(mock));

    EXPECT_FALSE(svc.m_update_available);
    EXPECT_EQ(svc.m_update_type, "");
    EXPECT_EQ(svc.m_download_state, "idle");
    EXPECT_EQ(svc.m_download_progress, 0);

    sd_event_unref(event);
    sd_bus_unref(bus);
}

/* 15. CheckUpdateAvailable calls updater when available */
TEST_F(ServiceTest, CheckUpdateAvailable_Available)
{
    mock_->check_result_ = {true, "fw", "2.1.0", 0};

    sd_bus_message* msg = make_msg("CheckUpdateAvailable");
    call_check_update_available(msg);

    EXPECT_EQ(mock_->check_calls_, 1);
}

/* 16. CheckUpdateAvailable calls updater when not available */
TEST_F(ServiceTest, CheckUpdateAvailable_NotAvailable)
{
    mock_->check_result_ = {false, "", "", 0};

    sd_bus_message* msg = make_msg("CheckUpdateAvailable");
    call_check_update_available(msg);

    EXPECT_EQ(mock_->check_calls_, 1);
}

/* 17. ReportDownloadProgress clamps at 100 when bytes > size */
TEST_F(ServiceTest, ReportDownloadProgress_ClampsAt100)
{
    svc_->m_session_id      = 1;
    svc_->m_session_counter = 1;
    svc_->m_download_state  = "in_progress";
    svc_->m_update_size     = 500;

    sd_bus_message* msg = make_msg("ReportDownloadProgress");
    sd_bus_message_append(msg, "ut",
        static_cast<uint32_t>(1), static_cast<uint64_t>(9999));
    call_report_download_progress(msg);

    EXPECT_EQ(svc_->m_download_progress, 100);
}

/* 18. StartDownload resets install state for a new session */
TEST_F(ServiceTest, StartDownload_ResetsInstallState)
{
    svc_->m_session_id       = 3;
    svc_->m_session_counter  = 3;
    svc_->m_download_state   = "finished";
    svc_->m_install_state    = "finished";
    svc_->m_install_progress = 100;
    svc_->m_reboot_pending   = true;

    sd_bus_message* msg = make_msg("StartDownload");
    sd_bus_message_append(msg, "sst", "app", "4.0", static_cast<uint64_t>(300));
    call_start_download(msg);

    EXPECT_EQ(svc_->m_install_state, "idle");
    EXPECT_EQ(svc_->m_install_progress, 0);
    EXPECT_FALSE(svc_->m_reboot_pending);
    EXPECT_EQ(svc_->m_download_state, "in_progress");
}

/* 19. StartInstall rejects wrong session id */
TEST_F(ServiceTest, StartInstall_RejectsWrongSession)
{
    prime_for_install();

    sd_bus_message* msg = make_msg("StartInstall");
    sd_bus_message_append(msg, "us", static_cast<uint32_t>(99), "fw");
    int r = call_start_install(msg);

    EXPECT_LT(r, 0);
    EXPECT_EQ(svc_->m_install_state, "idle");
}

/* session_id 0 is the "no active session" sentinel per the interface
 * doc; StartInstall must reject it explicitly so it does not collide
 * with a default-initialised m_session_id on a freshly-restarted
 * service that just recovered UpdateAvailable from the lib. */
TEST_F(ServiceTest, StartInstall_RejectsReservedSessionZero)
{
    /* Do not prime_for_install: leave m_session_id at its default 0.
     * Set the other gates the way a recovered service would have them. */
    svc_->m_update_type    = "fw";
    svc_->m_download_state = "finished";
    svc_->m_payload_path   = "/tmp/test.raucb";

    sd_bus_message* msg = make_msg("StartInstall");
    sd_bus_message_append(msg, "us", static_cast<uint32_t>(0), "fw");
    int r = call_start_install(msg);

    EXPECT_LT(r, 0);
    EXPECT_EQ(svc_->m_install_state, "idle");
    EXPECT_EQ(svc_->m_install_progress, 0);
}

/* 20. StartInstall rejects when download not finished */
TEST_F(ServiceTest, StartInstall_RejectsIfDownloadNotFinished)
{
    prime_for_install();
    svc_->m_download_state = "in_progress";

    sd_bus_message* msg = make_msg("StartInstall");
    sd_bus_message_append(msg, "us", static_cast<uint32_t>(1), "fw");
    int r = call_start_install(msg);

    EXPECT_LT(r, 0);
    EXPECT_EQ(svc_->m_install_state, "idle");
}

/* 21. StartInstall rejects when install is already active or done */
TEST_F(ServiceTest, StartInstall_RejectsIfInstallNotIdle)
{
    prime_for_install();
    svc_->m_install_state = "in_progress";

    sd_bus_message* msg = make_msg("StartInstall");
    sd_bus_message_append(msg, "us", static_cast<uint32_t>(1), "fw");
    int r = call_start_install(msg);

    EXPECT_LT(r, 0);
    EXPECT_EQ(svc_->m_install_state, "in_progress");
}

/* 22. StartInstall rejects when update_type does not match session */
TEST_F(ServiceTest, StartInstall_RejectsIfTypeMismatch)
{
    prime_for_install();

    sd_bus_message* msg = make_msg("StartInstall");
    sd_bus_message_append(msg, "us", static_cast<uint32_t>(1), "app");
    int r = call_start_install(msg);

    EXPECT_LT(r, 0);
    EXPECT_EQ(svc_->m_install_state, "idle");
}

/* 23. StartInstall sets install_state to in_progress immediately */
TEST_F(ServiceTest, StartInstall_SetsInProgressStateImmediately)
{
    prime_for_install();
    mock_->arm_install();   /* hold thread inside update_image */

    sd_bus_message* msg = make_msg("StartInstall");
    sd_bus_message_append(msg, "us", static_cast<uint32_t>(1), "fw");
    int r = call_start_install(msg);

    ASSERT_GE(r, 0);
    EXPECT_EQ(svc_->m_install_state, "in_progress");
    EXPECT_EQ(svc_->m_install_progress, 0);

    mock_->release_install();
    if (svc_->m_install_thread.joinable())
        svc_->m_install_thread.join();
}

/* 24. Progress callback posts event; on_wakeup updates install_progress */
TEST_F(ServiceTest, StartInstall_ProgressCallback_UpdatesProgress)
{
    prime_for_install();
    mock_->arm_install();   /* hold thread in update_image */

    sd_bus_message* msg = make_msg("StartInstall");
    sd_bus_message_append(msg, "us", static_cast<uint32_t>(1), "fw");
    ASSERT_GE(call_start_install(msg), 0);

    /* invoke callback directly while thread is still blocked — deterministic */
    ASSERT_TRUE(mock_->progress_cb_);
    mock_->progress_cb_(42);

    wait_until_quiescent();

    EXPECT_EQ(svc_->m_install_progress, 42);

    mock_->release_install();
    if (svc_->m_install_thread.joinable())
        svc_->m_install_thread.join();
}

/* 25. Successful install thread sets install_state to finished */
TEST_F(ServiceTest, StartInstall_ThreadCompletes_SetsFinished)
{
    prime_for_install();

    sd_bus_message* msg = make_msg("StartInstall");
    sd_bus_message_append(msg, "us", static_cast<uint32_t>(1), "fw");
    ASSERT_GE(call_start_install(msg), 0);

    if (svc_->m_install_thread.joinable())
        svc_->m_install_thread.join();

    wait_until_quiescent();

    EXPECT_EQ(svc_->m_install_state, "finished");
    EXPECT_EQ(svc_->m_install_progress, 100);
    EXPECT_EQ(mock_->update_calls_, 1);
}

/* 26. Failed install thread (exception) sets install_state to failed */
TEST_F(ServiceTest, StartInstall_ThreadFails_SetsFailed)
{
    prime_for_install();
    mock_->update_should_throw_ = true;

    sd_bus_message* msg = make_msg("StartInstall");
    sd_bus_message_append(msg, "us", static_cast<uint32_t>(1), "fw");
    ASSERT_GE(call_start_install(msg), 0);

    if (svc_->m_install_thread.joinable())
        svc_->m_install_thread.join();

    wait_until_quiescent();

    EXPECT_EQ(svc_->m_install_state, "failed");
    EXPECT_EQ(mock_->update_calls_, 1);
}

/* 26b. A non-std exception from the lib must not kill the process —
 * the worker converts it to a failed terminal so InstallCompleted
 * still fires for waiting clients. */
TEST_F(ServiceTest, StartInstall_ThreadThrowsNonStd_SetsFailed)
{
    prime_for_install();
    mock_->update_should_throw_nonstd_ = true;

    sd_bus_message* msg = make_msg("StartInstall");
    sd_bus_message_append(msg, "us", static_cast<uint32_t>(1), "fw");
    ASSERT_GE(call_start_install(msg), 0);

    if (svc_->m_install_thread.joinable())
        svc_->m_install_thread.join();

    wait_until_quiescent();

    EXPECT_EQ(svc_->m_install_state, "failed");
    EXPECT_EQ(mock_->update_calls_, 1);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Workflow tests — composed multi-method flows
 *
 * Download: FR-SVC-020..029
 * Install (happy): FR-SVC-030..039
 * Install (failure): FR-SVC-040..049
 * Apply/Rollback: FR-SVC-050..059
 * Concurrency/errors: FR-SVC-060..079
 * ═══════════════════════════════════════════════════════════════════════════ */

/* W3: StartDownload → ReportProgress → FinishDownload (fw, success)
 * FR-SVC-020 placeholder */
TEST_F(ServiceTest, Workflow_W3_FwDownloadHappy)
{
    sd_bus_message* msg = make_msg("StartDownload");
    sd_bus_message_append(msg, "sst", "fw", "2.0.0", static_cast<uint64_t>(2000));
    ASSERT_GE(call_start_download(msg), 0);

    uint32_t sid = svc_->m_session_id;
    ASSERT_NE(sid, 0u);
    EXPECT_EQ(svc_->m_download_state, "in_progress");

    {
        sd_bus_message* m = make_msg("ReportDownloadProgress");
        sd_bus_message_append(m, "ut", sid, static_cast<uint64_t>(1000));
        EXPECT_GE(call_report_download_progress(m), 0);
        EXPECT_EQ(svc_->m_download_progress, 50);
    }

    {
        sd_bus_message* m = make_msg("FinishDownload");
        sd_bus_message_append(m, "ubs", sid, static_cast<int>(1), "/tmp/fw.raucb");
        EXPECT_GE(call_finish_download(m), 0);
    }

    EXPECT_EQ(svc_->m_download_state, "finished");
    EXPECT_EQ(svc_->m_download_progress, 100);
    EXPECT_EQ(svc_->m_payload_path, "/tmp/fw.raucb");
    EXPECT_EQ(svc_->m_update_type, "fw");
}

/* W4: App download happy path
 * FR-SVC-021 placeholder */
TEST_F(ServiceTest, Workflow_W4_AppDownloadHappy)
{
    sd_bus_message* msg = make_msg("StartDownload");
    sd_bus_message_append(msg, "sst", "app", "1.5.0", static_cast<uint64_t>(512));
    ASSERT_GE(call_start_download(msg), 0);

    uint32_t sid = svc_->m_session_id;

    {
        sd_bus_message* m = make_msg("FinishDownload");
        sd_bus_message_append(m, "ubs", sid, static_cast<int>(1), "/tmp/app.raucb");
        EXPECT_GE(call_finish_download(m), 0);
    }

    EXPECT_EQ(svc_->m_download_state, "finished");
    EXPECT_EQ(svc_->m_update_type, "app");
    EXPECT_EQ(svc_->m_payload_path, "/tmp/app.raucb");
}

/* W7: Full fw flow — download → install → apply (reboot required)
 * FR-SVC-030 placeholder */
TEST_F(ServiceTest, Workflow_W7_FwInstallToApply)
{
    /* Download */
    sd_bus_message* dl = make_msg("StartDownload");
    sd_bus_message_append(dl, "sst", "fw", "2.0.0", static_cast<uint64_t>(1024));
    ASSERT_GE(call_start_download(dl), 0);
    uint32_t sid = svc_->m_session_id;

    {
        sd_bus_message* m = make_msg("FinishDownload");
        sd_bus_message_append(m, "ubs", sid, static_cast<int>(1), "/tmp/fw.raucb");
        ASSERT_GE(call_finish_download(m), 0);
    }
    ASSERT_EQ(svc_->m_download_state, "finished");

    /* Install */
    {
        sd_bus_message* m = make_msg("StartInstall");
        sd_bus_message_append(m, "us", sid, "fw");
        ASSERT_GE(call_start_install(m), 0);
    }

    if (svc_->m_install_thread.joinable())
        svc_->m_install_thread.join();
    wait_until_quiescent();

    ASSERT_EQ(svc_->m_install_state, "finished");
    EXPECT_EQ(mock_->update_calls_, 1);
    EXPECT_EQ(mock_->last_update_type_, "fw");

    /* Apply: fw install delegates to lib's apply_pending_update, which
     * reports reboot required. Service propagates as m_reboot_pending. */
    mock_->apply_returns_reboot_ = true;
    ASSERT_GE(call_apply(make_msg("Apply")), 0);

    EXPECT_EQ(mock_->apply_calls_, 1);
    EXPECT_EQ(mock_->last_apply_type_, "fw");
    EXPECT_TRUE(svc_->m_reboot_pending);
    EXPECT_EQ(svc_->m_session_id, 0u);
    EXPECT_EQ(svc_->m_install_state, "idle");
    EXPECT_EQ(svc_->m_install_progress, 0);
}

/* W8: App install — service propagates the lib's reboot signal.
 * The new app squashfs is only picked up at preinit, so the lib's
 * apply_pending_update reports reboot required for app-only too.
 * FR-SVC-031 placeholder */
TEST_F(ServiceTest, Workflow_W8_AppInstallApplyRebootRequired)
{
    sd_bus_message* dl = make_msg("StartDownload");
    sd_bus_message_append(dl, "sst", "app", "1.5.0", static_cast<uint64_t>(512));
    ASSERT_GE(call_start_download(dl), 0);
    uint32_t sid = svc_->m_session_id;

    {
        sd_bus_message* m = make_msg("FinishDownload");
        sd_bus_message_append(m, "ubs", sid, static_cast<int>(1), "/tmp/app.raucb");
        ASSERT_GE(call_finish_download(m), 0);
    }

    {
        sd_bus_message* m = make_msg("StartInstall");
        sd_bus_message_append(m, "us", sid, "app");
        ASSERT_GE(call_start_install(m), 0);
    }

    if (svc_->m_install_thread.joinable())
        svc_->m_install_thread.join();
    wait_until_quiescent();

    ASSERT_EQ(svc_->m_install_state, "finished");

    mock_->apply_returns_reboot_ = true;
    ASSERT_GE(call_apply(make_msg("Apply")), 0);

    EXPECT_EQ(mock_->apply_calls_, 1);
    EXPECT_EQ(mock_->last_apply_type_, "app");
    EXPECT_TRUE(svc_->m_reboot_pending);
    EXPECT_EQ(svc_->m_session_id, 0u);
}

/* W11: Install failure → state = failed → rollback resets
 * FR-SVC-040 placeholder */
TEST_F(ServiceTest, Workflow_W11_InstallFailureThenRollback)
{
    prime_for_install();
    mock_->update_should_throw_ = true;

    {
        sd_bus_message* m = make_msg("StartInstall");
        sd_bus_message_append(m, "us", static_cast<uint32_t>(1), "fw");
        ASSERT_GE(call_start_install(m), 0);
    }

    if (svc_->m_install_thread.joinable())
        svc_->m_install_thread.join();
    wait_until_quiescent();

    ASSERT_EQ(svc_->m_install_state, "failed");

    ASSERT_GE(call_rollback(make_msg("Rollback")), 0);

    EXPECT_EQ(mock_->rollback_calls_, 1);
    EXPECT_EQ(svc_->m_install_state, "idle");
    EXPECT_EQ(svc_->m_download_state, "idle");
    EXPECT_EQ(svc_->m_session_id, 0u);
}

/* W16: Download failure → rollback resets
 * FR-SVC-050 placeholder */
TEST_F(ServiceTest, Workflow_W16_DownloadFailureThenRollback)
{
    sd_bus_message* dl = make_msg("StartDownload");
    sd_bus_message_append(dl, "sst", "fw", "2.0.0", static_cast<uint64_t>(1024));
    ASSERT_GE(call_start_download(dl), 0);
    uint32_t sid = svc_->m_session_id;

    {
        sd_bus_message* m = make_msg("FinishDownload");
        sd_bus_message_append(m, "ubs", sid, static_cast<int>(0), "");
        ASSERT_GE(call_finish_download(m), 0);
    }
    ASSERT_EQ(svc_->m_download_state, "failed");

    ASSERT_GE(call_rollback(make_msg("Rollback")), 0);

    EXPECT_EQ(mock_->rollback_calls_, 1);
    EXPECT_EQ(svc_->m_download_state, "idle");
    EXPECT_EQ(svc_->m_session_id, 0u);
}

/* W21: Second StartInstall while already in_progress is rejected
 * FR-SVC-060 placeholder */
TEST_F(ServiceTest, Workflow_W21_ConcurrentInstallRejected)
{
    prime_for_install();
    mock_->arm_install();

    {
        sd_bus_message* m = make_msg("StartInstall");
        sd_bus_message_append(m, "us", static_cast<uint32_t>(1), "fw");
        ASSERT_GE(call_start_install(m), 0);
    }
    EXPECT_EQ(svc_->m_install_state, "in_progress");

    {
        sd_bus_message* m = make_msg("StartInstall");
        sd_bus_message_append(m, "us", static_cast<uint32_t>(1), "fw");
        EXPECT_LT(call_start_install(m), 0);
    }

    EXPECT_EQ(svc_->m_install_state, "in_progress");   /* unchanged */

    mock_->release_install();
    if (svc_->m_install_thread.joinable())
        svc_->m_install_thread.join();
}

/* W22: Type mismatch between downloaded type and StartInstall type is rejected
 * FR-SVC-061 placeholder */
TEST_F(ServiceTest, Workflow_W22_StartInstall_TypeMismatch)
{
    sd_bus_message* dl = make_msg("StartDownload");
    sd_bus_message_append(dl, "sst", "fw", "2.0.0", static_cast<uint64_t>(1024));
    ASSERT_GE(call_start_download(dl), 0);
    uint32_t sid = svc_->m_session_id;

    {
        sd_bus_message* m = make_msg("FinishDownload");
        sd_bus_message_append(m, "ubs", sid, static_cast<int>(1), "/tmp/fw.raucb");
        ASSERT_GE(call_finish_download(m), 0);
    }

    /* Download was "fw"; try to start install as "app" */
    {
        sd_bus_message* m = make_msg("StartInstall");
        sd_bus_message_append(m, "us", sid, "app");
        EXPECT_LT(call_start_install(m), 0);
    }

    EXPECT_EQ(svc_->m_install_state, "idle");
}

/* W30: Rollback while install in_progress must return -EBUSY
 * FR-SVC-063 placeholder */
TEST_F(ServiceTest, Workflow_W30_RollbackDuringInstall_G2)
{
    prime_for_install();
    mock_->arm_install();

    {
        sd_bus_message* m = make_msg("StartInstall");
        sd_bus_message_append(m, "us", static_cast<uint32_t>(1), "fw");
        ASSERT_GE(call_start_install(m), 0);
    }
    ASSERT_EQ(svc_->m_install_state, "in_progress");

    EXPECT_LT(call_rollback(make_msg("Rollback")), 0);
    EXPECT_EQ(svc_->m_install_state, "in_progress");
    EXPECT_EQ(mock_->rollback_calls_, 0);

    mock_->release_install();
    if (svc_->m_install_thread.joinable())
        svc_->m_install_thread.join();
}

/* Every "operation already in flight" rejection uses -EBUSY
 * plus the distinct de.fsembedded.fsupdate1.Error.Busy name, so the client
 * can tell busy apart from bad arguments (-EINVAL) and a missing updater
 * (-ENOSYS). Pins all six sites — StartDownload, StartInstall, InstallLocal,
 * Apply, Rollback and Commit — to the same contract.
 *
 * State-park: set m_install_state to "in_progress" directly rather than
 * spawning a live worker. Every busy site is reached before any thread
 * spawns, so no arm/release/join scaffolding is needed and the state under
 * test is unambiguous. */
TEST_F(ServiceTest, BusyContract_AllSitesUnified)
{
    prime_for_install();
    svc_->m_install_state = "in_progress";

    /* Capture rc before reading name: they must not be sibling arguments to
     * expect_busy, whose evaluation order is unspecified (name is filled by
     * the call). */
    auto expect_busy = [](int r, const std::string& name, const char* leg) {
        EXPECT_EQ(r, -EBUSY) << leg;
        EXPECT_EQ(name, "de.fsembedded.fsupdate1.Error.Busy") << leg;
    };

    /* StartDownload during a running install is the only busy site that also
     * changes the returned errno (it was ungated before), so pin it too. */
    {
        sd_bus_message* m = make_msg("StartDownload");
        sd_bus_message_append(m, "sst", "fw", "1.0", static_cast<uint64_t>(1));
        std::string name;
        const int r = call_start_download(m, &name);
        expect_busy(r, name, "StartDownload");
    }
    {
        sd_bus_message* m = make_msg("StartInstall");
        sd_bus_message_append(m, "us", static_cast<uint32_t>(1), "fw");
        std::string name;
        const int r = call_start_install(m, &name);
        expect_busy(r, name, "StartInstall");
    }
    /* /dev/null passes the path/access gates so the check reaches busy. */
    {
        sd_bus_message* m = make_msg("InstallLocal");
        sd_bus_message_append(m, "s", "/dev/null");
        std::string name;
        const int r = call_install_local(m, &name);
        expect_busy(r, name, "InstallLocal");
    }
    {
        std::string name;
        const int r = call_apply(make_msg("Apply"), &name);
        expect_busy(r, name, "Apply");
    }
    {
        std::string name;
        const int r = call_rollback(make_msg("Rollback"), &name);
        expect_busy(r, name, "Rollback");
    }
    {
        sd_bus_message* m = make_msg("Commit");
        std::string name;
        const int r = call_commit(m, &name);
        expect_busy(r, name, "Commit");
    }

    /* No worker was ever spawned. */
    EXPECT_EQ(svc_->m_install_state, "in_progress");
}

/* S2: StartInstall on a terminal install_state (finished/failed) is a
 * permanent condition, not transient — it must report FAILED/-EINVAL, not the
 * busy contract, so a client does not retry-loop waiting for self-clearing. */
TEST_F(ServiceTest, StartInstall_TerminalStateRejectsPermanently)
{
    prime_for_install();

    for (const char* terminal : {"finished", "failed"}) {
        svc_->m_install_state = terminal;
        sd_bus_message* m = make_msg("StartInstall");
        sd_bus_message_append(m, "us", static_cast<uint32_t>(1), "fw");
        std::string name;
        const int r = call_start_install(m, &name);
        EXPECT_EQ(r, -EINVAL) << terminal;
        EXPECT_EQ(name, "org.freedesktop.DBus.Error.Failed") << terminal;
    }
}

/* S6: a missing updater backend reports the distinct Error.NoUpdater name (and
 * -ENOSYS), not a generic Failed — the errno alone cannot cross the bus, so
 * without the name a client cannot tell "no updater" apart from any failure.
 * Rebuild the service with a null updater and call directly (native path:
 * -ENOSYS survives because there is no wire round-trip). */
TEST_F(ServiceTest, NoUpdater_ReportsDistinctName)
{
    if (slot_) { sd_bus_slot_unref(slot_); slot_ = nullptr; }
    svc_ = std::make_unique<Service>(bus_, event_, nullptr);
    ASSERT_GE(sd_bus_add_object_vtable(bus_, &slot_,
                  "/de/fsembedded/fsupdate1", "de.fsembedded.fsupdate1",
                  Service::service_vtable, svc_.get()), 0);
    mock_ = nullptr;

    std::string name;
    const int r = call_apply(make_msg("Apply"), &name);
    EXPECT_EQ(r, -ENOSYS);
    EXPECT_EQ(name, "de.fsembedded.fsupdate1.Error.NoUpdater");
}

/* S1: a second StartDownload during a running install must be rejected (busy),
 * not mint a new session over the live worker. download_state reads "finished"
 * throughout the install phase, so only the install-state guard stops the
 * hijack; the original session must survive untouched. */
TEST_F(ServiceTest, StartDownload_RejectedDuringInstall)
{
    prime_for_install();
    mock_->arm_install();

    {
        sd_bus_message* m = make_msg("StartInstall");
        sd_bus_message_append(m, "us", static_cast<uint32_t>(1), "fw");
        ASSERT_GE(call_start_install(m), 0);
    }
    ASSERT_EQ(svc_->m_install_state, "in_progress");
    const uint32_t sid_before = svc_->m_session_id;

    {
        sd_bus_message* m = make_msg("StartDownload");
        sd_bus_message_append(m, "sst", "fw", "9.9", static_cast<uint64_t>(1));
        std::string name;
        const int r = call_start_download(m, &name);
        EXPECT_EQ(r, -EBUSY);
        EXPECT_EQ(name, "de.fsembedded.fsupdate1.Error.Busy");
    }
    EXPECT_EQ(svc_->m_session_id, sid_before);
    EXPECT_EQ(svc_->m_install_state, "in_progress");

    mock_->release_install();
    if (svc_->m_install_thread.joinable())
        svc_->m_install_thread.join();
}

/* W31: Rollback clears m_reboot_pending
 * FR-SVC-064 placeholder */
TEST_F(ServiceTest, Workflow_W31_RebootPendingClearedByRollback_G3)
{
    svc_->m_reboot_pending = true;

    ASSERT_GE(call_rollback(make_msg("Rollback")), 0);

    EXPECT_FALSE(svc_->m_reboot_pending);
    EXPECT_EQ(svc_->m_install_state, "idle");
    EXPECT_EQ(svc_->m_download_state, "idle");
    EXPECT_EQ(mock_->rollback_calls_, 1);
}

/* W32: Apply clears download_state and download_progress
 * FR-SVC-065 placeholder */
TEST_F(ServiceTest, Workflow_W32_DownloadStateClearedByApply_G4)
{
    prime_for_install();

    {
        sd_bus_message* m = make_msg("StartInstall");
        sd_bus_message_append(m, "us", static_cast<uint32_t>(1), "fw");
        ASSERT_GE(call_start_install(m), 0);
    }

    if (svc_->m_install_thread.joinable())
        svc_->m_install_thread.join();
    wait_until_quiescent();

    ASSERT_EQ(svc_->m_install_state, "finished");
    ASSERT_EQ(svc_->m_download_state, "finished");

    ASSERT_GE(call_apply(make_msg("Apply")), 0);

    EXPECT_EQ(svc_->m_download_state, "idle");
    EXPECT_EQ(svc_->m_download_progress, 0);
    EXPECT_EQ(svc_->m_install_state, "idle");
    EXPECT_EQ(svc_->m_session_id, 0u);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * InstallLocal — unified-install local entrypoint
 * ═══════════════════════════════════════════════════════════════════════════ */

/* RAII tempfile for the happy-path test (InstallLocal access()-checks
 * the bundle exists). Created via mkstemp under /tmp; unlinked on
 * destruction. */
class TempFile {
public:
    TempFile() {
        char tmpl[] = "/tmp/fsupd-test-XXXXXX";
        fd_ = ::mkstemp(tmpl);
        if (fd_ < 0) throw std::runtime_error("mkstemp failed");
        path_ = tmpl;
    }
    ~TempFile() {
        if (fd_ >= 0) ::close(fd_);
        if (!path_.empty()) ::unlink(path_.c_str());
    }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    const std::string& path() const { return path_; }
private:
    int         fd_ = -1;
    std::string path_;
};

TEST_F(ServiceTest, InstallLocal_RejectsEmptyPath)
{
    sd_bus_message* msg = make_msg("InstallLocal");
    sd_bus_message_append(msg, "s", "");
    int r = call_install_local(msg);

    EXPECT_LT(r, 0);
    EXPECT_EQ(svc_->m_install_state, "idle");
    EXPECT_EQ(svc_->m_session_id, 0u);
    EXPECT_FALSE(svc_->m_install_thread.joinable());
}

TEST_F(ServiceTest, InstallLocal_RejectsMissingPath)
{
    sd_bus_message* msg = make_msg("InstallLocal");
    sd_bus_message_append(msg, "s", "/nonexistent/path/to/bundle.fs");
    int r = call_install_local(msg);

    EXPECT_LT(r, 0);
    EXPECT_EQ(svc_->m_install_state, "idle");
    EXPECT_EQ(svc_->m_session_id, 0u);
}

TEST_F(ServiceTest, InstallLocal_RejectsConcurrentInstall)
{
    /* Park the service in InstallState=in_progress without actually
     * spawning a worker (tests for state-machine gates, not threading). */
    svc_->m_install_state = "in_progress";

    TempFile bundle;
    sd_bus_message* msg = make_msg("InstallLocal");
    sd_bus_message_append(msg, "s", bundle.path().c_str());
    int r = call_install_local(msg);

    EXPECT_LT(r, 0);
    EXPECT_EQ(svc_->m_install_state, "in_progress"); /* untouched */
}

TEST_F(ServiceTest, InstallLocal_RejectsConcurrentDownload)
{
    svc_->m_download_state = "in_progress";

    TempFile bundle;
    sd_bus_message* msg = make_msg("InstallLocal");
    sd_bus_message_append(msg, "s", bundle.path().c_str());
    int r = call_install_local(msg);

    EXPECT_LT(r, 0);
    EXPECT_EQ(svc_->m_install_state, "idle");
    EXPECT_EQ(svc_->m_download_state, "in_progress"); /* untouched */
}

TEST_F(ServiceTest, InstallLocal_MintsSessionAndStartsWorker)
{
    TempFile bundle;

    /* Block the install worker until we've inspected the state, so
     * we observe the just-started condition rather than a completed run. */
    mock_->arm_install();

    sd_bus_message* msg = make_msg("InstallLocal");
    sd_bus_message_append(msg, "s", bundle.path().c_str());
    int r = call_install_local(msg);
    EXPECT_GE(r, 0);

    EXPECT_NE(svc_->m_session_id, 0u);
    EXPECT_EQ(svc_->m_session_id, svc_->m_session_counter);
    EXPECT_EQ(svc_->m_payload_path,     bundle.path());
    EXPECT_EQ(svc_->m_download_state,   "finished");
    EXPECT_EQ(svc_->m_download_progress, 100);
    EXPECT_EQ(svc_->m_install_state,    "in_progress");
    EXPECT_TRUE(svc_->m_update_available);
    EXPECT_TRUE(svc_->m_install_thread.joinable());

    /* Unblock the worker so TearDown can join cleanly. */
    mock_->release_install();
}

TEST_F(ServiceTest, InstallLocal_WorkerSuccessReachesFinished)
{
    TempFile bundle;

    sd_bus_message* msg = make_msg("InstallLocal");
    sd_bus_message_append(msg, "s", bundle.path().c_str());
    ASSERT_GE(call_install_local(msg), 0);

    if (svc_->m_install_thread.joinable())
        svc_->m_install_thread.join();
    wait_until_quiescent();

    EXPECT_EQ(svc_->m_install_state, "finished");
    EXPECT_EQ(svc_->m_install_progress, 100);
    EXPECT_EQ(mock_->update_calls_, 1);
    EXPECT_EQ(mock_->last_update_path_, bundle.path());
    /* default inspect_result_ is empty, so update_type is "" */
    EXPECT_EQ(mock_->last_update_type_, "");
}

/* IUpdater::inspect_bundle result is reflected into UpdateType /
 * UpdateVersion / UpdateSize before the worker spawns, and the
 * discovered type flows into update_image() so the lib gets a hint. */
TEST_F(ServiceTest, InstallLocal_PopulatesStateFromInspectBundle)
{
    TempFile bundle;
    mock_->inspect_result_ = {true, "fw+app", "9.8.7", 12345};
    mock_->arm_install();   /* freeze worker so we observe pre-finish state */

    sd_bus_message* msg = make_msg("InstallLocal");
    sd_bus_message_append(msg, "s", bundle.path().c_str());
    ASSERT_GE(call_install_local(msg), 0);

    EXPECT_EQ(mock_->inspect_calls_, 1);
    EXPECT_EQ(mock_->last_inspect_path_, bundle.path());
    EXPECT_EQ(svc_->m_update_type,    "fw+app");
    EXPECT_EQ(svc_->m_update_version, "9.8.7");
    EXPECT_EQ(svc_->m_update_size,    12345u);
    EXPECT_TRUE(svc_->m_update_available);

    mock_->release_install();
    if (svc_->m_install_thread.joinable())
        svc_->m_install_thread.join();
    wait_until_quiescent();

    /* The discovered type was passed through to update_image. */
    EXPECT_EQ(mock_->last_update_type_, "fw+app");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * CancelInstall — best-effort install cancellation
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_F(ServiceTest, CancelInstall_RejectsReservedSessionZero)
{
    sd_bus_message* msg = make_msg("CancelInstall");
    sd_bus_message_append(msg, "u", static_cast<uint32_t>(0));
    int r = call_cancel_install(msg);

    EXPECT_LT(r, 0);
    EXPECT_FALSE(svc_->m_cancel_requested.load(std::memory_order_acquire));
}

TEST_F(ServiceTest, CancelInstall_RejectsWrongSession)
{
    svc_->m_session_id   = 1;
    svc_->m_install_state = "in_progress";

    sd_bus_message* msg = make_msg("CancelInstall");
    sd_bus_message_append(msg, "u", static_cast<uint32_t>(99));
    int r = call_cancel_install(msg);

    EXPECT_LT(r, 0);
    EXPECT_FALSE(svc_->m_cancel_requested.load(std::memory_order_acquire));
}

TEST_F(ServiceTest, CancelInstall_RejectsWhenIdle)
{
    svc_->m_session_id   = 1;
    svc_->m_install_state = "idle";

    sd_bus_message* msg = make_msg("CancelInstall");
    sd_bus_message_append(msg, "u", static_cast<uint32_t>(1));
    int r = call_cancel_install(msg);

    EXPECT_LT(r, 0);
    EXPECT_FALSE(svc_->m_cancel_requested.load(std::memory_order_acquire));
}

TEST_F(ServiceTest, CancelInstall_AcceptsAndSetsFlagDuringInstall)
{
    TempFile bundle;
    mock_->arm_install();   /* block worker so install_state stays in_progress */

    sd_bus_message* start = make_msg("InstallLocal");
    sd_bus_message_append(start, "s", bundle.path().c_str());
    ASSERT_GE(call_install_local(start), 0);
    const uint32_t sid = svc_->m_session_id;

    sd_bus_message* cancel = make_msg("CancelInstall");
    sd_bus_message_append(cancel, "u", sid);
    int r = call_cancel_install(cancel);

    EXPECT_GE(r, 0);
    EXPECT_TRUE(svc_->m_cancel_requested.load(std::memory_order_acquire));

    mock_->release_install();
}

/* End-to-end: cancel fires while the worker is blocked; after the
 * worker completes (mock returns success), on_wakeup overrides the
 * outcome to failed/cancelled per the best-effort contract. */
TEST_F(ServiceTest, CancelInstall_OverridesCompletionToFailed)
{
    TempFile bundle;
    mock_->arm_install();

    sd_bus_message* start = make_msg("InstallLocal");
    sd_bus_message_append(start, "s", bundle.path().c_str());
    ASSERT_GE(call_install_local(start), 0);
    const uint32_t sid = svc_->m_session_id;

    sd_bus_message* cancel = make_msg("CancelInstall");
    sd_bus_message_append(cancel, "u", sid);
    ASSERT_GE(call_cancel_install(cancel), 0);

    mock_->release_install();
    if (svc_->m_install_thread.joinable())
        svc_->m_install_thread.join();
    wait_until_quiescent();

    EXPECT_EQ(svc_->m_install_state, "failed");
    EXPECT_EQ(mock_->update_calls_, 1);   /* worker did run the lib */
}

/* The cancel flag is reset when a fresh install spawns; otherwise a
 * stale cancel from a previous session would taint the next one. */
TEST_F(ServiceTest, CancelInstall_FlagClearedByNextInstall)
{
    svc_->m_cancel_requested.store(true, std::memory_order_release);

    TempFile bundle;
    sd_bus_message* msg = make_msg("InstallLocal");
    sd_bus_message_append(msg, "s", bundle.path().c_str());
    ASSERT_GE(call_install_local(msg), 0);

    EXPECT_FALSE(svc_->m_cancel_requested.load(std::memory_order_acquire));

    if (svc_->m_install_thread.joinable())
        svc_->m_install_thread.join();
    wait_until_quiescent();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Worker-tuning knobs — FSUPDATE_PROGRESS_MS time-throttled callback
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Rebuild svc_/mock_ with a custom FSUPDATE_PROGRESS_MS so the
 * env knob is read by the new ctor. TearDown handles cleanup of
 * the replaced unique_ptr<Service>. The bus registration must move
 * with it: dropping and re-adding the vtable slot keeps the getters'
 * userdata pointing at the live Service (stale userdata is a
 * use-after-free the property emits would hit). */
static void rebuild_with_throttle(std::unique_ptr<Service>& svc,
                                  MockUpdater*&             mock_raw,
                                  sd_bus*                   bus,
                                  sd_event*                 event,
                                  sd_bus_slot*&             slot,
                                  const char*               throttle_ms)
{
    ::setenv("FSUPDATE_PROGRESS_MS", throttle_ms, /*overwrite=*/1);
    if (slot != nullptr) {
        sd_bus_slot_unref(slot);
        slot = nullptr;
    }
    svc.reset();   /* joins prior install_thread if any */
    auto mock = std::make_unique<MockUpdater>();
    mock_raw = mock.get();
    svc = std::make_unique<Service>(bus, event, std::move(mock));
    mock_raw->check_calls_ = 0;
    int r = sd_bus_add_object_vtable(bus, &slot,
        "/de/fsembedded/fsupdate1",
        "de.fsembedded.fsupdate1",
        Service::service_vtable, svc.get());
    ASSERT_GE(r, 0) << "re-registering vtable failed: " << r;
    ::unsetenv("FSUPDATE_PROGRESS_MS");
}

TEST_F(ServiceTest, ProgressThrottle_Disabled_EmitsEveryChange)
{
    rebuild_with_throttle(svc_, mock_, bus_, event_, slot_, "0");

    TempFile bundle;
    mock_->arm_install();

    sd_bus_message* msg = make_msg("InstallLocal");
    sd_bus_message_append(msg, "s", bundle.path().c_str());
    ASSERT_GE(call_install_local(msg), 0);

    ASSERT_TRUE(mock_->progress_cb_);
    /* Fire several distinct percents rapidly; with throttle=0 each
     * should reach the property. */
    mock_->progress_cb_(10);
    wait_until_quiescent();
    EXPECT_EQ(svc_->m_install_progress, 10);

    mock_->progress_cb_(20);
    wait_until_quiescent();
    EXPECT_EQ(svc_->m_install_progress, 20);

    mock_->progress_cb_(30);
    wait_until_quiescent();
    EXPECT_EQ(svc_->m_install_progress, 30);

    mock_->release_install();
    if (svc_->m_install_thread.joinable())
        svc_->m_install_thread.join();
}

TEST_F(ServiceTest, ProgressThrottle_Active_CoalescesRapidEvents)
{
    /* 10 seconds — easily larger than the test duration so any
     * subsequent ticks are suppressed unless they are 100. */
    rebuild_with_throttle(svc_, mock_, bus_, event_, slot_, "10000");

    TempFile bundle;
    mock_->arm_install();

    sd_bus_message* msg = make_msg("InstallLocal");
    sd_bus_message_append(msg, "s", bundle.path().c_str());
    ASSERT_GE(call_install_local(msg), 0);

    ASSERT_TRUE(mock_->progress_cb_);
    mock_->progress_cb_(10);   /* first → always passes */
    wait_until_quiescent();
    EXPECT_EQ(svc_->m_install_progress, 10);

    mock_->progress_cb_(20);   /* throttled */
    mock_->progress_cb_(30);   /* throttled */
    wait_until_quiescent();
    EXPECT_EQ(svc_->m_install_progress, 10);

    /* 100 always passes regardless of throttle so the bar reaches end. */
    mock_->progress_cb_(100);
    wait_until_quiescent();
    EXPECT_EQ(svc_->m_install_progress, 100);

    mock_->release_install();
    if (svc_->m_install_thread.joinable())
        svc_->m_install_thread.join();
}

/* ── Commit method ───────────────────────────────────────────────────────── */

/* Commit routes to the updater's commit_update() */
TEST_F(ServiceTest, Commit_CallsUpdaterCommit)
{
    sd_bus_message* msg = make_msg("Commit");
    call_commit(msg);

    EXPECT_EQ(mock_->commit_calls_, 1);
}

/* Commit is rejected while an install is in progress, without committing */
TEST_F(ServiceTest, Commit_RejectedWhileInstallInProgress)
{
    svc_->m_install_state = "in_progress";

    sd_bus_message* msg = make_msg("Commit");
    const int r = call_commit(msg);

    EXPECT_LT(r, 0);
    EXPECT_EQ(mock_->commit_calls_, 0);
}

/* A successful Commit clears the pending-update session state (so GetState /
 * UpdateAvailable no longer report a phantom pending update), like Rollback. */
TEST_F(ServiceTest, Commit_ClearsStateAfterSuccess)
{
    svc_->m_session_id       = 1;
    svc_->m_update_type      = "fw";
    svc_->m_update_version   = "2.0";
    svc_->m_update_size      = 2048;
    svc_->m_update_available = true;
    svc_->m_install_state    = "finished";
    svc_->m_reboot_pending   = true;

    sd_bus_message* msg = make_msg("Commit");
    call_commit(msg);

    EXPECT_EQ(mock_->commit_calls_, 1);
    EXPECT_EQ(svc_->m_session_id, 0u);
    EXPECT_EQ(svc_->m_install_state, "idle");
    EXPECT_FALSE(svc_->m_reboot_pending);
    EXPECT_FALSE(svc_->m_update_available);
    EXPECT_EQ(svc_->m_update_type, "");
    EXPECT_EQ(svc_->m_update_version, "");
    EXPECT_EQ(svc_->m_update_size, 0u);
}
