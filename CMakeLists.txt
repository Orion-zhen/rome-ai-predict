cmake_minimum_required(VERSION 3.20)
project(rome-ai-predict LANGUAGES CXX)

# 默认用户级安装。显式指定 CMAKE_INSTALL_PREFIX 时遵循调用方的选择。
if(UNIX AND CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/.local" CACHE PATH "Install prefix" FORCE)
endif()
include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(ROME_AI_FCITX5_DEFAULT OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(ROME_AI_FCITX5_DEFAULT ON)
endif()
option(BUILD_FCITX5 "Build and install the Fcitx5 addon" ${ROME_AI_FCITX5_DEFAULT})
option(BUILD_TESTING "Build tests" OFF)
option(BUILD_SQUIRREL "Build the macOS librime extension for Squirrel" OFF)
option(BUILD_RIME_TESTS "Test the Squirrel component using real librime and a simulated text host" OFF)
option(BUILD_WEASEL "Build and install the Windows Weasel adapter" OFF)
if(MSVC)
    set(ROME_AI_WARNINGS /W4 /utf-8 /permissive-)
else()
    set(ROME_AI_WARNINGS -Wall -Wextra -Wpedantic)
endif()
if(BUILD_RIME_TESTS AND NOT BUILD_TESTING)
    message(FATAL_ERROR "BUILD_RIME_TESTS requires BUILD_TESTING=ON")
endif()
include(CTest)

find_package(Threads REQUIRED)
if(WIN32)
    find_package(CURL REQUIRED)
    find_package(jsoncpp CONFIG REQUIRED)
    find_package(yaml-cpp CONFIG REQUIRED)
    set(ROME_AI_LIBRARIES CURL::libcurl JsonCpp::JsonCpp yaml-cpp::yaml-cpp)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
    pkg_check_modules(JSONCPP REQUIRED IMPORTED_TARGET jsoncpp)
    pkg_check_modules(YAML REQUIRED IMPORTED_TARGET yaml-cpp)
    set(ROME_AI_LIBRARIES PkgConfig::CURL PkgConfig::JSONCPP PkgConfig::YAML)
endif()

add_library(rome-ai-backend STATIC
    src/shared/settings.cpp
    src/shared/completion.cpp
    src/shared/unicode.cpp
    src/shared/surrounding.cpp)
set_target_properties(rome-ai-backend PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(rome-ai-backend PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/shared")
target_link_libraries(rome-ai-backend PUBLIC Threads::Threads
    ${ROME_AI_LIBRARIES})
target_compile_options(rome-ai-backend PRIVATE ${ROME_AI_WARNINGS})
target_compile_definitions(rome-ai-backend PRIVATE "$<$<PLATFORM_ID:Windows>:NOMINMAX>")

if(BUILD_SQUIRREL OR BUILD_RIME_TESTS)
    include(cmake/RimePlugin.cmake)
endif()

if(BUILD_FCITX5)
    find_package(Fcitx5Core REQUIRED)
    add_library(rome-ai-predict MODULE src/fcitx5/addon.cpp)
    target_link_libraries(rome-ai-predict PRIVATE Fcitx5::Core rome-ai-backend)
    target_compile_options(rome-ai-predict PRIVATE -Wall -Wextra -Wpedantic)

    # 用户安装遵循 GNUInstallDirs，保留调用方指定的绝对目录。
    set(ROME_AI_INSTALL_ADDONDIR "${CMAKE_INSTALL_LIBDIR}/fcitx5")
    set(ROME_AI_INSTALL_PKGDATADIR "${CMAKE_INSTALL_DATADIR}/fcitx5")
    # 系统安装采用 Fcitx5 提供的布局，同一前缀下支持 --prefix 和 DESTDIR。
    if(FCITX_INSTALL_USE_FCITX_SYS_PATHS)
        foreach(directory ADDONDIR PKGDATADIR)
            if(FCITX_INSTALL_PREFIX STREQUAL CMAKE_INSTALL_PREFIX)
                file(RELATIVE_PATH ROME_AI_INSTALL_${directory}
                    "${CMAKE_INSTALL_PREFIX}" "${FCITX_INSTALL_${directory}}")
            else()
                set(ROME_AI_INSTALL_${directory} "${FCITX_INSTALL_${directory}}")
            endif()
        endforeach()
    endif()
    install(TARGETS rome-ai-predict
        LIBRARY DESTINATION "${ROME_AI_INSTALL_ADDONDIR}")
    install(FILES data/rome-ai-predict.yaml
        DESTINATION "${ROME_AI_INSTALL_PKGDATADIR}/conf")

    # 用户级描述文件需要安装时的绝对库路径，而不是构建目录或 DESTDIR 路径。
    configure_file(cmake/InstallAddon.cmake.in InstallAddon.cmake @ONLY)
    install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/InstallAddon.cmake")

    if(BUILD_TESTING)
        # 构建目录中的描述文件仅供隔离测试。
        set(ROME_AI_LIBRARY librome-ai-predict)
        configure_file(data/fcitx5/rome-ai-predict.conf.in
            share/fcitx5/addon/rome-ai-predict.conf @ONLY)
        configure_file(data/rome-ai-predict.yaml
            share/fcitx5/conf/rome-ai-predict.yaml COPYONLY)
    endif()
endif()
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
if(BUILD_WEASEL)
    add_subdirectory(src/weasel)
endif()
