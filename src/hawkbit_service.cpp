#include "hawkbit_service.h"

#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <sys/wait.h>
#include <spawn.h>
#include <signal.h>
#include <unistd.h>

#include <systemd/sd-journal.h>
#include <syslog.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

extern char** environ;

namespace {

static constexpr const char* INSTALLER_INTERFACE = "de.fsembedded.fsupdate1.Installer";
static constexpr const char* ERROR_NOT_IMPLEMENTED = "org.freedesktop.DBus.Error.NotSupported";

int return_not_implemented(sd_bus_error* ret_err)
{
    sd_bus_error_set(ret_err, ERROR_NOT_IMPLEMENTED, "Not implemented");
    return -EOPNOTSUPP;
}

int read_env_int_or(const char* name, int fallback, int minimum, int maximum)
{
    const char* raw = std::getenv(name);
    if (raw == nullptr || raw[0] == '\0')
        return fallback;

    char* end = nullptr;
    const long value = std::strtol(raw, &end, 10);
    if (end == raw || *end != '\0' || value < minimum || value > maximum)
        return fallback;
    return static_cast<int>(value);
}

} // namespace

HawkbitService::HawkbitService(sd_bus* bus, sd_event* event)
    : m_bus(bus)
    , m_event(event)
{
    m_install_timeout_sec = read_env_int_or("FSUPDATE_INSTALL_TIMEOUT_SEC", 1800,
                                             /*minimum=*/0, /*maximum=*/86400);

    m_completion_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (m_completion_fd < 0) {
        sd_journal_print(LOG_ERR, "fs-updater-service: installer eventfd: %s\n",
                         strerror(errno));
        return;
    }

    const int r = sd_event_add_io(m_event, &m_completion_src, m_completion_fd,
                                  EPOLLIN, &HawkbitService::on_completion, this);
    if (r < 0) {
        sd_journal_print(LOG_ERR, "fs-updater-service: installer event source: %s\n",
                         strerror(-r));
    }
}

HawkbitService::~HawkbitService()
{
    if (m_child_pid > 0)
        ::kill(m_child_pid, SIGTERM);
    if (m_wait_thread.joinable())
        m_wait_thread.join();
    if (m_completion_src != nullptr)
        sd_event_source_unref(m_completion_src);
    if (m_completion_fd >= 0)
        ::close(m_completion_fd);
}

int HawkbitService::read_bundle_options(sd_bus_message* msg, BundleOptions& options)
{
    int r = sd_bus_message_enter_container(msg, SD_BUS_TYPE_ARRAY, "{sv}");
    if (r < 0)
        return r;

    while ((r = sd_bus_message_enter_container(msg, SD_BUS_TYPE_DICT_ENTRY, "sv")) > 0) {
        const char* key = nullptr;
        r = sd_bus_message_read(msg, "s", &key);
        if (r < 0)
            return r;

        const char* variant_type = nullptr;
        if (std::strcmp(key, "http-headers") == 0)
            variant_type = "as";
        else if (std::strcmp(key, "tls-no-verify") == 0)
            variant_type = "b";
        else if (std::strcmp(key, "tls-key") == 0 || std::strcmp(key, "tls-cert") == 0)
            variant_type = "s";

        if (variant_type == nullptr) {
            r = sd_bus_message_skip(msg, "v");
        } else {
            r = sd_bus_message_enter_container(msg, SD_BUS_TYPE_VARIANT, variant_type);
            if (r >= 0 && std::strcmp(key, "http-headers") == 0) {
                r = sd_bus_message_enter_container(msg, SD_BUS_TYPE_ARRAY, "s");
                if (r >= 0) {
                    const char* header = nullptr;
                    while ((r = sd_bus_message_read(msg, "s", &header)) > 0)
                        options.http_headers.emplace_back(header);
                    if (r >= 0)
                        r = sd_bus_message_exit_container(msg);
                }
            } else if (r >= 0 && std::strcmp(key, "tls-no-verify") == 0) {
                int value = 0;
                r = sd_bus_message_read(msg, "b", &value);
                if (r > 0)
                    options.tls_no_verify = value != 0;
            } else if (r >= 0) {
                const char* value = nullptr;
                r = sd_bus_message_read(msg, "s", &value);
                if (r > 0) {
                    if (std::strcmp(key, "tls-key") == 0)
                        options.tls_key = value;
                    else
                        options.tls_cert = value;
                }
            }
            if (r >= 0)
                r = sd_bus_message_exit_container(msg);
        }
        if (r < 0)
            return r;

        r = sd_bus_message_exit_container(msg);
        if (r < 0)
            return r;
    }
    if (r < 0)
        return r;
    return sd_bus_message_exit_container(msg);
}

