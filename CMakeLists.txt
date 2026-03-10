cmake_minimum_required(VERSION 3.16)

project(PADCamResolutionProbe LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt5 REQUIRED COMPONENTS
    Widgets
    Multimedia
    MultimediaWidgets
)

add_executable(PADCamResolutionProbe WIN32
    src/main.cpp
    src/MainWindow.h
    src/MainWindow.cpp
)

target_link_libraries(PADCamResolutionProbe PRIVATE
    Qt5::Widgets
    Qt5::Multimedia
    Qt5::MultimediaWidgets
)

if (MSVC)
    target_compile_options(PADCamResolutionProbe PRIVATE /utf-8)
endif()

target_compile_definitions(PADCamResolutionProbe PRIVATE
    QT_DEPRECATED_WARNINGS
)
