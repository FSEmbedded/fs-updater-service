#include "service.h"

#ifdef HAVE_FUS_LIB
#include "real_updater.h"
#endif

#include <systemd/sd-daemon.h>
#include <systemd/sd-journal.h>
#include <syslog.h>

#include <cerrno>
#include <cstring>

static constexpr const char* BUS_NAME  = "de.fsembedded.fsupdate1";
static constexpr const char* OBJ_PATH  = "/de/fsembedded/fsupdate1";
static constexpr const char* INTERFACE = "de.fsembedded.fsupdate1";

int main()
{
    sd_bus*   bus   = nullptr;
    sd_event* event = nullptr;

    int r = sd_bus_open_system(&bus);
    if (r < 0) {
        sd_journal_print(LOG_ERR, "fs-updater-service: open system bus: %s\n", strerror(-r));
        return 1;
    }

    r = sd_event_default(&event);
    if (r < 0) {
        sd_journal_print(LOG_ERR, "fs-updater-service: sd_event_default: %s\n", strerror(-r));
        sd_bus_unref(bus);
        return 1;
    }

    r = sd_bus_attach_event(bus, event, SD_EVENT_PRIORITY_NORMAL);
    if (r < 0) {
        sd_journal_print(LOG_ERR, "fs-updater-service: sd_bus_attach_event: %s\n", strerror(-r));
        sd_event_unref(event);
        sd_bus_unref(bus);
        return 1;
    }

#ifdef HAVE_FUS_LIB
    Service svc(bus, event, std::make_unique<RealUpdater>());
#else
    Service svc(bus, event, nullptr);
#endif

    r = sd_bus_add_object_vtable(bus, nullptr,
                                  OBJ_PATH, INTERFACE,
                                  Service::service_vtable, &svc);
    if (r < 0) {
        sd_journal_print(LOG_ERR, "fs-updater-service: register vtable: %s\n", strerror(-r));
        sd_event_unref(event);
        sd_bus_unref(bus);
        return 1;
    }

    r = sd_bus_request_name(bus, BUS_NAME, 0);
    if (r < 0) {
        sd_journal_print(LOG_ERR, "fs-updater-service: request bus name '%s': %s\n",
                BUS_NAME, strerror(-r));
        sd_event_unref(event);
        sd_bus_unref(bus);
        return 1;
    }

    sd_notify(0, "READY=1");

    r = sd_event_loop(event);
    if (r < 0)
        sd_journal_print(LOG_ERR, "fs-updater-service: event loop: %s\n", strerror(-r));

    sd_event_unref(event);
    sd_bus_unref(bus);
    return r < 0 ? 1 : 0;
}
