#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace HomeLayout {

using Pages = QVector<QStringList>;

Pages resolve(const Pages &saved, const QStringList &present);
int pageOf(const Pages &pages, const QString &key);
Pages moveToPage(Pages pages, const QString &key, int page);
Pages moveWithinPage(Pages pages, const QString &key, int index);
Pages addPage(Pages pages);
Pages removePage(Pages pages, int page);
QString toJson(const Pages &pages);
Pages fromJson(const QString &json);

}
