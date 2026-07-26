#pragma once

#include "IDispatchOptimizer.h"

// Эвристический (merit-order) оптимизатор диспетчеризации на скользящем
// горизонте. Реализует 7 шагов приоритета использования источников энергии,
// описанные в README (раздел "Эвристический алгоритм"):
//
//   1) солнце -> нагрузка
//   2) излишки солнца -> СНЭ, остаток -> продажа в сеть
//   3) разряд СНЭ в дорогие часы (если выгода превышает стоимость цикла)
//   4) покупка из сети
//   5) аварийный доразряд СНЭ при исчерпании лимита/недоступности сети
//   6) резервный генератор
//   7) ценовой арбитраж: заряд СНЭ от сети в дешёвые часы, если это окупается
//      разницей с дорогими часами горизонта
//
// Метод не требует внешних библиотек (в отличие от MilpDispatchOptimizer) и
// работает за время O(N log N) от числа интервалов горизонта N — пригоден для
// встраивания без HiGHS, как маршрут "по умолчанию".
class HeuristicDispatchOptimizer : public IDispatchOptimizer
{
public:
    DispatchPlan computeDispatch(const DispatchRequest &request) override;
    QString name() const override { return QStringLiteral("Heuristic (merit-order)"); }

private:
    // Шаг 7 выполняется как отдельный предварительный проход по всему горизонту
    // (см. .cpp) до основного хронологического цикла, т.к. требует глобального
    // знания цен всего горизонта (в отличие от шагов 1-6, которые применяются
    // последовательно интервал за интервалом). Результат прохода — план заряда
    // от сети по интервалам, накладываемый поверх заряда от солнца на шаге 2.
    struct ArbitragePlan
    {
        QVector<double> gridChargeKw; // по одному значению на каждый интервал горизонта
    };

    // cheapThreshold/expensiveThreshold - абсолютные цены (руб/кВт*ч), полученные
    // из перцентилей buyPrice по всему горизонту (см. .cpp, computePriceThresholds).
    static ArbitragePlan planPriceArbitrage(const DispatchRequest &request,
                                             double cheapThreshold,
                                             double expensiveThreshold);
};
