// Демонстрационный пример встраивания DispatcherPanel в существующее
// приложение Qt Widgets. Показывает минимально необходимую последовательность
// действий - см. README, раздел "Встраивание в существующий GUI" за
// подробностями по каждому шагу.
//
// Собирается только если включены опции CMake BUILD_GUI и BUILD_EXAMPLES
// (обе включены по умолчанию, см. CMakeLists.txt).

#include <QApplication>
#include <QMainWindow>
#include <QSqlDatabase>
#include <QSqlError>
#include <QTabWidget>

#include "DispatcherPanel.h"
#include "dispatcher/ModbusInverterClient.h"
#include "dispatcher/SystemConfig.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    // ---- Шаг 1: открываем (или переиспользуем уже открытое) соединение с БД,
    // из которого ForecastRepository будет читать прогноз (см. README,
    // "Источник данных"). Имя соединения передаётся в DispatchEngine ниже.
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QPSQL"), QStringLiteral("dispatcher_db"));
    db.setHostName(QStringLiteral("localhost"));
    db.setDatabaseName(QStringLiteral("energy_forecast"));
    db.setUserName(QStringLiteral("energy_user"));
    db.setPassword(QStringLiteral("CHANGE_ME"));
    if (!db.open())
    {
        qWarning("Не удалось открыть БД прогноза: %s", qPrintable(db.lastError().text()));
        // В демонстрационных целях продолжаем работу - панель покажет ошибку
        // отсутствия данных при первом пересчёте, но остальной GUI будет
        // функционален.
    }

    // ---- Шаг 2: создаём главное окно существующего приложения (в реальном
    // проекте это уже существующий QMainWindow - здесь для примера пустой).
    QMainWindow mainWindow;
    auto *tabs = new QTabWidget(&mainWindow);
    mainWindow.setCentralWidget(tabs);

    // ---- Шаг 3: создаём панель диспетчера и настраиваем движок.
    auto *panel = new DispatcherPanel(&mainWindow);

    SystemConfig config; // при необходимости переопределите значения по умолчанию
    config.gridMaxImportKw = 200.0;
    config.gridMaxExportKw = 200.0;
    config.batteryCapacityKwh = 500.0;
    panel->engine()->setSystemConfig(config);
    panel->engine()->setForecastConnectionName(QStringLiteral("dispatcher_db"));

    // ---- Шаг 4 (опционально): подключаем реальный инвертор.
    // Если этот шаг пропустить, DispatchEngine работает в режиме симуляции -
    // план считается и отображается, но команды никуда не записываются.
    auto *modbus = new ModbusInverterClient(&mainWindow);

    // Вариант А - прямое подключение по RS485:
    modbus->configureSerial(QStringLiteral("/dev/ttyUSB0"), /*baudRate=*/9600);

    // Вариант Б - по локальной сети через Wi-Fi/LAN логгер Deye (LSW-3/LSW-5 и
    // клоны). Такие логгеры почти всегда говорят проприетарным протоколом
    // Solarman V5 на порту 8899, а не "прозрачным" Modbus TCP - см. README,
    // "Подключение к инвертору по сети (логгер LSW-5 / Solarman V5)" за
    // процедурой проверки перед использованием на боевом объекте:
    // modbus->configureSolarmanV5(QStringLiteral("192.168.1.50"), 8899, /*loggerSerial=*/1234567890);

    modbus->setServerAddress(1);
    if (modbus->connectDevice())
        panel->engine()->setModbusClient(modbus);
    else
        qWarning("Не удалось открыть Modbus-соединение - продолжаем в режиме симуляции");

    // ---- Шаг 5: встраиваем панель как вкладку существующего окна.
    tabs->addTab(panel, QObject::tr("Диспетчер энергии"));

    mainWindow.resize(1100, 700);
    mainWindow.show();

    return app.exec();
}
