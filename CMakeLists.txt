cmake_minimum_required(VERSION 3.16)
project(HybridEnergyDispatcher LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)

# ---------------------------------------------------------------------------
# Опции сборки
# ---------------------------------------------------------------------------
option(ENABLE_HIGHS "Собрать MilpDispatchOptimizer с реальным MILP-солвером HiGHS \
(если выключено - метод MILP компилируется, но на этапе выполнения делегирует \
расчёт эвристике с предупреждением - см. README)" OFF)
option(BUILD_GUI "Собрать встраиваемый виджет DispatcherPanel (модуль Qt Widgets)" ON)
option(BUILD_EXAMPLES "Собрать демонстрационный пример интеграции" ON)

# ---------------------------------------------------------------------------
# Qt (поддержка и Qt5, и Qt6 - стандартный приём поиска через QT_VERSION_MAJOR)
# ---------------------------------------------------------------------------
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core)
# Network - нужен ModbusInverterClient (транспорт Solarman V5, QTcpSocket) и
# SolarmanDiscovery (поиск логгера в сети, QUdpSocket); SerialBus сам по себе
# тянет Network транзитивно, но эти два класса используют его напрямую.
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Sql SerialBus Network)
if(BUILD_GUI OR BUILD_EXAMPLES)
    find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets)
endif()

# ---------------------------------------------------------------------------
# dispatcher_core - вся логика диспетчеризации без GUI-зависимостей
# (Core/Sql/SerialBus), подходит для встраивания в headless-сервисы.
# ---------------------------------------------------------------------------
# Заголовки с Q_OBJECT (DispatchEngine.h, ModbusInverterClient.h) должны быть
# явно перечислены как источники цели - иначе CMake AUTOMOC их не находит и
# молча не генерирует moc-код (сигналы/vtable), что приводит к ошибкам
# компоновщика "undefined reference to vtable/staticMetaObject/сигнал" при
# сборке чего-либо, что реально линкуется с dispatcher_core (сама статическая
# библиотека при этом собирается без единой ошибки - проблема всплывает
# только на этапе линковки исполняемого файла).
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
# dispatcher_gui - встраиваемый виджет DispatcherPanel (см. gui/DispatcherPanel.h)
# ---------------------------------------------------------------------------
if(BUILD_GUI)
    add_library(dispatcher_gui STATIC
        gui/DispatcherPanel.cpp
        gui/DispatcherPanel.h
    )
    target_include_directories(dispatcher_gui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/gui)
    target_link_libraries(dispatcher_gui PUBLIC dispatcher_core Qt${QT_VERSION_MAJOR}::Widgets)
endif()

# ---------------------------------------------------------------------------
# Демонстрационный пример встраивания (см. examples/integration_example.cpp)
# ---------------------------------------------------------------------------
if(BUILD_EXAMPLES AND BUILD_GUI)
    add_executable(dispatcher_example examples/integration_example.cpp)
    target_link_libraries(dispatcher_example PRIVATE dispatcher_gui)
endif()
