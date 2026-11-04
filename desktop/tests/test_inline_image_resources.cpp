#include "../src/InlineImageResources.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QBuffer>
#include <QElapsedTimer>
#include <QPainter>
#include <QPixmap>
#include <QTextBlock>
#include <QTextEdit>
#include <QTextFragment>
#include <QTextImageFormat>
#include <QUrl>

#include <iostream>

namespace {

int failures = 0;

void check(bool ok, const QString &what)
{
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << what.toStdString() << '\n';
    }
}

QString pngBase64(int seed, int size)
{
    QPixmap pixmap(size, size);
    pixmap.fill(QColor::fromHsv((seed * 37) % 360, 200, 200));
    QPainter painter(&pixmap);
    painter.drawEllipse(4, 4, size - 8, size - 8);
    painter.drawText(size / 3, size / 2, QString::number(seed));
    painter.end();
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    pixmap.save(&buffer, "PNG");
    return QString::fromLatin1(png.toBase64());
}

QStringList imageNames(QTextDocument *document)
{
    QStringList names;
    for (QTextBlock block = document->begin(); block.isValid();
         block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextCharFormat format = it.fragment().charFormat();
            if (format.isImageFormat())
                names << format.toImageFormat().name();
        }
    }
    return names;
}

QString rowsHtml(const QStringList &images, int rows)
{
    QString html;
    for (int row = 0; row < rows; ++row)
        html += QStringLiteral("<p><img src='data:image/png;base64,%1' "
                               "width='20' height='20'> row %2 of the diff</p>")
                    .arg(images.at(row % images.size()))
                    .arg(row);
    return html;
}

void checkImagesResolve()
{
    QTextEdit view;
    const QString png = pngBase64(1, 32);
    const QString html =
        QStringLiteral("<p><img src='data:image/png;base64,%1' width='16' "
                       "height='16'> a <img src=\"data:image/png;base64,%1\"></p>")
            .arg(png);
    const QString adopted =
        forkmesh::ui::adoptInlineImages(view.document(), html);
    check(!adopted.contains(QStringLiteral("data:image/")),
          QStringLiteral("every inline image becomes a document resource"));
    view.setHtml(adopted);
    const QStringList names = imageNames(view.document());
    check(names.size() == 2, QStringLiteral("both images stay in the document"));
    for (const QString &name : names) {
        check(name.startsWith(forkmesh::ui::inlineImageScheme() + u':'),
              QStringLiteral("the image points at the registered resource"));
        const QVariant resource =
            view.document()->resource(QTextDocument::ImageResource, QUrl(name));
        check(resource.userType() == QMetaType::QPixmap &&
                  resource.value<QPixmap>().width() == 32,
              QStringLiteral("the resource survives setHtml as a decoded image"));
    }
    check(names.size() == 2 && names.at(0) == names.at(1),
          QStringLiteral("the same picture shares one resource"));
}

void checkTextLeftAlone()
{
    QTextEdit view;
    const QString png = pngBase64(2, 16);
    const QString html =
        QStringLiteral("<pre>+ src='data:image/png;base64,%1'</pre>"
                       "<p>data:image/png;base64,%1</p>"
                       "<p>data:image/png;base64,</p>"
                       "<img src='data:image/png;base64,not-a-png'>")
            .arg(png);
    const QString adopted =
        forkmesh::ui::adoptInlineImages(view.document(), html);
    check(adopted == html,
          QStringLiteral("diff text that mentions a data URL is not rewritten"));
    check(forkmesh::ui::adoptInlineImages(nullptr, html) == html,
          QStringLiteral("no document leaves the html as is"));
}

qint64 layoutMs(const QString &html, bool adopt, int resizes)
{
    QTextEdit view;
    view.resize(900, 700);
    view.show();
    QApplication::processEvents();
    QElapsedTimer timer;
    timer.start();
    view.setHtml(adopt ? forkmesh::ui::adoptInlineImages(view.document(), html)
                       : html);
    view.document()->documentLayout()->documentSize();
    QApplication::processEvents();
    for (int step = 0; step < resizes; ++step) {
        view.resize(700 + step * 30, 700);
        QApplication::processEvents();
    }
    return timer.elapsed();
}

void checkLayoutFaster()
{
    QStringList images;
    for (int i = 0; i < 40; ++i)
        images << pngBase64(i, 112);
    const QString html = rowsHtml(images, 400);
    layoutMs(html, true, 1);
    const qint64 before = layoutMs(html, false, 6);
    const qint64 after = layoutMs(html, true, 6);
    std::cout << "layout of 400 inline-image rows: data URLs " << before
              << " ms, adopted " << after << " ms\n";
    check(after * 3 < before * 2,
          QStringLiteral("adopted images lay out faster than data URLs"));
}

}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    checkImagesResolve();
    checkTextLeftAlone();
    checkLayoutFaster();
    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "inline image resources tests passed\n";
    return 0;
}
