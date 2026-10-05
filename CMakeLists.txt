cmake_minimum_required(VERSION 3.20)
project(rome-ai-predict LANGUAGES CXX)

# 默认用户级安装。显式指定 CMAKE_INSTALL_PREFIX 时遵循调用方的选择。
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/.local" CACHE PATH "Install prefix" FORCE)
endif()
include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(Fcitx5Core REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
pkg_check_modules(JSONCPP REQUIRED IMPORTED_TARGET jsoncpp)
pkg_check_modules(YAML REQUIRED IMPORTED_TARGET yaml-cpp)

add_library(rome-ai-backend STATIC src/settings.cpp src/completion.cpp)
set_target_properties(rome-ai-backend PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(rome-ai-backend PUBLIC Fcitx5::Utils Threads::Threads
    PkgConfig::CURL PkgConfig::JSONCPP PkgConfig::YAML)
target_compile_options(rome-ai-backend PRIVATE -Wall -Wextra -Wpedantic)

add_library(rome-ai-predict MODULE src/addon.cpp)
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

option(BUILD_TESTING "Build integration and installation tests" OFF)
include(CTest)
if(BUILD_TESTING)
    # 构建目录中的描述文件仅供隔离测试，库由测试环境的 FCITX_ADDON_DIRS 定位。
    set(ROME_AI_LIBRARY librome-ai-predict)
    configure_file(src/rome-ai-predict.conf.in
        share/fcitx5/addon/rome-ai-predict.conf @ONLY)
    configure_file(data/rome-ai-predict.yaml
        share/fcitx5/conf/rome-ai-predict.yaml COPYONLY)
    add_subdirectory(tests)
endif()
