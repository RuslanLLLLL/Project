#pragma once

#include "IDispatchOptimizer.h"

// MilpDispatchOptimizer формулирует диспетчеризацию горизонта как задачу
// смешанно-целочисленного линейного программирования (MILP) и решает её
// солвером HiGHS (https://highs.dev). Полная математическая формулировка -
// в README, раздел "MILP-модель".
//
// Класс скомпилирован ВСЕГДА (даже без HiGHS), чтобы GUI мог предлагать выбор
// метода без падения сборки. Если библиотека HiGHS не подключена (макрос
// ENABLE_HIGHS не определён сборочной системой - см. CMakeLists.txt), метод
// computeDispatch() логирует предупреждение через qWarning() и прозрачно
// делегирует расчёт эвристическому оптимизатору, чтобы GUI не оставался без
// результата. Как только HiGHS подключён (см. README, "Интеграция HiGHS"),
// пересобранный проект автоматически начинает использовать настоящий MILP.
class MilpDispatchOptimizer : public IDispatchOptimizer
{
public:
    DispatchPlan computeDispatch(const DispatchRequest &request) override;
    QString name() const override { return QStringLiteral("MILP (HiGHS)"); }

    // Признак того, что бинарный HiGHS реально подключён (true) или используется
    // резервный вызов эвристики (false) - полезно для отображения в GUI, чтобы
    // не вводить пользователя в заблуждение относительно того, какой метод
    // реально считает график.
    static bool isHighsAvailable();
};
