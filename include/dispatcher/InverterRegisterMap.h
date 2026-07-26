#pragma once

#include <QVector>
#include <cstdint>

// =============================================================================
// Карта регистров Modbus RTU для PCS-модуля инвертора Deye SUN...K-PCS01HP3.
//
// Все адреса и масштабные коэффициенты взяты из документа производителя
// "Modbus RTU Protocol V105 For PCS Module" (Ningbo Deye Inverter Technology
// Co., Ltd., 2025), присланного вместе с ТЗ. Номера страниц в комментариях
// относятся именно к этому документу ("Register Table", стр. 4-12).
//
// Функция чтения - 0x03 (Read Holding Registers), функция записи - 0x10
// (Write Multiple Registers); обе используют big-endian порядок байт внутри
// каждого 16-битного регистра ("The high byte First"), как и обычный Modbus RTU.
// 32-битные величины (например, PV Power) передаются двумя регистрами: сначала
// младшее слово (Low Word), затем старшее (High Word) - см. стр. 9-11.
// =============================================================================
namespace deye
{

// -----------------------------------------------------------------------------
// Регистры управления (R/W), используемые диспетчером на каждом шаге
// скользящего горизонта. См. InverterCommandMapper за логикой формирования
// значений и README ("Отображение на регистры инвертора") за обоснованием
// выбора именно этих регистров среди нескольких перекрывающихся по смыслу.
// -----------------------------------------------------------------------------

// Максимальный ток разряда СНЭ. Диапазон [0, 1750], единица 0.1 А. (стр. 5)
constexpr quint16 kRegMaxDischargeCurrent = 2040;
// Максимальный ток заряда СНЭ. Диапазон [-1750, 0], единица 0.1 А, знаковый
// (отрицательное значение - см. таблицу производителя). (стр. 5)
constexpr quint16 kRegMaxChargeCurrent = 2041;

// Максимальная мощность разряда/заряда СНЭ в процентах от номинала PCS.
// Диапазон [0, 1200] = [0%, 120.0%], единица 0.1%. Дispatcher держит эти два
// регистра "широко открытыми" (1200 = 120%), чтобы фактическим ограничителем
// мощности батареи выступали ТОКОВЫЕ пределы (2040/2041), которые вычисляются
// из требуемой мощности и измеренного напряжения СНЭ и не требуют знания
// номинальной мощности PCS в кВт (см. InverterCommandMapper). (стр. 5)
constexpr quint16 kRegMaxDischargePowerPct = 2044;
constexpr quint16 kRegMaxChargePowerPct = 2045;
constexpr quint16 kRegWideOpenPowerPct = 1200; // 120.0% - значение "не ограничивать"

// Верхняя/нижняя граница SOC, диапазон [0, 1000] = [0%, 100.0%], единица 0.1%. (стр. 5)
constexpr quint16 kRegSocMax = 2046;
constexpr quint16 kRegSocMin = 2047;

// Битовая маска "Battery Charge Setting" - несколько независимых флагов в одном
// 16-битном регистре, поэтому запись всегда должна быть read-modify-write
// (см. BitFieldWrite / ModbusInverterClient::applyCommandSet). (стр. 5)
//   бит 0-1: Grid Charge Enabled  (01 = разрешено, 00 = запрещено)
//   бит 2-3: Gen Charge Enabled
//   бит 6-7: Gen Signal (дистанционный запрос запуска генератора)
//   бит 8-9: Battery First (приоритет заряда СНЭ над продажей излишков солнца)
// Примечание: производитель не уточняет кодировку 2-битных полей для этого
// регистра так же явно, как для регистра 2100; по аналогии с ним диспетчер
// использует то же соглашение (01 = включено, 00 = выключено). Перед вводом в
// эксплуатацию ОБЯЗАТЕЛЬНО проверьте это соглашение по факту (см. README,
// раздел "Важные предупреждения").
constexpr quint16 kRegBatteryChargeSetting = 2065;
constexpr quint16 kMaskGridChargeEnabled = 0x0003;
constexpr quint16 kMaskGenChargeEnabled  = 0x000C;
constexpr quint16 kMaskGenSignal         = 0x00C0;
constexpr quint16 kMaskBatteryFirst      = 0x0300;
constexpr quint16 kBitFieldEnabledValue  = 0x1; // "01" внутри 2-битного поля, см. выше
constexpr int kShiftGridChargeEnabled = 0;
constexpr int kShiftGenChargeEnabled  = 2;
constexpr int kShiftGenSignal         = 6;
constexpr int kShiftBatteryFirst      = 8;

// Битовая маска "Export Limit Function". Диспетчер трогает только поле
// Solar Sell (бит 6-7); остальные поля (ZeroExport/SoftLimit/HardLimit/
// SellFirst) относятся к независимой функции защиты от перетока по счётчику/
// трансформатору тока и не входят в зону ответственности диспетчера - их
// настраивает пусконаладочный персонал один раз при вводе объекта в
// эксплуатацию. (стр. 5-6)
constexpr quint16 kRegExportLimitFunction = 2100;
constexpr quint16 kMaskSolarSell = 0x00C0;
constexpr int kShiftSolarSell = 6;

// Целевая мощность обмена с сетью, единица 0.1 кВт (ПРЯМАЯ величина в кВт, не
// в процентах от номинала PCS - в отличие от регистров 2048/2049!). Знак: "+"
// означает переток PCS -> сеть (экспорт/продажа), "-" означает переток
// сеть -> PCS (импорт/покупка) - то же соглашение, что и в DispatchInterval::
// gridNetPowerKw, поэтому конвертация не требует инверсии знака. Диспетчер
// пишет ОДНО И ТО ЖЕ значение в оба регистра (max=min=target), тем самым
// "приклеивая" фактический переток к плановому значению - см. README. (стр. 6)
constexpr quint16 kRegMaxSellPowerKw01 = 2103; // "Max sell Power" - верхняя граница (экспорт)
constexpr quint16 kRegPeakShavingPowerKw01 = 2104; // "Peak shaving Power" - нижняя граница (импорт)

// -----------------------------------------------------------------------------
// Регистры измерений (R), используемые диспетчером как обратная связь.
// -----------------------------------------------------------------------------

// Напряжение СНЭ постоянного тока, 0.1 В, знаковый S16. Нужно диспетчеру, чтобы
// пересчитать плановую мощность заряда/разряда (кВт) в токовые уставки
// (0.1 А) для регистров 2040/2041 (I = P*1000/U). (стр. 9)
constexpr quint16 kRegBatteryVoltage = 3015;

// SOC СНЭ, 0.1%, диапазон [0, 1000] = [0%, 100.0%]. Используется как
// DispatchRequest::initialSocFrac на каждом шаге скользящего горизонта. (стр. 10)
constexpr quint16 kRegSoc = 3088;

// Номинальная мощность PCS, 0.1 Вт, беззнаковый U32 (low/high word). Диспетчер
// НЕ использует это значение для масштабирования управляющих регистров (см.
// комментарий к kRegMaxDischargePowerPct выше), но читает его один раз при
// старте для отображения в GUI/диагностики. (стр. 4)
constexpr quint16 kRegRatedPowerLow = 20;
constexpr quint16 kRegRatedPowerHigh = 21;

// Мгновенные измеренные мощности, 1 Вт, знаковый S32 (low/high word) - только
// для отображения в GUI live-телеметрии, в контур управления не входят. (стр. 9-10)
constexpr quint16 kRegPvPowerLow = 3049;
constexpr quint16 kRegPvPowerHigh = 3050;
constexpr quint16 kRegBatteryPowerLow = 3051;
constexpr quint16 kRegBatteryPowerHigh = 3052;
constexpr quint16 kRegLoadPowerLow = 3117;
constexpr quint16 kRegLoadPowerHigh = 3118;

// Режим/состояние: 0 - On-grid, 1 - Off-grid (стр. 10)
constexpr quint16 kRegOperatingMode = 3067;
// 0: Standby, 1: Self-Check, 2: Running, 3: Alarm, 4: Fault (стр. 10)
constexpr quint16 kRegOperatingState = 3066;

// -----------------------------------------------------------------------------
// Вспомогательные структуры для записи регистров.
// -----------------------------------------------------------------------------

// Полная перезапись 16-битного регистра (используется для регистров, целиком
// принадлежащих диспетчеру - токовые пределы, SOC-лимиты, целевая мощность сети).
struct RegisterWrite
{
    quint16 address = 0;
    quint16 value = 0;
};

// Частичное обновление регистра, в котором несколько независимых настроек
// упакованы в разные биты (2065, 2100 и т.п.). ModbusInverterClient выполняет
// такую запись как read-modify-write: читает текущее значение регистра,
// заменяет только биты, покрытые mask, остальные оставляет как есть, и
// записывает результат обратно. Это необходимо, чтобы диспетчер не затирал
// настройки, за которые он не отвечает (например, ZeroExport/HardLimit в
// регистре 2100 - см. комментарий выше).
struct BitFieldWrite
{
    quint16 address = 0;
    quint16 mask = 0;   // какие биты разрешено менять (1 = менять)
    quint16 value = 0;  // новое значение этих битов (уже сдвинутое на нужную позицию)
};

// Полный набор изменений регистров для одного цикла управления.
struct InverterCommandSet
{
    QVector<RegisterWrite> fullWrites;
    QVector<BitFieldWrite> bitFieldWrites;
};

// Кодирует знаковое 16-битное значение (S16) в quint16 для передачи по Modbus
// (побитовое представление, как того требует протокол - "два дополнения").
inline quint16 encodeS16(int value)
{
    if (value > 32767) value = 32767;
    if (value < -32768) value = -32768;
    return static_cast<quint16>(static_cast<int16_t>(value));
}

// Обратное преобразование - декодирование S16 из "сырого" регистра.
inline int decodeS16(quint16 raw)
{
    return static_cast<int16_t>(raw);
}

// Собирает 32-битное знаковое значение из пары регистров (low, high),
// как этого требует протокол для PV Power/Battery Power/Load Power и т.п.
inline int32_t decodeS32(quint16 low, quint16 high)
{
    const uint32_t raw = (static_cast<uint32_t>(high) << 16) | static_cast<uint32_t>(low);
    return static_cast<int32_t>(raw);
}

} // namespace deye
