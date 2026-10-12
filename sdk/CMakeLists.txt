add_library(ravenemu_c_api STATIC src/bridge.cpp)
target_compile_features(ravenemu_c_api PUBLIC cxx_std_20)
target_include_directories(ravenemu_c_api PUBLIC include)
target_link_libraries(ravenemu_c_api PUBLIC ravenemu_cores)
set_target_properties(ravenemu_c_api PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(BUILD_TESTING)
    add_executable(ravenemu_c_api_tests tests/bridge_test.cpp)
    target_link_libraries(ravenemu_c_api_tests PRIVATE ravenemu_c_api)
    target_include_directories(ravenemu_c_api_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/cores/common/tests/support)
    add_test(NAME ravenemu_c_api COMMAND ravenemu_c_api_tests)
endif()
