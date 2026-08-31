#pragma once

#include <QJsonObject>
#include <QString>

#include "ForecastTypes.h"
#include "GridAvailabilityTimeline.h"
#include "IForecastRepository.h"

// Репозиторий прогноза "Вариант 2 (JSON)", источник по умолчанию (см.
// DispatchEngine::setForecastSourceType(), ForecastSourceType::Json).
//
// Читает loadSchedule.json - тот же файл, который ведёт LoadScheduleController
// в существующем интерфейсе приложения. См. README, "Источник прогноза: JSON"
// за полным описанием формата; кратко:
//
//   {
//     "zones": [ {"name": "Литейка", "ratedPowerKw": 500.0}, ... ],
//     "days":   { "2026-08-27": { "Литейка": [24 числа 0..100], ... }, ... },
//     "prices": { "2026-08-27": { "0": 2.45, ..., "23": 3.00 } },
//     "solar":  { "2026-08-27": { "0": 0.0, ..., "23": 0.0 } },
//     "outages":{ "2026-08-27": { "10": {"off": [15], "on": [45]}, ... } }
//   }
//
// В отличие от ForecastRepository (SQL-вариант), где четыре величины прогноза
// читаются из независимых таблиц, здесь нагрузка, цена, солнце и отключения
// сети ВСЕ приходят из одного файла - переключение источника (SQL/JSON)
// затрагивает их разом, а не по отдельности.
//
// Пересчёт нагрузки: loadKw(дата,час) = Σ по участкам ( %(участок,час)/100 *
// ratedPowerKw(участок) ). Цена в JSON одна на час (без разделения покупка/
// продажа) - используется как buyPrice И sellPrice одновременно (см. README
// за обоснованием этого решения). Отключения сети заданы минутными событиями
// включения/выключения (а не готовым булевым рядом) - см. buildOutageTimeline()
// и GridAvailabilityTimeline.h за тем, как это превращается в
// DispatchIntervalForecast::gridAvailable консервативно (интервал управления
// доступен только если сеть доступна на всём его протяжении).
class JsonForecastRepository : public IForecastRepository
{
public:
    // Пустая строка - loadSchedule.json рядом с исполняемым файлом (тот же
    // путь по умолчанию, что использует LoadScheduleController).
    explicit JsonForecastRepository(const QString &filePath = QString());

    void setFilePath(const QString &path) { m_filePath = path; }
    QString filePath() const { return m_filePath; }

    // Сигнатура намеренно идентична ForecastRepository::buildHorizon - см. его
    // комментарий за общим контрактом (ступенчатый ресэмплинг почасовых
    // данных на интервалы управления, errorMessage собирает непустые
    // предупреждения, но не мешает построению горизонта с значениями по
    // умолчанию для недостающих данных).
    ForecastHorizon buildHorizon(const QDateTime &from, int horizonHours, int controlPeriodMinutes,
                                 QString *errorMessage = nullptr) const override;

private:
    QVector<GridStateChange> buildOutageTimeline(const QJsonObject &outagesRoot,
                                                  const QDateTime &horizonEnd) const;

    QString m_filePath;
};
