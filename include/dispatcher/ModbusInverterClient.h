#pragma once

#include <QModbusClient>
#include <QModbusReply>
#include <QObject>
#include <QQueue>
#include <QSerialPort>
#include <functional>
#include <memory>

#include "InverterRegisterMap.h"

// Живые измеренные величины, читаемые из инвертора для отображения в GUI
// (не входят в контур управления - только диагностика/телеметрия).
struct InverterTelemetry
{
    double socFrac = 0.0;         // reg 3088
    double batteryVoltageV = 0.0; // reg 3015
    double batteryPowerKw = 0.0;  // reg 3051/3052, "+" разряд? см. протокол: знак совпадает с BAT Power
    double pvPowerKw = 0.0;       // reg 3049/3050
    double loadPowerKw = 0.0;     // reg 3117/3118
    int operatingState = -1;      // reg 3066
    int operatingMode = -1;       // reg 3067
    bool valid = false;
};

// Тонкая обёртка над QModbusClient (QtSerialBus) для чтения/записи регистров
// PCS-модуля Deye по Modbus RTU (через RS485) или Modbus TCP (через
// RTU-Ethernet шлюз - конструктор/настройка одинаковы, отличается только
// транспорт, создаваемый в configureSerial()/configureTcp()).
//
// Modbus RTU - это протокол "запрос-ответ" на общей шине: одновременно может
// выполняться только ОДИН запрос. Поэтому все операции этого класса
// проходят через внутреннюю очередь (см. .cpp, enqueue()/processQueue()) и
// выполняются строго последовательно, даже если вызывающий код (например,
// DispatchEngine) инициирует несколько операций подряд без ожидания.
class ModbusInverterClient : public QObject
{
    Q_OBJECT
public:
    explicit ModbusInverterClient(QObject *parent = nullptr);
    ~ModbusInverterClient() override;

    // --- Настройка соединения (вызывать до connectDevice()) --------------------
    void configureSerial(const QString &portName, int baudRate = 9600,
                          QSerialPort::Parity parity = QSerialPort::NoParity,
                          QSerialPort::DataBits dataBits = QSerialPort::Data8,
                          QSerialPort::StopBits stopBits = QSerialPort::OneStop);
    void configureTcp(const QString &host, int port = 502);
    void setServerAddress(int slaveId); // адрес устройства на шине Modbus, [1,247]

    bool connectDevice();
    void disconnectDevice();
    bool isConnected() const;

    // --- Операции (асинхронные - результат приходит через сигналы) -------------

    // Читает count регистров начиная с startAddress; requestTag прокидывается в
    // сигнал holdingRegistersRead без изменений - удобно для сопоставления
    // ответа с исходным запросом при нескольких параллельно инициированных чтениях.
    void readHoldingRegisters(quint16 startAddress, quint16 count, const QString &requestTag = QString());

    // Записывает набор изменений регистров, вычисленный InverterCommandMapper.
    // Полные перезаписи выполняются напрямую; битовые поля - через
    // read-modify-write (см. deye::BitFieldWrite). По завершении ВСЕХ операций
    // набора испускает commandSetApplied(true), если ни одна не завершилась
    // ошибкой, иначе commandSetApplied(false).
    void applyCommandSet(const deye::InverterCommandSet &commandSet);

    // Удобный метод: читает SOC (3088) и напряжение СНЭ (3015) одним вызовом -
    // именно эти две величины нужны DispatchEngine на каждом шаге горизонта
    // (SOC - как DispatchRequest::initialSocFrac, напряжение - для перевода
    // плановой мощности батареи в токовые уставки, см. InverterCommandMapper).
    void readSocAndVoltage();

    // Читает набор измерений для отображения в GUI (см. InverterTelemetry).
    void readTelemetry();

signals:
    void connectionStateChanged(bool connected);
    void errorOccurred(const QString &message);

    void holdingRegistersRead(const QString &requestTag, QVector<quint16> values);
    void commandSetApplied(bool success);
    void socAndVoltageRead(double socFrac, double batteryVoltageV);
    void telemetryRead(InverterTelemetry telemetry);

private:
    using Job = std::function<void()>; // job должен сам вызвать processQueue() по завершении

    void enqueue(Job job);
    void processQueue();

    // Низкоуровневые примитивы поверх QModbusClient, обёрнутые так, чтобы
    // каждый вызывал onDone() ровно один раз по завершении (успех или ошибка).
    void doReadHoldingRegisters(quint16 startAddress, quint16 count,
                                 const std::function<void(bool ok, QVector<quint16> values)> &onDone);
    void doWriteHoldingRegisters(quint16 startAddress, const QVector<quint16> &values,
                                  const std::function<void(bool ok)> &onDone);
    void doReadModifyWriteBitfield(const deye::BitFieldWrite &bf, const std::function<void(bool ok)> &onDone);

    QModbusClient *m_client = nullptr;
    int m_serverAddress = 1;
    QQueue<Job> m_queue;
    bool m_busy = false;
};
