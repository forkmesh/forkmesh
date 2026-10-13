#pragma once

#include <QProcess>

namespace forkmesh {

inline constexpr int kProcessStopGraceMs = 2000;

// code-health: allow(blocking-wait) only reached once a child already overran its deadline; SIGTERM lets git drop its index.lock, and the wait is capped at the grace period before SIGKILL
inline void stopProcessGracefully(QProcess &process,
                                  int graceMs = kProcessStopGraceMs)
{
    if (process.state() == QProcess::NotRunning)
        return;
    process.terminate();
    if (process.waitForFinished(graceMs))
        return;
    process.kill();
    process.waitForFinished(200);
}

}
