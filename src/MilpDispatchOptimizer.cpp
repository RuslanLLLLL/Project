#include "dispatcher/MilpDispatchOptimizer.h"

#include <QDebug>

#include "dispatcher/HeuristicDispatchOptimizer.h"

bool MilpDispatchOptimizer::isHighsAvailable()
{
#ifdef ENABLE_HIGHS
    return true;
#else
    return false;
#endif
}

#ifndef ENABLE_HIGHS

// ---------------------------------------------------------------------------
// Сборка без HiGHS: см. комментарий в заголовке. Это позволяет разрабатывать и
// тестировать GUI/DispatchEngine, не устанавливая HiGHS, и подключить его позже
// без изменений в остальном коде (см. README, "Интеграция HiGHS").
// ---------------------------------------------------------------------------
DispatchPlan MilpDispatchOptimizer::computeDispatch(const DispatchRequest &request)
{
    qWarning() << "MilpDispatchOptimizer: HiGHS не подключён к сборке (ENABLE_HIGHS не "
                  "определён). См. README, раздел 'Интеграция HiGHS'. Используется "
                  "резервный эвристический расчёт.";
    HeuristicDispatchOptimizer fallback;
    return fallback.computeDispatch(request);
}

#else // ENABLE_HIGHS

#include <algorithm>
#include <limits>

#include <Highs.h>

