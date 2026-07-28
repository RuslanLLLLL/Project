#include "EnergyMimicWidget.h"

#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>
#include <QLineF>
#include <QFont>
#include <QPen>
#include <QTimer>
#include <QResizeEvent>
#include <QtMath>
#include <algorithm>
#include <cmath>

// =============================================================================
// ЗАМЕНА ИКОНОК ДО СБОРКИ ПРОЕКТА
// =============================================================================
// У виджета два независимых способа заменить иконку узла на собственную:
//
//  1) ДО СБОРКИ (этот способ) - впишите путь к своему файлу (PNG/JPG/SVG - для
//     SVG нужен подключённый модуль Qt Svg, см. README) в массив
//     kDefaultIconPath ниже вместо пустой строки. Проще всего использовать
//     путь на Qt-ресурс (":/myicons/solar.png") из .qrc-файла вашего
//     проекта - тогда иконка "вшивается" в исполняемый файл. Можно также
//     указать абсолютный/относительный путь на диске, если иконки должны
//     подгружаться из внешних файлов.
//     Если путь пуст ИЛИ файл по указанному пути не найден/не загрузился -
//     виджет автоматически рисует иконку "по умолчанию" средствами QPainter
//     (см. drawDefaultIcon()), т.е. работает "из коробки" без единого файла
//     ресурсов.
//
//  2) В РАНТАЙМЕ - вызвать setSolarIcon()/setGridIcon()/... сразу после
//     создания виджета, передав свой QPixmap. Это работает независимо от
//     первого способа и имеет более высокий приоритет (см. iconForNode()).
//
// Порядок элементов массива соответствует перечислению EnergyMimicWidget::NodeId
// (Grid, Solar, Battery, Inverter, Generator, Load) - т.е. индекс в массиве
// равен числовому значению соответствующего NodeId.
static const char *const kDefaultIconPath[6] = {
    "", // Grid      - Сеть
    "", // Solar     - СЭС
    "", // Battery   - СНЭ
    "", // Inverter  - Инвертор
    "", // Generator - Резервный генератор
    "", // Load      - Нагрузка
};

namespace {

// Толщина линии связи и базовый радиус "частиц" анимации потока - вынесены в
// константы, чтобы масштаб мнемосхемы было легко подстроить в одном месте.
constexpr double kConnectionLineWidth = 2.5;
constexpr double kParticleRadius      = 4.0;
constexpr int    kParticlesPerLink    = 4;
constexpr int    kAnimTimerIntervalMs = 33; // ~30 кадров/сек - плавно и недорого по CPU

// Цвета тёмной темы виджета.
const QColor kBgTop(14, 17, 23);
const QColor kBgBottom(20, 25, 34);
const QColor kCardFill(28, 34, 48, 235);
const QColor kCardBorder(52, 61, 82);
const QColor kTitleColor(190, 198, 212);
const QColor kIdleColor(90, 98, 112);
const QColor kSupplyColor(46, 204, 113);   // элемент отдаёт мощность в инвертор
const QColor kConsumeColor(245, 166, 35);  // элемент потребляет мощность из инвертора

} // namespace

EnergyMimicWidget::EnergyMimicWidget(QWidget *parent) : QWidget(parent)
{
    // Тёмная тема рисуется вручную в paintEvent(), но выставим и палитру, чтобы
    // не было "мигания" светлым фоном между показом виджета и первой отрисовкой.
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, kBgTop);
    setPalette(pal);

    m_nodes[Grid].title      = tr("Сеть");
    m_nodes[Grid].accent     = QColor(79, 209, 255);   // голубой
    m_nodes[Solar].title     = tr("СЭС");
    m_nodes[Solar].accent    = QColor(255, 209, 102);  // жёлтый
    m_nodes[Battery].title   = tr("СНЭ");
    m_nodes[Battery].accent  = QColor(52, 211, 153);   // зелёный
    m_nodes[Inverter].title  = tr("Инвертор");
    m_nodes[Inverter].accent = QColor(167, 139, 250);  // фиолетовый
    m_nodes[Generator].title = tr("Генератор");
    m_nodes[Generator].accent= QColor(249, 115, 22);   // оранжевый
    m_nodes[Load].title      = tr("Нагрузка");
    m_nodes[Load].accent     = QColor(248, 113, 113);  // красный/розовый

    setMinimumSize(minimumSizeHint());

    // Таймер анимации потоков энергии по связям - не связан с частотой
    // обновления данных (setXxx можно дёргать реже или чаще, независимо).
    m_animTimer = new QTimer(this);
    connect(m_animTimer, &QTimer::timeout, this, &EnergyMimicWidget::advanceAnimation);
    m_animTimer->start(kAnimTimerIntervalMs);
}