int HawkbitService::install_bundle(const std::string& source, const BundleOptions& options,
                                   const std::string& object_path)
{
    if (m_install_running)
        return -EBUSY;
    if (m_completion_fd < 0 || m_completion_src == nullptr)
        return -EIO;

    std::string source_copy(source);
    char executable[] = "fs-updater";
    char install_option[] = "--install_update";
    char* argv[] = {executable, install_option, source_copy.data(), nullptr};
    pid_t child_pid = -1;
    const int spawn_result = ::posix_spawn(&child_pid, "/usr/sbin/fs-updater",
                                            nullptr, nullptr, argv, environ);
    if (spawn_result != 0)
        return -spawn_result;

    m_child_pid = child_pid;
    m_install_running = true;
    m_object_path = object_path;
    if (m_wait_thread.joinable())
        m_wait_thread.join();

    try {
        HawkbitService* service = this;
        const int timeout_sec = m_install_timeout_sec;
        m_wait_thread = std::thread([service, child_pid, timeout_sec]() {
            int wait_status = 0;
            pid_t waited_pid = 0;
            bool timed_out = false;
            int wait_error = 0;
            const auto deadline = std::chrono::steady_clock::now() +
                std::chrono::seconds(timeout_sec);

            while (true) {
                waited_pid = ::waitpid(child_pid, &wait_status, WNOHANG);
                if (waited_pid == child_pid)
                    break;
                if (waited_pid < 0) {
                    if (errno == EINTR)
                        continue;
                    wait_error = errno;
                    break;
                }

                if (timeout_sec > 0 && std::chrono::steady_clock::now() >= deadline) {
                    timed_out = true;
                    ::kill(child_pid, SIGTERM);
                    const auto terminate_deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(5);
                    while (std::chrono::steady_clock::now() < terminate_deadline) {
                        waited_pid = ::waitpid(child_pid, &wait_status, WNOHANG);
                        if (waited_pid == child_pid)
                            break;
                        if (waited_pid < 0) {
                            if (errno == EINTR)
                                continue;
                            wait_error = errno;
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }
                    if (waited_pid == 0) {
                        ::kill(child_pid, SIGKILL);
                        do {
                            waited_pid = ::waitpid(child_pid, &wait_status, 0);
                        } while (waited_pid < 0 && errno == EINTR);
                        if (waited_pid < 0)
                            wait_error = errno;
                    }
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            int result = timed_out ? 124 : 1;
            if (wait_error != 0) {
                sd_journal_print(LOG_ERR, "fs-updater-service: waitpid(fs-updater): %s\n",
                                 strerror(wait_error));
            } else if (!timed_out && waited_pid == child_pid) {
                if (WIFEXITED(wait_status))
                    result = WEXITSTATUS(wait_status);
                else if (WIFSIGNALED(wait_status))
                    result = 128 + WTERMSIG(wait_status);
            }
            if (timed_out) {
                sd_journal_print(LOG_ERR,
                    "fs-updater-service: fs-updater timed out after %d seconds\n",
                    timeout_sec);
            }

            {
                std::lock_guard<std::mutex> lock(service->m_completion_mutex);
                service->m_completion_result = result;
                service->m_completion_ready = true;
            }
            const uint64_t one = 1U;
            [[maybe_unused]] const ssize_t written =
                ::write(service->m_completion_fd, &one, sizeof(one));
        });
    } catch (const std::exception& e) {
        ::kill(child_pid, SIGTERM);
        int status = 0;
        while (::waitpid(child_pid, &status, 0) < 0 && errno == EINTR) {}
        m_child_pid = -1;
        m_install_running = false;
        m_object_path.clear();
        sd_journal_print(LOG_ERR,
            "fs-updater-service: failed to monitor fs-updater process: %s\n", e.what());
        return -EAGAIN;
    }

    (void)options;
    return 0;
}

int HawkbitService::on_completion(sd_event_source* /*src*/, int fd,
                                  uint32_t /*revents*/, void* userdata)
{
    uint64_t value = 0;
    [[maybe_unused]] const ssize_t bytes = ::read(fd, &value, sizeof(value));
    auto* service = static_cast<HawkbitService*>(userdata);

    int result = 1;
    {
        std::lock_guard<std::mutex> lock(service->m_completion_mutex);
        if (!service->m_completion_ready)
            return 0;
        result = service->m_completion_result;
        service->m_completion_ready = false;
    }

    if (service->m_wait_thread.joinable())
        service->m_wait_thread.join();

    service->m_install_running = false;
    service->m_child_pid = -1;
    const int r = sd_bus_emit_signal(service->m_bus, service->m_object_path.c_str(),
        INSTALLER_INTERFACE, "Completed", "i", result);
    if (r < 0)
        sd_journal_print(LOG_ERR, "fs-updater-service: emit Installer.Completed: %s\n",
                         strerror(-r));
    service->m_object_path.clear();
    return 0;
}

int HawkbitService::method_install(sd_bus_message* /*msg*/, void* /*userdata*/, sd_bus_error* ret_err)
{
    return return_not_implemented(ret_err);
}

int HawkbitService::method_install_bundle(sd_bus_message* msg, void* userdata,
                                           sd_bus_error* ret_err)
{
    auto* service = static_cast<HawkbitService*>(userdata);
    const char* source = nullptr;
    int r = sd_bus_message_read(msg, "s", &source);
    if (r < 0) {
        sd_bus_error_set(ret_err, SD_BUS_ERROR_INVALID_ARGS, "failed to read bundle source");
        return r;
    }
    if (source == nullptr || source[0] == '\0') {
        sd_bus_error_set(ret_err, SD_BUS_ERROR_INVALID_ARGS, "bundle source must be non-empty");
        return -EINVAL;
    }

    BundleOptions options;
    r = read_bundle_options(msg, options);
    if (r < 0) {
        sd_bus_error_set(ret_err, SD_BUS_ERROR_INVALID_ARGS, "failed to read bundle options");
        return r;
    }

    const char* object_path = sd_bus_message_get_path(msg);
    r = service->install_bundle(source, options, object_path != nullptr ? object_path : "/");
    if (r == -EBUSY) {
        sd_bus_error_set(ret_err, "de.fsembedded.fsupdate1.Error.Busy",
                         "another RAUC installation is in progress");
        return -EBUSY;
    }
    if (r < 0) {
        sd_bus_error_setf(ret_err, SD_BUS_ERROR_FAILED,
                          "failed to start fs-updater: %s", strerror(-r));
        return r;
    }
    return sd_bus_reply_method_return(msg, nullptr);
}

int HawkbitService::method_info(sd_bus_message* /*msg*/, void* /*userdata*/, sd_bus_error* ret_err)
{
    return return_not_implemented(ret_err);
}

int HawkbitService::method_inspect_bundle(sd_bus_message* /*msg*/, void* /*userdata*/,
                                          sd_bus_error* ret_err)
{
    return return_not_implemented(ret_err);
}

int HawkbitService::method_mark(sd_bus_message* /*msg*/, void* /*userdata*/, sd_bus_error* ret_err)
{
    return return_not_implemented(ret_err);
}

int HawkbitService::method_empty_slots(sd_bus_message* /*msg*/, void* /*userdata*/,
                                        sd_bus_error* ret_err)
{
    return return_not_implemented(ret_err);
}

int HawkbitService::method_empty_artifacts(sd_bus_message* /*msg*/, void* /*userdata*/,
                                            sd_bus_error* ret_err)
{
    return return_not_implemented(ret_err);
}

int HawkbitService::method_primary(sd_bus_message* /*msg*/, void* /*userdata*/,
                                   sd_bus_error* ret_err)
{
    return return_not_implemented(ret_err);
}

int HawkbitService::prop_string(sd_bus* /*bus*/, const char* /*path*/, const char* /*iface*/,
                                const char* /*prop*/, sd_bus_message* reply,
                                void* /*userdata*/, sd_bus_error* /*err*/)
{
    return sd_bus_message_append(reply, "s", "");
}

int HawkbitService::prop_progress(sd_bus* /*bus*/, const char* /*path*/, const char* /*iface*/,
                                  const char* /*prop*/, sd_bus_message* reply,
                                  void* /*userdata*/, sd_bus_error* /*err*/)
{
    return sd_bus_message_append(reply, "(isi)", 0, "", 0);
}

const sd_bus_vtable HawkbitService::installer_vtable[] = {
    SD_BUS_VTABLE_START(0),

    SD_BUS_METHOD("Install",           "s",      "",          method_install,          SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("InstallBundle",     "sa{sv}", "",          method_install_bundle,   SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Info",              "s",      "ss",        method_info,             SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("InspectBundle",     "sa{sv}", "a{sv}",     method_inspect_bundle,   SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Mark",              "ss",     "ss",        method_mark,             SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetSlotStatus",     "",       "a(sa{sv})", method_empty_slots,      SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetArtifactStatus", "",       "aa{sv}",    method_empty_artifacts,  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetPrimary",        "",       "s",         method_primary,          SD_BUS_VTABLE_UNPRIVILEGED),

    SD_BUS_PROPERTY("Operation",     "s",     prop_string,   0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("LastError",     "s",     prop_string,   0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("Progress",      "(isi)", prop_progress,0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("Compatible",    "s",     prop_string,   0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("Variant",       "s",     prop_string,   0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("SystemVersion", "s",     prop_string,   0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("BootSlot",      "s",     prop_string,   0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),

    SD_BUS_SIGNAL("Completed", "i", 0),

    SD_BUS_VTABLE_END
};