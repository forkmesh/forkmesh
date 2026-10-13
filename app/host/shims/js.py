"""Top-level ``js`` for the CPython host: the module is forkmesh_host.js."""
import sys

from forkmesh_host import js as _impl

sys.modules[__name__] = _impl
