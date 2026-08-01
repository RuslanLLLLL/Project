#include "DispatcherPanel.h"

#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <QVariant>

DispatcherPanel::DispatcherPanel(QWidget *parent) : QWidget(parent)
{
    buildUi();

    connect(&m_engine, &DispatchEngine::planReady, this, &DispatcherPanel::onPlanReady);
    connect(&m_engine, &DispatchEngine::statusMessage, this, &DispatcherPanel::onStatusMessage);
    connect(&m_engine, &DispatchEngine::errorOccurred, this, &DispatcherPanel::onErrorOccurred);

    applyPermissionsAndMethodToEngine();
}

void DispatcherPanel::buildUi()
{
    auto *rootLayout = new QVBoxLayout(this);

    // --- Верхняя панель: метод оптимизации + разрешения пользователя -----------
    auto *topRow = new QHBoxLayout();

    auto *methodBox = new QGroupBox(tr("Метод оптимизации"), this);
    auto *methodLayout = new QVBoxLayout(methodBox);
    m_methodCombo = new QComboBox(methodBox);
    m_methodCombo->addItem(tr("Эвристика (merit-order)"), QVariant::fromValue(int(DispatchMethod::Heuristic)));
    m_methodCombo->addItem(tr("MILP (HiGHS)"), QVariant::fromValue(int(DispatchMethod::Milp)));
    methodLayout->addWidget(m_methodCombo);
    topRow->addWidget(methodBox);

    auto *permBox = new QGroupBox(tr("Разрешения пользователя"), this);
    auto *permLayout = new QVBoxLayout(permBox);
    m_genEnableCheck = new QCheckBox(tr("Разрешить запуск генератора"), permBox);
    m_genChargeEnableCheck = new QCheckBox(tr("Разрешить заряд от генератора"), permBox);
    m_sollSellCheck = new QCheckBox(tr("Разрешить продажу в сеть"), permBox);
    m_sollSellCheck->setChecked(true);
    m_sollSellOfBatteriesCheck = new QCheckBox(tr("Разрешить разряд СНЭ для продажи"), permBox);
    permLayout->addWidget(m_genEnableCheck);
    permLayout->addWidget(m_genChargeEnableCheck);
    permLayout->addWidget(m_sollSellCheck);
    permLayout->addWidget(m_sollSellOfBatteriesCheck);
    topRow->addWidget(permBox);

    auto *timingBox = new QGroupBox(tr("Расписание расчёта"), this);
    auto *timingLayout = new QVBoxLayout(timingBox);
    auto *periodRow = new QHBoxLayout();
    periodRow->addWidget(new QLabel(tr("Период, мин:"), timingBox));
    m_controlPeriodSpin = new QSpinBox(timingBox);
    m_controlPeriodSpin->setRange(10, 60);
    m_controlPeriodSpin->setValue(15);
    m_controlPeriodSpin->setSingleStep(5);
    periodRow->addWidget(m_controlPeriodSpin);
    timingLayout->addLayout(periodRow);
    auto *horizonRow = new QHBoxLayout();
    horizonRow->addWidget(new QLabel(tr("Горизонт, ч:"), timingBox));
    m_horizonSpin = new QSpinBox(timingBox);
    m_horizonSpin->setRange(1, 48);
    m_horizonSpin->setValue(24);
    horizonRow->addWidget(m_horizonSpin);
    timingLayout->addLayout(horizonRow);
    m_startStopButton = new QPushButton(tr("Запустить"), timingBox);
    m_recalculateButton = new QPushButton(tr("Пересчитать сейчас"), timingBox);
    timingLayout->addWidget(m_startStopButton);
    timingLayout->addWidget(m_recalculateButton);
    topRow->addWidget(timingBox);

    rootLayout->addLayout(topRow);

    // --- Таблица плана диспетчеризации ------------------------------------------
    m_planTable = new QTableWidget(this);
    m_planTable->setColumnCount(8);
    m_planTable->setHorizontalHeaderLabels({tr("Время"), tr("Солнце, кВт"), tr("Нагрузка, кВт"),
                                             tr("СНЭ net, кВт"), tr("Сеть net, кВт"), tr("Генератор, кВт"),
                                             tr("SOC на конец, %"), tr("Стоимость, руб")});
    m_planTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_planTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    rootLayout->addWidget(m_planTable, /*stretch=*/1);

    // --- Строка статуса ----------------------------------------------------------
    m_statusLabel = new QLabel(tr("Диспетчер остановлен"), this);
    rootLayout->addWidget(m_statusLabel);

    connect(m_methodCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &DispatcherPanel::onPermissionsOrMethodChanged);
    connect(m_genEnableCheck, &QCheckBox::toggled, this, &DispatcherPanel::onPermissionsOrMethodChanged);
    connect(m_genChargeEnableCheck, &QCheckBox::toggled, this, &DispatcherPanel::onPermissionsOrMethodChanged);
    connect(m_sollSellCheck, &QCheckBox::toggled, this, &DispatcherPanel::onPermissionsOrMethodChanged);
    connect(m_sollSellOfBatteriesCheck, &QCheckBox::toggled, this,
            &DispatcherPanel::onPermissionsOrMethodChanged);
    connect(m_controlPeriodSpin, qOverload<int>(&QSpinBox::valueChanged), this,
            &DispatcherPanel::onControlPeriodChanged);
    connect(m_horizonSpin, qOverload<int>(&QSpinBox::valueChanged), this, &DispatcherPanel::onHorizonChanged);
    connect(m_startStopButton, &QPushButton::clicked, this, &DispatcherPanel::onStartStopClicked);
    connect(m_recalculateButton, &QPushButton::clicked, this, &DispatcherPanel::onRecalculateClicked);
}

