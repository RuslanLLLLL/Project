#include "dispatcher/JsonForecastRepository.h"

#include <QCoreApplication>
#include <QDate>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QMap>
#include <QStringList>
#include <QTime>
#include <algorithm>
#include <cmath>

namespace
{
// Единственная минутная точка изменения состояния сети внутри одного часа
// (см. "outages" в описании формата) - "on"/"off" события вперемешку.
struct MinuteEvent
{
    int minute;
    bool on;
};

// Суммарная нагрузка предприятия на (дата,час): проценты по участкам (0-100),
// заданные в "days", умноженные на номинальную мощность соответствующего
// участка и просуммированные. Участки без ratedPowerKw (см. вызывающий код)
// вносят 0 - их нагрузка попросту не может быть учтена без этого числа.
double loadKwForHour(const QJsonObject &daysRoot, const QMap<QString, double> &zoneRatedPowerKw,
                      const QString &dateKey, int hour)
{
    const QJsonObject dayObj = daysRoot.value(dateKey).toObject();
    double total = 0.0;
    for (auto it = zoneRatedPowerKw.constBegin(); it != zoneRatedPowerKw.constEnd(); ++it)
    {
        const QJsonArray arr = dayObj.value(it.key()).toArray();
        if (hour < arr.size())
        {
            const double percent = std::clamp(arr.at(hour).toDouble(), 0.0, 100.0);
            total += (percent / 100.0) * it.value();
        }
    }
    return total;
}

// Значение из секции вида "prices"/"solar": { "дата": { "час(0-23)": число } }.
// Отсутствие записи - defaultValue (см. README - тот же принцип, что и у
// LoadScheduleController: нет данных - используется нейтральное значение, а
// не ошибка).
double hourlyDouble(const QJsonObject &sectionRoot, const QString &dateKey, int hour, double defaultValue)
{
    const QJsonObject dayObj = sectionRoot.value(dateKey).toObject();
    const QJsonValue v = dayObj.value(QString::number(hour));
    return v.isDouble() ? v.toDouble() : defaultValue;
}
} // namespace

JsonForecastRepository::JsonForecastRepository(const QString &filePath)
    : m_filePath(filePath.isEmpty() ? QCoreApplication::applicationDirPath() + QStringLiteral("/loadSchedule.json")
                                     : filePath)
{
}

// Строит timeline переломов доступности сети (см. GridAvailabilityTimeline.h)
// из минутных событий "on"/"off" в "outages", начиная с САМОЙ РАННЕЙ даты,
// встречающейся в этой секции файла, и до конца горизонта. Состояние "до
// самой ранней даты" (и вообще при полном отсутствии секции "outages") -
// "сеть доступна" (см. isIntervalFullyAvailable, defaultBeforeTimeline) - то
// же соглашение, что использует LoadScheduleController (п.4 его ТЗ).
//
// Сканирование идёт час за часом от самой ранней даты - при многолетней
// истории это несколько десятков тысяч дешёвых обращений к QJsonObject, что
// пренебрежимо мало на фоне периода вызова (раз в 10-60 минут).
QVector<GridStateChange> JsonForecastRepository::buildOutageTimeline(const QJsonObject &outagesRoot,
                                                                       const QDateTime &horizonEnd) const
{
    QVector<GridStateChange> timeline;

    QDate earliest;
    for (auto it = outagesRoot.constBegin(); it != outagesRoot.constEnd(); ++it)
    {
        const QDate d = QDate::fromString(it.key(), Qt::ISODate);
        if (d.isValid() && (!earliest.isValid() || d < earliest))
            earliest = d;
    }
    if (!earliest.isValid())
        return timeline; // секции нет вообще - сеть всегда доступна

    bool state = true;
    for (QDate day = earliest; QDateTime(day, QTime(0, 0)) < horizonEnd; day = day.addDays(1))
    {
        const QJsonObject dayObj = outagesRoot.value(day.toString(Qt::ISODate)).toObject();
        for (int h = 0; h < 24; ++h)
        {
            const QJsonObject hourObj = dayObj.value(QString::number(h)).toObject();

            QList<MinuteEvent> events;
            const QJsonArray onArr = hourObj.value(QStringLiteral("on")).toArray();
            for (const QJsonValue &v : onArr)
                events.append({std::clamp(v.toInt(), 0, 59), true});
            const QJsonArray offArr = hourObj.value(QStringLiteral("off")).toArray();
            for (const QJsonValue &v : offArr)
                events.append({std::clamp(v.toInt(), 0, 59), false}); // добавлены после "on" - при равной минуте выигрывает "off"

            std::stable_sort(events.begin(), events.end(),
                              [](const MinuteEvent &a, const MinuteEvent &b) { return a.minute < b.minute; });

            for (const MinuteEvent &e : events)
            {
                if (e.on != state)
                {
                    state = e.on;
                    timeline.push_back({QDateTime(day, QTime(h, e.minute)), state});
                }
            }
        }
    }
    return timeline;
}