EnergyMimicWidget::~EnergyMimicWidget() = default;

// ---------------------------------------------------------------------------
// Слоты обновления данных
// ---------------------------------------------------------------------------
void EnergyMimicWidget::setSolarPowerKw(double kw)
{
    m_sollarPow = kw;
    update();
}

void EnergyMimicWidget::setLoadPowerKw(double kw)
{
    m_loadPow = std::abs(kw); // нагрузка - всегда потребитель, см. конвенцию в .h
    update();
}

void EnergyMimicWidget::setGeneratorVisible(bool visible)
{
    m_genVisible = visible;
    update();
}

void EnergyMimicWidget::setGeneratorPowerKw(double kw)
{
    m_genPow = kw;
    update();
}

void EnergyMimicWidget::setGridPowerKw(double kw)
{
    m_gridPov = kw;
    update();
}

void EnergyMimicWidget::setBatterySocPercent(double socPercent)
{
    m_soc = std::clamp(socPercent, 0.0, 100.0);
    update();
}

void EnergyMimicWidget::setSystemState(double solarPowerKw,
                                        double gridPowerKw,
                                        double loadPowerKw,
                                        bool generatorVisible,
                                        double generatorPowerKw,
                                        double batterySocPercent)
{
    m_sollarPow  = solarPowerKw;
    m_gridPov    = gridPowerKw;
    m_loadPow    = std::abs(loadPowerKw);
    m_genVisible = generatorVisible;
    m_genPow     = generatorPowerKw;
    m_soc        = std::clamp(batterySocPercent, 0.0, 100.0);
    update();
}

void EnergyMimicWidget::setGridIcon(const QPixmap &icon)      { m_nodes[Grid].customIcon = icon; update(); }
void EnergyMimicWidget::setSolarIcon(const QPixmap &icon)     { m_nodes[Solar].customIcon = icon; update(); }
void EnergyMimicWidget::setBatteryIcon(const QPixmap &icon)   { m_nodes[Battery].customIcon = icon; update(); }
void EnergyMimicWidget::setInverterIcon(const QPixmap &icon)  { m_nodes[Inverter].customIcon = icon; update(); }
void EnergyMimicWidget::setGeneratorIcon(const QPixmap &icon) { m_nodes[Generator].customIcon = icon; update(); }
void EnergyMimicWidget::setLoadIcon(const QPixmap &icon)      { m_nodes[Load].customIcon = icon; update(); }

void EnergyMimicWidget::setReferencePowerKw(double kw)
{
    m_referencePowerKw = kw > 0.0 ? kw : 1.0;
    update();
}

// ---------------------------------------------------------------------------
// Баланс мощности СНЭ (см. подробное объяснение в EnergyMimicWidget.h)
// ---------------------------------------------------------------------------
double EnergyMimicWidget::batteryFlowKw() const
{
    // Приток в инвертор от источников, не считая СНЭ.
    const double supplyKw = m_sollarPow
                           + (m_genVisible ? std::max(m_genPow, 0.0) : 0.0)
                           + std::max(m_gridPov, 0.0); // импорт из сети
    // Отток из инвертора, не считая СНЭ.
    const double demandKw = m_loadPow
                           + std::max(-m_gridPov, 0.0); // экспорт в сеть

    // demand > supply -> недостачу покрывает разряд СНЭ (результат > 0);
    // supply > demand -> избыток идёт на заряд СНЭ (результат < 0).
    return demandKw - supplyKw;
}

// ---------------------------------------------------------------------------
// Раскладка узлов
// ---------------------------------------------------------------------------
void EnergyMimicWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    recomputeLayout();
}

