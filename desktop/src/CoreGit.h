#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include "LogSource.h"

#include <functional>

class CoreClient;

namespace forkmesh {
struct GitAttempt;
}

namespace forkmesh::coregit {

void follow(CoreClient *client);
void attach(const QString &stateDirectory);
void detach();
bool attached();

using CoreEventHandler = std::function<void(const QString &event, const QJsonObject &data)>;

struct DetachedRequest {
    QString dir;
    QStringList args;
    int timeoutMs = 0;
    bool mergeStderr = false;
    QString detail;
    QString routine;
    qint64 queuedMs = -1;
    forkmesh::LogSource source;
};

struct DetachedResult {
    bool ok = false;
    QByteArray output;
};

bool runDetached(const DetachedRequest &request, DetachedResult *out);

struct ReadRequest {
    QString dir;
    QStringList args;
    const QByteArray *input = nullptr;
    int timeoutMs = 0;
    QString detail;
    forkmesh::LogSource source;
};

bool runRead(const ReadRequest &request, GitAttempt *out);

struct CommandRequest {
    QString dir;
    QStringList args;
    bool write = false;
    int timeoutMs = 0;
    QStringList env;
    QString detail;
    forkmesh::LogSource source;
};

struct CommandResult {
    bool ok = false;
    bool timedOut = false;
    QByteArray output;
    QString error;
};

bool runCommand(const CommandRequest &request, CommandResult *out);

bool callCore(const QString &method, const QJsonObject &params, qint64 timeoutMs,
              QJsonObject *result, QString *error, bool *sent = nullptr,
              const CoreEventHandler &onEvent = {});

}
