"""Top-level ``workers`` for the CPython host: forkmesh_host.workers."""
import sys

from forkmesh_host import workers as _impl

sys.modules[__name__] = _impl
