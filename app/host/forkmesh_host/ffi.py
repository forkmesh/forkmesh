"""CPython stand-ins for the parts of ``pyodide.ffi`` the Worker app uses.

Under Pyodide a JavaScript value crosses into Python as a ``JsProxy``. The app
only relies on a few of its behaviours, all emulated here:

* ``jsnull`` is a falsy singleton whose type is named ``JsNull`` (see
  ``js_nullish`` / ``d1_bind_args`` in entry.py);
* a JS object made by ``to_js(dict, dict_converter=Object.fromEntries)``
  supports attribute access (``jwk.d``, ``attachment.room``) and ``to_py()``;
* JS arrays and typed arrays expose ``to_py()``.

``JsObject`` is a ``dict`` subclass so code that checks ``isinstance(x, dict)``
or calls ``.get`` keeps working on either side of the boundary.
"""

from __future__ import annotations

__all__ = [
    "JsArray",
    "JsException",
    "JsNull",
    "JsObject",
    "JsProxy",
    "create_proxy",
    "create_once_callable",
    "jsnull",
    "to_js",
    "to_py",
]


class JsNull:
    """The JavaScript ``null`` value (Pyodide >= 0.28 keeps it distinct)."""

    _instance = None

    def __new__(cls):
        if cls._instance is None:
            cls._instance = super().__new__(cls)
        return cls._instance

    def __bool__(self):
        return False

    def __repr__(self):
        return "jsnull"

    def __eq__(self, other):
        return other is self

    def __hash__(self):
        return 0

    def to_py(self):
        return None


jsnull = JsNull()


class JsProxy:
    """Marker base for host objects that model a JavaScript value."""

    def to_py(self, *args, **kwargs):  # pragma: no cover - overridden
        return self


class JsException(Exception):
    """A JavaScript error surfaced into Python."""

    def __init__(self, message="", name="Error"):
        super().__init__(message)
        self.name = name
        self.message = message


def to_py(value, depth=-1):
    """Deep-convert host JS values into plain Python values."""
    if depth == 0:
        return value
    if isinstance(value, JsNull):
        return None
    if isinstance(value, dict):
        return {k: to_py(v, depth - 1) for k, v in value.items()}
    if isinstance(value, (list, tuple)) and not isinstance(value, str):
        return [to_py(v, depth - 1) for v in value]
    converter = getattr(value, "to_py", None)
    if callable(converter) and not isinstance(value, type):
        return converter()
    return value


class JsObject(dict):
    """A plain JS object: item and attribute access, ``to_py()``."""

    __slots__ = ()

    def __getattr__(self, name):
        try:
            return self[name]
        except KeyError:
            raise AttributeError(name) from None

    def __setattr__(self, name, value):
        self[name] = value

    def __delattr__(self, name):
        try:
            del self[name]
        except KeyError:
            raise AttributeError(name) from None

    def to_py(self, *args, **kwargs):
        return {k: to_py(v) for k, v in self.items()}

    def as_object_map(self):
        return self

    def object_entries(self):
        return JsArray([k, v] for k, v in self.items())

    def object_keys(self):
        return JsArray(self.keys())

    def object_values(self):
        return JsArray(self.values())


class JsArray(list):
    """A JS array (a list with ``to_py``)."""

    __slots__ = ()

    def to_py(self, *args, **kwargs):
        return [to_py(v) for v in self]

    @property
    def length(self):
        return len(self)


def _convert(value, dict_converter, default_converter, seen):
    if isinstance(value, dict) and not isinstance(value, JsObject):
        key = id(value)
        if key in seen:
            return seen[key]
        converted_items = [
            [k, _convert(v, dict_converter, default_converter, seen)]
            for k, v in value.items()
        ]
        if dict_converter is not None:
            result = dict_converter(converted_items)
        else:
            # Pyodide makes a Map; a dict already behaves like one here.
            result = {k: v for k, v in converted_items}
        seen[key] = result
        return result
    if isinstance(value, JsObject) and dict_converter is not None:
        return value
    if isinstance(value, (list, tuple)) and not isinstance(value, JsArray):
        return JsArray(
            _convert(v, dict_converter, default_converter, seen)
            for v in value)
    if isinstance(value, (bytes, bytearray, memoryview)):
        from .js import Uint8Array
        return Uint8Array.new(bytes(value))
    if isinstance(value, (set, frozenset)):
        return JsArray(value)
    if default_converter is not None and not isinstance(
            value, (str, int, float, bool, type(None), JsNull, JsProxy)):
        return default_converter(value, lambda v: _convert(
            v, dict_converter, default_converter, seen), None)
    return value


def to_js(value, *, depth=-1, pyproxies=None, create_pyproxies=True,
          dict_converter=None, default_converter=None, eager_converter=None):
    """Return ``value`` as the host's JS model.

    Dicts are converted with ``dict_converter`` (``Object.fromEntries`` in
    the app) when one is given, recursively, like Pyodide. Without one a dict
    stays a dict (Pyodide would make a ``Map``). ``bytes`` become a
    ``Uint8Array``; lists and tuples become a ``JsArray``.
    """
    del depth, pyproxies, create_pyproxies, eager_converter
    if isinstance(value, (str, int, float, bool, type(None), JsNull)):
        return value
    return _convert(value, dict_converter, default_converter, {})


def create_proxy(obj, *, capture_this=False, roundtrip=True):
    """Pyodide's proxy wrapper is an identity function on the host."""
    del capture_this, roundtrip
    return obj


def create_once_callable(obj, *, _may_syncify=False):
    del _may_syncify
    return obj


def destroy_proxies(proxies):
    del proxies
