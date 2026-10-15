#pragma once

#include <QJsonObject>
#include <QString>
#include <QtGlobal>

class CoreClient;

namespace forkmesh::corevitals {

void follow(CoreClient *client);
void apply(const QJsonObject &sample);
void reset();
bool fresh();

qint64 totalMemoryBytes();
qint64 availableMemoryBytes();
qint64 totalSwapBytes();
qint64 freeSwapBytes();
qint64 diskTotalBytes(const QString &path);
qint64 diskFreeBytes(const QString &path);
double hostCpuPercent();
bool appCpuPercent(double *percent);
qint64 residentBytes();
int threadCount();
int openFileCount();
int openFileSoftLimit();

}
