#include "../src/LocalControlSecrets.h"

#include <QCoreApplication>
#include <QJsonObject>

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition) {
        qInfo("PASS: %s", message);
    } else {
        qCritical("FAIL: %s", message);
        ++failures;
    }
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    namespace secrets = forkmesh::localcontrol::secrets;

    check(secrets::familyPrefix() == QLatin1String("secrets."), "the family prefix is secrets.");
    check(secrets::validName(QStringLiteral("CLOUDFLARE_API_TOKEN")) &&
              secrets::validName(QStringLiteral("_x9")),
          "shell-style variable names are accepted");
    check(!secrets::validName(QStringLiteral("9x")) && !secrets::validName(QStringLiteral("A-B")) &&
              !secrets::validName(QString()) &&
              !secrets::validName(QString(129, QLatin1Char('A'))) &&
              !secrets::validName(QStringLiteral("ÄB")),
          "names that are not variable names, or too long, are refused");
    check(secrets::validValue(QString(16 * 1024, QLatin1Char('x'))) &&
              !secrets::validValue(QString(16 * 1024 + 1, QLatin1Char('x'))),
          "values are bounded at 16 KB");
    check(secrets::isMutatingAction(QStringLiteral("set")) &&
              secrets::isMutatingAction(QStringLiteral("delete")) &&
              !secrets::isMutatingAction(QStringLiteral("list")),
          "set and delete need a confirmation, list does not");
    check(secrets::actionSummary(QStringLiteral("set"), QStringLiteral("TOKEN")).contains(QStringLiteral("TOKEN")),
          "the summary names the variable");

    QMap<QString, QString> vars{{QStringLiteral("TOKEN"), QStringLiteral("s3cret")},
                                {QStringLiteral("bad name"), QStringLiteral("x")}};
    const QJsonArray rows = secrets::listRows(vars);
    check(rows.size() == 1 &&
              rows.at(0).toObject().value(QStringLiteral("name")).toString() == QLatin1String("TOKEN") &&
              !rows.at(0).toObject().contains(QStringLiteral("value")),
          "the list names variables and never carries a value");

    QMap<QString, QString> many;
    for (int i = 0; i < 600; ++i)
        many.insert(QStringLiteral("V%1").arg(i), QStringLiteral("x"));
    check(secrets::listRows(many).size() == secrets::kMaxRows, "the list is bounded");

    const auto set = secrets::applySet(vars, QStringLiteral("NEW"), QStringLiteral("v"));
    check(set.value(QStringLiteral("NEW")) == QLatin1String("v"), "set writes the value");
    const auto refused = secrets::applySet(vars, QStringLiteral("no good"), QStringLiteral("v"));
    check(refused == vars, "set refuses an invalid name");
    const auto removed = secrets::applyDelete(set, QStringLiteral("TOKEN"));
    check(!removed.contains(QStringLiteral("TOKEN")) && removed.contains(QStringLiteral("NEW")),
          "delete removes only that variable");
    check(secrets::confirmationHolder(QStringLiteral("k"), 3) == QLatin1String("k#3"),
          "confirmations are bound to the device and its connection");

    return failures == 0 ? 0 : 1;
}
