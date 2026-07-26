# =============================================================================
# Подключение HiGHS (https://highs.dev) для MilpDispatchOptimizer.
#
# Порядок поиска:
#   1) find_package(HiGHS CONFIG) - находит HiGHS, установленный системным
#      пакетным менеджером, vcpkg или conan (все они ставят HiGHSConfig.cmake).
#   2) Если не найден - исходники скачиваются с GitHub через FetchContent и
#      собираются как подпроект прямо в рамках сборки этого проекта.
#
# В обоих случаях по завершении доступна цель highs::highs, которую можно
# сразу передавать в target_link_libraries().
#
# Подробное пошаговое описание (какой вариант когда выбирать, типичные ошибки
# сборки и их решения) - см. README.md, раздел "Интеграция HiGHS".
# =============================================================================

find_package(HiGHS CONFIG QUIET)

if(HiGHS_FOUND)
    message(STATUS "dispatcher: HiGHS найден через find_package (CONFIG)")
    if(NOT TARGET highs::highs)
        # Некоторые версии HiGHS экспортируют цель без namespace-префикса.
        add_library(highs::highs ALIAS highs)
    endif()
else()
    message(STATUS "dispatcher: HiGHS не найден в системе - будет собран из "
                    "исходников через FetchContent (см. README, 'Интеграция HiGHS')")
    include(FetchContent)

    # Настройки сборки HiGHS, ускоряющие и упрощающие её как встроенный
    # подпроект: без тестов, статической библиотекой (проще линковка в
    # финальное приложение без танцев с RPATH/установкой .so/.dll рядом).
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
        highs
        GIT_REPOSITORY https://github.com/ERGO-Code/HiGHS.git
        GIT_TAG        v1.7.2
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(highs)

    if(NOT TARGET highs::highs)
        add_library(highs::highs ALIAS highs)
    endif()
endif()
