#pragma once

#include <QWidget>

#include "dispatcher/DispatchEngine.h"

class QComboBox;
class QCheckBox;
class QSpinBox;
class QPushButton;
class QTableWidget;
class QLabel;

// Готовый к встраиванию виджет панели диспетчера. Рассчитан на добавление как
// вкладка/dock-widget в уже существующее главное окно (см. README, раздел
// "Встраивание в существующий GUI" и examples/integration_example.cpp):
//
//   auto *panel = new DispatcherPanel(this);
//   panel->engine()->setSystemConfig(myConfig);
//   panel->engine()->setModbusClient(myModbusClient);       // или не вызывать - режим симуляции
//   panel->engine()->setForecastConnectionName("main_db");  // имя уже открытого QSqlDatabase
//   ui->tabWidget->addTab(panel, tr("Диспетчер энергии"));
//
// Виджет сам создаёт и владеет DispatchEngine (см. engine()) - вся логика
// расчёта и обмена с инвертором инкапсулирована там, панель лишь отображает
// результат и транслирует действия пользователя (чекбоксы разрешений, выбор
// метода, период оптимизации) в вызовы движка.
class DispatcherPanel : public QWidget
{
    Q_OBJECT
public:
    explicit DispatcherPanel(QWidget *parent = nullptr);

    // Доступ к движку для расширенной настройки, которую нет смысла дублировать
    // в самом виджете (SystemConfig, подключение Modbus/БД).
    DispatchEngine *engine() { return &m_engine; }

private slots:
    void onPlanReady(DispatchPlan plan);
    void onStatusMessage(const QString &message);
    void onErrorOccurred(const QString &message);
    void onStartStopClicked();
    void onRecalculateClicked();
    void onPermissionsOrMethodChanged();
    void onControlPeriodChanged(int minutes);
    void onHorizonChanged(int hours);

private:
    void buildUi();
    void populateTable(const DispatchPlan &plan);
    void applyPermissionsAndMethodToEngine();

    DispatchEngine m_engine;

    QComboBox *m_methodCombo = nullptr;
    QCheckBox *m_genEnableCheck = nullptr;
    QCheckBox *m_genChargeEnableCheck = nullptr;
    QCheckBox *m_sollSellCheck = nullptr;
    QCheckBox *m_sollSellOfBatteriesCheck = nullptr;
    QSpinBox *m_controlPeriodSpin = nullptr;
    QSpinBox *m_horizonSpin = nullptr;
    QPushButton *m_startStopButton = nullptr;
    QPushButton *m_recalculateButton = nullptr;
    QTableWidget *m_planTable = nullptr;
    QLabel *m_statusLabel = nullptr;
};
