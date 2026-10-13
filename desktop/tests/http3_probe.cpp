// Manual check of forkmesh::net::Http3Get against a real HTTP/3 server:
//   forkmesh-http3-probe https://host/path ["Name: value"]... [--seconds N]
// Prints the status, then the body as it arrives, then how the request ended.
// Exits 0 on a clean end (or when --seconds elapses while streaming).

#include "Http3Get.h"

#include <QCoreApplication>
#include <QTimer>

#include <cstdio>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments().mid(1);
    if (args.isEmpty()) {
        std::fprintf(stderr, "usage: forkmesh-http3-probe URL [\"Name: value\"]... "
                             "[--seconds N]\n");
        return 2;
    }
    QList<forkmesh::net::Http3Get::Header> headers;
    int seconds = 0;
    for (int i = 1; i < args.size(); ++i) {
        if (args.at(i) == QLatin1String("--seconds") && i + 1 < args.size()) {
            seconds = args.at(++i).toInt();
            continue;
        }
        const int colon = args.at(i).indexOf(QLatin1Char(':'));
        if (colon > 0)
            headers.append({args.at(i).left(colon).trimmed().toUtf8(),
                            args.at(i).mid(colon + 1).trimmed().toUtf8()});
    }
    int exitCode = 1;
    auto *get = new forkmesh::net::Http3Get(&app);
    get->onHeaders = [](int status) {
        std::printf("status %d\n", status);
        std::fflush(stdout);
    };
    get->onData = [](const QByteArray &chunk) {
        std::fwrite(chunk.constData(), 1, size_t(chunk.size()), stdout);
        std::fflush(stdout);
    };
    get->onFinished = [&exitCode](const QString &error) {
        std::printf("\nfinished: %s\n",
                    error.isEmpty() ? "clean" : qPrintable(error));
        exitCode = error.isEmpty() ? 0 : 1;
        QCoreApplication::quit();
    };
    QString error;
    if (!get->start(QUrl(args.first()), headers, &error)) {
        std::fprintf(stderr, "cannot start: %s\n", qPrintable(error));
        return 1;
    }
    if (seconds > 0) {
        QTimer::singleShot(seconds * 1000, &app, [&exitCode, get] {
            std::printf("\nstill streaming after the time limit\n");
            get->abort();
            exitCode = 0;
            QCoreApplication::quit();
        });
    }
    app.exec();
    return exitCode;
}
