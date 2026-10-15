#pragma once

#include <QString>

#include <functional>

class CoreClient;
class QObject;

namespace forkmesh::corevault {

void install(QObject *context, std::function<void(const QString &)> log);
void follow(CoreClient *client);
void invalidate();

}
