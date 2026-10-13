#pragma once

class QTextBrowser;
class QTextDocument;

namespace forkmesh::ui {

void fitReadmeImages(QTextBrowser *view, int maxWidth);
void styleReadmeDocument(QTextBrowser *view);
void anchorReadmeHeadings(QTextDocument *doc);

}