void EnergyMimicWidget::recomputeLayout()
{
    // Относительные положения центров узлов - строго по составу системы из
    // задания: Сеть - вверху справа, СЭС - вверху слева, СНЭ - внизу слева,
    // Инвертор - в центре, генератор - вверху посередине, Нагрузка - внизу
    // справа. Раскладка фиксированная (не "переливается" при скрытии
    // генератора), т.к. так удобнее оператору - элементы не "прыгают".
    m_nodes[Solar].centerRel     = QPointF(0.16, 0.17);
    m_nodes[Generator].centerRel = QPointF(0.50, 0.14);
    m_nodes[Grid].centerRel      = QPointF(0.84, 0.17);
    m_nodes[Inverter].centerRel  = QPointF(0.50, 0.52);
    m_nodes[Battery].centerRel   = QPointF(0.16, 0.84);
    m_nodes[Load].centerRel      = QPointF(0.84, 0.84);

    const double w = width();
    const double h = height();
    // Размер карточки узла - доля от меньшей стороны виджета, чтобы мнемосхема
    // адекватно масштабировалась и в широком, и в почти квадратном окне/доке.
    const double cardW = std::clamp(std::min(w, h) * 0.31, 130.0, 235.0);
    const double cardH = cardW * 0.74;

    for (auto &node : m_nodes)
    {
        const QPointF center(node.centerRel.x() * w, node.centerRel.y() * h);
        node.rect = QRectF(center.x() - cardW / 2.0, center.y() - cardH / 2.0, cardW, cardH);
    }
}

void EnergyMimicWidget::advanceAnimation()
{
    m_animPhase += 0.012; // скорость "базовой" фазы - модулируется мощностью потока при отрисовке
    if (m_animPhase >= 1.0)
        m_animPhase -= 1.0;
    update();
}

