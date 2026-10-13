#include "BleMeshLink.h"

#include <QtGlobal>

#ifdef FORKMESH_HAS_BLUETOOTH
#include <QBluetoothUuid>
#include <QLowEnergyController>
#include <QLowEnergyDescriptor>
#include <QPointer>
#include <QTimer>
#endif

namespace {

constexpr int kMaxWriteBytes = 512;
constexpr int kDefaultMtu = 23;
constexpr int kAttHeaderBytes = 3;
#ifdef FORKMESH_HAS_BLUETOOTH
constexpr int kOpenTimeoutMs = 20000;
#endif

} // namespace

namespace forkmesh::localmesh {

QString bleServiceUuid()
{
    return QStringLiteral("7f9e5c14-4d3b-4a21-9c6f-2b7ad1e05f11");
}

QString bleInboxUuid()
{
    return QStringLiteral("7f9e5c15-4d3b-4a21-9c6f-2b7ad1e05f11");
}

QString bleOutboxUuid()
{
    return QStringLiteral("7f9e5c16-4d3b-4a21-9c6f-2b7ad1e05f11");
}

int bleMaxWriteBytes()
{
    return kMaxWriteBytes;
}

int bleChunkBytes(int mtu)
{
    if (mtu <= kDefaultMtu)
        return kMaxWriteBytes;
    return qMin(mtu - kAttHeaderBytes, kMaxWriteBytes);
}

} // namespace forkmesh::localmesh

#ifdef FORKMESH_HAS_BLUETOOTH

using namespace forkmesh::localmesh;

BleMeshLink::BleMeshLink(const QBluetoothDeviceInfo &device, const QString &key,
                         QObject *parent)
    : QIODevice(parent), m_device(device), m_key(key)
{
}

BleMeshLink::~BleMeshLink()
{
    m_finished = true;
    teardown();
}

void BleMeshLink::dial()
{
    if (m_controller || m_finished)
        return;
    m_controller = QLowEnergyController::createCentral(m_device, this);
    connect(m_controller, &QLowEnergyController::connected, this,
            [this] { m_controller->discoverServices(); });
    connect(m_controller, &QLowEnergyController::discoveryFinished, this,
            &BleMeshLink::onServicesDiscovered);
    connect(m_controller, &QLowEnergyController::disconnected, this,
            [this] { fail(QStringLiteral("disconnected")); });
    connect(m_controller, &QLowEnergyController::errorOccurred, this,
            [this](QLowEnergyController::Error) {
                fail(m_controller->errorString());
            });
    QPointer<BleMeshLink> self(this);
    QTimer::singleShot(kOpenTimeoutMs, this, [self] {
        if (self && !self->isOpen())
            self->fail(QStringLiteral("timed out"));
    });
    m_controller->connectToDevice();
}

void BleMeshLink::onServicesDiscovered()
{
    if (m_finished || m_service)
        return;
    const QBluetoothUuid uuid(bleServiceUuid());
    if (!m_controller->services().contains(uuid)) {
        fail(QStringLiteral("no mesh service"));
        return;
    }
    m_service = m_controller->createServiceObject(uuid, this);
    if (!m_service) {
        fail(QStringLiteral("no mesh service"));
        return;
    }
    connect(m_service, &QLowEnergyService::stateChanged, this,
            &BleMeshLink::onServiceState);
    connect(m_service, &QLowEnergyService::characteristicChanged, this,
            &BleMeshLink::onNotified);
    connect(m_service, &QLowEnergyService::characteristicWritten, this,
            &BleMeshLink::onWritten);
    connect(m_service, &QLowEnergyService::descriptorWritten, this,
            [this](const QLowEnergyDescriptor &descriptor, const QByteArray &) {
                if (m_finished || isOpen() ||
                    descriptor.type() != QBluetoothUuid::DescriptorType::
                                             ClientCharacteristicConfiguration)
                    return;
                QIODevice::open(QIODevice::ReadWrite);
                if (onOpened)
                    onOpened();
            });
    connect(m_service, &QLowEnergyService::errorOccurred, this,
            [this](QLowEnergyService::ServiceError error) {
                fail(QStringLiteral("service error %1").arg(int(error)));
            });
    m_service->discoverDetails();
}

