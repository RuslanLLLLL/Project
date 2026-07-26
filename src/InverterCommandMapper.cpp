#include "dispatcher/InverterCommandMapper.h"

#include <QDebug>
#include <algorithm>
#include <cmath>

using namespace deye;

namespace
{
// Инвертор комплектуется СНЭ с номинальным напряжением в несколько сотен вольт;
// если по какой-то причине живое измерение недоступно/некорректно (0 или
// отрицательное), используем консервативное значение, чтобы не делить на 0 и
// не выставить в регистры абсурдно большой ток. Это исключительно "защита от
// дурака" - при штатной работе liveBatteryVoltageV всегда приходит из
// регистра 3015 и валиден.
constexpr double kFallbackBatteryVoltageV = 700.0;

// Аппаратные пределы регистров 2040/2041, см. InverterRegisterMap.h (стр. 5
// протокола): диапазон тока [0, 175.0] А по модулю, единица регистра 0.1 А.
constexpr double kMaxBatteryCurrentA = 175.0;

quint16 clampToRegisterRangeUnsigned(long value, quint16 maxValue)
{
    if (value < 0) return 0;
    if (value > maxValue) return maxValue;
    return static_cast<quint16>(value);
}

int clampInt(double value, int lo, int hi)
{
    return static_cast<int>(std::clamp(value, static_cast<double>(lo), static_cast<double>(hi)));
}
} // namespace

deye::InverterCommandSet InverterCommandMapper::buildCommandSet(const DispatchInterval &interval,
                                                                   const SystemConfig &config,
                                                                   const DispatchPermissions &permissions,
                                                                   double liveBatteryVoltageV)
{
    InverterCommandSet cmd;

    const double voltage = (liveBatteryVoltageV > 1.0) ? liveBatteryVoltageV : kFallbackBatteryVoltageV;
    if (liveBatteryVoltageV <= 1.0)
    {
        qWarning() << "InverterCommandMapper: некорректное напряжение СНЭ" << liveBatteryVoltageV
                   << "В, используется запасное значение" << kFallbackBatteryVoltageV << "В";
    }

    // --- 2040/2041: токовые пределы разряда/заряда, рассчитанные из плановой --
    // --- мощности батареи (interval.batteryNetPowerKw) и живого напряжения. ---
    const double chargeKw = std::max(0.0, interval.batteryNetPowerKw);
    const double dischargeKw = std::max(0.0, -interval.batteryNetPowerKw);

    const double chargeCurrentA = std::clamp((chargeKw * 1000.0) / voltage, 0.0, kMaxBatteryCurrentA);
    const double dischargeCurrentA =
        std::clamp((dischargeKw * 1000.0) / voltage, 0.0, kMaxBatteryCurrentA);

    cmd.fullWrites.push_back({kRegMaxDischargeCurrent,
                               clampToRegisterRangeUnsigned(std::lround(dischargeCurrentA * 10.0), 1750)});
    // Регистр заряда хранит ОТРИЦАТЕЛЬНОЕ значение (диапазон [-1750, 0]), см.
    // протокол, стр. 5 - поэтому кодируем как знаковое S16.
    cmd.fullWrites.push_back(
        {kRegMaxChargeCurrent, encodeS16(-static_cast<int>(std::lround(chargeCurrentA * 10.0)))});

    // --- 2044/2045: держим "широко открытыми", т.к. реальным ограничителем ---
    // --- мощности выступают токовые уставки выше (см. InverterRegisterMap.h). -
    cmd.fullWrites.push_back({kRegMaxDischargePowerPct, kRegWideOpenPowerPct});
    cmd.fullWrites.push_back({kRegMaxChargePowerPct, kRegWideOpenPowerPct});

    // --- 2046/2047: пределы SOC. Нижний предел берём тот, что фактически ------
    // --- использовал оптимизатор для этого интервала (обычный резерв или ------
    // --- аварийный socMin - см. DispatchInterval::socFloorUsedFrac). ----------
    cmd.fullWrites.push_back(
        {kRegSocMax, clampToRegisterRangeUnsigned(std::lround(config.socMax * 1000.0), 1000)});
    cmd.fullWrites.push_back(
        {kRegSocMin, clampToRegisterRangeUnsigned(std::lround(interval.socFloorUsedFrac * 1000.0), 1000)});

    // --- 2103/2104: целевая мощность обмена с сетью (кВт*10, знаковая). --------
    // Пишем ОДНО И ТО ЖЕ значение в верхнюю (Max sell Power) и нижнюю (Peak
    // shaving Power) границу, тем самым "прикалывая" фактический переток к
    // плановому значению - см. README, "Отображение на регистры инвертора".
    // Дополнительно подрезаем плановое значение конфигурационными лимитами
    // (defense in depth - план и так должен их соблюдать).
    const double gridTargetKw =
        std::clamp(interval.gridNetPowerKw, -config.gridMaxImportKw, config.gridMaxExportKw);
    const quint16 gridTargetRaw = encodeS16(clampInt(gridTargetKw * 10.0, -30000, 30000));
    cmd.fullWrites.push_back({kRegMaxSellPowerKw01, gridTargetRaw});
    cmd.fullWrites.push_back({kRegPeakShavingPowerKw01, gridTargetRaw});

    // --- 2065: битовое поле (read-modify-write) --------------------------------
    {
        quint16 mask = kMaskGridChargeEnabled | kMaskGenChargeEnabled | kMaskGenSignal | kMaskBatteryFirst;
        quint16 value = 0;
        if (interval.gridChargeUsed)
            value |= (kBitFieldEnabledValue << kShiftGridChargeEnabled);
        if (permissions.m_genChargeEnable)
            value |= (kBitFieldEnabledValue << kShiftGenChargeEnabled);
        if (interval.genRunning)
            value |= (kBitFieldEnabledValue << kShiftGenSignal);
        // "Battery First" - политика диспетчера: излишки солнца всегда сперва
        // идут на заряд СНЭ и только потом - на продажу (см. эвристику, шаг 2,
        // и MILP - штраф за заряд там меньше выгоды от него же). Отражаем это
        // явно в устройстве, чтобы его собственная логика не расходилась с
        // расчётом диспетчера в периоды, когда связь с ним временно недоступна.
        value |= (kBitFieldEnabledValue << kShiftBatteryFirst);
        cmd.bitFieldWrites.push_back({kRegBatteryChargeSetting, mask, value});
    }

    // --- 2100: битовое поле (read-modify-write), только поле Solar Sell --------
    {
        quint16 mask = kMaskSolarSell;
        quint16 value = permissions.m_sollSell ? (kBitFieldEnabledValue << kShiftSolarSell) : 0;
        cmd.bitFieldWrites.push_back({kRegExportLimitFunction, mask, value});
    }

    return cmd;
}
