#include "SessionBusGuard.h"

#ifdef FORKMESH_HAVE_GIO
#include <gio/gio.h>
#endif

namespace forkmesh::sessionbus {

#ifdef FORKMESH_HAVE_GIO
namespace {

constexpr guint kRearmSeconds = 5;

void arm();

gboolean rearm(gpointer)
{
    arm();
    return G_SOURCE_REMOVE;
}

void onClosed(GDBusConnection *connection, gboolean, GError *, gpointer)
{
    g_signal_handlers_disconnect_by_func(connection, reinterpret_cast<gpointer>(onClosed),
                                         nullptr);
    g_object_unref(connection);
    g_timeout_add_seconds(kRearmSeconds, rearm, nullptr);
}

bool busReachable()
{
    const gchar *address = g_getenv("DBUS_SESSION_BUS_ADDRESS");
    if (address && *address)
        return true;
    const gchar *runtime = g_getenv("XDG_RUNTIME_DIR");
    if (!runtime || !*runtime)
        return false;
    gchar *socket = g_build_filename(runtime, "bus", nullptr);
    const bool exists = g_file_test(socket, G_FILE_TEST_EXISTS);
    g_free(socket);
    return exists;
}

void arm()
{
    if (!busReachable())
        return;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    if (!bus)
        return;
    g_dbus_connection_set_exit_on_close(bus, FALSE);
    if (g_dbus_connection_is_closed(bus)) {
        g_object_unref(bus);
        return;
    }
    g_signal_connect(bus, "closed", G_CALLBACK(onClosed), nullptr);
}

} // namespace
#endif

void keepRunningWhenBusCloses()
{
#ifdef FORKMESH_HAVE_GIO
    arm();
#endif
}

} // namespace forkmesh::sessionbus
