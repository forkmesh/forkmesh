#pragma once

#include "SystemdTimers.h"

#include <QString>

#include <functional>

class QWidget;

namespace forkmesh::systemdtimers {

struct PanelOptions {
    Runner runner;
    std::function<QString()> unitSourceDirectory;
    std::function<qint64()> nowUsec;
    int refreshAfterShowMs = 30000;
};

QWidget *buildSettingsPanel(const PanelOptions &options, QWidget *parent = nullptr);

} // namespace forkmesh::systemdtimers
