cmake_minimum_required(VERSION 3.18)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib)
if(MSVC)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS=1)
  # add_link_options(/MANIFEST:NO)
endif()  # MSVC

project(MpvPlayer LANGUAGES C CXX)

option(BUILD_SAMPLE "" OFF)

find_package(QT NAMES Qt6 Qt5 REQUIRED
  COMPONENTS Core Gui Widgets Qml Quick QuickWidgets
)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED
  COMPONENTS Core Gui Widgets Qml Quick QuickWidgets
)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

add_library(${PROJECT_NAME})

target_sources(${PROJECT_NAME} PRIVATE
  include/MpvPlayer.hpp
  src/MpvPlayer.cpp
)

target_include_directories(${PROJECT_NAME} PUBLIC
  ${CMAKE_CURRENT_LIST_DIR}/include
)

target_link_libraries(${PROJECT_NAME} PUBLIC
  Qt${QT_VERSION_MAJOR}::Core
  Qt${QT_VERSION_MAJOR}::Gui
  Qt${QT_VERSION_MAJOR}::Widgets
  Qt${QT_VERSION_MAJOR}::Qml
  Qt${QT_VERSION_MAJOR}::Quick
  Qt${QT_VERSION_MAJOR}::QuickWidgets
)

add_subdirectory(libmpv)
target_link_libraries(${PROJECT_NAME} PUBLIC libmpv)

if(BUILD_SAMPLE)
  add_executable(${PROJECT_NAME}Sample
    sample/sample.hpp
    sample/sample.cpp
    sample/sample.qrc
  )
  target_link_libraries(${PROJECT_NAME}Sample PUBLIC ${PROJECT_NAME})
endif()  # BUILD_SAMPLE
