cmake_minimum_required(VERSION 3.16)

project(qt_vm_launcher LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(_qt_vm_launcher_build_types Debug Release RelWithDebInfo MinSizeRel)
if(CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES "${_qt_vm_launcher_build_types}" CACHE STRING "Multi-config build types" FORCE)
else()
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "${_qt_vm_launcher_build_types}")
    if(NOT CMAKE_BUILD_TYPE)
        message(STATUS "No build type set. Defaulting to RelWithDebInfo for usable debug symbols.")
        set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
    endif()
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 6.2 REQUIRED COMPONENTS Widgets Network)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBVNCCLIENT REQUIRED libvncclient)

add_executable(qt-vm-launcher
    src/main.cpp
    src/MainWindow.cpp
    src/MainWindow.h
    src/VncClient.cpp
    src/VncClient.h
    src/ProcessUtils.cpp
    src/ProcessUtils.h
    src/TerminalWidget.cpp
    src/TerminalWidget.h
)

target_include_directories(qt-vm-launcher PRIVATE
    ${LIBVNCCLIENT_INCLUDE_DIRS}
)

target_link_directories(qt-vm-launcher PRIVATE
    ${LIBVNCCLIENT_LIBRARY_DIRS}
)

target_compile_definitions(qt-vm-launcher PRIVATE
    ${LIBVNCCLIENT_CFLAGS_OTHER}
)

target_link_libraries(qt-vm-launcher PRIVATE
    Qt6::Widgets
    Qt6::Network
    ${LIBVNCCLIENT_LIBRARIES}
)

install(TARGETS qt-vm-launcher DESTINATION bin)
