#include "real_updater.h"

#include <fs_update_framework/handle_update/updateDefinitions.h>
#include <fs_update_framework/logger/LoggerSinkStdout.h>

RealUpdater::RealUpdater()
{
    auto sink = std::make_shared<logger::LoggerSinkStdout>(logger::logLevel::WARNING);
    m_logger  = logger::LoggerHandler::initLogger(sink);
    m_fsu     = std::make_unique<fs::FSUpdate>(m_logger);
}

CheckUpdateInfo RealUpdater::check_update_available()
{
    using Flags = update_definitions::UBootBootstateFlags;

    Flags state = m_fsu->get_update_reboot_state();

    if (state == Flags::INCOMPLETE_FW_UPDATE ||
        state == Flags::INCOMPLETE_APP_UPDATE ||
        state == Flags::INCOMPLETE_APP_FW_UPDATE)
    {
        std::string type;
        std::string version;

        if (state == Flags::INCOMPLETE_FW_UPDATE) {
            type    = "fw";
            version = m_fsu->get_firmware_version();
        } else if (state == Flags::INCOMPLETE_APP_UPDATE) {
            type    = "app";
            version = m_fsu->get_application_version();
        } else {
            type    = "fw+app";
            version = m_fsu->get_firmware_version();
        }

        return CheckUpdateInfo{true, std::move(type), std::move(version), 0};
    }

    return CheckUpdateInfo{false, "", "", 0};
}

CheckUpdateInfo RealUpdater::inspect_bundle(const std::string& path)
{
    fs::BundleInfo info = fs::inspect_bundle(path);
    return CheckUpdateInfo{info.valid, info.update_type, info.version, info.size};
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
    m_fsu->update_image(path, detect, installed_update_type);
}

bool RealUpdater::apply_pending_update(const std::string& /*type*/)
{
    /* The lib reads update_reboot_state from U-Boot env (authoritative
     * source); the service-side type mirror is informational only. */
    return m_fsu->apply_pending_update();
}

void RealUpdater::rollback(std::string type)
{
    if (type == "fw") {
        m_fsu->rollback_firmware();
    } else if (type == "app") {
        m_fsu->rollback_application();
    } else if (type == "fw+app") {
        m_fsu->rollback_firmware();
        m_fsu->rollback_application();
    }
}

bool RealUpdater::commit_update()
{
    /* The lib transitions the pending update / rollback to IDLE using the
     * authoritative U-Boot update_reboot_state; returns whether a commit
     * was performed. */
    return m_fsu->commit_update();
}

void RealUpdater::setProgressCallback(std::function<void(int)> cb)
{
    m_fsu->setInstallProgressCallback(std::move(cb));
}
