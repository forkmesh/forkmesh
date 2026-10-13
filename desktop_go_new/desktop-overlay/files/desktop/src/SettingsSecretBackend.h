#pragma once

#include <QMutex>
#include <QMutexLocker>
#include <QSettings>
#include <QString>
#include <QVariant>

#include <functional>
#include <memory>

namespace forkmesh::settingsecrets {

enum class WriteResult { Unhandled, Written, Refused };

struct Backend {
    std::function<bool(const QString &key, QString *value)> read;
    std::function<WriteResult(const QString &key, const QString &value)> write;
};

inline QString marker()
{
    return QStringLiteral("[stored in the ForkMesh vault]");
}

struct Slot {
    QMutex mutex;
    std::shared_ptr<Backend> backend;
};

inline Slot &slot()
{
    static Slot current;
    return current;
}

inline void setBackend(Backend backend)
{
    QMutexLocker lock(&slot().mutex);
    slot().backend = std::make_shared<Backend>(std::move(backend));
}

inline std::shared_ptr<Backend> backend()
{
    QMutexLocker lock(&slot().mutex);
    return slot().backend;
}

inline QString plaintext(const QSettings &settings, const QString &key)
{
    const QString text = QString::fromUtf8(settings.value(key).toByteArray());
    return text == marker() ? QString() : text;
}

inline QString value(const QSettings &settings, const QString &key)
{
    if (const std::shared_ptr<Backend> current = backend(); current && current->read) {
        QString out;
        if (current->read(key, &out))
            return out;
    }
    return plaintext(settings, key);
}

inline bool store(QSettings &settings, const QString &key, const QString &secret,
                  const QVariant &plainForm)
{
    if (const std::shared_ptr<Backend> current = backend(); current && current->write) {
        switch (current->write(key, secret)) {
        case WriteResult::Written:
            settings.setValue(key, secret.isEmpty() ? QString() : marker());
            return true;
        case WriteResult::Refused:
            return false;
        case WriteResult::Unhandled:
            break;
        }
    }
    settings.setValue(key, plainForm);
    return true;
}

}
