cmake_minimum_required(VERSION 3.16)
project(HybridEnergyDispatcher LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# ---------------------------------------------------------------------------
# Опции сборки
# ---------------------------------------------------------------------------
option(ENABLE_HIGHS "Собрать MilpDispatchOptimizer с реальным MILP-солвером HiGHS \
(если выключено - метод MILP компилируется, но на этапе выполнения делегирует \
расчёт эвристике с предупреждением - см. README)" OFF)
option(BUILD_EXAMPLES "Собрать демонстрационный пример интеграции" ON)

# ---------------------------------------------------------------------------
# Qt (поддержка и Qt5, и Qt6 - стандартный приём поиска через QT_VERSION_MAJOR)
# ---------------------------------------------------------------------------
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core)
# Network - нужен ModbusInverterClient (транспорт Solarman V5, QTcpSocket) и
# SolarmanDiscovery (поиск логгера в сети, QUdpSocket); SerialBus сам по себе
# тянет Network транзитивно, но эти два класса используют его напрямую.
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Sql SerialBus Network)

# ---------------------------------------------------------------------------
# dispatcher_core - вся логика диспетчеризации: оптимизаторы, отображение на
# регистры инвертора, транспорт Modbus (RTU/Solarman V5), загрузка прогноза
# из SQL, QObject-контроллер скользящего горизонта. Это единственная цель
# сборки этого репозитория - весь код GUI-нейтрален (зависит только от Qt
# Core/Sql/SerialBus/Network) и рассчитан на встраивание в уже существующий
# интерфейс хост-приложения, а не на использование собственного виджета.
# ---------------------------------------------------------------------------
# Заголовки с Q_OBJECT (DispatchEngine.h, ModbusInverterClient.h,
# SolarmanDiscovery.h) должны быть явно перечислены как источники цели -
# иначе CMake AUTOMOC их не находит и молча не генерирует moc-код (сигналы/
# vtable), что приводит к ошибкам компоновщика "undefined reference to
# vtable/staticMetaObject/сигнал" при сборке чего-либо, что реально линкуется
# с dispatcher_core (сама статическая библиотека при этом собирается без
# единой ошибки - проблема всплывает только на этапе линковки исполняемого
# файла).
add_library(dispatcher_core STATIC
    include/dispatcher/DispatchEngine.h
    include/dispatcher/ModbusInverterClient.h
    include/dispatcher/SolarmanDiscovery.h
    src/HeuristicDispatchOptimizer.cpp
    src/MilpDispatchOptimizer.cpp
    src/InverterCommandMapper.cpp
    src/ModbusInverterClient.cpp
    src/SolarmanV5Codec.cpp
    src/SolarmanDiscovery.cpp
    src/ForecastRepository.cpp
    src/DispatchEngine.cpp
)
target_include_directories(dispatcher_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(dispatcher_core PUBLIC
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Sql
    Qt${QT_VERSION_MAJOR}::SerialBus
    Qt${QT_VERSION_MAJOR}::Network
)

if(ENABLE_HIGHS)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/FindOrFetchHiGHS.cmake)
    target_link_libraries(dispatcher_core PUBLIC highs::highs)
    target_compile_definitions(dispatcher_core PUBLIC ENABLE_HIGHS)
endif()

# ---------------------------------------------------------------------------
# Демонстрационный пример встраивания (см. examples/integration_example.cpp) -
# консольное приложение, показывающее подключение DispatchEngine к БД и
# инвертору и обработку его сигналов; никакого GUI-фреймворка не навязывает.
# ---------------------------------------------------------------------------
if(BUILD_EXAMPLES)
    add_executable(dispatcher_example examples/integration_example.cpp)
    target_link_libraries(dispatcher_example PRIVATE dispatcher_core)
endif()
