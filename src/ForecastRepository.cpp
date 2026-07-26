#include "dispatcher/ForecastRepository.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <algorithm>
#include <cmath>

ForecastRepository::ForecastRepository(const QString &connectionName) : m_connectionName(connectionName) {}

QVector<HourlyForecastPoint> ForecastRepository::loadHourly(const QDateTime &from, int horizonHours,
                                                              QString *errorMessage) const
{
    QVector<HourlyForecastPoint> result;

    QSqlDatabase db = m_connectionName.isEmpty() ? QSqlDatabase::database() : QSqlDatabase::database(m_connectionName);
    if (!db.isOpen())
    {
        if (errorMessage)
            *errorMessage = QStringLiteral("ForecastRepository: соединение с БД '%1' не открыто")
                                .arg(m_connectionName.isEmpty() ? QStringLiteral("(default)") : m_connectionName);
        return result;
    }

    const QDateTime to = from.addSecs(qint64(horizonHours) * 3600);

    QSqlQuery query(db);
    const QString sql = QStringLiteral("SELECT %1, %2, %3, %4, %5 FROM %6 "
                                        "WHERE %1 >= :from AND %1 < :to ORDER BY %1 ASC")
                             .arg(m_schema.hourlyTimestampColumn, m_schema.buyPriceColumn,
                                  m_schema.sellPriceColumn, m_schema.solarKwColumn, m_schema.loadKwColumn,
                                  m_schema.hourlyTable);
    query.prepare(sql);
    query.bindValue(QStringLiteral(":from"), from);
    query.bindValue(QStringLiteral(":to"), to);

    if (!query.exec())
    {
        if (errorMessage)
            *errorMessage = QStringLiteral("ForecastRepository: ошибка запроса почасового прогноза: %1")
                                .arg(query.lastError().text());
        return result;
    }

    result.reserve(horizonHours + 1);
    while (query.next())
    {
        HourlyForecastPoint p;
        p.timestamp = query.value(0).toDateTime();
        p.buyPrice = query.value(1).toDouble();
        p.sellPrice = query.value(2).toDouble();
        p.solarKw = query.value(3).toDouble();
        p.loadKw = query.value(4).toDouble();
        result.push_back(p);
    }
    return result;
}

QVector<GridAvailabilityPoint> ForecastRepository::loadGridAvailability(const QDateTime &from, int horizonHours,
                                                                          QString *errorMessage) const
{
    QVector<GridAvailabilityPoint> result;

    QSqlDatabase db = m_connectionName.isEmpty() ? QSqlDatabase::database() : QSqlDatabase::database(m_connectionName);
    if (!db.isOpen())
    {
        if (errorMessage)
            *errorMessage = QStringLiteral("ForecastRepository: соединение с БД '%1' не открыто")
                                .arg(m_connectionName.isEmpty() ? QStringLiteral("(default)") : m_connectionName);
        return result;
    }

    const QDateTime to = from.addSecs(qint64(horizonHours) * 3600);

    QSqlQuery query(db);
    const QString sql = QStringLiteral("SELECT %1, %2 FROM %3 WHERE %1 >= :from AND %1 < :to ORDER BY %1 ASC")
                             .arg(m_schema.gridTimestampColumn, m_schema.gridOnColumn, m_schema.gridAvailabilityTable);
    query.prepare(sql);
    query.bindValue(QStringLiteral(":from"), from);
    query.bindValue(QStringLiteral(":to"), to);

    if (!query.exec())
    {
        if (errorMessage)
            *errorMessage = QStringLiteral("ForecastRepository: ошибка запроса доступности сети: %1")
                                .arg(query.lastError().text());
        return result;
    }

    result.reserve(horizonHours * 2 + 2);
    while (query.next())
    {
        GridAvailabilityPoint p;
        p.timestamp = query.value(0).toDateTime();
        p.gridOn = query.value(1).toBool();
        result.push_back(p);
    }
    return result;
}

namespace
{
// Возвращает индекс последнего элемента vec, чей timestamp <= ts (т.е.
// "интервал, действующий на момент ts" при ступенчатом ресэмплинге), либо -1,
// если ts раньше самого первого элемента.
template <typename T>
int findFloorIndex(const QVector<T> &vec, const QDateTime &ts)
{
    int lo = 0, hi = vec.size() - 1, ans = -1;
    while (lo <= hi)
    {
        const int mid = (lo + hi) / 2;
        if (vec[mid].timestamp <= ts)
        {
            ans = mid;
            lo = mid + 1;
        }
        else
        {
            hi = mid - 1;
        }
    }
    return ans;
}
} // namespace

ForecastHorizon ForecastRepository::buildHorizon(const QDateTime &from, int horizonHours,
                                                  int controlPeriodMinutes, QString *errorMessage) const
{
    ForecastHorizon horizon;
    if (controlPeriodMinutes <= 0 || horizonHours <= 0)
        return horizon;

    QString hourlyError, gridError;
    const QVector<HourlyForecastPoint> hourly = loadHourly(from, horizonHours, &hourlyError);
    const QVector<GridAvailabilityPoint> gridAvail = loadGridAvailability(from, horizonHours, &gridError);

    QStringList warnings;
    if (!hourlyError.isEmpty())
        warnings << hourlyError;
    if (!gridError.isEmpty())
        warnings << gridError;

    const double durationHours = controlPeriodMinutes / 60.0;
    const int intervalCount =
        static_cast<int>(std::ceil((qint64(horizonHours) * 60) / double(controlPeriodMinutes)));

    int missingHourly = 0;
    int missingGrid = 0;

    horizon.reserve(intervalCount);
    for (int i = 0; i < intervalCount; ++i)
    {
        const QDateTime ts = from.addSecs(qint64(i) * controlPeriodMinutes * 60);

        DispatchIntervalForecast f;
        f.timestamp = ts;
        f.durationHours = durationHours;

        const int hIdx = findFloorIndex(hourly, ts);
        if (hIdx >= 0)
        {
            f.buyPrice = hourly[hIdx].buyPrice;
            f.sellPrice = hourly[hIdx].sellPrice;
            f.solarKw = hourly[hIdx].solarKw;
            f.loadKw = hourly[hIdx].loadKw;
        }
        else
        {
            ++missingHourly;
        }

        const int gIdx = findFloorIndex(gridAvail, ts);
        f.gridAvailable = (gIdx >= 0) ? gridAvail[gIdx].gridOn : true; // по умолчанию считаем сеть доступной
        if (gIdx < 0)
            ++missingGrid;

        horizon.push_back(f);
    }

    if (missingHourly > 0)
        warnings << QStringLiteral("Нет почасовых данных для %1 из %2 интервалов горизонта - "
                                    "использованы нулевые значения цены/солнца/нагрузки")
                        .arg(missingHourly)
                        .arg(intervalCount);
    if (missingGrid > 0)
        warnings << QStringLiteral("Нет данных о доступности сети для %1 из %2 интервалов горизонта - "
                                    "принято gridOn=true по умолчанию")
                        .arg(missingGrid)
                        .arg(intervalCount);

    if (errorMessage)
        *errorMessage = warnings.join(QStringLiteral("; "));

    return horizon;
}
