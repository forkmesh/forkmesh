#pragma once

#include <QString>
#include <QStringList>

#include <functional>

class CoreClient;

namespace forkmesh::corediag {

void follow(CoreClient *client, std::function<QStringList()> stalls);
void pushStall(const QString &entry);
void clearStalls();

}
