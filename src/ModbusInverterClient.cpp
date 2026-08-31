#include "dispatcher/ModbusInverterClient.h"

#include <QAbstractSocket>
#include <QDebug>
#include <QModbusDataUnit>
#include <QTcpSocket>
#include <QTimer>
#include <QVariant>

#include "dispatcher/SolarmanV5Codec.h"

// QModbusRtuSerialMaster был переименован в QModbusRtuSerialClient в Qt6
// (терминология master/slave заменена на client/server). Оборачиваем это в
// псевдоним типа, чтобы один и тот же .cpp собирался и на Qt5, и на Qt6.
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QModbusRtuSerialClient>
using QModbusRtuSerialClientType = QModbusRtuSerialClient;
#else
#include <QModbusRtuSerialMaster>
using QModbusRtuSerialClientType = QModbusRtuSerialMaster;
#endif

using namespace deye;

ModbusInverterClient::ModbusInverterClient(QObject *parent) : QObject(parent) {}

ModbusInverterClient::~ModbusInverterClient()
{
    if (m_client)
        m_client->disconnectDevice();
    if (m_v5Socket)
        m_v5Socket->disconnectFromHost();
}

void ModbusInverterClient::configureSerial(const QString &portName, int baudRate, QSerialPort::Parity parity,
                                            QSerialPort::DataBits dataBits, QSerialPort::StopBits stopBits)
{
    if (m_client)
    {
        m_client->disconnectDevice();
        m_client->deleteLater();
        m_client = nullptr;
    }
    if (m_v5Socket)
    {
        m_v5Socket->disconnectFromHost();
        m_v5Socket->deleteLater();
        m_v5Socket = nullptr;
    }
    m_mode = TransportMode::SerialRtu;

    auto *client = new QModbusRtuSerialClientType(this);
    client->setConnectionParameter(QModbusDevice::SerialPortNameParameter, portName);
    client->setConnectionParameter(QModbusDevice::SerialParityParameter, parity);
    client->setConnectionParameter(QModbusDevice::SerialBaudRateParameter, baudRate);
    client->setConnectionParameter(QModbusDevice::SerialDataBitsParameter, dataBits);
    client->setConnectionParameter(QModbusDevice::SerialStopBitsParameter, stopBits);
    client->setTimeout(1000);
    client->setNumberOfRetries(2);
    m_client = client;

    connect(m_client, &QModbusClient::stateChanged, this, [this](QModbusDevice::State state) {
        emit connectionStateChanged(state == QModbusDevice::ConnectedState);
    });
    connect(m_client, &QModbusClient::errorOccurred, this,
            [this](QModbusDevice::Error) { emit errorOccurred(m_client->errorString()); });
}

void ModbusInverterClient::configureSolarmanV5(const QString &host, quint16 port, quint32 loggerSerial)
{
    if (m_client)
    {
        m_client->disconnectDevice();
        m_client->deleteLater();
        m_client = nullptr;
    }
    if (m_v5Socket)
    {
        m_v5Socket->disconnectFromHost();
        m_v5Socket->deleteLater();
    }
    m_mode = TransportMode::SolarmanV5;
    m_v5Host = host;
    m_v5Port = port;
    m_v5LoggerSerial = loggerSerial;
    m_v5RecvBuffer.clear();

    m_v5Socket = new QTcpSocket(this);
    connect(m_v5Socket, &QTcpSocket::connected, this, [this] { emit connectionStateChanged(true); });
    connect(m_v5Socket, &QTcpSocket::disconnected, this, [this] { emit connectionStateChanged(false); });
    connect(m_v5Socket, &QAbstractSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        emit errorOccurred(QStringLiteral("Solarman V5: %1").arg(m_v5Socket->errorString()));
    });
}

void ModbusInverterClient::setServerAddress(int slaveId)
{
    m_serverAddress = slaveId;
}

bool ModbusInverterClient::connectDevice()
{
    if (m_mode == TransportMode::SolarmanV5)
    {
        if (!m_v5Socket)
        {
            emit errorOccurred(QStringLiteral("Modbus: транспорт не настроен - вызовите "
                                               "configureSolarmanV5() перед connectDevice()"));
            return false;
        }
        m_v5Socket->connectToHost(m_v5Host, m_v5Port);
        return true;
    }
    if (!m_client)
    {
        emit errorOccurred(QStringLiteral("Modbus: транспорт не настроен - вызовите "
                                           "configureSerial()/configureSolarmanV5() перед connectDevice()"));
        return false;
    }
    return m_client->connectDevice();
}

