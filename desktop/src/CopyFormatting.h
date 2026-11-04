#pragma once

#include <QString>

class QApplication;
class QMimeData;
class QTextDocument;

namespace forkmesh::copyformat {

QString plainText(const QTextDocument &document);
QString plainTextFromHtml(const QString &html);
QMimeData *formattedCopy(const QMimeData *source);
void installClipboardFormatting(QApplication *app);

}
