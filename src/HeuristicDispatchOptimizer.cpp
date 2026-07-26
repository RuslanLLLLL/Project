#include "dispatcher/HeuristicDispatchOptimizer.h"

#include <algorithm>
#include <cmath>

namespace
{
// -----------------------------------------------------------------------------
// Перцентиль по линейной интерполяции (тот же метод, что и numpy.percentile по
// умолчанию / Excel PERCENTILE.INC). Используется, чтобы разделить часы горизонта
// на "дешёвые"/"дорогие" по цене покупки (см. README, формулы (1)-(2)).
// -----------------------------------------------------------------------------
double percentile(QVector<double> sortedAscending, double p)
{
    if (sortedAscending.isEmpty())
        return 0.0;
    if (sortedAscending.size() == 1)
        return sortedAscending.first();

    p = std::clamp(p, 0.0, 1.0);
    const double rank = p * static_cast<double>(sortedAscending.size() - 1);
    const int lo = static_cast<int>(std::floor(rank));
    const int hi = static_cast<int>(std::ceil(rank));
    if (lo == hi)
        return sortedAscending[lo];
    const double frac = rank - lo;
    return sortedAscending[lo] * (1.0 - frac) + sortedAscending[hi] * frac;
}

struct PriceThresholds
{
    double cheap = 0.0;
    double expensive = 0.0;
};

PriceThresholds computePriceThresholds(const ForecastHorizon &horizon, const SystemConfig &cfg)
{
    QVector<double> prices;
    prices.reserve(horizon.size());
    for (const auto &h : horizon)
        prices.push_back(h.buyPrice);
    std::sort(prices.begin(), prices.end());

    PriceThresholds t;
    t.cheap = percentile(prices, cfg.cheapPricePercentile);
    t.expensive = percentile(prices, cfg.expensivePricePercentile);
    return t;
}

// Приводит значение в диапазон [0, +inf) - вспомогательная функция, чтобы не
// заносить в план отрицательные мощности из-за накопления погрешностей округления.
double nonNegative(double v)
{
    return v > 0.0 ? v : 0.0;
}
} // namespace