void ModbusInverterClient::disconnectDevice()
{
    if (m_mode == TransportMode::SolarmanV5)
    {
        if (m_v5Socket)
            m_v5Socket->disconnectFromHost();
        return;
    }
    if (m_client)
        m_client->disconnectDevice();
}

bool ModbusInverterClient::isConnected() const
{
    if (m_mode == TransportMode::SolarmanV5)
        return m_v5Socket && m_v5Socket->state() == QAbstractSocket::ConnectedState;
    return m_client && m_client->state() == QModbusDevice::ConnectedState;
}

// =============================================================================
// Внутренняя очередь: Modbus RTU - протокол "запрос-ответ" на общей шине, в
// любой момент времени может быть только один запрос "в полёте". Все публичные
// операции складывают свою работу в очередь; каждая "работа" (job) обязана сама
// вызвать processQueue() ровно один раз по завершении - это даёт возможность
// одной работе выполнить НЕСКОЛЬКО последовательных Modbus-обменов (например,
// read-modify-write для битового поля) и лишь потом отдать очередь дальше.
// =============================================================================
void ModbusInverterClient::enqueue(Job job)
{
    m_queue.enqueue(std::move(job));
    if (!m_busy)
        processQueue();
}

void ModbusInverterClient::processQueue()
{
    if (m_queue.isEmpty())
    {
        m_busy = false;
        return;
    }
    m_busy = true;
    Job job = m_queue.dequeue();
    job();
}

void ModbusInverterClient::doReadHoldingRegisters(
    quint16 startAddress, quint16 count, const std::function<void(bool ok, QVector<quint16> values)> &onDone)
{
    if (m_mode == TransportMode::SolarmanV5)
    {
        doV5ReadHoldingRegisters(startAddress, count, onDone);
        return;
    }

    if (!isConnected())
    {
        emit errorOccurred(QStringLiteral("Modbus: устройство не подключено (чтение рег. %1)").arg(startAddress));
        onDone(false, {});
        return;
    }

    QModbusDataUnit unit(QModbusDataUnit::HoldingRegisters, startAddress, count);
    QModbusReply *reply = m_client->sendReadRequest(unit, m_serverAddress);
    if (!reply)
    {
        emit errorOccurred(m_client->errorString());
        onDone(false, {});
        return;
    }
    if (reply->isFinished())
    {
        // Немедленная ошибка (например, устройство только что отключилось).
        const bool ok = reply->error() == QModbusDevice::NoError;
        if (!ok)
            emit errorOccurred(reply->errorString());
        reply->deleteLater();
        onDone(ok, {});
        return;
    }

    connect(reply, &QModbusReply::finished, this, [this, reply, onDone]() {
        QVector<quint16> values;
        bool ok = false;
        if (reply->error() == QModbusDevice::NoError)
        {
            const QModbusDataUnit result = reply->result();
            values.reserve(static_cast<int>(result.valueCount()));
            for (uint i = 0; i < result.valueCount(); ++i)
                values.push_back(result.value(i));
            ok = true;
        }
        else
        {
            emit errorOccurred(reply->errorString());
        }
        reply->deleteLater();
        onDone(ok, values);
    });
}

void ModbusInverterClient::doWriteHoldingRegisters(quint16 startAddress, const QVector<quint16> &values,
                                                     const std::function<void(bool ok)> &onDone)
{
    if (m_mode == TransportMode::SolarmanV5)
    {
        doV5WriteHoldingRegisters(startAddress, values, onDone);
        return;
    }

    if (!isConnected())
    {
        emit errorOccurred(QStringLiteral("Modbus: устройство не подключено (запись рег. %1)").arg(startAddress));
        onDone(false);
        return;
    }

    QModbusDataUnit unit(QModbusDataUnit::HoldingRegisters, startAddress,
                          static_cast<uint>(values.size()));
    for (int i = 0; i < values.size(); ++i)
        unit.setValue(i, values[i]);

    QModbusReply *reply = m_client->sendWriteRequest(unit, m_serverAddress);
    if (!reply)
    {
        emit errorOccurred(m_client->errorString());
        onDone(false);
        return;
    }
    if (reply->isFinished())
    {
        const bool ok = reply->error() == QModbusDevice::NoError;
        if (!ok)
            emit errorOccurred(reply->errorString());
        reply->deleteLater();
        onDone(ok);
        return;
    }

    connect(reply, &QModbusReply::finished, this, [this, reply, onDone]() {
        const bool ok = reply->error() == QModbusDevice::NoError;
        if (!ok)
            emit errorOccurred(reply->errorString());
        reply->deleteLater();
        onDone(ok);
    });
}