// ---------------------------------------------------------------------------
// Иконки
// ---------------------------------------------------------------------------
QPixmap EnergyMimicWidget::iconForNode(NodeId id, int pixelSize) const
{
    // Приоритет 1: иконка, заданная в рантайме через setXxxIcon().
    if (!m_nodes[id].customIcon.isNull())
        return m_nodes[id].customIcon.scaled(pixelSize, pixelSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);

    // Приоритет 2: файл, прописанный в kDefaultIconPath (замена до сборки).
    const QString path = QString::fromUtf8(kDefaultIconPath[id]);
    if (!path.isEmpty())
    {
        QPixmap pm(path);
        if (!pm.isNull())
            return pm.scaled(pixelSize, pixelSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }

    // Приоритет 3 (fallback "из коробки"): простая векторная иконка, нарисованная QPainter.
    return drawDefaultIcon(id, pixelSize);
}

QPixmap EnergyMimicWidget::drawDefaultIcon(NodeId id, int pixelSize) const
{
    QPixmap pm(pixelSize, pixelSize);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QColor c = m_nodes[id].accent;
    QPen pen(c);
    pen.setWidthF(pixelSize * 0.06);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);

    const double s = pixelSize;

    switch (id)
    {
    case Grid:
    {
        // Условная схема ЛЭП/опоры: трапеция + траверсы + провода.
        QPainterPath tower;
        tower.moveTo(s * 0.50, s * 0.08);
        tower.lineTo(s * 0.30, s * 0.92);
        tower.moveTo(s * 0.50, s * 0.08);
        tower.lineTo(s * 0.70, s * 0.92);
        p.drawPath(tower);
        p.drawLine(QPointF(s * 0.36, s * 0.40), QPointF(s * 0.64, s * 0.40));
        p.drawLine(QPointF(s * 0.32, s * 0.62), QPointF(s * 0.68, s * 0.62));
        p.setBrush(c);
        p.drawEllipse(QPointF(s * 0.36, s * 0.40), s * 0.03, s * 0.03);
        p.drawEllipse(QPointF(s * 0.64, s * 0.40), s * 0.03, s * 0.03);
        break;
    }
    case Solar:
    {
        // Солнце: круг + лучи.
        p.setBrush(QColor(c.red(), c.green(), c.blue(), 60));
        p.drawEllipse(QRectF(s * 0.30, s * 0.30, s * 0.40, s * 0.40));
        p.setBrush(Qt::NoBrush);
        const QPointF center(s * 0.5, s * 0.5);
        for (int i = 0; i < 8; ++i)
        {
            const double angle = i * M_PI / 4.0;
            const QPointF from(center.x() + std::cos(angle) * s * 0.26, center.y() + std::sin(angle) * s * 0.26);
            const QPointF to(center.x() + std::cos(angle) * s * 0.42, center.y() + std::sin(angle) * s * 0.42);
            p.drawLine(from, to);
        }
        break;
    }
    case Battery:
    {
        // Классический символ батареи: корпус + положительный контакт + уровень заряда.
        QRectF body(s * 0.22, s * 0.30, s * 0.56, s * 0.42);
        p.drawRoundedRect(body, s * 0.04, s * 0.04);
        QRectF nub(s * 0.44, s * 0.20, s * 0.12, s * 0.10);
        p.drawRoundedRect(nub, s * 0.02, s * 0.02);
        const double socFrac = std::clamp(m_soc / 100.0, 0.0, 1.0);
        QColor fillColor = socFrac < 0.2 ? QColor(239, 68, 68)
                          : socFrac < 0.5 ? QColor(245, 166, 35)
                                          : QColor(52, 211, 153);
        p.setPen(Qt::NoPen);
        p.setBrush(fillColor);
        QRectF fillRect(body.left() + s * 0.03,
                         body.bottom() - s * 0.03 - (body.height() - s * 0.06) * socFrac,
                         body.width() - s * 0.06,
                         (body.height() - s * 0.06) * socFrac);
        p.drawRect(fillRect);
        break;
    }
    case Inverter:
    {
        // Корпус инвертора с символами DC (слева, прямая линия) и AC (справа, синусоида).
        QRectF body(s * 0.16, s * 0.24, s * 0.68, s * 0.52);
        p.drawRoundedRect(body, s * 0.06, s * 0.06);
        p.drawLine(QPointF(s * 0.26, s * 0.5), QPointF(s * 0.44, s * 0.5)); // DC
        QPainterPath sine;
        sine.moveTo(s * 0.56, s * 0.5);
        sine.cubicTo(s * 0.61, s * 0.36, s * 0.69, s * 0.36, s * 0.74, s * 0.5);
        sine.cubicTo(s * 0.79, s * 0.64, s * 0.87, s * 0.64, s * 0.92, s * 0.5); // AC
        p.drawPath(sine);
        break;
    }
    case Generator:
    {
        // Круг с буквой "G" - общепринятое условное обозначение генератора.
        p.drawEllipse(QRectF(s * 0.20, s * 0.20, s * 0.60, s * 0.60));
        QFont f = p.font();
        f.setPixelSize(int(s * 0.34));
        f.setBold(true);
        p.setFont(f);
        p.drawText(QRectF(s * 0.20, s * 0.20, s * 0.60, s * 0.60), Qt::AlignCenter, QStringLiteral("G"));
        break;
    }
    case Load:
    {
        // Домик/предприятие: крыша + корпус - условное обозначение потребителя.
        QPainterPath house;
        house.moveTo(s * 0.5, s * 0.14);
        house.lineTo(s * 0.86, s * 0.42);
        house.lineTo(s * 0.86, s * 0.86);
        house.lineTo(s * 0.14, s * 0.86);
        house.lineTo(s * 0.14, s * 0.42);
        house.closeSubpath();
        p.drawPath(house);
        p.drawRect(QRectF(s * 0.42, s * 0.58, s * 0.16, s * 0.28));
        break;
    }
    default:
        break;
    }

    p.end();
    return pm;
}

// ---------------------------------------------------------------------------
// Отрисовка
// ---------------------------------------------------------------------------
QColor EnergyMimicWidget::flowColor(bool idle, bool intoInverter) const
{
    if (idle)
        return kIdleColor;
    return intoInverter ? kSupplyColor : kConsumeColor;
}

QString EnergyMimicWidget::formatPower(double kw)
{
    return QString::number(kw, 'f', 1) + QStringLiteral(" кВт");
}

