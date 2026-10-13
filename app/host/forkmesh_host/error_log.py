from __future__ import annotations

import sys
import traceback


def report(context, error=None):
    if error is None:
        error = sys.exc_info()[1]
    lines = ["ERROR %s: %r" % (context, error)]
    if isinstance(error, BaseException):
        lines.extend(line for chunk in traceback.format_exception(
            type(error), error, error.__traceback__) for line in chunk.splitlines())
    sys.stdout.write("\n".join(lines) + "\n")
    sys.stdout.flush()