// =============================================================================
// Транспорт Solarman V5 (см. SolarmanV5Codec.h за форматом кадра и READM за
// процедурой проверки перед подключением к боевому логгеру).
// =============================================================================

void ModbusInverterClient::doV5ReadHoldingRegisters(
    quint16 startAddress, quint16 count, const std::function<void(bool ok, QVector<quint16> values)> &onDone)
{
    if (!isConnected() && !m_v5DryRun)
    {
        emit errorOccurred(
            QStringLiteral("Solarman V5: сокет не подключён (чтение рег. %1)").arg(startAddress));
        onDone(false, {});
        return;
    }

    const QByteArray modbusReq =
        solarman_v5::buildModbusReadRequest(static_cast<quint8>(m_serverAddress), startAddress, count);

    sendV5AndAwaitResponse(modbusReq, [this, startAddress, onDone](bool ok, const QByteArray &respRtu) {
        if (!ok)
        {
            onDone(false, {});
            return;
        }
        const auto parsed = solarman_v5::parseModbusReadResponse(respRtu, static_cast<quint8>(m_serverAddress));
        if (!parsed.ok)
        {
            emit errorOccurred(QStringLiteral("Solarman V5: ошибка разбора ответа на чтение рег. %1: %2")
                                    .arg(startAddress)
                                    .arg(parsed.error));
            onDone(false, {});
            return;
        }
        onDone(true, parsed.values);
    });
}

void ModbusInverterClient::doV5WriteHoldingRegisters(quint16 startAddress, const QVector<quint16> &values,
                                                       const std::function<void(bool ok)> &onDone)
{
    if (!isConnected() && !m_v5DryRun)
    {
        emit errorOccurred(
            QStringLiteral("Solarman V5: сокет не подключён (запись рег. %1)").arg(startAddress));
        onDone(false);
        return;
    }

    const QByteArray modbusReq =
        solarman_v5::buildModbusWriteRequest(static_cast<quint8>(m_serverAddress), startAddress, values);
    const quint16 count = static_cast<quint16>(values.size());

    sendV5AndAwaitResponse(modbusReq, [this, startAddress, count, onDone](bool ok, const QByteArray &respRtu) {
        if (!ok)
        {
            onDone(false);
            return;
        }
        const auto parsed = solarman_v5::parseModbusWriteResponse(
            respRtu, static_cast<quint8>(m_serverAddress), startAddress, count);
        if (!parsed.ok)
        {
            emit errorOccurred(QStringLiteral("Solarman V5: ошибка разбора ответа на запись рег. %1: %2")
                                    .arg(startAddress)
                                    .arg(parsed.error));
            onDone(false);
            return;
        }
        onDone(true);
    });
}

