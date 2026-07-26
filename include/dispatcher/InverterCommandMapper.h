#pragma once

#include "DispatchPermissions.h"
#include "DispatchTypes.h"
#include "InverterRegisterMap.h"

// Преобразует результат оптимизатора (DispatchInterval - как правило, ПЕРВЫЙ
// интервал только что рассчитанного плана) в набор записей Modbus-регистров
// инвертора Deye SUN...K-PCS01HP3 (см. InverterRegisterMap.h за обоснованием
// выбора конкретных регистров).
//
// Мэппер намеренно не содержит операций ввода-вывода (не знает о Modbus/QIODevice) -
// это чистая функция преобразования данных, что упрощает модульное тестирование.
// Реальную запись в устройство выполняет ModbusInverterClient::applyCommandSet().
class InverterCommandMapper
{
public:
    // liveBatteryVoltageV - последнее измеренное напряжение СНЭ (регистр 3015,
    // уже переведённое в вольты), необходимо для перевода плановой мощности
    // заряда/разряда в токовые уставки регистров 2040/2041 (см. README,
    // "Отображение на регистры инвертора").
    static deye::InverterCommandSet buildCommandSet(const DispatchInterval &interval,
                                                      const SystemConfig &config,
                                                      const DispatchPermissions &permissions,
                                                      double liveBatteryVoltageV);
};
