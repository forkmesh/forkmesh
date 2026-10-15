#pragma once

#include <QJsonObject>
#include <QString>

#include <functional>

class CoreClient;

namespace forkmesh::coresecrets {

QString apply(const QJsonObject &params);
void registerHandlers(CoreClient *client, std::function<void()> changed);

} // namespace forkmesh::coresecrets
