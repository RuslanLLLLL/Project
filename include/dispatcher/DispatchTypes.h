#pragma once

#include <QDateTime>
#include <QVector>

#include "DispatchPermissions.h"
#include "ForecastTypes.h"
#include "SystemConfig.h"

// =============================================================================
// Соглашения о знаках (одинаковы для эвристики, MILP и маппера регистров, чтобы
// исключить путаницу при переносе значений в Modbus-регистры):
//
//   batteryNetPowerKw : > 0 - заряд (энергия идёт В батарею)
//                       < 0 - разряд (энергия идёт ИЗ батареи)
//   gridNetPowerKw    : > 0 - экспорт (PCS -> сеть, "продажа")
//                       < 0 - импорт (сеть -> PCS, "покупка")
//                       Это соответствует регистру 2048 Modbus PCS
//                       ("Positive means from PCS To Grid, Negative means from
//                       Grid To PCS"), поэтому DispatchInterval::gridNetPowerKw
//                       можно передавать в маппер регистров без инверсии знака.
//   genPowerKw        : >= 0 (генератор только отдаёт мощность)
// =============================================================================

// Классификация часа по цене покупки относительно горизонта планирования —
// используется эвристическим оптимизатором (см. README, формулы 1-2) и
// сохраняется в результате для диагностики/отображения в GUI (например,
// подсветкой строк таблицы плана).
enum class PriceClass
{
    Cheap,      // buyPrice <= порог cheapPricePercentile
    Normal,
    Expensive   // buyPrice >= порог expensivePricePercentile
};

// Результат диспетчеризации для одного интервала управления. Разбивка потоков
// (solarToLoadKw и т.д.) не обязательна для записи в инвертор (там используются
// только batteryNetPowerKw/gridNetPowerKw/genPowerKw), но она бесценна для
// отладки и для отображения "откуда куда шла энергия" в GUI.
struct DispatchInterval
{
    QDateTime timestamp;
    double durationHours = 1.0;

    // --- Итоговые команды (то, что попадает в InverterCommandMapper) ---------
    double batteryNetPowerKw = 0.0; // + заряд, - разряд
    double gridNetPowerKw    = 0.0; // + экспорт, - импорт
    double genPowerKw        = 0.0; // >= 0

    // --- Разбивка потоков мощности для диагностики/GUI ------------------------
    double solarToLoadKw      = 0.0;
    double solarToBatteryKw   = 0.0;
    double solarToGridKw      = 0.0;
    double solarCurtailedKw   = 0.0; // излишки солнца, которые некуда деть
    double batteryToLoadKw    = 0.0;
    double batteryToGridKw    = 0.0;
    double gridToLoadKw       = 0.0; // импорт, идущий на нагрузку
    double gridToBatteryKw    = 0.0; // импорт, идущий на заряд (ценовой арбитраж)
    double genToLoadKw        = 0.0;
    double genToBatteryKw     = 0.0;
    double unservedLoadKw     = 0.0; // должно быть 0 при разумных настройках;
                                      // > 0 означает физическую невозможность
                                      // покрыть нагрузку (см. README)

    // --- Состояние СНЭ ---------------------------------------------------------
    double socStartFrac = 0.0;  // SOC на начало интервала, доля [0..1]
    double socEndFrac   = 0.0;  // SOC на конец интервала, доля [0..1]
    double socEndKwh    = 0.0;  // SOC на конец интервала, кВт*ч
    double socFloorUsedFrac = 0.0; // фактически применённый нижний предел SOC
                                    // в этом интервале (socMin+socReserve, либо
                                    // socMin в аварийном режиме, см. шаг 5)

    // --- Признаки для маппера регистров и для GUI ------------------------------
    bool gridChargeUsed    = false; // заряд от сети использовался (ценовой арбитраж)
    bool genChargeUsed     = false; // заряд от генератора использовался
    bool genRunning        = false; // генератор запущен в этом интервале (план
                                     // оптимизатора - для расчёта стоимости/
                                     // энергобаланса и диагностики/GUI; на
                                     // Gen Signal регистра 2065 НЕ влияет, см.
                                     // InverterCommandMapper)
    bool exportUsed        = false; // был экспорт в сеть (солнце и/или батарея)
    bool batteryExportUsed = false; // экспорт включал разряд батареи
    bool emergencyDischarge = false; // сработал аварийный доразряд (шаг 5)
    bool arbitrageCharge    = false; // сработал ценовой арбитраж (шаг 7)

    PriceClass priceClass = PriceClass::Normal;

    double intervalCostRub = 0.0; // стоимость интервала (для диагностики и суммирования)
};

using DispatchPlan = QVector<DispatchInterval>;

// Метод оптимизации, выбираемый пользователем в GUI.
enum class DispatchMethod
{
    Heuristic, // эвристический merit-order оптимизатор на скользящем горизонте
    Milp       // MILP-солвер HiGHS
};

// Полный запрос на расчёт диспетчеризации горизонта. Формируется DispatchEngine
// перед каждым вызовом оптимизатора (см. DispatchEngine::recomputeNow()).
struct DispatchRequest
{
    ForecastHorizon horizon;      // прогноз, ресэмплированный на период управления
    double initialSocFrac = 0.5;  // текущий измеренный SOC (доля от ёмкости), reg 3088
    SystemConfig config;
    DispatchPermissions permissions;
    int controlPeriodMinutes = 15; // период оптимизации, 10..60 минут
};
