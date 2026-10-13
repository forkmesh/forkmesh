#pragma once

#include <QString>

class QImage;

namespace AgentPromptImages {

QString directory();

QString save(const QImage &image);

QString resolve(const QString &path);

void migrateLegacy();

} // namespace AgentPromptImages