// =============================================================================
// Шаг 7 (ценовой арбитраж) - предварительный проход по всему горизонту.
//
// Идея: находим пары (дешёвый интервал i, дорогой интервал j>i), в которых
// выгодно зарядить СНЭ от сети в i, чтобы разрядить её в j. Обрабатываем дешёвые
// интервалы в порядке возрастания цены (самые выгодные для заряда - первыми),
// а для каждого из них - дорогие интервалы (позже по времени) в порядке убывания
// цены (самые выгодные для разряда - первыми). Как только очередная пара
// становится невыгодной, дальнейший перебор для данного дешёвого интервала
// прекращаем: раз "лучшая из оставшихся" пара невыгодна, все остальные (более
// дешёвая продажа или менее дешёвая покупка) тем более невыгодны.
//
// Прибыль на 1 кВт*ч энергии, взятой из сети (E_grid) для последующей продажи/
// использования в дорогой час (см. README, формула (7)):
//
//   profitPerKwh = chargeEff * dischargeEff * priceExpensive
//                  - priceCheap
//                  - degradationCostPerKwh * (1 + chargeEff * dischargeEff)
//
// Первое слагаемое - деньги, сэкономленные на покупке в дорогой час (с учётом
// того, что из E_grid кВт*ч, взятых из сети, до нагрузки/продажи дойдёт только
// E_grid*chargeEff*dischargeEff кВт*ч). Второе - стоимость самой покупки в
// дешёвый час. Третье - деградационная стоимость СНЭ, приложенная к обеим
// операциям (заряд E_grid кВт*ч и последующий разряд E_grid*chargeEff*dischargeEff
// кВт*ч).
//
// Результат прохода - это ЗАЯВКА на заряд от сети по интервалам; она не является
// жёсткой командой: основной хронологический цикл (computeDispatch) наложит
// поверх неё реальные ограничения по мощности заряда и по фактической траектории
// SOC (которая на момент планирования arbitrage ещё не известна точно, т.к.
// зависит и от заряда солнцем). Такое разделение - осознанное упрощение
// эвристики; точный совместный расчёт даёт MILP-оптимизатор.
// =============================================================================
HeuristicDispatchOptimizer::ArbitragePlan HeuristicDispatchOptimizer::planPriceArbitrage(
    const DispatchRequest &request, double cheapThreshold, double expensiveThreshold)
{
    const auto &horizon = request.horizon;
    const SystemConfig &cfg = request.config;
    const double chargeEff = cfg.chargeEfficiency;
    const double dischargeEff = cfg.dischargeEfficiency;
    const double degCost = degradationCostPerKwh(cfg);

    ArbitragePlan plan;
    plan.gridChargeKw.fill(0.0, horizon.size());

    // Индексы дешёвых интервалов (по возрастанию цены) и дорогих (по убыванию).
    QVector<int> cheapIdx, expensiveIdx;
    for (int i = 0; i < horizon.size(); ++i)
    {
        if (!horizon[i].gridAvailable)
            continue; // нельзя ни зарядить от сети, ни (в рамках этого прохода) учитывать как цель продажи
        if (horizon[i].buyPrice <= cheapThreshold)
            cheapIdx.push_back(i);
        if (horizon[i].buyPrice >= expensiveThreshold)
            expensiveIdx.push_back(i);
    }
    std::sort(cheapIdx.begin(), cheapIdx.end(), [&](int a, int b) {
        return horizon[a].buyPrice < horizon[b].buyPrice;
    });
    std::sort(expensiveIdx.begin(), expensiveIdx.end(), [&](int a, int b) {
        return horizon[a].buyPrice > horizon[b].buyPrice;
    });

    // Остаточная мощность заряда каждого дешёвого интервала (кВт) - изначально
    // ограничена только паспортной мощностью заряда СНЭ; фактические ограничения
    // по одновременному заряду от солнца учтёт основной цикл.
    QVector<double> cheapRemainingKw(horizon.size(), 0.0);
    for (int i : cheapIdx)
        cheapRemainingKw[i] = cfg.batteryMaxChargeKw;

    // Остаточная ёмкость разряда каждого дорогого интервала, выраженная в кВт*ч
    // "доставленной" (после двойного КПД) энергии.
    QVector<double> expensiveRemainingKwh(horizon.size(), 0.0);
    for (int i : expensiveIdx)
        expensiveRemainingKwh[i] = cfg.batteryMaxDischargeKw * horizon[i].durationHours;

    // Общий бюджет энергии, которую вообще можно "прокачать" через СНЭ туда-обратно
    // в пределах рабочего диапазона SOC - грубая (но консервативная) оценка сверху,
    // не учитывающая параллельный заряд от солнца (опять же, скорректируется в
    // основном цикле).
    double arbitrageBudgetGridKwh =
        (cfg.socMax - cfg.socMin - cfg.socReserve) * cfg.batteryCapacityKwh / std::max(1e-6, chargeEff);

    for (int ci : cheapIdx)
    {
        if (arbitrageBudgetGridKwh <= 1e-9)
            break;
        const double cheapPrice = horizon[ci].buyPrice;

        for (int ei : expensiveIdx)
        {
            if (ei <= ci)
                continue; // причинность: заряжаем раньше, чем разряжаем
            if (expensiveRemainingKwh[ei] <= 1e-9)
                continue;
            if (cheapRemainingKw[ci] <= 1e-9)
                break;

            const double expensivePrice = horizon[ei].buyPrice;
            const double profitPerKwh = chargeEff * dischargeEff * expensivePrice - cheapPrice -
                                         degCost * (1.0 + chargeEff * dischargeEff);
            if (profitPerKwh <= 0.0)
                break; // ei отсортированы по убыванию цены -> дальше только хуже

            // Сколько кВт*ч со стороны сети (E_grid) можно выделить на эту пару,
            // ограничившись: остаточной мощностью заряда дешёвого интервала,
            // остаточной ёмкостью разряда дорогого интервала (в пересчёте на
            // сторону сети) и общим бюджетом.
            const double cheapCapKwh = cheapRemainingKw[ci] * horizon[ci].durationHours;
            const double expensiveCapAsGridKwh =
                expensiveRemainingKwh[ei] / std::max(1e-6, chargeEff * dischargeEff);
            const double allocGridKwh =
                std::min({cheapCapKwh, expensiveCapAsGridKwh, arbitrageBudgetGridKwh});
            if (allocGridKwh <= 1e-9)
                continue;

            plan.gridChargeKw[ci] += allocGridKwh / horizon[ci].durationHours;
            cheapRemainingKw[ci] -= allocGridKwh / horizon[ci].durationHours;
            expensiveRemainingKwh[ei] -= allocGridKwh * chargeEff * dischargeEff;
            arbitrageBudgetGridKwh -= allocGridKwh;
        }
    }

    return plan;
}

