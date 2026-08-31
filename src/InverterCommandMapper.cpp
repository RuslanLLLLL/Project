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

    // --- 2065: битовое поле (read-modify-write) --------------------------------
    {
        quint16 mask = kMaskGridChargeEnabled | kMaskGenChargeEnabled | kMaskGenSignal | kMaskBatteryFirst;
        quint16 value = 0;
        if (interval.gridChargeUsed)
            value |= (kBitFieldEnabledValue << kShiftGridChargeEnabled);
        if (permissions.m_genChargeEnable)
            value |= (kBitFieldEnabledValue << kShiftGenChargeEnabled);
        // Gen Signal - НЕ команда конкретного интервала (interval.genRunning
        // используется только внутри оптимизатора для расчёта плана/стоимости,
        // на этот бит не влияет). Пока разрешение на генератор в принципе
        // включено (m_genEnable), бит держится ВСЕГДА =1 - фактический
        // запуск/останов генератора инвертор выполняет самостоятельно по
        // собственным порогам SOC (регистры 2107 - запуск при разряде СНЭ до
        // этого значения, 2106 - останов при заряде СНЭ до этого значения),
        // которые диспетчер не трогает (настраиваются отдельно). Так исключено
        // расхождение между планом диспетчера и автоматикой инвертора.
        if (permissions.m_genEnable)
            value |= (kBitFieldEnabledValue << kShiftGenSignal);
        // "Load First" (бит НЕ ставим - 00) - поле действует только в режиме
        // ZeroExport, где продажи почти нет (случай C) или она и так жёстко
        // ограничена излишками (случай B), поэтому сравнивать тут "заряд vs
        // продажа" некорректно - реальный выбор этого поля "заряд vs
        // нагрузка". Battery First в этом сравнении хуже: при нехватке
        // солнца инвертор самостоятельно, в обход диспетчера, забирает из
        // сети мощность одновременно и на заряд СНЭ, и на нагрузку - то есть
        // может покупать электроэнергию для заряда именно тогда, когда то же
        // солнце можно было бы напрямую пустить на нагрузку. При Load First
        // сеть привлекается только туда, где без неё не обойтись (нагрузка),
        // а заряд от сети остаётся полностью под контролем диспетчера - через
        // явный бит Grid Charge Enabled и плановый предел тока (2041), а не
        // через фоновую автоматику инвертора. kMaskBatteryFirst уже входит в
        // mask выше - это гарантирует, что 00 записывается на каждом цикле
        // явно, а не полагается на то, что бит и так был нулевым.
        cmd.bitFieldWrites.push_back({kRegBatteryChargeSetting, mask, value});
    }

    // --- 2100 + 2103/2105: режим продажи в сеть и её мощность ------------------
    //
    // Три взаимоисключающих режима, целиком определяемых правами пользователя
    // (m_sollSell/m_sollSellOfBatteries) - см. README, "Отображение на регистры
    // инвертора":
    //
    //   A) Продажа разрешена И разряд СНЭ на продажу разрешён:
    //      режим Sell First (биты 8-9) = 1, ZeroExport (биты 0-1) = 0;
    //      мощность продажи регулируется Max sell Power (2103).
    //   B) Продажа разрешена, НО разряд СНЭ на продажу запрещён:
    //      режим ZeroExport (биты 0-1) = 1, Sell First (биты 8-9) = 0;
    //      мощность продажи регулируется Zero export Power (2105) - в неё
    //      попадают только излишки солнца (батарея в план продажи уже не
    //      включена самим оптимизатором, см. DispatchPermissions).
    //   C) Продажа запрещена полностью:
    //      режим ZeroExport = 1, Zero export Power (2105) жёстко = 0.
    //
    // Флаг Solar Sell (биты 6-7) - общий "разрешить функции продажи", держим
    // его ВСЕГДА включённым (в т.ч. и в режиме C) - фактическое ограничение
    // объёма продажи в этом режиме выполняет Zero export Power = 0, а не сам
    // флаг. Sell First и ZeroExport взаимоисключены - никогда не включаются
    // одновременно.
    //
    // Оба регистра мощности продажи (2103 и 2105) пишутся на КАЖДОМ цикле
    // независимо от активного режима: тот, что относится к неактивному
    // режиму, обнуляется - это исключает риск устаревшего ненулевого значения
    // в регистре, который может стать активным при внешнем/ручном
    // переключении режима на самом инверторе.
    {
        const bool sellAllowed = permissions.m_sollSell;
        const bool sellFirstMode = sellAllowed && permissions.m_sollSellOfBatteries; // случай A
        // случаи B и C оба используют ZeroExport - отличаются только целевой мощностью

        const double exportTargetKw =
            sellAllowed ? std::clamp(interval.gridNetPowerKw, 0.0, config.gridMaxExportKw) : 0.0;
        const quint16 exportTargetRaw = encodeS16(clampInt(exportTargetKw * 10.0, 0, 30000));

        cmd.fullWrites.push_back({kRegMaxSellPowerKw01, sellFirstMode ? exportTargetRaw : quint16(0)});
        cmd.fullWrites.push_back({kRegZeroExportPowerKw01, sellFirstMode ? quint16(0) : exportTargetRaw});

        quint16 mask = kMaskZeroExport | kMaskSolarSell | kMaskSellFirst;
        quint16 value = (kBitFieldEnabledValue << kShiftSolarSell); // всегда включён, см. комментарий выше
        value |= (kBitFieldEnabledValue << (sellFirstMode ? kShiftSellFirst : kShiftZeroExport));
        cmd.bitFieldWrites.push_back({kRegExportLimitFunction, mask, value});
    }

    return cmd;
}
