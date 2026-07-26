#pragma once

#include <QString>

#include "DispatchTypes.h"

// Общий интерфейс для двух взаимозаменяемых методов диспетчеризации:
//   - HeuristicDispatchOptimizer (merit-order эвристика, см. README §2)
//   - MilpDispatchOptimizer      (MILP через HiGHS, см. README §3)
//
// DispatchEngine хранит указатель на IDispatchOptimizer и вызывает
// computeDispatch() заново на каждом шаге скользящего окна (см. README §1,
// "Скользящий горизонт"), передавая обновлённый прогноз и текущий SOC.
// Реализации должны быть детерминированными и не иметь внутреннего состояния,
// зависящего от предыдущих вызовов (весь необходимый контекст приходит через
// DispatchRequest::initialSocFrac) — это то, что и делает управление "скользящим
// горизонтом" (rolling horizon control) корректным: пересчёт не зависит от
// истории, только от текущего измеренного состояния и свежего прогноза.
class IDispatchOptimizer
{
public:
    virtual ~IDispatchOptimizer() = default;

    // Рассчитывает почасовой (точнее, по интервалам управления) график
    // диспетчеризации на весь горизонт запроса. Возвращает пустой вектор,
    // если request.horizon пуст.
    virtual DispatchPlan computeDispatch(const DispatchRequest &request) = 0;

    // Человекочитаемое имя метода (для логов и GUI).
    virtual QString name() const = 0;
};
