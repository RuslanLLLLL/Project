#pragma once

#include <QWidget>
#include <QPixmap>
#include <QString>
#include <QColor>
#include <array>

class QTimer;
class QPaintEvent;
class QResizeEvent;

// =============================================================================
// EnergyMimicWidget - интерактивная анимированная мнемосхема состояния гибридной
// энергосистемы предприятия, построенной на инверторе Deye SUN...K-PCS01HP3.
//
// Виджет НЕ содержит логики диспетчеризации/оптимизации и не зависит от
// DispatchEngine - это чисто отображающий компонент ("dumb view"), не имеющий
// собственных таймеров опроса. Актуальные параметры системы поступают извне
// через набор setXxx()-слотов (например, из DispatchEngine::planReady(), из
// ModbusInverterClient, либо из любого другого источника телеметрии) и
// хранятся во внутренних переменных m_sollarPow, m_loadPow, m_genVisible,
// m_genPow, m_gridPov, m_soc - имена и назначение зафиксированы в ТЗ на виджет
// (см. README.md, раздел "Мнемосхема энергосистемы (EnergyMimicWidget)").
//
// Схема (см. README для картинки):
//        [СЭС]      [Резервный генератор] (опционально)     [Сеть]
//            \                |                             /
//             \               |                            /
//              \--------- [ИНВЕРТОР] --------------------/
//             /                |                            \
//            /          [AC Couple] (опционально)             \
//        [СНЭ, SOC%]                                     [Нагрузка]
//
// AC Couple - опциональный узел под инвертором (видимость управляется
// булевой m_acCoupleEnable, см. setAcCoupleEnable()); входного значения
// мощности для него не предусмотрено - это статический индикатор наличия
// AC-присоединения в составе системы, аналогично генератору наверху.
//
// -----------------------------------------------------------------------------
// Конвенция знака мощности (кВт), ЕДИНАЯ для всех бинарных потоков виджета:
//   > 0 - энергия течёт ОТ элемента К ИНВЕРТОРУ (элемент отдаёт мощность);
//   < 0 - энергия течёт ОТ ИНВЕРТОРА К элементу (элемент потребляет мощность).
//
//   m_sollarPow (СЭС)      - физически только >= 0 (панели не потребляют).
//   m_genPow    (генератор)- физически только >= 0 (учитывается, только если
//                            m_genVisible == true; при m_genVisible == false
//                            узел и связь генератора не отображаются вовсе).
//   m_gridPov   (сеть)     - знакопеременная: > 0 импорт (сеть -> инвертор),
//                            < 0 экспорт (инвертор -> сеть).
//   m_loadPow   (нагрузка) - хранится как модуль потребления (>= 0), т.к.
//                            нагрузка физически не может отдавать мощность в
//                            инвертор; направление связи для неё всегда
//                            "инвертор -> нагрузка".
//   m_soc       (СНЭ)      - это НЕ мощность, а текущий заряд батареи, %
//                            (0..100).
//
// О мощности СНЭ (заряд/разряд): отдельной входной переменной для неё в ТЗ не
// предусмотрено (в списке входных данных виджета есть только m_soc). Поэтому
// поток мощности СНЭ восстанавливается виджетом из баланса мощности на шинах
// инвертора (см. batteryFlowKw() в .cpp): избыток источников идёт на заряд,
// недостаток покрывается разрядом. Если в вашем проекте есть отдельный
// Modbus-регистр фактической мощности СНЭ - замените вызов batteryFlowKw() в
// paintEvent() на собственное значение (см. README, раздел "Точки расширения").
// =============================================================================
class EnergyMimicWidget : public QWidget
{
    Q_OBJECT
public:
    explicit EnergyMimicWidget(QWidget *parent = nullptr);
    ~EnergyMimicWidget() override;

    QSize sizeHint() const override { return QSize(900, 560); }
    QSize minimumSizeHint() const override { return QSize(480, 320); }

public slots:
    // ---- Обновление данных (вызывать при получении новых значений от
    // инвертора/DispatchEngine - см. README, раздел "Подключение к источнику
    // данных"). Каждый вызов помечает виджет на перерисовку. -------------------
    void setSolarPowerKw(double kw);
    void setLoadPowerKw(double kw);
    void setGeneratorVisible(bool visible);
    void setGeneratorPowerKw(double kw);
    void setGridPowerKw(double kw);
    void setBatterySocPercent(double socPercent);

    // Видимость узла "AC Couple" (см. пояснение в комментарии к классу выше).
    void setAcCoupleEnable(bool enable);