void BleMeshLink::onServiceState(QLowEnergyService::ServiceState state)
{
    if (m_finished || state != QLowEnergyService::RemoteServiceDiscovered)
        return;
    m_inbox = m_service->characteristic(QBluetoothUuid(bleInboxUuid()));
    const QLowEnergyCharacteristic outbox =
        m_service->characteristic(QBluetoothUuid(bleOutboxUuid()));
    if (!m_inbox.isValid() || !outbox.isValid()) {
        fail(QStringLiteral("mesh characteristics missing"));
        return;
    }
    const QLowEnergyDescriptor config =
        outbox.clientCharacteristicConfiguration();
    if (!config.isValid()) {
        fail(QStringLiteral("mesh outbox cannot notify"));
        return;
    }
    m_service->writeDescriptor(config,
                               QLowEnergyCharacteristic::CCCDEnableNotification);
}

void BleMeshLink::onNotified(const QLowEnergyCharacteristic &characteristic,
                             const QByteArray &value)
{
    if (m_finished ||
        characteristic.uuid() != QBluetoothUuid(bleOutboxUuid()))
        return;
    if (value.isEmpty()) {
        fail(QStringLiteral("closed by peer"));
        return;
    }
    if (!isOpen())
        return;
    m_readBuffer.append(value);
    emit readyRead();
}

void BleMeshLink::onWritten(const QLowEnergyCharacteristic &characteristic,
                            const QByteArray &)
{
    if (m_finished || characteristic.uuid() != m_inbox.uuid() || !m_inFlight)
        return;
    const qsizetype written = m_inFlight;
    m_writeBuffer.remove(0, written);
    m_inFlight = 0;
    emit bytesWritten(written);
    pump();
}

void BleMeshLink::pump()
{
    if (m_finished || m_inFlight || m_writeBuffer.isEmpty() || !m_service)
        return;
    const qsizetype chunk =
        qMin<qsizetype>(m_writeBuffer.size(), bleChunkBytes(m_controller->mtu()));
    m_inFlight = chunk;
    m_service->writeCharacteristic(m_inbox, m_writeBuffer.left(chunk),
                                   QLowEnergyService::WriteWithResponse);
}

qint64 BleMeshLink::bytesAvailable() const
{
    return m_readBuffer.size() + QIODevice::bytesAvailable();
}

qint64 BleMeshLink::bytesToWrite() const
{
    return m_writeBuffer.size();
}

qint64 BleMeshLink::readData(char *data, qint64 maxSize)
{
    const qint64 count = qMin<qint64>(maxSize, m_readBuffer.size());
    if (count <= 0)
        return m_finished ? -1 : 0;
    memcpy(data, m_readBuffer.constData(), size_t(count));
    m_readBuffer.remove(0, count);
    return count;
}

qint64 BleMeshLink::writeData(const char *data, qint64 size)
{
    if (m_finished || !isOpen())
        return -1;
    m_writeBuffer.append(data, size);
    pump();
    return size;
}

void BleMeshLink::close()
{
    const bool wasFinished = m_finished;
    m_finished = true;
    if (isOpen())
        QIODevice::close();
    if (!wasFinished)
        teardown();
}

void BleMeshLink::fail(const QString &reason)
{
    if (m_finished)
        return;
    m_finished = true;
    const bool wasOpen = isOpen();
    teardown();
    if (wasOpen)
        QIODevice::close();
    if (onClosed)
        onClosed(reason);
}

void BleMeshLink::teardown()
{
    m_writeBuffer.clear();
    m_inFlight = 0;
    if (m_service) {
        m_service->disconnect(this);
        m_service->deleteLater();
        m_service = nullptr;
    }
    if (m_controller) {
        m_controller->disconnect(this);
        m_controller->disconnectFromDevice();
        m_controller->deleteLater();
        m_controller = nullptr;
    }
}

#endif
