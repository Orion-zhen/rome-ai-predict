find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_executable(test-shared shared/shared.cpp)
target_link_libraries(test-shared PRIVATE rome-ai-backend)
target_compile_options(test-shared PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME shared-surrounding COMMAND test-shared surrounding)
add_test(NAME shared-settings COMMAND test-shared settings "${CMAKE_CURRENT_BINARY_DIR}")
foreach(case llama standard auth bytes dedup filter utf8 length special first-position empty
             json duplicate-key trailing-json shape score invalid-bytes missing-bytes
             http-error redirect big timeout queued-cancel queued-replaced queued-destroyed
             active-cancel active-replaced active-shutdown)
    add_test(NAME shared-${case}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/shared/shared.py
            $<TARGET_FILE:test-shared> ${case})
    set_tests_properties(shared-${case} PROPERTIES TIMEOUT 15)
endforeach()

if(BUILD_RIME_TESTS)
    add_executable(test-rime rime/rime.cpp ${PROJECT_SOURCE_DIR}/src/rime/rime_module.cpp)
    target_link_libraries(test-rime PRIVATE rome-ai-rime)
    set_source_files_properties(${PROJECT_SOURCE_DIR}/src/rime/rime_module.cpp
        TARGET_DIRECTORY test-rime PROPERTIES COMPILE_OPTIONS "-Wno-missing-field-initializers")
    target_compile_options(test-rime PRIVATE -Wall -Wextra -Wpedantic)
    foreach(case automatic tab number keypad mouse continued unicode selection formatted
                 no-context disabled ascii invalid-config no-ack unexpected-edit destroyed
                 stale-result focus same-text-other-control input-source api-error cursor
                 window-origin escape typing space ctrl-tab menu-off stale-mouse busy-panel
                 replaced-panel new-session schema-change reload release switch-menu)
        add_test(NAME rime-${case}
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/rime/rime.py
                $<TARGET_FILE:test-rime> ${CMAKE_CURRENT_BINARY_DIR} ${case})
        set_tests_properties(rime-${case} PROPERTIES TIMEOUT 15)
    endforeach()
endif()

if(NOT BUILD_FCITX5)
    return()
endif()
pkg_check_modules(RIME REQUIRED IMPORTED_TARGET rime)
# 诊断工具仅供开发测试，不安装。
add_executable(rome-ai-probe ${PROJECT_SOURCE_DIR}/tools/probe.cpp)
target_link_libraries(rome-ai-probe PRIVATE rome-ai-backend Fcitx5::Utils)
target_compile_options(rome-ai-probe PRIVATE -Wall -Wextra -Wpedantic)

add_executable(test-integration fcitx5/integration.cpp)
target_link_libraries(test-integration PRIVATE Fcitx5::Core PkgConfig::RIME)
target_compile_options(test-integration PRIVATE -Wall -Wextra -Wpedantic)
add_dependencies(test-integration rome-ai-predict rome-ai-probe)

foreach(case automatic continued typing escape cursor focus destroyed
             no-context stale-context selection stale-candidate timeout
             other-im busy-panel replaced-panel unicode switch-im
             disabled no-extension missing-config invalid-config
             global-config partial-config empty-config invalid-global
             menu-on menu-off menu-cancel api-error api-json api-shape
             api-empty api-timeout api-dedup api-auth api-big api-probe
             new-request shutdown api-space api-standard api-first-position
             api-bytes api-special api-score api-invalid-bytes release-after-show
             release-ai-selection release-typing release-shift release-cursor
             release-panel release-filtered release-switch
             physical-digit physical-tab physical-escape physical-keypad
             physical-numlock physical-ctrl physical-alt physical-out-of-range
             physical-shift physical-ctrl-tab physical-held-tab)
    add_test(NAME ${case}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/fcitx5/run.py
            $<TARGET_FILE:test-integration> ${CMAKE_CURRENT_BINARY_DIR} ${case}
            ${PROJECT_BINARY_DIR}/share $<TARGET_FILE_DIR:rome-ai-predict>
            $<TARGET_FILE:rome-ai-probe>
            ${PROJECT_SOURCE_DIR}/data/rome-ai-predict.yaml)
    set_tests_properties(${case} PROPERTIES TIMEOUT 20)
endforeach()

add_test(NAME install-standalone
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/fcitx5/install.py
        ${PROJECT_SOURCE_DIR} ${PROJECT_BINARY_DIR} ${CMAKE_COMMAND}
        ${CMAKE_GENERATOR} ${FCITX_SYS_INSTALL_PREFIX})
set_tests_properties(install-standalone PROPERTIES TIMEOUT 180)
