#include "dispatcher/DispatchEngine.h"

#include <QDateTime>
#include <QTimer>
#include <algorithm>

#include "dispatcher/HeuristicDispatchOptimizer.h"
#include "dispatcher/InverterCommandMapper.h"
#include "dispatcher/MilpDispatchOptimizer.h"
#include "dispatcher/ModbusInverterClient.h"

DispatchEngine::DispatchEngine(QObject *parent)
    : QObject(parent),
      m_forecastRepo(std::make_unique<ForecastRepository>()),
      m_heuristic(std::make_unique<HeuristicDispatchOptimizer>()),
      m_milp(std::make_unique<MilpDispatchOptimizer>())
{
}

DispatchEngine::~DispatchEngine() = default;

void DispatchEngine::setControlPeriodMinutes(int minutes)
{
    // ТЗ: период оптимизации настраивается пользователем в диапазоне 10..60 минут.
    const int clamped = std::clamp(minutes, 10, 60);
    if (clamped != minutes)
        emit statusMessage(QStringLiteral("Период оптимизации %1 мин вне диапазона [10,60], "
                                            "принято %2 мин")
                                .arg(minutes)
                                .arg(clamped));
    m_controlPeriodMinutes = clamped;
    if (m_timer && m_timer->isActive())
        m_timer->setInterval(m_controlPeriodMinutes * 60 * 1000);
}

void DispatchEngine::setHorizonHours(int hours)
{
    m_horizonHours = std::max(1, hours);
}

void DispatchEngine::setForecastConnectionName(const QString &connectionName)
{
    const ForecastSchema schema = m_forecastRepo->schema();
    m_forecastRepo = std::make_unique<ForecastRepository>(connectionName);
    m_forecastRepo->setSchema(schema);
}

void DispatchEngine::setForecastSchema(const ForecastSchema &schema)
{
    m_forecastRepo->setSchema(schema);
}

void DispatchEngine::setModbusClient(ModbusInverterClient *client)
{
    if (m_modbus)
        m_modbus->disconnect(this);

    m_modbus = client;
    if (m_modbus)
    {
        connect(m_modbus, &ModbusInverterClient::socAndVoltageRead, this, &DispatchEngine::onSocAndVoltageRead);
        connect(m_modbus, &ModbusInverterClient::commandSetApplied, this, &DispatchEngine::onCommandSetApplied);
        connect(m_modbus, &ModbusInverterClient::errorOccurred, this, &DispatchEngine::errorOccurred);
    }
}

bool DispatchEngine::isRunning() const
{
    return m_timer && m_timer->isActive();
}

void DispatchEngine::start()
{
    if (!m_timer)
    {
        m_timer = new QTimer(this);
        connect(m_timer, &QTimer::timeout, this, &DispatchEngine::recomputeNow);
    }
    m_timer->setInterval(m_controlPeriodMinutes * 60 * 1000);
    m_timer->start();
    emit statusMessage(QStringLiteral("Диспетчер запущен, период %1 мин").arg(m_controlPeriodMinutes));
    recomputeNow(); // немедленный первый расчёт, не дожидаясь первого тика таймера
}

void DispatchEngine::stop()
{
    if (m_timer)
        m_timer->stop();
    emit statusMessage(QStringLiteral("Диспетчер остановлен"));
}

void DispatchEngine::recomputeNow()
{
    if (m_cycleInProgress)
    {
        emit statusMessage(QStringLiteral("Пересчёт уже выполняется, повторный запрос пропущен"));
        return;
    }
    m_cycleInProgress = true;
    emit cycleStarted();

    if (m_modbus)
    {
        // Дожидаемся живого SOC/напряжения СНЭ перед расчётом - см.
        // onSocAndVoltageRead(), которая продолжит цикл через continueRecompute().
        m_waitingForSoc = true;
        m_modbus->readSocAndVoltage();
    }
    else
    {
        // Режим симуляции/разработки без реального инвертора - используем
        // значение, заданное вручную через setManualSocFrac().
        continueRecompute();
    }
}

void DispatchEngine::onSocAndVoltageRead(double socFrac, double batteryVoltageV)
{
    if (!m_waitingForSoc)
        return; // событие не относится к текущему циклу (например, GUI сам запросил телеметрию)
    m_waitingForSoc = false;
    m_pendingSocFrac = socFrac;
    m_pendingVoltageV = batteryVoltageV;
    continueRecompute();
}

void DispatchEngine::continueRecompute()
{
    QString warning;
    ForecastHorizon horizon = m_forecastRepo->buildHorizon(QDateTime::currentDateTime(), m_horizonHours,
                                                            m_controlPeriodMinutes, &warning);
    if (!warning.isEmpty())
        emit statusMessage(warning);

    if (horizon.isEmpty())
    {
        emit errorOccurred(QStringLiteral("DispatchEngine: не удалось построить горизонт прогноза - "
                                           "проверьте подключение к БД и наличие данных"));
        m_cycleInProgress = false;
        emit cycleFinished(false);
        return;
    }

    DispatchRequest request;
    request.horizon = horizon;
    request.initialSocFrac = m_pendingSocFrac;
    request.config = m_config;
    request.permissions = m_permissions;
    request.controlPeriodMinutes = m_controlPeriodMinutes;

    IDispatchOptimizer *optimizer = (m_method == DispatchMethod::Milp) ? m_milp.get() : m_heuristic.get();
    const DispatchPlan plan = optimizer->computeDispatch(request);

    if (plan.isEmpty())
    {
        emit errorOccurred(QStringLiteral("DispatchEngine: оптимизатор '%1' вернул пустой план")
                                .arg(optimizer->name()));
        m_cycleInProgress = false;
        emit cycleFinished(false);
        return;
    }

    emit planReady(plan);

    const deye::InverterCommandSet commandSet =
        InverterCommandMapper::buildCommandSet(plan.first(), m_config, m_permissions, m_pendingVoltageV);

    if (m_modbus)
    {
        m_modbus->applyCommandSet(commandSet);
        // Цикл завершится асинхронно в onCommandSetApplied().
    }
    else
    {
        m_cycleInProgress = false;
        emit cycleFinished(true);
    }
}

void DispatchEngine::onCommandSetApplied(bool success)
{
    if (!m_cycleInProgress)
        return;
    m_cycleInProgress = false;
    if (!success)
        emit errorOccurred(QStringLiteral("DispatchEngine: не все регистры инвертора удалось записать"));
    emit cycleFinished(success);
}
