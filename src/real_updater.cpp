#include "real_updater.h"

#include <fs_update_framework/handle_update/updateDefinitions.h>
#include <fs_update_framework/logger/LoggerSinkStdout.h>

namespace {

/* Run a library call and re-raise whatever it throws in the interface's
 * vocabulary, so every verb answers a caller the same way instead of only the
 * install path doing so.
 *
 * The category is the library's own -- append-only, ABI-stable, covered by its
 * tests, and RTTI-free because it dispatches by re-raising inside a catch
 * block rather than asking typeid (dynamic_cast is banned here). Writing a
 * second ladder is what this avoids; one already drifted in two tokens before
 * it was replaced by this.
 *
 * An UpdaterError passes through untouched: it is already translated, and
 * classifying it again would report every such failure as "system".
 */
/* The classification itself does not depend on the wrapped call's type, so it
 * lives in a plain function: the template below is instantiated once per call
 * site, and only the try/catch shell is duplicated with it. The coding
 * standard asks for exactly this split -- factor non-type-dependent logic into
 * non-template functions -- because template instantiations are binary size on
 * a device that has to fit. */
[[noreturn]] void rethrow_as_updater_error(const std::exception& e)
{
    const fs::ErrorInfo info = fs::classify_active_exception();
    throw UpdaterError(e.what(), info.errno_val, std::string(fs::describe(info.code)));
}

template <typename F>
auto translated(F&& fn) -> decltype(fn())
{
    try {
        return fn();
    } catch (const UpdaterError&) {
        throw;
    } catch (const std::exception& e) {
        rethrow_as_updater_error(e);
    }
}

/* Shared by check_update_available() and rollback_candidate_type(): both
 * suppress an incomplete-update answer only on a definite false from this
 * evidence check, and both must fail open (true) on a caught exception --
 * evidence the device cannot read must not make a real pending update or
 * rollback disappear. */
bool pending_update_actionable_or_unknown(fs::FSUpdate& fsu)
{
    try {
        return fsu.pending_update_actionable();
    } catch (const std::exception&) {
        return true;
    }
}

} // namespace

RealUpdater::RealUpdater()
{
    auto sink = std::make_shared<logger::LoggerSinkStdout>(logger::logLevel::WARNING);
    m_logger  = logger::LoggerHandler::initLogger(sink);
    m_fsu     = std::make_unique<fs::FSUpdate>(m_logger);
}

CheckUpdateInfo RealUpdater::check_update_available()
{
    using Flags = update_definitions::UBootBootstateFlags;

    return translated([&]() -> CheckUpdateInfo {
        Flags state = m_fsu->get_update_reboot_state();

        if (state == Flags::INCOMPLETE_FW_UPDATE ||
            state == Flags::INCOMPLETE_APP_UPDATE ||
            state == Flags::INCOMPLETE_APP_FW_UPDATE)
        {
            /* The raw flag alone would offer an install that never reached the
             * boot order as one the caller can commit. The library separates
             * the two from evidence; a state it rules out is not available. */
            if (!pending_update_actionable_or_unknown(*m_fsu)) {
                return CheckUpdateInfo{false, "", "", 0};
            }

            std::string type;
            std::string version;

            if (state == Flags::INCOMPLETE_FW_UPDATE) {
                type = "fw";
            } else if (state == Flags::INCOMPLETE_APP_UPDATE) {
                type = "app";
            } else {
                type = "fw+app";
            }

            /* The version is decoration on the answer, not the answer: a
             * version file the device cannot read must not make the pending
             * update itself disappear. Empty is the contract's "no version
             * to read". */
            try {
                version = (state == Flags::INCOMPLETE_APP_UPDATE)
                              ? m_fsu->get_application_version()
                              : m_fsu->get_firmware_version();
            } catch (const std::exception&) {
                version.clear();
            }

            return CheckUpdateInfo{true, std::move(type), std::move(version), 0};
        }

        return CheckUpdateInfo{false, "", "", 0};
    });
}


bool RealUpdater::reboot_pending()
{
    using Flags = update_definitions::UBootBootstateFlags;

    /* No default arm, deliberately: every enumerator is spelled so that a
     * future value trips -Wswitch here instead of silently reading as "no
     * reboot owed" in exactly the function meant to prevent that class of
     * divergence. */
    return translated([&]() -> bool {
        const Flags state = m_fsu->get_update_reboot_state();
        switch (state) {
        case Flags::INCOMPLETE_FW_UPDATE:
        case Flags::INCOMPLETE_APP_UPDATE:
        case Flags::INCOMPLETE_APP_FW_UPDATE:
            /* The raw flag cannot separate "installed, reboot outstanding"
             * from "rebooted, commit outstanding": both persist the same
             * value. The library separates them from evidence (booted slot
             * against boot order); use its classification -- the same one
             * the status verb reports from -- rather than a second
             * translation of the raw value. Indeterminate or unreadable
             * evidence reads false: this property affirms an owed reboot
             * only on evidence, and the status verb reports the
             * indeterminate shape distinctly. */
            try {
                return m_fsu->is_reboot_complete(state != Flags::INCOMPLETE_APP_UPDATE)
                       == fs::RebootCompleteState::PENDING;
            } catch (const std::exception&) {
                return false;
            }
        case Flags::ROLLBACK_FW_REBOOT_PENDING:
        case Flags::ROLLBACK_APP_FW_REBOOT_PENDING:
            /* pendingUpdateRollback() true means the rollback already
             * landed and waits on its commit -- no reboot owed then. */
            try {
                return !m_fsu->pendingUpdateRollback();
            } catch (const std::exception&) {
                return false;
            }
        case Flags::ROLLBACK_APP_REBOOT_PENDING:
            try {
                return m_fsu->classify_app_rollback()
                       == updater::Bootstate::AppRollbackOutcome::REBOOT_OUTSTANDING;
            } catch (const std::exception&) {
                return false;
            }
        case Flags::NO_UPDATE_REBOOT_PENDING:
        case Flags::FW_UPDATE_REBOOT_FAILED:
        case Flags::FAILED_FW_UPDATE:
        case Flags::FAILED_APP_UPDATE:
        case Flags::INCOMPLETE_FW_ROLLBACK:
        case Flags::INCOMPLETE_APP_ROLLBACK:
        case Flags::INCOMPLETE_APP_FW_ROLLBACK:
        case Flags::UNKNOWN_STATE:
            /* These wait on a commit, or hold nothing. */
            return false;
        }
        return false;
    });
}

