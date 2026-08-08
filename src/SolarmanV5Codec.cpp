#include "dispatcher/SolarmanV5Codec.h"

namespace solarman_v5
{

quint16 modbusCrc16(const QByteArray &data)
{
    quint16 crc = 0xFFFF;
    for (unsigned char byte : data)
    {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit)
        {
            if (crc & 0x0001)
                crc = (crc >> 1) ^ 0xA001;
            else
                crc >>= 1;
        }
    }
    return crc;
}

namespace
{
// Добавляет CRC16 (младший байт первым, как того требует Modbus RTU) в конец кадра.
void appendCrc(QByteArray &frame)
{
    const quint16 crc = modbusCrc16(frame);
    frame.append(static_cast<char>(crc & 0xFF));
    frame.append(static_cast<char>((crc >> 8) & 0xFF));
}

void appendU16Be(QByteArray &out, quint16 v)
{
    out.append(static_cast<char>((v >> 8) & 0xFF));
    out.append(static_cast<char>(v & 0xFF));
}
} // namespace

QByteArray buildModbusReadRequest(quint8 slaveId, quint16 startAddress, quint16 count)
{
    QByteArray frame;
    frame.append(static_cast<char>(slaveId));
    frame.append(static_cast<char>(0x03));
    appendU16Be(frame, startAddress);
    appendU16Be(frame, count);
    appendCrc(frame);
    return frame;
}

QByteArray buildModbusWriteRequest(quint8 slaveId, quint16 startAddress, const QVector<quint16> &values)
{
    QByteArray frame;
    frame.append(static_cast<char>(slaveId));
    frame.append(static_cast<char>(0x10));
    appendU16Be(frame, startAddress);
    appendU16Be(frame, static_cast<quint16>(values.size()));
    frame.append(static_cast<char>(values.size() * 2)); // byte count
    for (quint16 v : values)
        appendU16Be(frame, v);
    appendCrc(frame);
    return frame;
}

ModbusReadResult parseModbusReadResponse(const QByteArray &rtuFrame, quint8 expectedSlaveId)
{
    ModbusReadResult result;
    if (rtuFrame.size() < 5)
    {
        result.error = QStringLiteral("слишком короткий Modbus-ответ (%1 байт)").arg(rtuFrame.size());
        return result;
    }

    const QByteArray body = rtuFrame.left(rtuFrame.size() - 2);
    const quint16 gotCrc = static_cast<quint8>(rtuFrame[rtuFrame.size() - 2]) |
                            (static_cast<quint8>(rtuFrame[rtuFrame.size() - 1]) << 8);
    if (modbusCrc16(body) != gotCrc)
    {
        result.error = QStringLiteral("несовпадение CRC16 Modbus-ответа");
        return result;
    }

    const quint8 slaveId = static_cast<quint8>(rtuFrame[0]);
    const quint8 functionCode = static_cast<quint8>(rtuFrame[1]);
    if (slaveId != expectedSlaveId)
    {
        result.error = QStringLiteral("неожиданный адрес устройства в ответе: %1 (ожидался %2)")
                            .arg(slaveId)
                            .arg(expectedSlaveId);
        return result;
    }
    if (functionCode == (0x03 | 0x80))
    {
        result.error = QStringLiteral("устройство вернуло ошибку Modbus, код исключения %1")
                            .arg(rtuFrame.size() > 2 ? static_cast<quint8>(rtuFrame[2]) : 0);
        return result;
    }
    if (functionCode != 0x03)
    {
        result.error = QStringLiteral("неожиданный код функции в ответе: 0x%1").arg(functionCode, 0, 16);
        return result;
    }

    const quint8 byteCount = static_cast<quint8>(rtuFrame[2]);
    if (rtuFrame.size() < 3 + byteCount + 2)
    {
        result.error = QStringLiteral("длина ответа не соответствует заявленному byte count");
        return result;
    }

    result.values.reserve(byteCount / 2);
    for (int i = 0; i < byteCount; i += 2)
    {
        const quint16 v = (static_cast<quint8>(rtuFrame[3 + i]) << 8) | static_cast<quint8>(rtuFrame[3 + i + 1]);
        result.values.push_back(v);
    }
    result.ok = true;
    return result;
}