void ModbusInverterClient::sendV5AndAwaitResponse(
    const QByteArray &modbusRtuFrame, const std::function<void(bool ok, QByteArray responseRtuFrame)> &onDone)
{
    const quint16 seq = m_v5NextSequence++;
    const QByteArray v5Frame = solarman_v5::wrapV5Request(m_v5LoggerSerial, seq, modbusRtuFrame);

    if (m_v5DryRun)
    {
        qInfo().noquote() << "Solarman V5 [сухой прогон] Modbus PDU:" << modbusRtuFrame.toHex(' ')
                           << "| полный V5-кадр:" << v5Frame.toHex(' ');
        onDone(false, {}); // намеренно не считаем успехом - см. setV5DryRun()
        return;
    }

    m_v5Socket->write(v5Frame);

    // Таймер и точка подключения к readyRead живут в shared_ptr, чтобы их
    // можно было безопасно захватить в обоих лямбда-обработчиках (успешный
    // разбор кадра и таймаут) и корректно "погасить" друг друга - ровно один
    // из двух путей должен вызвать onDone().
    auto timeoutTimer = std::make_shared<QTimer>();
    timeoutTimer->setSingleShot(true);
    auto connection = std::make_shared<QMetaObject::Connection>();

    *connection = connect(m_v5Socket, &QTcpSocket::readyRead, this, [this, onDone, connection, timeoutTimer]() {
        m_v5RecvBuffer.append(m_v5Socket->readAll());

        for (;;)
        {
            solarman_v5::V5Frame frame;
            QString error;
            const auto status = solarman_v5::tryExtractV5Frame(m_v5RecvBuffer, frame, error);

            if (status == solarman_v5::ExtractStatus::NeedMoreData)
                return; // ждём ещё байт из сокета

            if (status == solarman_v5::ExtractStatus::Resynced)
            {
                emit errorOccurred(QStringLiteral("Solarman V5: повреждённый фрагмент потока отброшен (%1)")
                                        .arg(error));
                continue; // в буфере может быть ещё один кадр - пробуем разобрать дальше
            }

            // FrameReady: убеждаемся, что это ответ на запрос (0x1510), а не,
            // например, периодическое уведомление логгера с другим кодом.
            if (frame.controlCode != solarman_v5::kResponseControlCode)
                continue;

            timeoutTimer->stop();
            QObject::disconnect(*connection);
            onDone(true, frame.modbusRtuFrame);
            return;
        }
    });

    connect(timeoutTimer.get(), &QTimer::timeout, this, [this, onDone, connection]() {
        QObject::disconnect(*connection);
        emit errorOccurred(QStringLiteral("Solarman V5: таймаут ожидания ответа логгера"));
        onDone(false, {});
    });
    timeoutTimer->start(3000);
}

void ModbusInverterClient::doReadModifyWriteBitfield(const BitFieldWrite &bf,
                                                       const std::function<void(bool ok)> &onDone)
{
    doReadHoldingRegisters(bf.address, 1, [this, bf, onDone](bool ok, QVector<quint16> values) {
        if (!ok || values.isEmpty())
        {
            onDone(false);
            return;
        }
        const quint16 current = values[0];
        const quint16 updated = static_cast<quint16>((current & ~bf.mask) | (bf.value & bf.mask));
        doWriteHoldingRegisters(bf.address, {updated}, onDone);
    });
}

void ModbusInverterClient::readHoldingRegisters(quint16 startAddress, quint16 count, const QString &requestTag)
{
    enqueue([this, startAddress, count, requestTag]() {
        doReadHoldingRegisters(startAddress, count, [this, requestTag](bool ok, QVector<quint16> values) {
            if (ok)
                emit holdingRegistersRead(requestTag, values);
            processQueue();
        });
    });
}

void ModbusInverterClient::applyCommandSet(const InverterCommandSet &commandSet)
{
    const int total = commandSet.fullWrites.size() + commandSet.bitFieldWrites.size();
    if (total == 0)
    {
        emit commandSetApplied(true);
        return;
    }

    auto remaining = std::make_shared<int>(total);
    auto allOk = std::make_shared<bool>(true);
    auto finishOne = [this, remaining, allOk](bool ok) {
        if (!ok)
            *allOk = false;
        if (--(*remaining) == 0)
            emit commandSetApplied(*allOk);
    };

    for (const RegisterWrite &w : commandSet.fullWrites)
    {
        enqueue([this, w, finishOne]() {
            doWriteHoldingRegisters(w.address, {w.value}, [this, finishOne](bool ok) {
                finishOne(ok);
                processQueue();
            });
        });
    }
    for (const BitFieldWrite &bf : commandSet.bitFieldWrites)
    {
        enqueue([this, bf, finishOne]() {
            doReadModifyWriteBitfield(bf, [this, finishOne](bool ok) {
                finishOne(ok);
                processQueue();
            });
        });
    }
}