std::string RealUpdater::rollback_candidate_type()
{
    using Flags = update_definitions::UBootBootstateFlags;

    /* All enumerators spelled, no default -- same reasoning as in
     * reboot_pending(). The taken-rollback states still name their
     * dimension: the library answers a rollback there with its own
     * "commit required" rejection, which is the honest reply. The two
     * incomplete-update states are split out from their neighbours: a
     * firmware digit still in flight for a slot BOOT_ORDER does not
     * prefer never reached the boot order, so pending_update_actionable()
     * -- the same evidence check check_update_available() already uses --
     * answers "" (nothing to roll back) instead of naming a dimension that
     * was never staged. Unreadable evidence must not make a real pending
     * rollback disappear, so a caught exception falls through unchanged. */
    return translated([&]() -> std::string {
        switch (m_fsu->get_update_reboot_state()) {
        case Flags::INCOMPLETE_FW_UPDATE:
            if (!pending_update_actionable_or_unknown(*m_fsu)) {
                return "";
            }
            return "fw";
        case Flags::FAILED_FW_UPDATE:
        case Flags::FW_UPDATE_REBOOT_FAILED:
        case Flags::ROLLBACK_FW_REBOOT_PENDING:
        case Flags::INCOMPLETE_FW_ROLLBACK:
            return "fw";
        case Flags::INCOMPLETE_APP_UPDATE:
        case Flags::FAILED_APP_UPDATE:
        case Flags::ROLLBACK_APP_REBOOT_PENDING:
        case Flags::INCOMPLETE_APP_ROLLBACK:
            return "app";
        case Flags::INCOMPLETE_APP_FW_UPDATE:
            if (!pending_update_actionable_or_unknown(*m_fsu)) {
                return "";
            }
            return "fw+app";
        case Flags::ROLLBACK_APP_FW_REBOOT_PENDING:
        case Flags::INCOMPLETE_APP_FW_ROLLBACK:
            return "fw+app";
        case Flags::NO_UPDATE_REBOOT_PENDING:
        case Flags::UNKNOWN_STATE:
            return "";
        }
        return "";
    });
}

CheckUpdateInfo RealUpdater::inspect_bundle(const std::string& path)
{
    return translated([&] {
        fs::BundleInfo info = fs::inspect_bundle(path);
        return CheckUpdateInfo{info.valid, info.update_type, info.version, info.size};
    });
}

void RealUpdater::update_image(std::string type, std::string path)
{
    uint8_t installed_update_type = 0;
    /* The lib's v2.0 extract path is gated on type.empty() — passing the
     * service-side discovered type ("fw" / "app" / "fw+app", populated
     * by inspect_bundle) skips the extract loop and falls through to
     * "Invalid update". The descriptor carries authoritative member
     * types; let the lib re-derive them. */
    (void)type;
    std::string detect{};
    translated([&] { m_fsu->update_image(path, detect, installed_update_type); });
}

bool RealUpdater::apply_pending_update(const std::string& /*type*/)
{
    /* The lib reads update_reboot_state from U-Boot env (authoritative
     * source); the service-side type mirror is informational only. */
    return translated([&] { return m_fsu->apply_pending_update(); });
}

void RealUpdater::rollback(std::string type)
{
    translated([&] {
        if (type == "fw") {
            m_fsu->rollback_firmware();
        } else if (type == "app") {
            m_fsu->rollback_application();
        } else if (type == "fw+app") {
            /* One call, not two: for a pending fw+app update the library's
             * firmware rollback completes the application half itself and
             * records the combined rollback state. A second application
             * rollback here would act again on the state the first call
             * just wrote. */
            m_fsu->rollback_firmware();
        }
    });
}

bool RealUpdater::commit_update()
{
    /* The lib transitions the pending update / rollback to IDLE using the
     * authoritative U-Boot update_reboot_state; returns whether a commit
     * was performed. */
    return translated([&] { return m_fsu->commit_update(); });
}

void RealUpdater::setProgressCallback(std::function<void(int)> cb)
{
    m_fsu->setInstallProgressCallback(std::move(cb));
}
