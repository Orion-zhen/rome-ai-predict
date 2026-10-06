foreach(test host composition)
    add_executable(test-weasel-${test} ${test}.cpp)
    target_link_libraries(test-weasel-${test} PRIVATE rome-weasel-runtime)
    target_compile_options(test-weasel-${test} PRIVATE ${ROME_AI_WARNINGS})
    add_test(NAME weasel-${test} COMMAND test-weasel-${test})
endforeach()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_executable(test-weasel-session session.cpp)
target_link_libraries(test-weasel-session PRIVATE rome-weasel-runtime)
target_compile_options(test-weasel-session PRIVATE ${ROME_AI_WARNINGS})
foreach(scenario retained-range no-text-edit cold-preedit cold-missing cold-recovery late-baseline
                 preview-block read-race changed-target changed-cursor changed-suffix
                 focus-lost late-model menu-selection unowned-space shifted-menu new-composition)
    add_test(NAME weasel-session-${scenario} COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/session.py"
        "$<TARGET_FILE:test-weasel-session>" "${scenario}")
    set_tests_properties(weasel-session-${scenario} PROPERTIES ENVIRONMENT "PYTHONUTF8=1" TIMEOUT 20)
endforeach()
if(WEASEL_RIME_DLL)
    add_test(NAME weasel-installed-package COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/installed.py"
        "${CMAKE_COMMAND}" "${PROJECT_BINARY_DIR}" "$<CONFIG>" "${WEASEL_RIME_DLL}")
    set_tests_properties(weasel-installed-package PROPERTIES ENVIRONMENT "PYTHONUTF8=1" TIMEOUT 60)
endif()