QString EnergyMimicWidget::formatSoc(double socPercent)
{
    return QStringLiteral("SOC: ") + QString::number(socPercent, 'f', 0) + QStringLiteral("%");
}

void EnergyMimicWidget::paintEvent(QPaintEvent * /*event*/)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    drawBackground(p);

    const double battery = batteryFlowKw();

    // --- Связи рисуем ДО карточек узлов, чтобы линии визуально "заходили"
    // под карточки, а не перекрывали их текст/иконки. ---------------------------
    drawConnection(p, Solar, Inverter, m_sollarPow, /*intoInverter=*/true);
    drawConnection(p, Grid, Inverter, m_gridPov, /*intoInverter=*/m_gridPov > kPowerEpsilonKw);
    drawConnection(p, Battery, Inverter, battery, /*intoInverter=*/battery > kPowerEpsilonKw);
    drawConnection(p, Load, Inverter, m_loadPow, /*intoInverter=*/false);
    if (m_genVisible)
        drawConnection(p, Generator, Inverter, m_genPow, /*intoInverter=*/true);

    // --- Карточки узлов (подсветка по периметру + иконка + текст) ------------
    drawNodeCard(p, Solar);
    drawNodeCard(p, Grid);
    drawNodeCard(p, Battery);
    drawNodeCard(p, Load);
    drawNodeCard(p, Inverter);
    if (m_genVisible)
        drawNodeCard(p, Generator);
}

void EnergyMimicWidget::drawBackground(QPainter &p) const
{
    QLinearGradient bg(QPointF(0, 0), QPointF(0, height()));
    bg.setColorAt(0.0, kBgTop);
    bg.setColorAt(1.0, kBgBottom);
    p.fillRect(rect(), bg);
}

void EnergyMimicWidget::drawGlow(QPainter &p, const QRectF &cardRect, const QColor &color, double intensity01) const
{
    // Равномерная "подсветка" по всему периметру карточки - мягкий цветной
    // ореол, выступающий на одинаковое расстояние от каждого края (сверху,
    // снизу, слева и справа), а не только снизу. Реализовано как стопка
    // вложенных скруглённых прямоугольников, повторяющих форму карточки с
    // увеличивающимся равномерным отступом и убывающей непрозрачностью - так
    // имитируется мягкое размытие без зависимости направления от формы
    // карточки (в отличие от кругового/эллиптического градиента). Яркость и
    // отступ зависят от intensity01 (0 - узел простаивает, 1 - узел на полной
    // мощности относительно m_referencePowerKw).
    const double intensity = std::clamp(intensity01, 0.0, 1.0);
    const int maxAlpha = 55 + int(140 * intensity); // даже в простое лёгкое тусклое свечение видно
    const double maxSpread = cardRect.height() * (0.22 + 0.16 * intensity);
    const double baseRadius = cardRect.height() * 0.16;

    p.save();
    p.setPen(Qt::NoPen);
    const int steps = 14;
    for (int i = steps; i >= 1; --i)
    {
        const double frac = double(i) / steps;   // 1.0 (самый внешний слой) .. ~0.07 (у самой карточки)
        const double spread = maxSpread * frac;
        QColor layer = color;
        layer.setAlpha(int(maxAlpha * (1.0 - frac) * (1.0 - frac) / steps * 3.0));
        p.setBrush(layer);
        const QRectF glowRect = cardRect.adjusted(-spread, -spread, spread, spread);
        p.drawRoundedRect(glowRect, baseRadius + spread, baseRadius + spread);
    }
    p.restore();
}