ModbusWriteResult parseModbusWriteResponse(const QByteArray &rtuFrame, quint8 expectedSlaveId,
                                            quint16 expectedAddress, quint16 expectedCount)
{
    ModbusWriteResult result;
    if (rtuFrame.size() < 8)
    {
        result.error = QStringLiteral("слишком короткий Modbus-ответ на запись (%1 байт)").arg(rtuFrame.size());
        return result;
    }

    const QByteArray body = rtuFrame.left(rtuFrame.size() - 2);
    const quint16 gotCrc = static_cast<quint8>(rtuFrame[rtuFrame.size() - 2]) |
                            (static_cast<quint8>(rtuFrame[rtuFrame.size() - 1]) << 8);
    if (modbusCrc16(body) != gotCrc)
    {
        result.error = QStringLiteral("несовпадение CRC16 Modbus-ответа на запись");
        return result;
    }

    const quint8 slaveId = static_cast<quint8>(rtuFrame[0]);
    const quint8 functionCode = static_cast<quint8>(rtuFrame[1]);
    if (slaveId != expectedSlaveId)
    {
        result.error = QStringLiteral("неожиданный адрес устройства в ответе на запись: %1").arg(slaveId);
        return result;
    }
    if (functionCode == (0x10 | 0x80))
    {
        result.error = QStringLiteral("устройство отклонило запись, код исключения %1")
                            .arg(rtuFrame.size() > 2 ? static_cast<quint8>(rtuFrame[2]) : 0);
        return result;
    }
    if (functionCode != 0x10)
    {
        result.error = QStringLiteral("неожиданный код функции в ответе на запись: 0x%1").arg(functionCode, 0, 16);
        return result;
    }

    const quint16 gotAddress = (static_cast<quint8>(rtuFrame[2]) << 8) | static_cast<quint8>(rtuFrame[3]);
    const quint16 gotCount = (static_cast<quint8>(rtuFrame[4]) << 8) | static_cast<quint8>(rtuFrame[5]);
    if (gotAddress != expectedAddress || gotCount != expectedCount)
    {
        result.error = QStringLiteral("устройство подтвердило запись других параметров (адрес %1, "
                                       "кол-во %2), ожидалось (%3, %4)")
                            .arg(gotAddress)
                            .arg(gotCount)
                            .arg(expectedAddress)
                            .arg(expectedCount);
        return result;
    }

    result.ok = true;
    return result;
}

QByteArray wrapV5Request(quint32 loggerSerial, quint16 sequenceId, const QByteArray &modbusRtuFrame)
{
    // Префикс полезной нагрузки (15 байт), предшествующий встроенному Modbus
    // RTU кадру: тип кадра (0x02 = "передать Modbus дальше"), тип датчика
    // (не используется для нашего сценария - 0x0000) и три 4-байтных поля
    // временных меток (общее время работы / время с включения / смещение),
    // которые логгер не проверяет на входящих запросах - заполняем нулями.
    QByteArray payload;
    payload.append(static_cast<char>(0x02));
    payload.append(2, static_cast<char>(0x00)); // sensor type
    payload.append(4, static_cast<char>(0x00)); // total working time
    payload.append(4, static_cast<char>(0x00)); // power-on time
    payload.append(4, static_cast<char>(0x00)); // offset time
    payload.append(modbusRtuFrame);

    QByteArray frame;
    frame.append(static_cast<char>(0xA5)); // start

    const quint16 length = static_cast<quint16>(2 + 2 + 4 + payload.size()); // control+seq+serial+payload
    frame.append(static_cast<char>(length & 0xFF));
    frame.append(static_cast<char>((length >> 8) & 0xFF));

    frame.append(static_cast<char>(kRequestControlCode & 0xFF));
    frame.append(static_cast<char>((kRequestControlCode >> 8) & 0xFF));

    frame.append(static_cast<char>(sequenceId & 0xFF));
    frame.append(static_cast<char>((sequenceId >> 8) & 0xFF));

    frame.append(static_cast<char>(loggerSerial & 0xFF));
    frame.append(static_cast<char>((loggerSerial >> 8) & 0xFF));
    frame.append(static_cast<char>((loggerSerial >> 16) & 0xFF));
    frame.append(static_cast<char>((loggerSerial >> 24) & 0xFF));

    frame.append(payload);

    // Контрольная сумма - сумма всех байт ПОСЛЕ стартового байта (т.е. length
    // .. конец payload) по модулю 256, затем стоп-байт.
    quint8 checksum = 0;
    for (int i = 1; i < frame.size(); ++i)
        checksum = static_cast<quint8>(checksum + static_cast<quint8>(frame[i]));
    frame.append(static_cast<char>(checksum));
    frame.append(static_cast<char>(0x15)); // stop

    return frame;
}

