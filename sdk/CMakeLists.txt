add_library(ravenemu_c_api STATIC src/bridge.cpp)
target_compile_features(ravenemu_c_api PUBLIC cxx_std_20)
target_include_directories(ravenemu_c_api PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(ravenemu_c_api PUBLIC ravencore)
target_compile_options(ravenemu_c_api PRIVATE ${RAVENEMU_WARNING_OPTIONS})
target_compile_definitions(ravenemu_c_api PRIVATE RAVENEMU_VERSION="${PROJECT_VERSION}")
set_target_properties(ravenemu_c_api PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(BUILD_TESTING)
    add_executable(ravenemu_c_api_tests tests/bridge_test.cpp)
    target_link_libraries(ravenemu_c_api_tests PRIVATE ravenemu_c_api)
    target_include_directories(ravenemu_c_api_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/cores/common/tests/support)
    target_compile_options(ravenemu_c_api_tests PRIVATE ${RAVENEMU_WARNING_OPTIONS})
    target_compile_definitions(ravenemu_c_api_tests PRIVATE RAVENEMU_VERSION="${PROJECT_VERSION}")
    add_test(NAME ravenemu_c_api COMMAND ravenemu_c_api_tests)

    add_executable(ravenemu_remote_link_tests tests/remote_link_test.cpp)
    target_link_libraries(ravenemu_remote_link_tests PRIVATE ravenemu_c_api)
    target_include_directories(ravenemu_remote_link_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/cores/common/tests/support)
    target_compile_options(ravenemu_remote_link_tests PRIVATE ${RAVENEMU_WARNING_OPTIONS})
    add_test(NAME ravenemu_remote_link COMMAND ravenemu_remote_link_tests)
endif()
