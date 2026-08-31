cmake_minimum_required(VERSION 3.16)
project(CopperDownloadManager VERSION 0.3.6 LANGUAGES CXX)

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
    enable_language(RC)
elseif(APPLE)
    add_definitions(-DPLATFORM_MACOS)
else()
    add_definitions(-DPLATFORM_LINUX)
endif()

file(GLOB_RECURSE SOURCES "src/*.cpp")
file(GLOB_RECURSE HEADERS "include/*.h")

set(RESOURCE_FILES resources.qrc)

set(ALL_SOURCES main.cpp ${SOURCES} ${HEADERS} ${RESOURCE_FILES})

if(WIN32)
    # Compile the version/icon resource from a copy in the (space-free) build
    # tree. MinGW windres re-tokenizes include/argument lists and breaks when
    # the source path contains spaces (e.g. "Copper Download Manager"). By
    # compiling the RC from a build-tree copy we sidestep that entirely.
    set(RESRC_DIR "${CMAKE_BINARY_DIR}/_resrc")
    set(RESRC_RC "${RESRC_DIR}/app.rc")
    configure_file("${CMAKE_SOURCE_DIR}/app.rc" "${RESRC_RC}" COPYONLY)
    file(COPY "${CMAKE_SOURCE_DIR}/Assets/app.ico" DESTINATION "${RESRC_DIR}/Assets")
    list(APPEND ALL_SOURCES "${RESRC_RC}")
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

# Project include dirs and the LOG_DIR define are for C++ sources only.
# GNU windres re-tokenizes arguments and does not honor quotes, so passing a
# path containing spaces (as in ".../Copper Download Manager/...") to the
# resource compiler breaks preprocessing ("fatal error: Manager/include/db").
# Guard them with $<COMPILE_LANGUAGE:RESOURCE> so the RC step receives none.
target_include_directories(${PROJECT_NAME} PRIVATE
    "$<$<NOT:$<COMPILE_LANGUAGE:RC>>:${CMAKE_SOURCE_DIR}/include>"
    "$<$<NOT:$<COMPILE_LANGUAGE:RC>>:${CMAKE_SOURCE_DIR}/include/core>"
    "$<$<NOT:$<COMPILE_LANGUAGE:RC>>:${CMAKE_SOURCE_DIR}/include/ui>"
    "$<$<NOT:$<COMPILE_LANGUAGE:RC>>:${CMAKE_SOURCE_DIR}/include/utils>"
    "$<$<NOT:$<COMPILE_LANGUAGE:RC>>:${CMAKE_SOURCE_DIR}/include/torrent>"
    "$<$<NOT:$<COMPILE_LANGUAGE:RC>>:${CMAKE_SOURCE_DIR}/include/db>"
)
target_link_libraries(${PROJECT_NAME} Qt6::Core Qt6::Widgets Qt6::Network Qt6::Sql Qt6::Gui)

if(LIBTORRENT_FOUND)
    target_link_libraries(${PROJECT_NAME} ${LIBTORRENT_LIBRARIES})
endif()

if(WIN32)
    target_link_libraries(${PROJECT_NAME} ws2_32 shell32)
    # Link as a GUI application (subsystem 2) for ALL Windows compilers, not
    # only MSVC. Without this, MinGW builds the executable as a console app
    # (subsystem 3), which opens an unwanted log/terminal window on launch
    # displaying qDebug() output.
    set_target_properties(${PROJECT_NAME} PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

if(UNIX AND NOT APPLE)
    target_link_libraries(${PROJECT_NAME} pthread)
endif()

# Native messaging host: small Qt Core console executable spawned by the browser
# to forward extension requests to the desktop app over the named pipe. It must
# link Core only (no Widgets/GUI) and stay a console subsystem app so stdin/stdout
# native messaging works when the browser launches it.
add_executable(copper_native_host nativehost/main.cpp)
set_target_properties(copper_native_host PROPERTIES
    WIN32_EXECUTABLE FALSE
)
target_include_directories(copper_native_host PRIVATE "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(copper_native_host Qt6::Core Qt6::Network)
if(WIN32)
    target_link_libraries(copper_native_host ws2_32)
endif()

# Ship the host next to the app exe and into the release folder.
if(WIN32)
    set(RELEASE_DIR "${CMAKE_SOURCE_DIR}/installer/release/${PROJECT_VERSION}")
    add_custom_command(TARGET copper_native_host POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${RELEASE_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:copper_native_host>" "${RELEASE_DIR}/"
        COMMENT "Deploying native messaging host to ${RELEASE_DIR}/"
    )
endif()

install(TARGETS copper_native_host RUNTIME DESTINATION bin)

install(TARGETS ${PROJECT_NAME}
    BUNDLE DESTINATION .
    RUNTIME DESTINATION bin
)

if(WIN32)
    find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${QT_DIR}/../../../bin" "${_qt_bin_dir}")
    if(NOT WINDEPLOYQT_EXECUTABLE)
        set(WINDEPLOYQT_EXECUTABLE "${CMAKE_PREFIX_PATH}/bin/windeployqt.exe")
    endif()

    set(RELEASE_DIR "${CMAKE_SOURCE_DIR}/installer/release/${PROJECT_VERSION}")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${RELEASE_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:${PROJECT_NAME}>" "${RELEASE_DIR}/"
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/THIRD-PARTY-NOTICES.txt" "${RELEASE_DIR}/"
        COMMAND "${WINDEPLOYQT_EXECUTABLE}" --release --no-translations --no-opengl-sw --dir "${RELEASE_DIR}" "$<TARGET_FILE:${PROJECT_NAME}>"
        COMMAND ${CMAKE_COMMAND} -E remove -f "${RELEASE_DIR}/sqldrivers/qsqlibase.dll" "${RELEASE_DIR}/sqldrivers/qsqlmimer.dll" "${RELEASE_DIR}/sqldrivers/qsqloci.dll" "${RELEASE_DIR}/sqldrivers/qsqlodbc.dll" "${RELEASE_DIR}/sqldrivers/qsqlpsql.dll"
        COMMENT "Deploying to installer/release/${PROJECT_VERSION}/"
    )
endif()