// Читает SOC всех pcsCount() PCS-модулей ОДНИМ блочным запросом (регистры
// kRegPcsSocBlockBase + kRegPcsSocBlockStride .. kRegPcsSocBlockBase +
// kRegPcsSocBlockStride*N - нумерация модулей N начинается с 1, см.
// InverterRegisterMap.h) и усредняет их. Регистры между SOC соседних
// модулей (мощности по фазам, батарея, СЭС того же модуля) тоже попадают в
// ответ - они просто игнорируются здесь, чтение всего диапазона одним
// запросом эффективнее, чем N отдельных обращений по одному регистру.
void ModbusInverterClient::doReadAveragedSoc(const std::function<void(bool ok, double socFrac)> &onDone)
{
    const int n = std::max(1, m_PCScount);
    const quint16 blockStart = static_cast<quint16>(kRegPcsSocBlockBase + kRegPcsSocBlockStride);
    const quint16 blockCount = static_cast<quint16>(kRegPcsSocBlockStride * n);

    doReadHoldingRegisters(blockStart, blockCount, [onDone, n](bool ok, QVector<quint16> block) {
        if (!ok)
        {
            onDone(false, 0.0);
            return;
        }
        double sum = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const int idx = i * kRegPcsSocBlockStride;
            if (idx >= block.size())
            {
                onDone(false, 0.0); // ответ короче ожидаемого - не все модули на связи
                return;
            }
            sum += block[idx] / 1000.0;
        }
        onDone(true, sum / n);
    });
}

void ModbusInverterClient::readSocAndVoltage()
{
    enqueue([this]() {
        doReadAveragedSoc([this](bool okSoc, double soc) {
            doReadHoldingRegisters(kRegBatteryVoltage, 1, [this, okSoc, soc](bool okVolt, QVector<quint16> voltValues) {
                const double voltage =
                    (okVolt && !voltValues.isEmpty()) ? decodeS16(voltValues[0]) / 10.0 : 0.0;
                if (okSoc && okVolt)
                    emit socAndVoltageRead(soc, voltage);
                else
                    emit errorOccurred(QStringLiteral("Modbus: не удалось прочитать SOC/напряжение СНЭ"));
                processQueue();
            });
        });
    });
}

void ModbusInverterClient::readTelemetry()
{
    enqueue([this]() {
        auto telemetry = std::make_shared<InverterTelemetry>();
        auto allOk = std::make_shared<bool>(true);

        doReadAveragedSoc([this, telemetry, allOk](bool ok, double soc) {
            if (ok)
                telemetry->socFrac = soc;
            else
                *allOk = false;

            doReadHoldingRegisters(kRegBatteryVoltage, 1, [this, telemetry, allOk](bool ok2, QVector<quint16> v2) {
                if (ok2 && !v2.isEmpty())
                    telemetry->batteryVoltageV = decodeS16(v2[0]) / 10.0;
                else
                    *allOk = false;

                // Регистры 3049-3052 идут подряд: PV Power Low/High, Battery Power Low/High.
                doReadHoldingRegisters(
                    kRegPvPowerLow, 4, [this, telemetry, allOk](bool ok3, QVector<quint16> v3) {
                        if (ok3 && v3.size() == 4)
                        {
                            telemetry->pvPowerKw = decodeS32(v3[0], v3[1]) / 1000.0;
                            telemetry->batteryPowerKw = decodeS32(v3[2], v3[3]) / 1000.0;
                        }
                        else
                        {
                            *allOk = false;
                        }

                        doReadHoldingRegisters(
                            kRegLoadPowerLow, 2, [this, telemetry, allOk](bool ok4, QVector<quint16> v4) {
                                if (ok4 && v4.size() == 2)
                                    telemetry->loadPowerKw = decodeS32(v4[0], v4[1]) / 1000.0;
                                else
                                    *allOk = false;

                                // Регистры 3066-3067: Operating State, Operating Mode.
                                doReadHoldingRegisters(
                                    kRegOperatingState, 2,
                                    [this, telemetry, allOk](bool ok5, QVector<quint16> v5) {
                                        if (ok5 && v5.size() == 2)
                                        {
                                            telemetry->operatingState = v5[0];
                                            telemetry->operatingMode = v5[1];
                                        }
                                        else
                                        {
                                            *allOk = false;
                                        }
                                        telemetry->valid = *allOk;
                                        emit telemetryRead(*telemetry);
                                        processQueue();
                                    });
                            });
                    });
            });
        });
    });
}
