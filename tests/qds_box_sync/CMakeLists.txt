get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)

add_executable(${_TEST_NAME}_tests
    qds_box_sync_tests.cpp
    ${PROJECT_SOURCE_DIR}/src/slic3r/GUI/DeviceCore/QDSBoxSync.cpp
)

target_include_directories(${_TEST_NAME}_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(${_TEST_NAME}_tests test_common)
set_property(TARGET ${_TEST_NAME}_tests PROPERTY FOLDER "tests")
set_property(TARGET ${_TEST_NAME}_tests PROPERTY MACOSX_BUNDLE FALSE)
add_test(${_TEST_NAME}_tests ${_TEST_NAME}_tests "${CATCH_EXTRA_ARGS} exclude:[NotWorking]")

if (APPLE)
    find_package(wxWidgets 3.1 REQUIRED COMPONENTS core base)
    include(${wxWidgets_USE_FILE})

    add_executable(qds_dialog_lifecycle_tests
        qds_dialog_lifecycle_tests.cpp
    )
    target_include_directories(qds_dialog_lifecycle_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/src
        ${wxWidgets_INCLUDE_DIRS}
    )
    target_compile_definitions(qds_dialog_lifecycle_tests PRIVATE wxDEBUG_LEVEL=0)
    target_link_libraries(qds_dialog_lifecycle_tests PRIVATE ${wxWidgets_LIBRARIES})
    set_property(TARGET qds_dialog_lifecycle_tests PROPERTY FOLDER "tests")
    set_property(TARGET qds_dialog_lifecycle_tests PROPERTY MACOSX_BUNDLE FALSE)
    add_test(qds_dialog_lifecycle_tests qds_dialog_lifecycle_tests)
endif()
