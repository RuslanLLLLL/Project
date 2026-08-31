#pragma once

#include <QObject>
#include <QString>
#include <memory>

#include "DispatchPermissions.h"
#include "DispatchTypes.h"
#include "ForecastRepository.h"
#include "ForecastTypes.h"
#include "IForecastRepository.h"
#include "JsonForecastRepository.h"
#include "SystemConfig.h"

class QTimer;
class IDispatchOptimizer;
class ModbusInverterClient;

// DispatchEngine - "дирижёр" всей диспетчеризации: держит конфигурацию,
// периодически (раз в controlPeriodMinutes, скользящее окно) подтягивает
// свежий SOC и прогноз, вызывает выбранный пользователем оптимизатор и
// записывает результат первого интервала плана в инвертор.
//
// Это единственный класс, который нужно встроить в существующий интерфейс,
// чтобы получить работающую диспетчеризацию - подключайте его сигналы/слоты
// напрямую к своим элементам управления (см. examples/integration_example.cpp
// и README, "Встраивание в существующий интерфейс").
//
// Класс работает и БЕЗ подключённого ModbusInverterClient (см. setModbusClient) -
// в этом случае используется значение SOC, заданное вручную через
// setManualSocFrac() (удобно для разработки/тестирования GUI без реального
// инвертора), а результат плана только эмитится сигналом planReady() без
// записи в устройство.
class DispatchEngine : public QObject
{
    Q_OBJECT
public:
    explicit DispatchEngine(QObject *parent = nullptr);
    ~DispatchEngine() override;

    // --- Конфигурация -----------------------------------------------------------
    void setSystemConfig(const SystemConfig &config) { m_config = config; }
    SystemConfig systemConfig() const { return m_config; }

    void setPermissions(const DispatchPermissions &permissions) { m_permissions = permissions; }
    DispatchPermissions permissions() const { return m_permissions; }

    void setMethod(DispatchMethod method) { m_method = method; }
    DispatchMethod method() const { return m_method; }

    // Период оптимизации, минуты. Требование ТЗ: настраивается пользователем от
    // 10 до 60 минут - значения вне диапазона обрезаются с предупреждением.
    void setControlPeriodMinutes(int minutes);
    int controlPeriodMinutes() const { return m_controlPeriodMinutes; }

    // Длина горизонта планирования, часы (по умолчанию 24).
    void setHorizonHours(int hours);
    int horizonHours() const { return m_horizonHours; }

    // --- Источники данных --------------------------------------------------------

    // Выбор источника прогноза (цена/солнце/нагрузка/отключения сети). По
    // умолчанию - ForecastSourceType::Json (см. README, "Источник прогноза").
    void setForecastSourceType(ForecastSourceType type) { m_forecastSource = type; }
    ForecastSourceType forecastSourceType() const { return m_forecastSource; }

    // Вариант 1 (SQL) - настройка соединения/схемы, используется только если
    // forecastSourceType() == ForecastSourceType::Sql.
    void setForecastConnectionName(const QString &connectionName);
    void setForecastSchema(const ForecastSchema &schema);

    // Вариант 2 (JSON) - путь к loadSchedule.json, используется только если
    // forecastSourceType() == ForecastSourceType::Json. По умолчанию - тот же
    // файл (и то же соглашение о расположении), что использует
    // LoadScheduleController в существующем интерфейсе.
    void setJsonForecastFilePath(const QString &path) { m_jsonForecastRepo->setFilePath(path); }
    QString jsonForecastFilePath() const { return m_jsonForecastRepo->filePath(); }

    // Вариант 3 (пользовательский источник) - используется только если
    // forecastSourceType() == ForecastSourceType::Custom. repo реализует
    // IForecastRepository (см. README, "Пользовательский источник прогноза",
    // например для интеграции с уже существующим в приложении хранилищем
    // телеметрии). Не владеет объектом - как и с setModbusClient(), время
    // жизни repo остаётся за вызывающим кодом и должно пережить engine (либо
    // источник должен быть переключён/repo сброшен в nullptr до удаления).
    void setCustomForecastRepository(IForecastRepository *repo) { m_customForecastRepo = repo; }
    IForecastRepository *customForecastRepository() const { return m_customForecastRepo; }

    // Внешний Modbus-клиент (см. ModbusInverterClient). Не владеет объектом -
    // время жизни управляется вызывающим кодом (обычно тем же GUI). Передайте
    // nullptr, чтобы работать в режиме симуляции без реального инвертора.
    void setModbusClient(ModbusInverterClient *client);

    // Ручной SOC (доля 0..1), используется только если ModbusClient не задан.
    void setManualSocFrac(double socFrac) { m_pendingSocFrac = socFrac; }

    bool isRunning() const;

public slots:
    // Запускает таймер скользящего горизонта (период = controlPeriodMinutes) и
    // немедленно инициирует первый расчёт.
    void start();
    void stop();

    // Ручной пересчёт вне расписания (например, кнопка "Пересчитать сейчас" в GUI,
    // или реакция на ручное изменение пользователем прав/конфигурации).
    void recomputeNow();

signals:
    void planReady(DispatchPlan plan);
    void statusMessage(QString message);
    void errorOccurred(QString message);
    void cycleStarted();
    void cycleFinished(bool success);

private slots:
    void onSocAndVoltageRead(double socFrac, double batteryVoltageV);
    void onCommandSetApplied(bool success);

private:
    void continueRecompute();

    SystemConfig m_config;
    DispatchPermissions m_permissions;
    DispatchMethod m_method = DispatchMethod::Heuristic;
    int m_controlPeriodMinutes = 15;
    int m_horizonHours = 24;

    ForecastSourceType m_forecastSource = ForecastSourceType::Json; // по умолчанию - вариант 2 (JSON)
    std::unique_ptr<ForecastRepository> m_forecastRepo;         // вариант 1 (SQL)
    std::unique_ptr<JsonForecastRepository> m_jsonForecastRepo; // вариант 2 (JSON)
    IForecastRepository *m_customForecastRepo = nullptr;        // вариант 3, не владеет

    ModbusInverterClient *m_modbus = nullptr; // не владеет
    QTimer *m_timer = nullptr;

    std::unique_ptr<IDispatchOptimizer> m_heuristic;
    std::unique_ptr<IDispatchOptimizer> m_milp;

    bool m_cycleInProgress = false;
    bool m_waitingForSoc = false;
    double m_pendingSocFrac = 0.5;
    double m_pendingVoltageV = 0.0;
};