// =============================================================================
// Основной хронологический цикл: применяет шаги 1-6 к каждому интервалу по
// порядку, с уже наложенной "заявкой" шага 7 (арбитражный заряд от сети).
// =============================================================================
DispatchPlan HeuristicDispatchOptimizer::computeDispatch(const DispatchRequest &request)
{
    DispatchPlan plan;
    const auto &horizon = request.horizon;
    if (horizon.isEmpty())
        return plan;

    const SystemConfig &cfg = request.config;
    const DispatchPermissions &perm = request.permissions;
    const double capKwh = cfg.batteryCapacityKwh;
    const double chargeEff = cfg.chargeEfficiency;
    const double dischargeEff = cfg.dischargeEfficiency;
    const double degCost = degradationCostPerKwh(cfg);

    const PriceThresholds thresholds = computePriceThresholds(horizon, cfg);
    const ArbitragePlan arbitrage = planPriceArbitrage(request, thresholds.cheap, thresholds.expensive);

    double soc = std::clamp(request.initialSocFrac, 0.0, 1.0) * capKwh;

    plan.reserve(horizon.size());
    for (int i = 0; i < horizon.size(); ++i)
    {
        const DispatchIntervalForecast &f = horizon[i];
        const double dt = f.durationHours > 0.0 ? f.durationHours : 1.0;

        DispatchInterval out;
        out.timestamp = f.timestamp;
        out.durationHours = dt;
        out.socStartFrac = soc / capKwh;
        out.priceClass = (f.buyPrice <= thresholds.cheap)
                              ? PriceClass::Cheap
                              : (f.buyPrice >= thresholds.expensive ? PriceClass::Expensive : PriceClass::Normal);

        const double normalFloorKwh = (cfg.socMin + cfg.socReserve) * capKwh;
        const double emergencyFloorKwh = cfg.socMin * capKwh;
        const double ceilingKwh = cfg.socMax * capKwh;
        out.socFloorUsedFrac = cfg.socMin + cfg.socReserve;

        // ---- Шаг 1: солнце -> нагрузка -------------------------------------
        double remSolar = f.solarKw;
        double remLoad = f.loadKw;
        out.solarToLoadKw = std::min(remSolar, remLoad);
        remSolar -= out.solarToLoadKw;
        remLoad -= out.solarToLoadKw;

        // ---- Шаг 2: излишки солнца -> СНЭ -----------------------------------
        {
            const double headroomKwh = nonNegative(ceilingKwh - soc);
            const double maxByHeadroom = (chargeEff * dt) > 0.0 ? headroomKwh / (chargeEff * dt)
                                                                 : 0.0;
            out.solarToBatteryKw = std::min({remSolar, cfg.batteryMaxChargeKw, maxByHeadroom});
            out.solarToBatteryKw = nonNegative(out.solarToBatteryKw);
            soc += out.solarToBatteryKw * chargeEff * dt;
            remSolar -= out.solarToBatteryKw;
        }

        // ---- Шаг 7 (применение заявки): арбитражный заряд от сети -----------
        double gridChargeKw = 0.0;
        if (f.gridAvailable && arbitrage.gridChargeKw[i] > 0.0)
        {
            const double headroomKwh = nonNegative(ceilingKwh - soc);
            const double maxByHeadroom = (chargeEff * dt) > 0.0 ? headroomKwh / (chargeEff * dt) : 0.0;
            const double remainingChargePowerKw = nonNegative(cfg.batteryMaxChargeKw - out.solarToBatteryKw);
            gridChargeKw = std::min({arbitrage.gridChargeKw[i], remainingChargePowerKw, maxByHeadroom,
                                      cfg.gridMaxImportKw});
            gridChargeKw = nonNegative(gridChargeKw);
            soc += gridChargeKw * chargeEff * dt;
            out.arbitrageCharge = gridChargeKw > 1e-9;
            out.gridChargeUsed = out.arbitrageCharge;
            out.gridToBatteryKw = gridChargeKw;
        }

        // ---- Остаток солнца: продажа в сеть, иначе - скос (curtailment) -----
        double exportUsedKw = 0.0;
        if (remSolar > 0.0)
        {
            if (perm.m_sollSell && f.gridAvailable)
            {
                out.solarToGridKw = std::min(remSolar, cfg.gridMaxExportKw);
                remSolar -= out.solarToGridKw;
                exportUsedKw += out.solarToGridKw;
            }
            out.solarCurtailedKw = remSolar;
        }

        // ---- Шаг 3: разряд СНЭ в дорогие часы --------------------------------
        if (remLoad > 0.0 && out.priceClass == PriceClass::Expensive)
        {
            const double benefitPerKwhDelivered = f.buyPrice - degCost / std::max(1e-6, dischargeEff);
            if (benefitPerKwhDelivered > 0.0)
            {
                const double dischargeHeadroomKwh = nonNegative(soc - normalFloorKwh);
                const double maxByHeadroom = dischargeHeadroomKwh * dischargeEff / dt;
                out.batteryToLoadKw = std::min({remLoad, cfg.batteryMaxDischargeKw, maxByHeadroom});
                out.batteryToLoadKw = nonNegative(out.batteryToLoadKw);
                soc -= (out.batteryToLoadKw * dt) / dischargeEff;
                remLoad -= out.batteryToLoadKw;
            }
        }

        // ---- Продажа разряда СНЭ в сеть (если это отдельно разрешено) -------
        if (perm.m_sollSell && perm.m_sollSellOfBatteries && f.gridAvailable &&
            out.priceClass == PriceClass::Expensive)
        {
            const double benefitPerKwhDelivered = f.sellPrice - degCost / std::max(1e-6, dischargeEff);
            if (benefitPerKwhDelivered > 0.0)
            {
                const double exportHeadroomKw = nonNegative(cfg.gridMaxExportKw - exportUsedKw);
                const double remainingDischargePowerKw =
                    nonNegative(cfg.batteryMaxDischargeKw - out.batteryToLoadKw);
                const double dischargeHeadroomKwh = nonNegative(soc - normalFloorKwh);
                const double maxByHeadroom = dischargeHeadroomKwh * dischargeEff / dt;
                out.batteryToGridKw =
                    std::min({exportHeadroomKw, remainingDischargePowerKw, maxByHeadroom});
                out.batteryToGridKw = nonNegative(out.batteryToGridKw);
                soc -= (out.batteryToGridKw * dt) / dischargeEff;
                exportUsedKw += out.batteryToGridKw;
                out.batteryExportUsed = out.batteryToGridKw > 1e-9;
            }
        }
        out.exportUsed = exportUsedKw > 1e-9;

        // ---- Шаг 4: покупка из сети ------------------------------------------
        if (remLoad > 0.0 && f.gridAvailable)
        {
            const double importHeadroomKw = nonNegative(cfg.gridMaxImportKw - gridChargeKw);
            out.gridToLoadKw = std::min(remLoad, importHeadroomKw);
            remLoad -= out.gridToLoadKw;
        }

        // ---- Шаг 5: аварийный доразряд СНЭ -----------------------------------
        if (remLoad > 0.0)
        {
            const double emergencyHeadroomKwh = nonNegative(soc - emergencyFloorKwh);
            const double maxByHeadroom = emergencyHeadroomKwh * dischargeEff / dt;
            const double remainingDischargePowerKw =
                nonNegative(cfg.batteryMaxDischargeKw - out.batteryToLoadKw - out.batteryToGridKw);
            const double extraKw = nonNegative(std::min({remLoad, remainingDischargePowerKw, maxByHeadroom}));
            if (extraKw > 1e-9)
            {
                soc -= (extraKw * dt) / dischargeEff;
                out.batteryToLoadKw += extraKw;
                remLoad -= extraKw;
                out.emergencyDischarge = true;
                out.socFloorUsedFrac = cfg.socMin;
            }
        }

        // ---- Шаг 6: резервный генератор ---------------------------------------
        if (remLoad > 0.0 && perm.m_genEnable)
        {
            out.genToLoadKw = std::min(remLoad, cfg.genMaxPowerKw);
            remLoad -= out.genToLoadKw;
            out.genRunning = out.genToLoadKw > 1e-9;
        }

        // ---- Заряд СНЭ от генератора (если генератор уже запущен и разрешено) -
        if (out.genRunning && perm.m_genChargeEnable)
        {
            const double spareGenKw = nonNegative(cfg.genMaxPowerKw - out.genToLoadKw);
            const double chargeHeadroomKw =
                nonNegative(cfg.batteryMaxChargeKw - out.solarToBatteryKw - gridChargeKw);
            const double headroomKwh = nonNegative(ceilingKwh - soc);
            const double maxByHeadroom = (chargeEff * dt) > 0.0 ? headroomKwh / (chargeEff * dt) : 0.0;
            out.genToBatteryKw = std::min({spareGenKw, chargeHeadroomKw, maxByHeadroom});
            out.genToBatteryKw = nonNegative(out.genToBatteryKw);
            soc += out.genToBatteryKw * chargeEff * dt;
            out.genChargeUsed = out.genToBatteryKw > 1e-9;
        }

        out.unservedLoadKw = nonNegative(remLoad);

        // ---- Итоговые агрегаты ------------------------------------------------
        soc = std::clamp(soc, 0.0, capKwh);
        out.socEndKwh = soc;
        out.socEndFrac = soc / capKwh;

        out.batteryNetPowerKw = (out.solarToBatteryKw + gridChargeKw + out.genToBatteryKw) -
                                 (out.batteryToLoadKw + out.batteryToGridKw);
        out.gridNetPowerKw = (out.solarToGridKw + out.batteryToGridKw) - (out.gridToLoadKw + gridChargeKw);
        out.genPowerKw = out.genToLoadKw + out.genToBatteryKw;

        const double throughputKwh =
            (out.solarToBatteryKw + gridChargeKw + out.genToBatteryKw + out.batteryToLoadKw + out.batteryToGridKw) *
            dt;
        out.intervalCostRub = dt * (f.buyPrice * (out.gridToLoadKw + gridChargeKw) -
                                     f.sellPrice * (out.solarToGridKw + out.batteryToGridKw) +
                                     cfg.genCostPerKwh * out.genPowerKw) +
                              degCost * throughputKwh;

        plan.push_back(out);
    }

    return plan;
}
