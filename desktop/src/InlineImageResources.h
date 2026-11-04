#pragma once

#include <QString>

class QTextDocument;

namespace forkmesh::ui {

QString adoptInlineImages(QTextDocument *document, const QString &html);
QString inlineImageScheme();

}