ExtractStatus tryExtractV5Frame(QByteArray &buffer, V5Frame &outFrame, QString &error)
{
    const int start = buffer.indexOf(static_cast<char>(0xA5));
    if (start < 0)
    {
        // Нет ни одного стартового байта - в буфере точно нет полезных данных.
        buffer.clear();
        return ExtractStatus::NeedMoreData;
    }
    if (start > 0)
        buffer.remove(0, start); // отбрасываем мусор перед стартовым байтом

    constexpr int kFixedHeaderSize = 11; // start(1)+length(2)+control(2)+seq(2)+serial(4)
    if (buffer.size() < kFixedHeaderSize)
        return ExtractStatus::NeedMoreData;

    const quint16 length =
        static_cast<quint8>(buffer[1]) | (static_cast<quint16>(static_cast<quint8>(buffer[2])) << 8);
    const int totalFrameSize = 1 + 2 + length + 2; // start+lenfield+ (control..payload) +checksum+stop

    if (totalFrameSize > 65540) // защита от заведомо мусорного значения length
    {
        error = QStringLiteral("подозрительно большая длина кадра (%1) - вероятно, потеряна "
                                "синхронизация потока")
                    .arg(length);
        buffer.remove(0, 1); // сдвигаемся на один байт и пробуем найти следующий 0xA5
        return ExtractStatus::Resynced;
    }
    if (buffer.size() < totalFrameSize)
        return ExtractStatus::NeedMoreData;

    quint8 checksum = 0;
    for (int i = 1; i < 1 + 2 + length; ++i)
        checksum = static_cast<quint8>(checksum + static_cast<quint8>(buffer[i]));
    const quint8 gotChecksum = static_cast<quint8>(buffer[1 + 2 + length]);
    const quint8 stopByte = static_cast<quint8>(buffer[1 + 2 + length + 1]);

    if (stopByte != 0x15)
    {
        error = QStringLiteral("неверный стоп-байт V5-кадра (0x%1)").arg(stopByte, 0, 16);
        buffer.remove(0, 1); // теряем синхронизацию - пробуем со следующего байта
        return ExtractStatus::Resynced;
    }
    if (checksum != gotChecksum)
    {
        error = QStringLiteral("несовпадение контрольной суммы V5-кадра");
        buffer.remove(0, totalFrameSize);
        return ExtractStatus::Resynced;
    }

    outFrame.controlCode =
        static_cast<quint8>(buffer[3]) | (static_cast<quint16>(static_cast<quint8>(buffer[4])) << 8);
    outFrame.sequenceId =
        static_cast<quint8>(buffer[5]) | (static_cast<quint16>(static_cast<quint8>(buffer[6])) << 8);
    outFrame.loggerSerial = static_cast<quint32>(static_cast<quint8>(buffer[7])) |
                             (static_cast<quint32>(static_cast<quint8>(buffer[8])) << 8) |
                             (static_cast<quint32>(static_cast<quint8>(buffer[9])) << 16) |
                             (static_cast<quint32>(static_cast<quint8>(buffer[10])) << 24);

    constexpr int kPayloadPrefixSize = 15; // frame type(1)+sensor(2)+3*время(4)
    const int payloadStart = kFixedHeaderSize;
    const int payloadEnd = 1 + 2 + length; // исключая
    const QByteArray fullPayload = buffer.mid(payloadStart, payloadEnd - payloadStart);

    if (fullPayload.size() < kPayloadPrefixSize)
    {
        error = QStringLiteral("слишком короткий payload V5-кадра (%1 байт)").arg(fullPayload.size());
        buffer.remove(0, totalFrameSize);
        return ExtractStatus::Resynced;
    }
    outFrame.modbusRtuFrame = fullPayload.mid(kPayloadPrefixSize);

    buffer.remove(0, totalFrameSize);
    return ExtractStatus::FrameReady;
}

} // namespace solarman_v5
