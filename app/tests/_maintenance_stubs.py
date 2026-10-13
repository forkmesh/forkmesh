

async def _maintenance_stop_alarm(_durable_object):
    return None


class ReadOnlyModeError(Exception):
    pass


MAINTENANCE_OFF = {
    "ReadOnlyModeError": ReadOnlyModeError,
    "_read_only": lambda _env: False,
    "_decommissioned": lambda _env: False,
    "_d1_write_guard": lambda _env, _sql: None,
    "_maintenance_gate_response": lambda _env, _request: None,
    "_maintenance_socket_closed": lambda _env, _ws: False,
    "_maintenance_stop_alarm": _maintenance_stop_alarm,
}


def with_maintenance_off(namespace):
    for name, value in MAINTENANCE_OFF.items():
        namespace.setdefault(name, value)
    return namespace
