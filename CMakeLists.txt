cmake_minimum_required(VERSION 3.16)
project(sysmon VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(SYSMON_BUILD_GUI  "Build the Qt application" ON)
option(SYSMON_BUILD_DUMP "Build sysmon-dump, a console tool that prints what the backend reads" OFF)

find_package(Threads REQUIRED)

# ---- Backend: plain C++, no Qt ---------------------------------------------
add_library(sysbackend STATIC
    source/sys/sysinfo.h
    source/sys/internal.h
    source/sys/util.h
    source/sys/util.cpp
    source/sys/sampler.cpp
    source/sys/procs.cpp
    source/sys/gpu.cpp
)
target_include_directories(sysbackend PUBLIC source)
target_link_libraries(sysbackend PUBLIC ${CMAKE_DL_LIBS} Threads::Threads)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(sysbackend PRIVATE -Wall -Wextra)
endif()

if(SYSMON_BUILD_DUMP)
    add_executable(sysmon-dump tests/backend_dump.cpp)
    target_link_libraries(sysmon-dump PRIVATE sysbackend)
endif()

# ---- Qt application (Qt 6, or Qt 5.15 if Qt 6 is not installed) -------------
if(SYSMON_BUILD_GUI)
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTORCC ON)
    find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets)
    find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets)

    add_executable(sysmon
        source/main.cpp
        source/format.h            source/format.cpp
        source/samplerworker.h     source/samplerworker.cpp
        source/graphwidget.h       source/graphwidget.cpp
        source/performancetab.h    source/performancetab.cpp
        source/typeaheadview.h     source/typeaheadview.cpp
        source/processmodel.h      source/processmodel.cpp
        source/processtab.h        source/processtab.cpp
        source/servicestab.h       source/servicestab.cpp
        source/mainwindow.h        source/mainwindow.cpp
        data/sysmon.qrc
    )
    target_link_libraries(sysmon PRIVATE sysbackend Qt${QT_VERSION_MAJOR}::Widgets)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(sysmon PRIVATE -Wall -Wextra)
    endif()

    include(GNUInstallDirs)
    install(TARGETS sysmon RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES sysmon.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    foreach(size 16 22 24 32 48 64 128 256 512)
        install(FILES data/icons/sysmon-${size}.png
                DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps
                RENAME sysmon.png)
    endforeach()
    install(FILES data/icons/sysmon.svg
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
        install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/sysmon)
    endif()

    # .deb (Debian, Ubuntu) and .rpm (Fedora) packages: run `cpack` in the build directory.
    include(cmake/Packaging.cmake)
endif()
