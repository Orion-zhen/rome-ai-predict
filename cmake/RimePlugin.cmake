set(RIME_SOURCE_DIR "" CACHE PATH "Source tree matching the host librime")
set(RIME_BUILD_DIR "" CACHE PATH "Host librime build tree containing src/rime/build_config.h")
set(RIME_LIBRARY "" CACHE FILEPATH "Squirrel's embedded librime dynamic library")

if(NOT EXISTS "${RIME_SOURCE_DIR}/src/rime/context.h")
    message(FATAL_ERROR "Set RIME_SOURCE_DIR to the source tree matching the host librime")
endif()
find_package(Boost CONFIG REQUIRED)
add_library(rome-rime-host INTERFACE)
target_include_directories(rome-rime-host SYSTEM INTERFACE "${RIME_SOURCE_DIR}/src")
target_link_libraries(rome-rime-host INTERFACE Boost::headers)
# 上游 RIME_REGISTER_MODULE 宏使用 C 风格的 {0} 初始化。
set_source_files_properties(src/rime/rime_module.cpp PROPERTIES
    COMPILE_OPTIONS "-Wno-missing-field-initializers")
if(BUILD_SQUIRREL)
    if(NOT APPLE)
        message(FATAL_ERROR "BUILD_SQUIRREL requires macOS")
    endif()
    if(NOT EXISTS "${RIME_BUILD_DIR}/src/rime/build_config.h" OR NOT EXISTS "${RIME_LIBRARY}")
        message(FATAL_ERROR "Set RIME_BUILD_DIR and RIME_LIBRARY to Squirrel's actual librime build and library")
    endif()
    target_include_directories(rome-rime-host INTERFACE "${RIME_BUILD_DIR}/src")
    target_link_libraries(rome-rime-host INTERFACE "${RIME_LIBRARY}")
else()
    pkg_check_modules(RIME REQUIRED IMPORTED_TARGET rime)
    configure_file(tests/rime/rime-build-config.h rime-test-include/rime/build_config.h COPYONLY)
    target_include_directories(rome-rime-host INTERFACE "${PROJECT_BINARY_DIR}/rime-test-include")
    target_link_libraries(rome-rime-host INTERFACE PkgConfig::RIME)
endif()

add_library(rome-ai-rime STATIC src/rime/rime_predictor.cpp)
set_target_properties(rome-ai-rime PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(rome-ai-rime PUBLIC "${PROJECT_SOURCE_DIR}/src/rime")
target_link_libraries(rome-ai-rime PUBLIC rome-ai-backend rome-rime-host)
target_compile_options(rome-ai-rime PRIVATE -Wall -Wextra -Wpedantic)

if(BUILD_SQUIRREL)
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 20)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
    set(CMAKE_OBJCXX_EXTENSIONS OFF)
    add_library(rome-ai-squirrel MODULE src/rime/rime_module.cpp src/squirrel/squirrel_host.mm)
    target_link_libraries(rome-ai-squirrel PRIVATE rome-ai-rime
        "-framework Foundation" "-framework AppKit" "-framework ApplicationServices" "-framework Carbon")
    target_compile_options(rome-ai-squirrel PRIVATE -Wall -Wextra -Wpedantic
        "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
    set_target_properties(rome-ai-squirrel PROPERTIES
        PREFIX "librime-" OUTPUT_NAME "rome-ai-predict" SUFFIX ".dylib"
        INSTALL_RPATH "@loader_path/..")
    set(SQUIRREL_PLUGIN_DIR "/Library/Input Methods/Squirrel.app/Contents/Frameworks/rime-plugins"
        CACHE STRING "Squirrel librime plugin install directory")
    install(TARGETS rome-ai-squirrel LIBRARY DESTINATION "${SQUIRREL_PLUGIN_DIR}")
    install(FILES data/rome-ai-predict.yaml DESTINATION "${SQUIRREL_PLUGIN_DIR}"
        RENAME rome-ai-predict.defaults.yaml)
endif()