    // Удобный групповой сеттер - один вызов вместо шести, чтобы обновлять все
    // параметры атомарно (без промежуточных перерисовок между отдельными
    // setXxx). generatorPowerKw игнорируется, если generatorVisible == false.
    void setSystemState(double solarPowerKw,
                         double gridPowerKw,
                         double loadPowerKw,
                         bool generatorVisible,
                         double generatorPowerKw,
                         double batterySocPercent);

    // ---- Замена иконок в обход процедурной отрисовки "по умолчанию" ----------
    // (второй, рантаймовый способ замены - см. README, раздел "Замена иконок".
    // Первый способ - до сборки проекта - см. комментарий к kDefaultIconPath в
    // EnergyMimicWidget.cpp). Пустой (null) QPixmap возвращает узел к иконке по
    // умолчанию.
    void setGridIcon(const QPixmap &icon);
    void setSolarIcon(const QPixmap &icon);
    void setBatteryIcon(const QPixmap &icon);
    void setInverterIcon(const QPixmap &icon);
    void setGeneratorIcon(const QPixmap &icon);
    void setLoadIcon(const QPixmap &icon);
    void setAcCoupleIcon(const QPixmap &icon);

    // Опорная мощность для нормировки скорости анимации и яркости подсветки
    // (кВт). Поток со значением |P| >= referencePowerKw анимируется с
    // максимальной скоростью/яркостью. Подберите под номинал вашей системы
    // (по умолчанию - 100 кВт).
    void setReferencePowerKw(double kw);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    // Идентификаторы узлов мнемосхемы. Порядок важен только для индексации
    // массива m_nodes - на раскладку и связи не влияет (см. recomputeLayout()).
    enum NodeId
    {
        Grid = 0,     // Сеть
        Solar,        // СЭС
        Battery,      // СНЭ
        Inverter,     // Инвертор (центральный узел)
        Generator,    // Резервный генератор (опционально)
        AcCouple,     // AC Couple (опционально, см. m_acCoupleEnable)
        Load,         // Нагрузка
        NodeCount
    };

    // Геометрия и статическое описание одного узла мнемосхемы, пересчитывается
    // в recomputeLayout() при изменении размера виджета.
    struct NodeVisual
    {
        QPointF centerRel;   // относительное положение центра узла, доли [0..1] от размера виджета
        QString title;       // подпись узла ("Сеть", "СЭС", ...)
        QColor  accent;      // акцентный цвет узла (иконка/подсветка/линия связи)
        QPixmap customIcon;  // иконка, заданная пользователем через setXxxIcon() (может быть null)
        QRectF  rect;        // абсолютный прямоугольник карточки узла в координатах виджета
    };

    void recomputeLayout();
    void advanceAnimation();

    // Возвращает мгновенный поток мощности СНЭ, вычисленный из баланса мощности
    // на шинах инвертора (см. пояснение в комментарии к классу выше).
    // > 0 - разряд (СНЭ -> инвертор), < 0 - заряд (инвертор -> СНЭ).
    double batteryFlowKw() const;

    QPixmap iconForNode(NodeId id, int pixelSize) const;
    QPixmap drawDefaultIcon(NodeId id, int pixelSize) const;

    void drawBackground(QPainter &p) const;
    void drawGlow(QPainter &p, const QRectF &cardRect, const QColor &color, double intensity01) const;
    void drawConnection(QPainter &p, NodeId from, NodeId to, double powerKw, bool intoInverter) const;
    void drawNodeCard(QPainter &p, NodeId id) const;

    QColor flowColor(bool idle, bool intoInverter) const;
    static QString formatPower(double kw);
    static QString formatSoc(double socPercent);

    // --- Параметры системы (см. подробную конвенцию знаков в комментарии к
    // классу в начале файла) ----------------------------------------------------
    double m_sollarPow  = 0.0;   // СЭС, кВт, >= 0
    double m_loadPow    = 0.0;   // Нагрузка, кВт, >= 0 (модуль потребления)
    bool   m_genVisible = false; // наличие резервного генератора в составе системы
    double m_genPow     = 0.0;   // Генератор, кВт, >= 0 (учитывается только если m_genVisible)
    double m_gridPov    = 0.0;   // Сеть, кВт: >0 импорт, <0 экспорт
    double m_soc        = 0.0;   // СНЭ, % заряда, 0..100
    bool   m_acCoupleEnable = false; // видимость узла "AC Couple" под инвертором

    double m_referencePowerKw = 100.0; // для нормировки скорости анимации/яркости подсветки

    std::array<NodeVisual, NodeCount> m_nodes;

    QTimer *m_animTimer = nullptr;
    double  m_animPhase = 0.0; // фаза анимации потоков, монотонно нарастает без оборота (см. advanceAnimation())

    static constexpr double kPowerEpsilonKw = 0.05; // порог, ниже которого поток считается нулевым ("простой")
};
