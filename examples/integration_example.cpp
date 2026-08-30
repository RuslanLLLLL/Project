// Демонстрационный пример встраивания DispatchEngine в существующее
// приложение. Никакого собственного GUI-виджета библиотека не навязывает -
// показанные здесь соединения сигнал/слот рассчитаны на то, что в реальном
// проекте они будут подключены к уже существующим элементам управления
// хост-приложения (таблице плана, чекбоксам разрешений, статус-бару и т.п.),
// а не к чему-то из этого репозитория. См. README, раздел "Встраивание в
// существующий интерфейс".
//
// Собирается, если включена опция CMake BUILD_EXAMPLES (включена по
// умолчанию, см. CMakeLists.txt).

#include <QCoreApplication>
#include <QDebug>
#include <QSqlDatabase>
#include <QSqlError>
#include <QTimer>

#include "dispatcher/DispatchEngine.h"
#include "dispatcher/ModbusInverterClient.h"
#include "dispatcher/SystemConfig.h"

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    // ---- Шаг 1 (только для варианта SQL): открываем (или переиспользуем уже
    // открытое) соединение с БД, из которого ForecastRepository будет читать
    // прогноз (см. README, "Источник прогноза: SQL или JSON"). Если ниже
    // выбран вариант JSON (значение по умолчанию) - этот шаг не нужен.
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QPSQL"), QStringLiteral("dispatcher_db"));
    db.setHostName(QStringLiteral("localhost"));
    db.setDatabaseName(QStringLiteral("energy_forecast"));
    db.setUserName(QStringLiteral("energy_user"));
    db.setPassword(QStringLiteral("CHANGE_ME"));
    if (!db.open())
    {
        qWarning("Не удалось открыть БД прогноза: %s", qPrintable(db.lastError().text()));
        // В демонстрационных целях продолжаем работу - DispatchEngine сообщит
        // об отсутствии данных через сигнал errorOccurred при первом пересчёте.
    }

    // ---- Шаг 2: настраиваем DispatchEngine - конфигурацию системы, права
    // пользователя и источник прогноза. В реальном проекте эти значения
    // приходят из уже существующих элементов управления вашего интерфейса
    // (спинбоксы, чекбоксы и т.п.), а не задаются константами, как здесь.
    DispatchEngine engine;

    SystemConfig config; // при необходимости переопределите значения по умолчанию
    config.gridMaxImportKw = 200.0;
    config.gridMaxExportKw = 200.0;
    config.batteryCapacityKwh = 500.0;
    config.gridTransportCostPerKwh = 1.2; // тариф на передачу, руб/кВт*ч - константа проекта
    engine.setSystemConfig(config);

    DispatchPermissions permissions;
    permissions.m_genEnable = false;
    permissions.m_genChargeEnable = false;
    permissions.m_sollSell = true;
    permissions.m_sollSellOfBatteries = false;
    engine.setPermissions(permissions);

    engine.setMethod(DispatchMethod::Heuristic); // либо DispatchMethod::Milp
    engine.setControlPeriodMinutes(15);           // 10..60 минут
    engine.setHorizonHours(24);

    // Источник прогноза: JSON (loadSchedule.json) - по умолчанию, можно не
    // вызывать явно; показано для наглядности. Для варианта SQL:
    //   engine.setForecastSourceType(ForecastSourceType::Sql);
    //   engine.setForecastConnectionName("dispatcher_db");
    engine.setForecastSourceType(ForecastSourceType::Json);
    engine.setJsonForecastFilePath(QStringLiteral("/путь/к/loadSchedule.json")); // по умолчанию - рядом с exe

    // ---- Шаг 3 (опционально): подключаем реальный инвертор.
    // Если этот шаг пропустить, DispatchEngine работает в режиме симуляции -
    // план считается и логируется, но команды никуда не записываются.
    ModbusInverterClient modbus;

    // Вариант А - прямое подключение по RS485:
    modbus.configureSerial(QStringLiteral("/dev/ttyUSB0"), /*baudRate=*/9600);

    // Вариант Б - по локальной сети через Wi-Fi/LAN логгер Deye (LSW-3/LSW-5 и
    // клоны). Такие логгеры почти всегда говорят проприетарным протоколом
    // Solarman V5 на порту 8899 - см. README, "Подключение к инвертору по
    // сети (логгер LSW-5 / Solarman V5)" за процедурой проверки перед
    // использованием на боевом объекте:
    // modbus.configureSolarmanV5(QStringLiteral("192.168.1.50"), 8899, /*loggerSerial=*/1234567890);

    modbus.setServerAddress(1);
    // Число параллельно работающих PCS-модулей (инверторов) в системе - от
    // него зависит, как считается общий SOC (среднее по модулям, см. README).
    // Для одиночного инвертора можно не вызывать - значение по умолчанию 1.
    modbus.setPcsCount(1);

    if (modbus.connectDevice())
        engine.setModbusClient(&modbus);
    else
        qWarning("Не удалось открыть Modbus-соединение - продолжаем в режиме симуляции");

    // ---- Шаг 4: подключаем сигналы движка к своей логике/интерфейсу. Здесь -
    // просто консольный вывод; в реальном проекте это будут вызовы методов
    // уже существующих виджетов (обновление таблицы плана, статус-бара и т.п.).
    QObject::connect(&engine, &DispatchEngine::planReady, &app, [](DispatchPlan plan) {
        qInfo() << "Новый план готов, интервалов:" << plan.size();
        if (!plan.isEmpty())
        {
            const auto &first = plan.first();
            qInfo() << "  ближайший интервал:" << first.timestamp.toString(Qt::ISODate)
                     << "батарея(кВт):" << first.batteryNetPowerKw << "сеть(кВт):" << first.gridNetPowerKw
                     << "генератор(кВт):" << first.genPowerKw;
        }
    });
    QObject::connect(&engine, &DispatchEngine::statusMessage, &app,
                      [](const QString &msg) { qInfo().noquote() << "[статус]" << msg; });
    QObject::connect(&engine, &DispatchEngine::errorOccurred, &app,
                      [](const QString &msg) { qWarning().noquote() << "[ошибка]" << msg; });
    QObject::connect(&engine, &DispatchEngine::cycleFinished, &app,
                      [](bool success) { qInfo() << "Цикл диспетчеризации завершён, успех:" << success; });

    // ---- Шаг 5: запускаем скользящий горизонт.
    engine.start();

    return app.exec();
}
