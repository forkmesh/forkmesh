#pragma once

#include "DesktopPowerAlert.h"

#include <QObject>

namespace forkmesh::poweralert {

class PowerSourceReader
{
public:
    explicit PowerSourceReader(QObject *owner);
    Reading read();

private:
    void refreshUPower();

    QObject *m_owner = nullptr;
    int m_upowerOnBattery = -1;
    bool m_upowerPending = false;
    bool m_upowerMissing = false;
};

} // namespace forkmesh::poweralert