void EnergyMimicWidget::drawConnection(QPainter &p, NodeId from, NodeId to, double powerKw, bool intoInverter) const
{
    // Если связь ведёт к скрытому узлу (генератор отсутствует) - не рисуем.
    if ((from == Generator || to == Generator) && !m_genVisible)
        return;

    const QPointF a = m_nodes[from].rect.center();
    const QPointF b = m_nodes[to].rect.center();
    // Связь рисуется только горизонтальными и вертикальными отрезками (без
    // диагоналей) - излом на полпути в точке (a.x, b.y): сперва вертикальный
    // отрезок от узла до высоты инвертора, затем горизонтальный до самого
    // инвертора. Для узлов, уже выровненных с инвертором по одной из осей
    // (например, Generator - строго над инвертором), один из отрезков
    // естественным образом вырождается в нулевую длину, и связь остаётся
    // простой прямой линией под прямым углом к границе карточки.
    const QPointF bend(a.x(), b.y());

    const double magnitude = std::abs(powerKw);
    const bool idle = magnitude <= kPowerEpsilonKw;
    const double activity = std::clamp(magnitude / m_referencePowerKw, 0.0, 1.0);
    const QColor lineColor = flowColor(idle, intoInverter);

    QPainterPath path;
    path.moveTo(a);
    path.lineTo(bend);
    path.lineTo(b);

    // Линия связи - тонкая, приглушённая в простое, чуть ярче и толще при потоке.
    QPen pen(lineColor);
    pen.setWidthF(kConnectionLineWidth * (idle ? 0.6 : (1.0 + 0.6 * activity)));
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
    p.setOpacity(idle ? 0.35 : 0.85);
    p.drawPath(path);
    p.setOpacity(1.0);

    if (idle)
        return; // нет потока - нет "бегущих" частиц

    // Анимированные "частицы" вдоль ломаной связи (a -> bend -> b), показывающие
    // направление движения энергии. Частицы всегда движутся К инвертору, если
    // intoInverter == true, и ОТ инвертора, если intoInverter == false.
    const double seg1Len = QLineF(a, bend).length();
    const double seg2Len = QLineF(bend, b).length();
    const double totalLen = seg1Len + seg2Len;
    auto pointAlongPath = [&](double t) -> QPointF {
        // t в [0,1] измеряется вдоль ломаной от a (t=0) к b (t=1).
        if (totalLen <= 0.0)
            return a;
        const double dist = t * totalLen;
        if (dist <= seg1Len)
        {
            const double localT = seg1Len > 0.0 ? dist / seg1Len : 0.0;
            return a + (bend - a) * localT;
        }
        const double localT = seg2Len > 0.0 ? (dist - seg1Len) / seg2Len : 0.0;
        return bend + (b - bend) * localT;
    };

    // Скорость "бега" частиц модулируется мощностью потока (activity), чтобы
    // на глаз было видно не только направление, но и интенсивность потока.
    const double speedFactor = 0.4 + 1.6 * activity;

    p.setPen(Qt::NoPen);
    p.setBrush(lineColor);
    for (int i = 0; i < kParticlesPerLink; ++i)
    {
        double t = std::fmod(m_animPhase * speedFactor + double(i) / kParticlesPerLink, 1.0);
        if (t < 0.0)
            t += 1.0;
        const QPointF pos = pointAlongPath(intoInverter ? t : (1.0 - t));
        // Частицы у краёв линии слегка "притушены" (эффект появления/исчезновения).
        const double edgeFade = std::min(1.0, std::min(t, 1.0 - t) * 6.0);
        p.setOpacity(0.35 + 0.65 * edgeFade);
        p.drawEllipse(pos, kParticleRadius * (0.7 + 0.5 * activity), kParticleRadius * (0.7 + 0.5 * activity));
    }
    p.setOpacity(1.0);
}