ForecastHorizon JsonForecastRepository::buildHorizon(const QDateTime &from, int horizonHours,
                                                      int controlPeriodMinutes, QString *errorMessage) const
{
    ForecastHorizon horizon;
    if (controlPeriodMinutes <= 0 || horizonHours <= 0)
        return horizon;

    QStringList warnings;
    QJsonObject root;

    QFile file(m_filePath);
    if (!file.open(QIODevice::ReadOnly))
    {
        warnings << QStringLiteral("JsonForecastRepository: не удалось открыть файл '%1'").arg(m_filePath);
    }
    else
    {
        QJsonParseError parseError{};
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject())
            warnings << QStringLiteral("JsonForecastRepository: ошибка разбора '%1': %2")
                            .arg(m_filePath, parseError.errorString());
        else
            root = doc.object();
    }

    // --- участки и их номинальная мощность (для перевода % в кВт) -------------
    QMap<QString, double> zoneRatedPowerKw;
    QStringList zonesMissingPower;
    {
        const QJsonArray zonesArr = root.value(QStringLiteral("zones")).toArray();
        for (const QJsonValue &v : zonesArr)
        {
            QString name;
            double rated = 0.0;
            if (v.isObject())
            {
                const QJsonObject zo = v.toObject();
                name = zo.value(QStringLiteral("name")).toString();
                rated = zo.value(QStringLiteral("ratedPowerKw")).toDouble(0.0);
            }
            else
            {
                // Устаревший формат ("zones": ["Литейка", ...], без мощности) -
                // читаем имя, мощность остаётся 0 (участок не вносит вклад в
                // нагрузку, пока пользователь не задаст её в интерфейсе).
                name = v.toString();
            }
            if (name.isEmpty())
                continue;
            zoneRatedPowerKw[name] = rated;
            if (rated <= 0.0)
                zonesMissingPower << name;
        }
    }
    if (!zonesMissingPower.isEmpty())
        warnings << QStringLiteral("Не задана номинальная мощность участков: %1 - их нагрузка "
                                    "принята нулевой")
                        .arg(zonesMissingPower.join(QStringLiteral(", ")));

    const QJsonObject daysRoot = root.value(QStringLiteral("days")).toObject();
    const QJsonObject pricesRoot = root.value(QStringLiteral("prices")).toObject();
    const QJsonObject solarRoot = root.value(QStringLiteral("solar")).toObject();
    const QJsonObject outagesRoot = root.value(QStringLiteral("outages")).toObject();

    const double durationHours = controlPeriodMinutes / 60.0;
    const int intervalCount =
        static_cast<int>(std::ceil((qint64(horizonHours) * 60) / double(controlPeriodMinutes)));
    const QDateTime horizonEnd = from.addSecs(qint64(horizonHours) * 3600);

    const QVector<GridStateChange> outageTimeline = buildOutageTimeline(outagesRoot, horizonEnd);

    horizon.reserve(intervalCount);
    for (int i = 0; i < intervalCount; ++i)
    {
        const QDateTime ts = from.addSecs(qint64(i) * controlPeriodMinutes * 60);
        const QString dateKey = ts.date().toString(Qt::ISODate);
        const int hour = ts.time().hour();

        DispatchIntervalForecast f;
        f.timestamp = ts;
        f.durationHours = durationHours;

        f.loadKw = loadKwForHour(daysRoot, zoneRatedPowerKw, dateKey, hour);
        f.buyPrice = hourlyDouble(pricesRoot, dateKey, hour, 0.0);
        // В JSON только одна цена в час (нет разделения покупка/продажа) -
        // используется и как buyPrice, и как sellPrice (см. README,
        // "Источник прогноза: JSON").
        f.sellPrice = f.buyPrice;
        f.solarKw = hourlyDouble(solarRoot, dateKey, hour, 0.0);
        f.gridAvailable = isIntervalFullyAvailable(outageTimeline, ts, durationHours, /*defaultBeforeTimeline=*/true);

        horizon.push_back(f);
    }

    if (errorMessage)
        *errorMessage = warnings.join(QStringLiteral("; "));

    return horizon;
}
