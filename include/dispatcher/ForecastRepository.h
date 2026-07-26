#pragma once

#include <QString>

#include "ForecastTypes.h"

// Имена таблиц/столбцов входных данных настраиваются здесь, а не жёстко
// зашиты в SQL-запросах, чтобы репозиторий можно было подключить к уже
// существующей схеме БД предприятия без правки кода - достаточно передать
// свой ForecastSchema в ForecastRepository::setSchema().
//
// Ожидаемая структура таблиц (см. README, "Источник данных"):
//
//   CREATE TABLE forecast_hourly (
//       timestamp  TIMESTAMP PRIMARY KEY, -- начало часа, ISO 8601
//       buy_price  DOUBLE PRECISION,      -- руб/кВт*ч
//       sell_price DOUBLE PRECISION,      -- руб/кВт*ч
//       solar_kw   DOUBLE PRECISION,      -- кВт
//       load_kw    DOUBLE PRECISION       -- кВт
//   );
//
//   CREATE TABLE grid_availability_forecast (
//       timestamp TIMESTAMP PRIMARY KEY,  -- начало получаса
//       grid_on   BOOLEAN                 -- 1 - сеть доступна, 0 - плановое отключение
//   );
struct ForecastSchema
{
    QString hourlyTable = QStringLiteral("forecast_hourly");
    QString hourlyTimestampColumn = QStringLiteral("timestamp");
    QString buyPriceColumn = QStringLiteral("buy_price");
    QString sellPriceColumn = QStringLiteral("sell_price");
    QString solarKwColumn = QStringLiteral("solar_kw");
    QString loadKwColumn = QStringLiteral("load_kw");

    QString gridAvailabilityTable = QStringLiteral("grid_availability_forecast");
    QString gridTimestampColumn = QStringLiteral("timestamp");
    QString gridOnColumn = QStringLiteral("grid_on");
};

// Загружает прогнозные данные из БД (через уже открытое, управляемое хост-
// приложением QSqlDatabase-соединение - см. README, "Требования к SQL") и
// ресэмплирует их на интервалы управления заданной длительности.
//
// Ресэмплинг - "ступенчатый": почасовые данные (цена/солнце/нагрузка) и
// получасовые данные (gridOn) не интерполируются, а просто копируются в каждый
// под-интервал, целиком попадающий в соответствующий час/получас (см. README,
// "Скользящий горизонт и ресэмплинг" за обоснованием этого выбора - входные
// прогнозы физически не имеют более высокого разрешения, интерполяция создала
// бы ложную точность).
class ForecastRepository
{
public:
    // connectionName - имя QSqlDatabase-соединения (см. QSqlDatabase::database()),
    // уже открытого хост-приложением. Пустая строка - соединение по умолчанию.
    explicit ForecastRepository(const QString &connectionName = QString());

    void setSchema(const ForecastSchema &schema) { m_schema = schema; }
    const ForecastSchema &schema() const { return m_schema; }

    // Загружает "сырые" почасовые точки в полуоткрытом интервале [from, from+horizonHours).
    QVector<HourlyForecastPoint> loadHourly(const QDateTime &from, int horizonHours,
                                             QString *errorMessage = nullptr) const;

    // Загружает "сырые" получасовые точки доступности сети в том же интервале.
    QVector<GridAvailabilityPoint> loadGridAvailability(const QDateTime &from, int horizonHours,
                                                         QString *errorMessage = nullptr) const;

    // Основной метод, используемый DispatchEngine: строит горизонт интервалов
    // управления длительностью controlPeriodMinutes (10..60 минут) из
    // почасовых/получасовых данных. errorMessage (если задан) получает
    // непустую строку, если часть интервалов осталась без данных (план для
    // них всё равно строится, но с нулевыми солнцем/нагрузкой - см. README).
    ForecastHorizon buildHorizon(const QDateTime &from, int horizonHours, int controlPeriodMinutes,
                                 QString *errorMessage = nullptr) const;

private:
    QString m_connectionName;
    ForecastSchema m_schema;
};
