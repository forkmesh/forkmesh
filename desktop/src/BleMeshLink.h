#pragma once

#include <QByteArray>
#include <QIODevice>
#include <QString>

#include <functional>

namespace forkmesh::localmesh {

QString bleServiceUuid();
QString bleInboxUuid();
QString bleOutboxUuid();
int bleMaxWriteBytes();
int bleChunkBytes(int mtu);

} // namespace forkmesh::localmesh

#ifdef FORKMESH_HAS_BLUETOOTH

#include <QBluetoothDeviceInfo>
#include <QLowEnergyCharacteristic>
#include <QLowEnergyService>

class QLowEnergyController;

class BleMeshLink : public QIODevice
{
public:
    BleMeshLink(const QBluetoothDeviceInfo &device, const QString &key,
                QObject *parent = nullptr);
    ~BleMeshLink() override;

    void dial();
    QString key() const { return m_key; }

    std::function<void()> onOpened;
    std::function<void(const QString &reason)> onClosed;

    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override;
    qint64 bytesToWrite() const override;
    void close() override;

protected:
    qint64 readData(char *data, qint64 maxSize) override;
    qint64 writeData(const char *data, qint64 size) override;

private:
    void onServicesDiscovered();
    void onServiceState(QLowEnergyService::ServiceState state);
    void onNotified(const QLowEnergyCharacteristic &characteristic,
                    const QByteArray &value);
    void onWritten(const QLowEnergyCharacteristic &characteristic,
                   const QByteArray &value);
    void pump();
    void fail(const QString &reason);
    void teardown();

    QBluetoothDeviceInfo m_device;
    QString m_key;
    QLowEnergyController *m_controller = nullptr;
    QLowEnergyService *m_service = nullptr;
    QLowEnergyCharacteristic m_inbox;
    QByteArray m_readBuffer;
    QByteArray m_writeBuffer;
    qsizetype m_inFlight = 0;
    bool m_finished = false;
};

#endif
