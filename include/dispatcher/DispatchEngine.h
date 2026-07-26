#pragma once

#include <QObject>
#include <QString>
#include <memory>

#include "DispatchPermissions.h"
#include "DispatchTypes.h"
#include "ForecastRepository.h"
#include "SystemConfig.h"

class QTimer;
class IDispatchOptimizer;
class ModbusInverterClient;

// DispatchEngine - "дирижёр" всей диспетчеризации: держит конфигурацию,
// периодически (раз в controlPeriodMinutes, скользящее окно) подтягивает
// свежий SOC и прогноз, вызывает выбранный пользователем оптимизатор и
// записывает результат первого интервала плана в инвертор.
//
// Это единственный класс, который нужно встроить в существующий GUI, чтобы
// получить работающую диспетчеризацию: см. gui/DispatcherPanel как готовый
// пример виджета поверх DispatchEngine, либо подключайте сигналы/слоты
// напрямую к своим элементам управления (см. examples/integration_example.cpp).
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
    void setForecastConnectionName(const QString &connectionName);
    void setForecastSchema(const ForecastSchema &schema);

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

    std::unique_ptr<ForecastRepository> m_forecastRepo;

    ModbusInverterClient *m_modbus = nullptr; // не владеет
    QTimer *m_timer = nullptr;

    std::unique_ptr<IDispatchOptimizer> m_heuristic;
    std::unique_ptr<IDispatchOptimizer> m_milp;

    bool m_cycleInProgress = false;
    bool m_waitingForSoc = false;
    double m_pendingSocFrac = 0.5;
    double m_pendingVoltageV = 0.0;
};
