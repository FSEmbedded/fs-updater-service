#pragma once

#include "service.h"

#include <fs_update_framework/handle_update/fsupdate.h>
#include <fs_update_framework/handle_update/error_mapping.h>
#include <fs_update_framework/handle_update/inspect_bundle.h>
#include <fs_update_framework/logger/LoggerHandler.h>

#include <memory>

class RealUpdater : public IUpdater {
public:
    RealUpdater();
    ~RealUpdater() override = default;

    CheckUpdateInfo check_update_available() override;
    CheckUpdateInfo inspect_bundle(const std::string& path) override;
    void            update_image(std::string type, std::string path) override;
    bool            apply_pending_update(const std::string& type) override;
    void            rollback(std::string type) override;
    bool            commit_update() override;
    void            setProgressCallback(std::function<void(int)> cb) override;

private:
    std::shared_ptr<logger::LoggerHandler> m_logger;
    std::unique_ptr<fs::FSUpdate>          m_fsu;
};
