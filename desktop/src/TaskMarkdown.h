#pragma once

#include <QPointF>
#include <QString>

class QTextDocument;

namespace forkmesh::taskmarkdown {

bool isSafeLink(const QString &href);
QString toHtml(const QString &markdown);
int taskItemCount(const QString &markdown);
QString toggleTaskItem(const QString &markdown, int index);
int taskItemIndexAt(const QTextDocument *document, const QPointF &position);

}
