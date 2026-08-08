#include "dispatcher/SolarmanDiscovery.h"

#include <QHostAddress>
#include <QTimer>
#include <QUdpSocket>

namespace
{
constexpr quint16 kDiscoveryPort = 48899;
const char kDiscoveryRequest[] = "WIFIKIT-214028-READ";
} // namespace

SolarmanDiscovery::SolarmanDiscovery(QObject *parent) : QObject(parent) {}

void SolarmanDiscovery::start(int timeoutMs)
{
    if (m_socket)
        return; // поиск уже выполняется

    m_socket = new QUdpSocket(this);
    m_socket->bind(QHostAddress::AnyIPv4, 0, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);

    connect(m_socket, &QUdpSocket::readyRead, this, [this]() {
        while (m_socket->hasPendingDatagrams())
        {
            QByteArray datagram;
            datagram.resize(static_cast<int>(m_socket->pendingDatagramSize()));
            m_socket->readDatagram(datagram.data(), datagram.size());

            // Ожидаемый формат ответа: "<ip>,<mac>,<serial>" в ASCII, разделители
            // строк/пробелы по краям обрезаем на всякий случай.
            const QString text = QString::fromLatin1(datagram).trimmed();
            const QStringList parts = text.split(QLatin1Char(','));
            if (parts.size() < 3)
                continue;

            bool serialOk = false;
            const quint32 serial = parts.at(2).trimmed().toUInt(&serialOk);
            if (!serialOk)
                continue;

            DiscoveredSolarmanLogger logger;
            logger.ip = parts.at(0).trimmed();
            logger.mac = parts.at(1).trimmed();
            logger.serial = serial;
            emit loggerFound(logger);
        }
    });

    m_socket->writeDatagram(QByteArray(kDiscoveryRequest), QHostAddress::Broadcast, kDiscoveryPort);

    QTimer::singleShot(timeoutMs, this, [this]() {
        if (m_socket)
        {
            m_socket->deleteLater();
            m_socket = nullptr;
        }
        emit finished();
    });
}
