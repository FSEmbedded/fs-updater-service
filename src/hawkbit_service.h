#pragma once

#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>

#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sys/types.h>

class HawkbitService {
public:
    HawkbitService(sd_bus* bus, sd_event* event);
    ~HawkbitService();

    HawkbitService(const HawkbitService&) = delete;
    HawkbitService& operator=(const HawkbitService&) = delete;
    HawkbitService(HawkbitService&&) = delete;
    HawkbitService& operator=(HawkbitService&&) = delete;

    static const sd_bus_vtable installer_vtable[];

private:
    struct BundleOptions {
        std::vector<std::string> http_headers;
        std::string tls_key;
        std::string tls_cert;
        bool tls_no_verify = false;
    };

    sd_bus* m_bus = nullptr;
    sd_event* m_event = nullptr;
    sd_event_source* m_completion_src = nullptr;
    int m_completion_fd = -1;
    pid_t m_child_pid = -1;
    bool m_install_running = false;
    int m_install_timeout_sec = 1800;
    std::string m_object_path;
    std::thread m_wait_thread;
    std::mutex m_completion_mutex;
    bool m_completion_ready = false;
    int m_completion_result = 1;

    static int on_completion(sd_event_source* src, int fd, uint32_t revents, void* userdata);
    static int read_bundle_options(sd_bus_message* msg, BundleOptions& options);

    int install_bundle(const std::string& source, const BundleOptions& options,
                       const std::string& object_path);

    static int method_install(sd_bus_message* msg, void* userdata, sd_bus_error* ret_err);
    static int method_install_bundle(sd_bus_message* msg, void* userdata, sd_bus_error* ret_err);
    static int method_info(sd_bus_message* msg, void* userdata, sd_bus_error* ret_err);
    static int method_inspect_bundle(sd_bus_message* msg, void* userdata, sd_bus_error* ret_err);
    static int method_mark(sd_bus_message* msg, void* userdata, sd_bus_error* ret_err);
    static int method_empty_slots(sd_bus_message* msg, void* userdata, sd_bus_error* ret_err);
    static int method_empty_artifacts(sd_bus_message* msg, void* userdata, sd_bus_error* ret_err);
    static int method_primary(sd_bus_message* msg, void* userdata, sd_bus_error* ret_err);

    static int prop_string(sd_bus* bus, const char* path, const char* iface, const char* prop,
                           sd_bus_message* reply, void* userdata, sd_bus_error* err);
    static int prop_progress(sd_bus* bus, const char* path, const char* iface, const char* prop,
                             sd_bus_message* reply, void* userdata, sd_bus_error* err);
};