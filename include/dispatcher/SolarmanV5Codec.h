#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>
#include <cstdint>

// =============================================================================
// Кодек протокола Solarman/Deye "V5" - проприетарного протокола, которым
// говорят Wi-Fi/LAN логгеры типа LSW-3/LSW-5 на TCP-порту 8899 (в отличие от
// "прозрачного" Modbus TCP на порту 502, который эти логгеры чаще всего НЕ
// поддерживают). Модуль общается с логгером по TCP, логгер сам ретранслирует
// Modbus RTU кадр на шину RS485, к которой подключён PCS-модуль инвертора.
//
// ВАЖНО: протокол V5 не публикуется производителем официально - раскладка
// полей ниже основана на многолетней и широко воспроизводимой работе
// сообщества (в частности, референсная реализация pysolarmanv5, на которой
// построена официальная интеграция Home Assistant "solarman"). Раскладка
// достаточно стабильна для семейства логгеров Deye/Sofar/Solis на одном и том
// же OEM-чипсете, но перед боевым использованием ОБЯЗАТЕЛЬНО:
//   1) включите ModbusInverterClient::setV5DryRun(true) и сверьте залогированные
//      байты запроса с перехватом трафика (Wireshark на Wi-Fi точке логгера)
//      или с выводом pysolarmanv5 для того же регистра;
//   2) начните с ЧТЕНИЯ безобидного регистра (например, 3088 SOC) и убедитесь,
//      что значение совпадает с показаниями штатного приложения Deye/Solarman;
//   3) только после этого переходите к записи регистров.
// См. README, раздел "Подключение через LSW-5 (Solarman V5)".
// =============================================================================
namespace solarman_v5
{

// CRC16 по алгоритму Modbus (полином 0xA001, начальное значение 0xFFFF).
// Возвращает значение в "машинном" порядке (младший байт - crc & 0xFF);
// сериализация в кадр (младший байт первым) выполняется вызывающим кодом.
quint16 modbusCrc16(const QByteArray &data);

// Строит "сырой" Modbus RTU PDU для чтения регистров (функция 0x03):
// [адрес][0x03][старший байт кол-ва][младший байт кол-ва][CRC lo][CRC hi]
QByteArray buildModbusReadRequest(quint8 slaveId, quint16 startAddress, quint16 count);

// Строит "сырой" Modbus RTU PDU для записи нескольких регистров (функция
// 0x10) - используется и для одного регистра (см. протокол Deye, стр. 2-3:
// "0x10 Writes single register or multiple registers").
QByteArray buildModbusWriteRequest(quint8 slaveId, quint16 startAddress, const QVector<quint16> &values);

struct ModbusReadResult
{
    bool ok = false;
    QString error;
    QVector<quint16> values;
};
// Разбирает ответ на чтение (0x03): проверяет CRC, код функции/адрес
// устройства, извлекает значения регистров.
ModbusReadResult parseModbusReadResponse(const QByteArray &rtuFrame, quint8 expectedSlaveId);

struct ModbusWriteResult
{
    bool ok = false;
    QString error;
};
// Разбирает ответ на запись (0x10): проверяет CRC и эхо начального адреса и
// количества регистров (штатный ответ Modbus на 0x10 не возвращает данные,
// только подтверждает параметры запроса).
ModbusWriteResult parseModbusWriteResponse(const QByteArray &rtuFrame, quint8 expectedSlaveId,
                                            quint16 expectedAddress, quint16 expectedCount);

// Заворачивает "сырой" Modbus RTU PDU в конверт V5 для отправки логгеру.
// controlCode: 0x4510 для запроса от клиента к логгеру (единственное значение,
// которое использует этот модуль - см. requestControlCode ниже).
constexpr quint16 kRequestControlCode = 0x4510;
constexpr quint16 kResponseControlCode = 0x1510;
QByteArray wrapV5Request(quint32 loggerSerial, quint16 sequenceId, const QByteArray &modbusRtuFrame);

enum class ExtractStatus
{
    NeedMoreData, // в буфере ещё нет полного кадра - ждём следующих байт из сокета
    FrameReady,   // кадр успешно извлечён и разобран (см. outFrame)
    Resynced      // найден и отброшен повреждённый/нераспознанный фрагмент; в
                  // буфере может остаться следующий кадр - вызывающий код
                  // должен вызвать функцию повторно для того же буфера
};

struct V5Frame
{
    quint16 controlCode = 0;
    quint16 sequenceId = 0;
    quint32 loggerSerial = 0;
    QByteArray modbusRtuFrame; // встроенный "сырой" Modbus RTU PDU (адрес+функция+данные+CRC)
};

// Пытается извлечь ОДИН полный V5-кадр из потокового буфера buffer. TCP не
// сохраняет границы сообщений, поэтому buffer может содержать часть кадра,
// ровно один кадр или несколько кадров подряд - вызывающий код должен звать
// эту функцию в цикле, пока не получит NeedMoreData (см. ModbusInverterClient::
// sendV5AndAwaitResponse). При FrameReady/Resynced разобранные/отброшенные
// байты удаляются из buffer.
ExtractStatus tryExtractV5Frame(QByteArray &buffer, V5Frame &outFrame, QString &error);

} // namespace solarman_v5
