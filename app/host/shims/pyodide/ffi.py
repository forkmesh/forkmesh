"""Top-level ``pyodide.ffi`` for the CPython host: forkmesh_host.ffi."""
import sys

from forkmesh_host import ffi as _impl

sys.modules[__name__] = _impl
