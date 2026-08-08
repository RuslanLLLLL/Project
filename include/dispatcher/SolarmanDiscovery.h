#pragma once

#include <QObject>
#include <QString>
#include <cstdint>

class QUdpSocket;

// Найденный в локальной сети логгер Solarman/Deye (LSW-3/LSW-5 и клоны на том
// же OEM-чипсете).
struct DiscoveredSolarmanLogger
{
    QString ip;
    QString mac;
    quint32 serial = 0;
};

// Однократный широковещательный поиск логгеров Solarman/Deye в локальной
// сети - решает главную практическую проблему при подключении по протоколу
// V5 (см. SolarmanV5Codec.h): откуда взять серийный номер логгера, если его
// не видно на этикетке под рукой.
//
// Механизм основан на широко документированном сообществом (pysolarmanv5 и
// производные проекты) поведении прошивки логгера: она слушает UDP-порт
// 48899 и отвечает на строку "WIFIKIT-214028-READ" ASCII-строкой вида
// "<ip>,<mac>,<serial>". Это read-only операция без побочных эффектов на
// инвертор - в худшем случае логгер просто не ответит (например, если
// функция отключена в прошивке), и всё, что нужно сделать в этом случае, -
// взять серийный номер с наклейки на корпусе устройства.
class SolarmanDiscovery : public QObject
{
    Q_OBJECT
public:
    explicit SolarmanDiscovery(QObject *parent = nullptr);

    // Рассылает discovery-запрос и timeoutMs миллисекунд собирает ответы;
    // на каждый ответ испускает loggerFound(), по истечении timeoutMs -
    // finished(). Повторный вызов start() до finished() не поддерживается.
    void start(int timeoutMs = 3000);

signals:
    void loggerFound(DiscoveredSolarmanLogger logger);
    void finished();

private:
    QUdpSocket *m_socket = nullptr;
};
