// Демонстрационный пример использования EnergyMimicWidget - анимированной
// мнемосхемы энергосистемы (см. README, раздел "Мнемосхема энергосистемы
// (EnergyMimicWidget)" за подробным описанием). Показывает два независимых
// способа получения данных виджетом:
//
//   А) "Автономный" режим - без DispatchEngine, значения приходят из любого
//      собственного источника телеметрии приложения (в примере - QTimer с
//      синтетическими данными, чтобы пример запускался без реальной БД и
//      инвертора).
//   Б) Реальное подключение к DispatchEngine::planReady() - закомментированный
//      блок ниже показывает, как пересчитать поля DispatchInterval в вызовы
//      setSystemState() с учётом разницы конвенций знака (см. README).
//
// Собирается только если включены опции CMake BUILD_GUI и BUILD_EXAMPLES.

#include <QApplication>
#include <QMainWindow>
#include <QTimer>
#include <QtMath>
#include <algorithm>
#include <cmath>

#include "EnergyMimicWidget.h"

// #include "dispatcher/DispatchEngine.h" // нужно только для варианта (Б)

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    QMainWindow mainWindow;
    auto *mimic = new EnergyMimicWidget(&mainWindow);
    mainWindow.setCentralWidget(mimic);

    // ---- Вариант (Б), реальное подключение (раскомментируйте и адаптируйте
    // под свой проект вместо блока с QTimer ниже):
    //
    // DispatchEngine engine;
    // engine.setSystemConfig(mySystemConfig);
    // engine.setForecastConnectionName(QStringLiteral("dispatcher_db"));
    // QObject::connect(&engine, &DispatchEngine::planReady, mimic,
    //     [mimic](DispatchPlan plan)
    //     {
    //         if (plan.isEmpty())
    //             return;
    //         const DispatchInterval &first = plan.constFirst();
    //
    //         // СЭС: суммарная выработка солнца независимо от того, куда она
    //         // была направлена диспетчером (нагрузка/СНЭ/сеть/излишки).
    //         const double solarKw = first.solarToLoadKw + first.solarToBatteryKw
    //                              + first.solarToGridKw + first.solarCurtailedKw;
    //
    //         // Нагрузка: суммарная мощность, покрытая из всех источников.
    //         const double loadKw = first.solarToLoadKw + first.batteryToLoadKw
    //                             + first.gridToLoadKw + first.genToLoadKw;
    //
    //         // Сеть: в DispatchInterval knak gridNetPowerKw > 0 - экспорт,
    //         // < 0 - импорт; у виджета конвенция обратная (см. EnergyMimicWidget.h) -
    //         // поэтому знак инвертируется.
    //         const double gridKw = -first.gridNetPowerKw;
    //
    //         // Мощность СНЭ виджет считает сам из баланса - отдельно передавать
    //         // её не нужно, достаточно передать SOC.
    //         mimic->setSystemState(solarKw, gridKw, loadKw,
    //                                /*generatorVisible=*/true, first.genPowerKw,
    //                                first.socEndFrac * 100.0);
    //     });
    // engine.start();

    // ---- Вариант (А): синтетические данные для запуска примера "как есть". ----
    auto *demoTimer = new QTimer(&mainWindow);
    double t = 0.0;
    QObject::connect(demoTimer, &QTimer::timeout, mimic, [mimic, &t]() {
        t += 0.05;
        const double solarKw = std::max(0.0, 60.0 * std::sin(t));       // "день/ночь"
        const double loadKw  = 40.0 + 15.0 * std::sin(t * 0.7 + 1.0);   // нагрузка предприятия
        const double gridKw  = 20.0 * std::sin(t * 0.3);                // то импорт, то экспорт
        const double genKw   = solarKw < 5.0 ? 25.0 : 0.0;               // генератор подхватывает ночью
        const double socPct  = 50.0 + 40.0 * std::sin(t * 0.15);        // медленное "дыхание" SOC

        mimic->setSystemState(solarKw, gridKw, loadKw,
                               /*generatorVisible=*/true, genKw, socPct);
    });
    demoTimer->start(200);

    mainWindow.resize(960, 600);
    mainWindow.show();

    return app.exec();
}