void EnergyMimicWidget::drawNodeCard(QPainter &p, NodeId id) const
{
    const NodeVisual &node = m_nodes[id];
    const QRectF &r = node.rect;

    // --- Определяем состояние потока для этого узла (для цвета текста и
    // яркости подсветки) - см. правило в заголовке класса. -------------------
    double magnitude = 0.0;
    bool intoInverter = true;
    bool showFlowValue = true; // для Inverter значение мощности не показываем (см. .h)

    switch (id)
    {
    case Solar:     magnitude = m_sollarPow; intoInverter = true; break;
    case Generator: magnitude = m_genPow;    intoInverter = true; break;
    case Grid:      magnitude = std::abs(m_gridPov); intoInverter = m_gridPov > kPowerEpsilonKw; break;
    case Battery:   magnitude = std::abs(batteryFlowKw()); intoInverter = batteryFlowKw() > kPowerEpsilonKw; break;
    case Load:      magnitude = m_loadPow; intoInverter = false; break;
    case Inverter:  showFlowValue = false; break;
    default: break;
    }

    const bool idle = magnitude <= kPowerEpsilonKw;
    const double activity = std::clamp(magnitude / m_referencePowerKw, 0.0, 1.0);

    // --- Подсветка по периметру карточки ("свечение") --------------------------
    // Для инвертора подсветка всегда акцентного цвета на средней яркости - это
    // "сердце" схемы и оно должно быть заметно в любом состоянии.
    const double glowIntensity = (id == Inverter) ? 0.5 : activity;
    const QColor glowColor = idle && id != Inverter ? kIdleColor : node.accent;
    drawGlow(p, r, glowColor, glowIntensity);

    // --- Карточка узла ---------------------------------------------------------
    QPainterPath cardPath;
    const double radius = r.height() * 0.16;
    cardPath.addRoundedRect(r, radius, radius);

    p.setPen(QPen(kCardBorder, 1.4));
    p.setBrush(kCardFill);
    p.drawPath(cardPath);

    // Тонкая цветная полоска сверху карточки - визуальная привязка к акцентному
    // цвету узла, дублирует смысл подсветки по периметру, но всегда видна одинаково.
    QRectF topStripe(r.left() + radius * 0.4, r.top(), r.width() - radius * 0.8, r.height() * 0.05);
    p.setPen(Qt::NoPen);
    p.setBrush(node.accent);
    p.drawRoundedRect(topStripe, topStripe.height() / 2.0, topStripe.height() / 2.0);

    // --- Иконка ------------------------------------------------------------
    // Раскладка строк внутри карточки (иконка/название/значение/SOC) уместена
    // строго в пределах [0, 1] высоты карточки, чтобы для СНЭ строка SOC% не
    // выходила за нижнюю границу карточки (см. ниже).
    const int iconSize = int(r.height() * 0.34);
    QPixmap icon = iconForNode(id, iconSize);
    const QPointF iconPos(r.center().x() - iconSize / 2.0, r.top() + r.height() * 0.10);
    p.drawPixmap(iconPos, icon);

    // --- Название узла -------------------------------------------------------
    QFont titleFont = p.font();
    titleFont.setPixelSize(std::max(10, int(r.height() * 0.13)));
    titleFont.setBold(true);
    p.setFont(titleFont);
    p.setPen(kTitleColor);
    QRectF titleRect(r.left(), r.top() + r.height() * 0.50, r.width(), r.height() * 0.15);
    p.drawText(titleRect, Qt::AlignHCenter | Qt::AlignVCenter, node.title);

    // --- Значение мощности (кроме инвертора, см. showFlowValue) ---------------
    QFont valueFont = p.font();
    valueFont.setPixelSize(std::max(10, int(r.height() * 0.14)));
    valueFont.setBold(false);
    p.setFont(valueFont);

    if (showFlowValue)
    {
        // Знак в подписи явный: "+" - отдаёт в инвертор, "-" - потребляет от
        // инвертора; для Load всегда "-" (см. конвенцию), для Solar/Generator
        // всегда "+" при ненулевом потоке.
        const QString sign = idle ? QString() : (intoInverter ? QStringLiteral("+") : QStringLiteral("-"));
        p.setPen(flowColor(idle, intoInverter));
        QRectF valueRect(r.left(), r.top() + r.height() * 0.67, r.width(), r.height() * 0.15);
        p.drawText(valueRect, Qt::AlignHCenter | Qt::AlignVCenter, sign + formatPower(magnitude));
    }

    // --- SOC% для СНЭ (требование ТЗ, п.4) ------------------------------------
    if (id == Battery)
    {
        QFont socFont = p.font();
        socFont.setPixelSize(std::max(9, int(r.height() * 0.12)));
        p.setFont(socFont);
        p.setPen(kTitleColor);
        QRectF socRect(r.left(), r.top() + r.height() * 0.85, r.width(), r.height() * 0.13);
        p.drawText(socRect, Qt::AlignHCenter | Qt::AlignTop, formatSoc(m_soc));
    }
}
