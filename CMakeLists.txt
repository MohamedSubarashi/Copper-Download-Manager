cmake_minimum_required(VERSION 3.16)
project(CopperDownloadManager VERSION 0.1.5 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/Zc:__cplusplus /permissive- /MP)
    add_definitions(-D_SILENCE_ALL_CXX17_DEPRECATION_WARNINGS)
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network Sql Gui)

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBTORRENT QUIET libtorrent-rasterbar)
endif()

if(LIBTORRENT_FOUND)
    include_directories(${LIBTORRENT_INCLUDE_DIRS})
    add_definitions(-DHAVE_LIBTORRENT)
    message(STATUS "libtorrent found - torrent support enabled")
else()
    message(STATUS "libtorrent not found - torrent support disabled (using aria2c)")
endif()

if(WIN32)
    add_definitions(-DPLATFORM_WINDOWS)
elseif(APPLE)
    add_definitions(-DPLATFORM_MACOS)
else()
    add_definitions(-DPLATFORM_LINUX)
endif()

add_definitions(-DLOG_DIR="${CMAKE_SOURCE_DIR}")

include_directories(${CMAKE_SOURCE_DIR}/include)
include_directories(${CMAKE_SOURCE_DIR}/include/core)
include_directories(${CMAKE_SOURCE_DIR}/include/ui)
include_directories(${CMAKE_SOURCE_DIR}/include/utils)
include_directories(${CMAKE_SOURCE_DIR}/include/torrent)
include_directories(${CMAKE_SOURCE_DIR}/include/db)

file(GLOB_RECURSE SOURCES "src/*.cpp")
file(GLOB_RECURSE HEADERS "include/*.h")

set(RESOURCE_FILES resources.qrc)

set(ALL_SOURCES main.cpp ${SOURCES} ${HEADERS} ${RESOURCE_FILES})

if(WIN32)
    list(APPEND ALL_SOURCES app.rc)
endif()

if(APPLE)
    set(MACOSX_BUNDLE_ICON_FILE app.ico)
    set(APP_ICON "${CMAKE_SOURCE_DIR}/Assets/app.ico")
    set_source_files_properties(${APP_ICON} PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources"
    )
    add_executable(${PROJECT_NAME} MACOSX_BUNDLE ${ALL_SOURCES} ${APP_ICON})
    set_target_properties(${PROJECT_NAME} PROPERTIES
        MACOSX_BUNDLE_BUNDLE_NAME "Copper Download Manager"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.copper.downloadmanager"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/Info.plist.in"
    )
else()
    add_executable(${PROJECT_NAME} ${ALL_SOURCES})
endif()

target_link_libraries(${PROJECT_NAME} Qt6::Core Qt6::Widgets Qt6::Network Qt6::Sql Qt6::Gui)

if(LIBTORRENT_FOUND)
    target_link_libraries(${PROJECT_NAME} ${LIBTORRENT_LIBRARIES})
endif()

if(WIN32)
    target_link_libraries(${PROJECT_NAME} ws2_32 shell32)
    if(MSVC)
        set_target_properties(${PROJECT_NAME} PROPERTIES WIN32_EXECUTABLE TRUE)
    endif()
endif()

if(UNIX AND NOT APPLE)
    target_link_libraries(${PROJECT_NAME} pthread)
endif()

install(TARGETS ${PROJECT_NAME}
    BUNDLE DESTINATION .
    RUNTIME DESTINATION bin
)

if(WIN32)
    find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${QT_DIR}/../../../bin" "${_qt_bin_dir}")
    if(NOT WINDEPLOYQT_EXECUTABLE)
        set(WINDEPLOYQT_EXECUTABLE "${CMAKE_PREFIX_PATH}/bin/windeployqt.exe")
    endif()

    set(RELEASE_DIR "${CMAKE_SOURCE_DIR}/releases/${PROJECT_VERSION}")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${RELEASE_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:${PROJECT_NAME}>" "${RELEASE_DIR}/"
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/THIRD-PARTY-NOTICES.txt" "${RELEASE_DIR}/"
        COMMAND "${WINDEPLOYQT_EXECUTABLE}" --release --no-translations --no-opengl-sw --dir "${RELEASE_DIR}" "$<TARGET_FILE:${PROJECT_NAME}>"
        COMMAND ${CMAKE_COMMAND} -E remove -f "${RELEASE_DIR}/sqldrivers/qsqlibase.dll" "${RELEASE_DIR}/sqldrivers/qsqlmimer.dll" "${RELEASE_DIR}/sqldrivers/qsqloci.dll" "${RELEASE_DIR}/sqldrivers/qsqlodbc.dll" "${RELEASE_DIR}/sqldrivers/qsqlpsql.dll"
        COMMENT "Deploying to releases/${PROJECT_VERSION}/"
    )
endif()
