# Differential-test harness (ADR-0006).

# Hypervisor.framework oracle: macOS on Apple Silicon only. The binary must be
# ad-hoc signed with the hypervisor entitlement, otherwise hv_vm_create fails.
if(LGP_ENABLE_HV)
    find_library(HYPERVISOR_FRAMEWORK Hypervisor REQUIRED)
    find_program(CODESIGN_EXECUTABLE codesign REQUIRED)

    add_executable(lgp_hv_spike hv/hv_spike.cpp)
    target_link_libraries(lgp_hv_spike PRIVATE ${HYPERVISOR_FRAMEWORK})
    add_custom_command(TARGET lgp_hv_spike POST_BUILD
        COMMAND "${CODESIGN_EXECUTABLE}" --force --sign -
                --entitlements "${CMAKE_CURRENT_SOURCE_DIR}/hv/hypervisor.entitlements"
                "$<TARGET_FILE:lgp_hv_spike>"
        COMMENT "Ad-hoc signing lgp_hv_spike with the hypervisor entitlement"
        VERBATIM)

    add_test(NAME difftest.hv_spike COMMAND lgp_hv_spike)
    set_tests_properties(difftest.hv_spike PROPERTIES LABELS "difftest;hv")

    # HV executor (P2.7): test code at EL0 with the MMU on.
    add_library(lgp_difftest_hv STATIC oracle.cpp hv/hv_machine.cpp)
    target_include_directories(lgp_difftest_hv PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(lgp_difftest_hv PUBLIC ${HYPERVISOR_FRAMEWORK})

    # Ad-hoc signature with the hypervisor entitlement (hv_vm_create needs it).
    function(lgp_hv_sign target)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CODESIGN_EXECUTABLE}" --force --sign -
                    --entitlements "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/hv/hypervisor.entitlements"
                    "$<TARGET_FILE:${target}>"
            COMMENT "Ad-hoc signing ${target} with the hypervisor entitlement"
            VERBATIM)
    endfunction()

    add_executable(lgp_test_hv_executor hv/test_hv_executor.cpp)
    target_link_libraries(lgp_test_hv_executor PRIVATE lgp_difftest_hv)
    if(TARGET lgp_difftest_unicorn)
        # oracle.cpp is in both libraries; link the Unicorn one without it twice.
        target_link_libraries(lgp_test_hv_executor PRIVATE lgp_difftest_unicorn)
        target_compile_definitions(lgp_test_hv_executor PRIVATE LGP_HV_TEST_UNICORN=1)
    endif()
    lgp_hv_sign(lgp_test_hv_executor)
    add_test(NAME difftest.hv_executor COMMAND lgp_test_hv_executor)
    set_tests_properties(difftest.hv_executor PROPERTIES LABELS "difftest;hv")
endif()

# Function-level differential test (stage 3); needs the HV library above.
if(NOT CMAKE_CROSSCOMPILING)
    add_subdirectory(fn)
endif()
