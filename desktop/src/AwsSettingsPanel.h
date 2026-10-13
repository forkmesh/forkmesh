#pragma once

#include "AwsCredentials.h"

#include <QByteArray>
#include <QDateTime>
#include <QString>

#include <functional>

class QObject;
class QWidget;

namespace forkmesh::aws {

struct HttpReply {
    int status = 0;
    QByteArray body;
    QString error;
};

using ReplyHandler = std::function<void(const HttpReply &)>;
using Transport = std::function<void(const Request &, QObject *context, ReplyHandler)>;

Transport networkTransport();

struct PanelOptions {
    Transport transport;
    std::function<QString()> fleetConfigDirectory;
    std::function<QDateTime()> clock;
    QString keyPath;
};

QWidget *buildSettingsPanel(const PanelOptions &options, QWidget *parent = nullptr);

} // namespace forkmesh::aws
