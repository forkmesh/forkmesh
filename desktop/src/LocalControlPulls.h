#pragma once

#include "PullMergeAll.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace forkmesh::localcontrol::pulls {

QString familyPrefix();

constexpr int kMaxSkippedInReply = 50;

QStringList actions();
bool isKnownAction(const QString &action);
bool validRepoSegment(const QString &segment);
bool validRequest(const QString &owner, const QString &name, const QString &action);
QString confirmationTarget(const QString &owner, const QString &name,
                           const QString &action);
QString actionSummary(const QString &owner, const QString &name, const QString &action);
QJsonObject launchReply(const forkmesh::pullmergeall::Launch &launch);

}
