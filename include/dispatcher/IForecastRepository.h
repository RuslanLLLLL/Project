#pragma once

#include <QString>

#include "ForecastTypes.h"

// Абстракция источника прогноза для DispatchEngine. ForecastRepository (SQL)
// и JsonForecastRepository уже реализуют этот интерфейс "бесплатно" - у обоих
// уже есть buildHorizon() с точно такой же сигнатурой. Он выделен в отдельный
// интерфейс, чтобы хост-приложение могло подключить СВОЙ источник прогноза
// (например, уже существующую в приложении систему хранения телеметрии/
// прогноза - см. README, "Пользовательский источник прогноза"), не внося в
// этот репозиторий зависимость от типов конкретного приложения.
//
// Использование: реализуйте buildHorizon() в своём классе (может жить вне
// этого репозитория - зависимость только от ForecastTypes.h), передайте
// указатель в DispatchEngine::setCustomForecastRepository() и выберите
// ForecastSourceType::Custom через DispatchEngine::setForecastSourceType().
// DispatchEngine не владеет переданным указателем - как и с
// setModbusClient(), жизненный цикл остаётся за вызывающим кодом.
class IForecastRepository
{
public:
    virtual ~IForecastRepository() = default;

    // Строит горизонт интервалов управления длительностью
    // controlPeriodMinutes (10..60 минут), начиная с from, на horizonHours
    // часов вперёд. errorMessage (если задан) должен получить непустую
    // строку при частичных/отсутствующих данных - план всё равно должен
    // быть построен (с разумными значениями по умолчанию для недостающих
    // интервалов), а не оставлен пустым, см. ForecastRepository/
    // JsonForecastRepository за примером ожидаемого поведения при неполных
    // данных.
    virtual ForecastHorizon buildHorizon(const QDateTime &from, int horizonHours, int controlPeriodMinutes,
                                          QString *errorMessage = nullptr) const = 0;
};
