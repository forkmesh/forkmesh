# name: Fix error logs
# description: Repair the crash or error batch the Fixer supplies, from its last successful checkpoint.

Fix the ForkMesh application errors in the batch supplied by the Fixer
teammate. Use the checkpoint, batch boundary, exact ERROR samples, and log paths
from the concrete assignment as the source of truth.

When the assignment is a crash, that crash is the work and it comes before any
queued errors. The app died where it stood, so nothing inside it observed the
fault: the block in the log was written by the dying process, and the lines
before it are the last thing the app was doing. Resolve the backtrace against
this build's sources, and where the frames are unsymbolized work from the
signal, the crash context and that surrounding log. Never make a crash
unobservable instead of fixing it - the crash handler, the signals it installs
and the backtrace it writes stay exactly as they are.

Read the current app log and any rotated `network_log.d` segments covering that
window. Group repeated symptoms, but account for every distinct failure. Trace
each one to its source, reproduce it where practical, fix the root cause, and
add focused regression coverage. Check nearby log context for the operation that
led to the failure and for secondary errors hidden by the first one.

Errors recorded after the batch boundary belong to a later run; do not claim
they were handled unless they were part of this assignment. Do not make the log
look healthy by clearing it, silencing messages, weakening validation, changing
ERROR classification, or treating a started run as a successful fix.
