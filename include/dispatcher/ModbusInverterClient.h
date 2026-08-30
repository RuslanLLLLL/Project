#pragma once

#include <QByteArray>
#include <QModbusClient>
#include <QModbusReply>
#include <QObject>
#include <QQueue>
#include <QSerialPort>
#include <algorithm>
#include <functional>
#include <memory>

#include "InverterRegisterMap.h"

class QTcpSocket;

// Живые измеренные величины, читаемые из инвертора для отображения в GUI
// (не входят в контур управления - только диагностика/телеметрия).
struct InverterTelemetry
{
    double socFrac = 0.0;         // среднее по всем PCS-модулям, см. setPcsCount()
    double batteryVoltageV = 0.0; // reg 3015
    double batteryPowerKw = 0.0;  // reg 3051/3052, "+" разряд? см. протокол: знак совпадает с BAT Power
    double pvPowerKw = 0.0;       // reg 3049/3050
    double loadPowerKw = 0.0;     // reg 3117/3118
    int operatingState = -1;      // reg 3066
    int operatingMode = -1;       // reg 3067
    bool valid = false;
};

// Тонкая обёртка над транспортом чтения/записи регистров PCS-модуля(ей) Deye.
// Поддерживает два взаимоисключающих способа связи (выбираются вызовом
// соответствующего configure*() перед connectDevice()):
//   - configureSerial()     - прямой Modbus RTU по RS485;
//   - configureSolarmanV5() - Wi-Fi/LAN логгер Deye/Solarman (LSW-3/LSW-5 и
//                          клоны), который говорит НЕ Modbus TCP, а
//                          проприетарным протоколом V5 на порту 8899 - см.
//                          README, "Подключение через LSW-5 (Solarman V5)".
// Обычный "прозрачный" Modbus TCP не поддерживается - в проекте он не нужен.
//
// Modbus RTU - это протокол "запрос-ответ" на общей шине: одновременно может
// выполняться только ОДИН запрос. Поэтому все операции этого класса
// проходят через внутреннюю очередь (см. .cpp, enqueue()/processQueue()) и
// выполняются строго последовательно, даже если вызывающий код (например,
// DispatchEngine) инициирует несколько операций подряд без ожидания. Это
// верно и для режима Solarman V5, хотя он и не использует QModbusClient.
//
// Публичный API (сигналы, readSocAndVoltage()/applyCommandSet() и т.п.)
// одинаков для обоих транспортов - остальной код не нужно менять при
// переключении между ними.
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

    // loggerSerial - серийный номер логгера (наклейка на корпусе LSW-5, либо
    // см. SolarmanDiscovery для автоматического поиска в сети) - обязателен,
    // протокол V5 использует его как часть адресации кадра.
    void configureSolarmanV5(const QString &host, quint16 port, quint32 loggerSerial);

    void setServerAddress(int slaveId); // адрес устройства на шине Modbus, [1,247]

    // Количество параллельно работающих PCS-модулей (инверторов) в системе -
    // от него зависит, сколько регистров блока kRegPcsSocBlockBase читать и
    // усреднять при определении общего SOC системы (см. doReadAveragedSoc() в
    // .cpp и README, раздел про многоинверторные системы). По умолчанию 1
    // (одиночный инвертор, полностью совместимо со старым поведением).
    void setPcsCount(int count) { m_PCScount = std::max(1, count); }
    int pcsCount() const { return m_PCScount; }

    // Режим "сухого прогона" для транспорта Solarman V5: запросы не
    // отправляются, вместо этого в лог (qInfo) выводятся байты Modbus RTU PDU
    // и итогового V5-кадра в hex - используйте перед первым подключением к
    // реальному логгеру, чтобы сверить раскладку протокола с перехватом
    // трафика (см. README, предупреждение в SolarmanV5Codec.h). Не влияет на
    // режим SerialRtu.
    void setV5DryRun(bool dryRun) { m_v5DryRun = dryRun; }

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

    // Удобный метод: читает общий SOC системы (среднее по pcsCount() модулям,
    // см. doReadAveragedSoc()) и напряжение СНЭ (3015) одним вызовом - именно
    // эти две величины нужны DispatchEngine на каждом шаге горизонта (SOC -
    // как DispatchRequest::initialSocFrac, напряжение - для перевода плановой
    // мощности батареи в токовые уставки, см. InverterCommandMapper).
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

    // Низкоуровневые примитивы, обёрнутые так, чтобы каждый вызывал onDone()
    // ровно один раз по завершении (успех или ошибка). Внутри сами выбирают
    // транспорт (QModbusClient либо Solarman V5) по текущему m_mode - весь
    // остальной код класса (и весь код выше по стеку - DispatchEngine и т.д.)
    // от этого выбора не зависит.
    void doReadHoldingRegisters(quint16 startAddress, quint16 count,
                                 const std::function<void(bool ok, QVector<quint16> values)> &onDone);
    void doWriteHoldingRegisters(quint16 startAddress, const QVector<quint16> &values,
                                  const std::function<void(bool ok)> &onDone);
    void doReadModifyWriteBitfield(const deye::BitFieldWrite &bf, const std::function<void(bool ok)> &onDone);

    // Читает блок регистров kRegPcsSocBlockBase..+10*(pcsCount()-1) ОДНИМ
    // запросом и усредняет SOC по всем pcsCount() модулям (см. README про
    // многоинверторные системы). onDone(false, ...) означает, что хотя бы
    // один из модулей не удалось прочитать - в этом случае усреднённому
    // значению доверять нельзя, вызывающий код не должен его использовать.
    void doReadAveragedSoc(const std::function<void(bool ok, double socFrac)> &onDone);

    // --- Реализация транспорта Solarman V5 (см. .cpp и SolarmanV5Codec.h) ------
    void doV5ReadHoldingRegisters(quint16 startAddress, quint16 count,
                                   const std::function<void(bool ok, QVector<quint16> values)> &onDone);
    void doV5WriteHoldingRegisters(quint16 startAddress, const QVector<quint16> &values,
                                    const std::function<void(bool ok)> &onDone);
    // Отправляет уже собранный Modbus RTU PDU, завёрнутый в V5, и асинхронно
    // ждёт ответного V5-кадра с встроенным Modbus-ответом (с таймаутом).
    void sendV5AndAwaitResponse(const QByteArray &modbusRtuFrame,
                                 const std::function<void(bool ok, QByteArray responseRtuFrame)> &onDone);

    enum class TransportMode
    {
        Uninitialized,
        SerialRtu,
        SolarmanV5
    };

    TransportMode m_mode = TransportMode::Uninitialized;
    QModbusClient *m_client = nullptr; // используется в режиме SerialRtu
    int m_serverAddress = 1;
    int m_PCScount = 1; // число параллельных PCS-модулей, см. setPcsCount()
    QQueue<Job> m_queue;
    bool m_busy = false;

    // --- Состояние транспорта Solarman V5 ---------------------------------------
    QTcpSocket *m_v5Socket = nullptr;
    QString m_v5Host;
    quint16 m_v5Port = 8899;
    quint32 m_v5LoggerSerial = 0;
    quint16 m_v5NextSequence = 0;
    QByteArray m_v5RecvBuffer;
    bool m_v5DryRun = false;
};