namespace
{
// Индексы столбцов (переменных) HiGHS для одного интервала горизонта. Индексы
// абсолютные (сквозные по всей модели), заполняются при добавлении столбцов в
// buildAndSolve(). Явные имена полей вместо "магических" смещений сделаны для
// читаемости - при 14 переменных на интервал их слишком легко перепутать.
struct IntervalColumns
{
    HighsInt fSolarLoad = -1;
    HighsInt fSolarBatt = -1;
    HighsInt fSolarGrid = -1;
    HighsInt fSolarCurtail = -1;
    HighsInt fGridLoad = -1;
    HighsInt fGridBatt = -1;
    HighsInt fGenLoad = -1;
    HighsInt fGenBatt = -1;
    HighsInt fBattLoad = -1;
    HighsInt fBattGrid = -1;
    HighsInt pUnserved = -1;
    HighsInt soc = -1;   // SOC (кВт*ч) на КОНЕЦ интервала
    HighsInt bChg = -1;  // бинарный признак "СНЭ заряжается в этом интервале"
    HighsInt bDis = -1;  // бинарный признак "СНЭ разряжается в этом интервале"
};

const double kInf = kHighsInf;

// Строит модель HiGHS для заданного нижнего предела SOC (socFloorFrac) и
// решает её. Вынесено в отдельную функцию, т.к. вызывается дважды: сперва с
// "мягким" пределом socMin+socReserve, а при инфизибильности - повторно с
// "аварийным" пределом socMin (см. README, "Резервный SOC и релаксация").
bool solveOnce(const DispatchRequest &request, double socFloorFrac, Highs &highs,
                QVector<IntervalColumns> &cols, HighsModelStatus &status)
{
    const auto &horizon = request.horizon;
    const SystemConfig &cfg = request.config;
    const DispatchPermissions &perm = request.permissions;
    const int n = horizon.size();

    const double chargeEff = std::max(1e-6, cfg.chargeEfficiency);
    const double dischargeEff = std::max(1e-6, cfg.dischargeEfficiency);
    const double degCost = degradationCostPerKwh(cfg);
    const double capKwh = cfg.batteryCapacityKwh;
    const double socFloorKwh = socFloorFrac * capKwh;
    const double socCeilKwh = cfg.socMax * capKwh;
    const double soc0Kwh = std::clamp(request.initialSocFrac, 0.0, 1.0) * capKwh;

    highs.clearModel();
    cols = QVector<IntervalColumns>(n); // сбрасываем на случай повторного вызова (релаксация SOC)

    HighsModel model;
    HighsLp &lp = model.lp_;
    lp.num_col_ = 0;
    lp.num_row_ = 0;
    lp.sense_ = ObjSense::kMinimize;

    // Вспомогательная лямбда: добавляет столбец (переменную) [lower,upper] с
    // коэффициентом цели cost и возвращает его индекс.
    auto addCol = [&](double lower, double upper, double cost) -> HighsInt {
        lp.col_cost_.push_back(cost);
        lp.col_lower_.push_back(lower);
        lp.col_upper_.push_back(upper);
        return lp.num_col_++;
    };

    for (int i = 0; i < n; ++i)
    {
        const auto &f = horizon[i];
        const double dt = f.durationHours > 0.0 ? f.durationHours : 1.0;
        IntervalColumns c;

        c.fSolarLoad = addCol(0.0, f.solarKw, 0.0);
        c.fSolarBatt = addCol(0.0, f.solarKw, dt * degCost);
        c.fSolarGrid = addCol(0.0, f.solarKw, -dt * f.sellPrice);
        c.fSolarCurtail = addCol(0.0, f.solarKw, 0.0);

        const bool gridImportAllowed = f.gridAvailable;
        const bool gridExportAllowed = f.gridAvailable && perm.m_sollSell;
        // Стоимость покупки - ЭФФЕКТИВНАЯ цена (с учётом стоимости
        // транспортировки, см. SystemConfig::effectiveBuyPrice); к продаже
        // (fSolarGrid/fBattGrid выше/ниже) эта надбавка не применяется.
        const double buyPriceEff = effectiveBuyPrice(cfg, f.buyPrice);
        c.fGridLoad = addCol(0.0, gridImportAllowed ? cfg.gridMaxImportKw : 0.0, dt * buyPriceEff);
        c.fGridBatt = addCol(0.0, gridImportAllowed ? cfg.gridMaxImportKw : 0.0, dt * (buyPriceEff + degCost));

        const bool genAllowed = perm.m_genEnable;
        c.fGenLoad = addCol(0.0, genAllowed ? cfg.genMaxPowerKw : 0.0, dt * cfg.genCostPerKwh);
        const bool genChargeAllowed = genAllowed && perm.m_genChargeEnable;
        c.fGenBatt = addCol(0.0, genChargeAllowed ? cfg.genMaxPowerKw : 0.0,
                             dt * (cfg.genCostPerKwh + degCost));

        c.fBattLoad = addCol(0.0, cfg.batteryMaxDischargeKw, dt * degCost);
        const bool battExportAllowed = gridExportAllowed && perm.m_sollSellOfBatteries;
        c.fBattGrid = addCol(0.0, battExportAllowed ? cfg.batteryMaxDischargeKw : 0.0,
                              dt * (degCost - f.sellPrice));

        c.pUnserved = addCol(0.0, f.loadKw, dt * cfg.lossOfLoadPenaltyPerKwh);

        c.soc = addCol(socFloorKwh, socCeilKwh, 0.0);

        c.bChg = addCol(0.0, 1.0, 0.0);
        c.bDis = addCol(0.0, 1.0, 0.0);

        cols[i] = c;
    }

    lp.integrality_.assign(lp.num_col_, HighsVarType::kContinuous);
    for (int i = 0; i < n; ++i)
    {
        lp.integrality_[cols[i].bChg] = HighsVarType::kInteger;
        lp.integrality_[cols[i].bDis] = HighsVarType::kInteger;
    }

    // Передаём каркас модели (только столбцы) в HiGHS, после чего добавляем
    // строки через инкрементальный API Highs::addRow (это проще и надёжнее,
    // чем вручную собирать разреженную матрицу в формате CSR/CSC).
    lp.a_matrix_.format_ = MatrixFormat::kColwise;
    lp.a_matrix_.start_.assign(lp.num_col_ + 1, 0);
    lp.a_matrix_.index_.clear();
    lp.a_matrix_.value_.clear();

    HighsStatus st = highs.passModel(model);
    if (st == HighsStatus::kError)
        return false;

    for (int i = 0; i < n; ++i)
    {
        const auto &f = horizon[i];
        const auto &c = cols[i];
        const double dt = f.durationHours > 0.0 ? f.durationHours : 1.0;

        // R1: баланс солнца (равенство) = solarKw[i]
        highs.addRow(f.solarKw, f.solarKw, 4,
                     std::vector<HighsInt>{c.fSolarLoad, c.fSolarBatt, c.fSolarGrid, c.fSolarCurtail}.data(),
                     std::vector<double>{1.0, 1.0, 1.0, 1.0}.data());

        // R2: баланс нагрузки (равенство) = loadKw[i], с "предохранителем" pUnserved
        highs.addRow(f.loadKw, f.loadKw, 5,
                     std::vector<HighsInt>{c.fSolarLoad, c.fGridLoad, c.fGenLoad, c.fBattLoad, c.pUnserved}
                         .data(),
                     std::vector<double>{1.0, 1.0, 1.0, 1.0, 1.0}.data());

        // R3: лимит импорта из сети
        highs.addRow(0.0, f.gridAvailable ? cfg.gridMaxImportKw : 0.0, 2,
                     std::vector<HighsInt>{c.fGridLoad, c.fGridBatt}.data(),
                     std::vector<double>{1.0, 1.0}.data());

        // R4: лимит экспорта в сеть
        const double exportCap = (f.gridAvailable && perm.m_sollSell) ? cfg.gridMaxExportKw : 0.0;
        highs.addRow(0.0, exportCap, 2, std::vector<HighsInt>{c.fSolarGrid, c.fBattGrid}.data(),
                     std::vector<double>{1.0, 1.0}.data());

        // R5: лимит генератора
        highs.addRow(0.0, perm.m_genEnable ? cfg.genMaxPowerKw : 0.0, 2,
                     std::vector<HighsInt>{c.fGenLoad, c.fGenBatt}.data(),
                     std::vector<double>{1.0, 1.0}.data());

        // R6: лимит мощности заряда СНЭ, увязанный с бинарным признаком bChg
        highs.addRow(-kInf, 0.0, 4,
                     std::vector<HighsInt>{c.fSolarBatt, c.fGridBatt, c.fGenBatt, c.bChg}.data(),
                     std::vector<double>{1.0, 1.0, 1.0, -cfg.batteryMaxChargeKw}.data());

        // R7: лимит мощности разряда СНЭ, увязанный с бинарным признаком bDis
        highs.addRow(-kInf, 0.0, 3, std::vector<HighsInt>{c.fBattLoad, c.fBattGrid, c.bDis}.data(),
                     std::vector<double>{1.0, 1.0, -cfg.batteryMaxDischargeKw}.data());

        // R8: заряд и разряд не одновременно
        highs.addRow(-kInf, 1.0, 2, std::vector<HighsInt>{c.bChg, c.bDis}.data(),
                     std::vector<double>{1.0, 1.0}.data());

        // R9: динамика SOC (равенство):
        //   soc[i] - soc[i-1] - dt*chargeEff*(fSolarBatt+fGridBatt+fGenBatt)
        //          + (dt/dischargeEff)*(fBattLoad+fBattGrid) = 0
        // Для i=0 предыдущий SOC - известная константа soc0Kwh (текущий
        // измеренный SOC), поэтому soc[-1] в уравнении отсутствует, а сама
        // константа переносится в границы строки.
        if (i == 0)
        {
            highs.addRow(soc0Kwh, soc0Kwh, 6,
                         std::vector<HighsInt>{c.soc, c.fSolarBatt, c.fGridBatt, c.fGenBatt, c.fBattLoad,
                                                c.fBattGrid}
                             .data(),
                         std::vector<double>{1.0, -dt * chargeEff, -dt * chargeEff, -dt * chargeEff,
                                              dt / dischargeEff, dt / dischargeEff}
                             .data());
        }
        else
        {
            highs.addRow(0.0, 0.0, 7,
                         std::vector<HighsInt>{c.soc, cols[i - 1].soc, c.fSolarBatt, c.fGridBatt, c.fGenBatt,
                                                c.fBattLoad, c.fBattGrid}
                             .data(),
                         std::vector<double>{1.0, -1.0, -dt * chargeEff, -dt * chargeEff, -dt * chargeEff,
                                              dt / dischargeEff, dt / dischargeEff}
                             .data());
        }
    }

    highs.setOptionValue("output_flag", false);
    highs.setOptionValue("mip_rel_gap", 1e-4);
    st = highs.run();
    if (st == HighsStatus::kError)
        return false;

    status = highs.getModelStatus();
    return true;
}
} // namespace

