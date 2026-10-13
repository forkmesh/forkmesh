#pragma once

#include <QJsonObject>

namespace forkmesh::coremirroractions {

bool delegateConfiguration(const QJsonObject &request, QJsonObject *response);
void install();

}
