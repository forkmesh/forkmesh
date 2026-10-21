#include "HiddenUnicode.h"
#include "HiddenUnicodeGuard.h"

#include <QApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QWidget>

#include <iostream>

namespace {

int failures = 0;

void check(bool condition, const QString &message)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << message.toStdString() << '\n';
    ++failures;
}

QString fromCodePoints(std::initializer_list<char32_t> points)
{
    QString out;
    for (char32_t cp : points)
        out.append(QString::fromUcs4(&cp, 1));
    return out;
}

void checkVectors()
{
    QFile file(QStringLiteral(FORKMESH_HIDDEN_UNICODE_VECTORS));
    check(file.open(QIODevice::ReadOnly), QStringLiteral("vectors file opens"));
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    check(root.value(QStringLiteral("policy")).toString()
              == QLatin1String(forkmesh::hiddenunicode::kPolicy),
          QStringLiteral("policy name matches"));
    const QJsonArray cases = root.value(QStringLiteral("cases")).toArray();
    check(cases.size() >= 40, QStringLiteral("vectors are present"));
    for (const QJsonValue &value : cases) {
        const QJsonObject vector = value.toObject();
        const QString id = vector.value(QStringLiteral("id")).toString();
        const QString input = vector.value(QStringLiteral("input")).toString();
        const QString output = vector.value(QStringLiteral("output")).toString();
        const QString cleaned = forkmesh::hiddenunicode::clean(input);
        check(cleaned == output, QStringLiteral("clean: ") + id);
        check(forkmesh::hiddenunicode::hasHidden(input) == vector.value(QStringLiteral("hidden")).toBool(),
              QStringLiteral("hasHidden: ") + id);
        check(forkmesh::hiddenunicode::clean(cleaned) == cleaned, QStringLiteral("idempotent: ") + id);
    }
}

void checkLineEdit(QWidget *window)
{
    auto *line = new QLineEdit(window);
    auto *password = new QLineEdit(window);
    password->setEchoMode(QLineEdit::Password);
    window->layout()->addWidget(line);
    window->layout()->addWidget(password);
    line->ensurePolished();
    password->ensurePolished();
    const QString hidden = QStringLiteral("ab") + fromCodePoints({0x200B, 0xE0041, 0x202E}) + QStringLiteral("cd");
    line->insert(hidden);
    check(line->text() == QStringLiteral("abcd"), QStringLiteral("line edit drops hidden characters"));
    check(line->cursorPosition() == 4, QStringLiteral("line edit cursor follows the cleaned text"));
    line->clear();
    const QString family = fromCodePoints({0x1F468, 0x200D, 0x1F469});
    line->insert(family);
    check(line->text() == family, QStringLiteral("line edit keeps emoji joiners"));
    password->insert(hidden);
    check(password->text() == hidden, QStringLiteral("password fields stay exact"));
}

void checkTextEditors(QWidget *window)
{
    auto *plain = new QPlainTextEdit(window);
    auto *rich = new QTextEdit(window);
    auto *viewer = new QPlainTextEdit(window);
    viewer->setReadOnly(true);
    window->layout()->addWidget(plain);
    window->layout()->addWidget(rich);
    window->layout()->addWidget(viewer);
    plain->ensurePolished();
    rich->ensurePolished();
    viewer->ensurePolished();
    const QString hidden = QStringLiteral("hi") + fromCodePoints({0xE0069, 0xE0067, 0x2066, 0xFEFF}) + QStringLiteral("!");

    plain->setFocus();
    plain->insertPlainText(QStringLiteral("say "));
    plain->insertPlainText(hidden);
    QApplication::processEvents();
    check(plain->toPlainText() == QStringLiteral("say hi!"), QStringLiteral("plain text editor drops hidden characters"));
    plain->undo();
    QApplication::processEvents();
    check(plain->toPlainText().startsWith(QStringLiteral("say hi")) && plain->toPlainText().size() > 7,
          QStringLiteral("undo restores the pasted text instead of cleaning it again"));
    plain->undo();
    QApplication::processEvents();
    check(plain->toPlainText().isEmpty(), QStringLiteral("a second undo removes the paste"));
    plain->redo();
    plain->redo();
    QApplication::processEvents();
    check(plain->toPlainText() == QStringLiteral("say hi!"), QStringLiteral("redo replays the paste and its cleanup"));

    rich->setFocus();
    rich->insertPlainText(hidden);
    QApplication::processEvents();
    check(rich->toPlainText() == QStringLiteral("hi!"), QStringLiteral("rich text editor drops hidden characters"));

    const QString persian = fromCodePoints({0x0645, 0x06CC, 0x200C, 0x062E});
    rich->clear();
    rich->insertPlainText(persian);
    QApplication::processEvents();
    check(rich->toPlainText() == persian, QStringLiteral("rich text editor keeps a Persian ZWNJ"));

    viewer->setPlainText(hidden);
    QApplication::processEvents();
    check(viewer->toPlainText() == hidden, QStringLiteral("read-only viewers show text exactly"));
}

}

int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    forkmesh::hiddenunicode::installInputGuard(&app);
    checkVectors();
    QWidget window;
    window.setLayout(new QVBoxLayout);
    checkLineEdit(&window);
    checkTextEditors(&window);
    window.show();
    QApplication::processEvents();
    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "hidden unicode tests passed\n";
    return 0;
}