DispatchPlan MilpDispatchOptimizer::computeDispatch(const DispatchRequest &request)
{
    DispatchPlan plan;
    const auto &horizon = request.horizon;
    if (horizon.isEmpty())
        return plan;

    const SystemConfig &cfg = request.config;

    Highs highs;
    QVector<IntervalColumns> cols;
    HighsModelStatus status = HighsModelStatus::kNotset;

    // Первая попытка: SOC не должен опускаться ниже "мягкого" эксплуатационного
    // резерва socMin+socReserve.
    double socFloorUsedFrac = cfg.socMin + cfg.socReserve;
    bool ok = solveOnce(request, socFloorUsedFrac, highs, cols, status);

    // Нужна ли релаксация до аварийного предела socMin? Два случая:
    //   (а) с мягким пределом решения не существует вовсе (infeasible);
    //   (б) решение существует, но задействован "предохранитель" pUnserved -
    //       т.е. модель предпочла НЕДООТПУСК нагрузки (штраф
    //       lossOfLoadPenaltyPerKwh) вместо того, чтобы использовать
    //       эксплуатационный резерв socReserve, потому что на первом проходе
    //       резерв - это ЖЁСТКАЯ граница, а не вариант с штрафом. Без этой
    //       проверки модель никогда не пришла бы к выводу "лучше распечатать
    //       резерв, чем недоотпустить нагрузку" - см. README, "Резервный SOC и
    //       релаксация".
    bool needsRelaxation = !ok || status != HighsModelStatus::kOptimal;
    if (ok && status == HighsModelStatus::kOptimal)
    {
        const std::vector<double> &x = highs.getSolution().col_value;
        double totalUnserved = 0.0;
        for (const auto &c : cols)
            totalUnserved += x[c.pUnserved];
        if (totalUnserved > 1e-6)
            needsRelaxation = true;
    }

    if (needsRelaxation)
    {
        qWarning() << "MilpDispatchOptimizer: с резервом SOC решение требует недоотпуска нагрузки "
                      "(статус"
                   << int(status) << "), повторный расчёт с аварийным пределом socMin.";
        socFloorUsedFrac = cfg.socMin;
        ok = solveOnce(request, socFloorUsedFrac, highs, cols, status);
    }

    if (!ok || status != HighsModelStatus::kOptimal)
    {
        qWarning() << "MilpDispatchOptimizer: MILP не решена (статус" << int(status)
                   << "), возвращён пустой план. Проверьте входные данные/ограничения.";
        return plan;
    }

    const HighsSolution &sol = highs.getSolution();
    const std::vector<double> &x = sol.col_value;

    double capKwh = cfg.batteryCapacityKwh;
    double socPrevKwh = std::clamp(request.initialSocFrac, 0.0, 1.0) * capKwh;

    plan.reserve(horizon.size());
    for (int i = 0; i < horizon.size(); ++i)
    {
        const auto &f = horizon[i];
        const auto &c = cols[i];
        const double dt = f.durationHours > 0.0 ? f.durationHours : 1.0;

        DispatchInterval out;
        out.timestamp = f.timestamp;
        out.durationHours = dt;

        out.solarToLoadKw = x[c.fSolarLoad];
        out.solarToBatteryKw = x[c.fSolarBatt];
        out.solarToGridKw = x[c.fSolarGrid];
        out.solarCurtailedKw = x[c.fSolarCurtail];
        out.gridToLoadKw = x[c.fGridLoad];
        out.gridToBatteryKw = x[c.fGridBatt];
        out.genToLoadKw = x[c.fGenLoad];
        out.genToBatteryKw = x[c.fGenBatt];
        out.batteryToLoadKw = x[c.fBattLoad];
        out.batteryToGridKw = x[c.fBattGrid];
        out.unservedLoadKw = x[c.pUnserved];

        out.socStartFrac = socPrevKwh / capKwh;
        out.socEndKwh = x[c.soc];
        out.socEndFrac = out.socEndKwh / capKwh;
        out.socFloorUsedFrac = socFloorUsedFrac; // нижний предел SOC, с которым решалась MILP
        socPrevKwh = out.socEndKwh;

        out.gridChargeUsed = out.gridToBatteryKw > 1e-6;
        out.genChargeUsed = out.genToBatteryKw > 1e-6;
        out.genRunning = (out.genToLoadKw + out.genToBatteryKw) > 1e-6;
        out.exportUsed = (out.solarToGridKw + out.batteryToGridKw) > 1e-6;
        out.batteryExportUsed = out.batteryToGridKw > 1e-6;
        out.arbitrageCharge = out.gridChargeUsed; // в MILP нет отдельной фазы "арбитраж" -
                                                   // это просто оптимальный результат
        out.emergencyDischarge = false; // MILP не различает "аварийный" и "плановый" разряд -
                                         // оба являются частью единого глобального оптимума

        out.batteryNetPowerKw =
            (out.solarToBatteryKw + out.gridToBatteryKw + out.genToBatteryKw) -
            (out.batteryToLoadKw + out.batteryToGridKw);
        out.gridNetPowerKw =
            (out.solarToGridKw + out.batteryToGridKw) - (out.gridToLoadKw + out.gridToBatteryKw);
        out.genPowerKw = out.genToLoadKw + out.genToBatteryKw;

        const double buyKw = out.gridToLoadKw + out.gridToBatteryKw;
        const double sellKw = out.solarToGridKw + out.batteryToGridKw;
        const double throughputKwh =
            (out.solarToBatteryKw + out.gridToBatteryKw + out.genToBatteryKw + out.batteryToLoadKw +
             out.batteryToGridKw) *
            dt;
        out.intervalCostRub = dt * (effectiveBuyPrice(cfg, f.buyPrice) * buyKw - f.sellPrice * sellKw +
                                     cfg.genCostPerKwh * out.genPowerKw) +
                              degradationCostPerKwh(cfg) * throughputKwh +
                              dt * cfg.lossOfLoadPenaltyPerKwh * out.unservedLoadKw;

        plan.push_back(out);
    }

    return plan;
}

#endif // ENABLE_HIGHS
