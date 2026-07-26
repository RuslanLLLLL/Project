#include "dispatcher/ModbusInverterClient.h"

#include <QDebug>
#include <QModbusDataUnit>
#include <QModbusTcpClient>
#include <QVariant>

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
}

void ModbusInverterClient::configureSerial(const QString &portName, int baudRate, QSerialPort::Parity parity,
                                            QSerialPort::DataBits dataBits, QSerialPort::StopBits stopBits)
{
    if (m_client)
    {
        m_client->disconnectDevice();
        m_client->deleteLater();
    }

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

void ModbusInverterClient::configureTcp(const QString &host, int port)
{
    if (m_client)
    {
        m_client->disconnectDevice();
        m_client->deleteLater();
    }

    auto *client = new QModbusTcpClient(this);
    client->setConnectionParameter(QModbusDevice::NetworkAddressParameter, host);
    client->setConnectionParameter(QModbusDevice::NetworkPortParameter, port);
    client->setTimeout(1000);
    client->setNumberOfRetries(2);
    m_client = client;

    connect(m_client, &QModbusClient::stateChanged, this, [this](QModbusDevice::State state) {
        emit connectionStateChanged(state == QModbusDevice::ConnectedState);
    });
    connect(m_client, &QModbusClient::errorOccurred, this,
            [this](QModbusDevice::Error) { emit errorOccurred(m_client->errorString()); });
}

void ModbusInverterClient::setServerAddress(int slaveId)
{
    m_serverAddress = slaveId;
}

bool ModbusInverterClient::connectDevice()
{
    if (!m_client)
    {
        emit errorOccurred(QStringLiteral("Modbus: транспорт не настроен - вызовите "
                                           "configureSerial()/configureTcp() перед connectDevice()"));
        return false;
    }
    return m_client->connectDevice();
}

void ModbusInverterClient::disconnectDevice()
{
    if (m_client)
        m_client->disconnectDevice();
}

bool ModbusInverterClient::isConnected() const
{
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

void ModbusInverterClient::readSocAndVoltage()
{
    enqueue([this]() {
        doReadHoldingRegisters(kRegSoc, 1, [this](bool okSoc, QVector<quint16> socValues) {
            const double soc = (okSoc && !socValues.isEmpty()) ? socValues[0] / 1000.0 : 0.0;
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

        doReadHoldingRegisters(kRegSoc, 1, [this, telemetry, allOk](bool ok, QVector<quint16> v) {
            if (ok && !v.isEmpty())
                telemetry->socFrac = v[0] / 1000.0;
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