void DispatcherPanel::applyPermissionsAndMethodToEngine()
{
    DispatchPermissions perm;
    perm.m_genEnable = m_genEnableCheck->isChecked();
    perm.m_genChargeEnable = m_genChargeEnableCheck->isChecked();
    perm.m_sollSell = m_sollSellCheck->isChecked();
    perm.m_sollSellOfBatteries = m_sollSellOfBatteriesCheck->isChecked();
    m_engine.setPermissions(perm);

    const auto method = static_cast<DispatchMethod>(m_methodCombo->currentData().toInt());
    m_engine.setMethod(method);

    m_engine.setControlPeriodMinutes(m_controlPeriodSpin->value());
    m_engine.setHorizonHours(m_horizonSpin->value());
}

void DispatcherPanel::onPermissionsOrMethodChanged()
{
    applyPermissionsAndMethodToEngine();
}

void DispatcherPanel::onControlPeriodChanged(int minutes)
{
    m_engine.setControlPeriodMinutes(minutes);
}

void DispatcherPanel::onHorizonChanged(int hours)
{
    m_engine.setHorizonHours(hours);
}

void DispatcherPanel::onStartStopClicked()
{
    if (m_engine.isRunning())
    {
        m_engine.stop();
        m_startStopButton->setText(tr("Запустить"));
    }
    else
    {
        applyPermissionsAndMethodToEngine();
        m_engine.start();
        m_startStopButton->setText(tr("Остановить"));
    }
}

void DispatcherPanel::onRecalculateClicked()
{
    applyPermissionsAndMethodToEngine();
    m_engine.recomputeNow();
}

void DispatcherPanel::onPlanReady(DispatchPlan plan)
{
    populateTable(plan);
}

void DispatcherPanel::onStatusMessage(const QString &message)
{
    m_statusLabel->setText(message);
}

void DispatcherPanel::onErrorOccurred(const QString &message)
{
    m_statusLabel->setText(tr("Ошибка: %1").arg(message));
}

void DispatcherPanel::populateTable(const DispatchPlan &plan)
{
    m_planTable->setRowCount(plan.size());
    for (int i = 0; i < plan.size(); ++i)
    {
        const DispatchInterval &row = plan[i];

        auto setCell = [this, i](int col, const QString &text) {
            auto *item = new QTableWidgetItem(text);
            item->setTextAlignment(Qt::AlignCenter);
            m_planTable->setItem(i, col, item);
        };

        setCell(0, row.timestamp.toString(QStringLiteral("dd.MM HH:mm")));
        setCell(1, QString::number(row.solarToLoadKw + row.solarToBatteryKw + row.solarToGridKw +
                                        row.solarCurtailedKw,
                                    'f', 1));
        setCell(2, QString::number(row.solarToLoadKw + row.gridToLoadKw + row.batteryToLoadKw +
                                        row.genToLoadKw + row.unservedLoadKw,
                                    'f', 1));
        setCell(3, QString::number(row.batteryNetPowerKw, 'f', 1));
        setCell(4, QString::number(row.gridNetPowerKw, 'f', 1));
        setCell(5, QString::number(row.genPowerKw, 'f', 1));
        setCell(6, QString::number(row.socEndFrac * 100.0, 'f', 1));
        setCell(7, QString::number(row.intervalCostRub, 'f', 2));

        // Лёгкая подсветка строк по классу цены - упрощает визуальный контроль
        // того, что дорогие часы действительно закрываются СНЭ/генератором, а
        // не покупкой из сети (см. README, эвристика, шаг 3).
        QColor rowColor = Qt::transparent;
        if (row.priceClass == PriceClass::Cheap)
            rowColor = QColor(210, 235, 255);
        else if (row.priceClass == PriceClass::Expensive)
            rowColor = QColor(255, 220, 210);
        for (int col = 0; col < m_planTable->columnCount(); ++col)
        {
            if (auto *item = m_planTable->item(i, col))
                item->setBackground(rowColor);
        }
    }
}
