# Projet externe minimal : vérifie qu'une installation de RavenCore se
# consomme par find_package, sans les sources du dépôt (voir la CI).
cmake_minimum_required(VERSION 3.22.1)
project(ravenemu_package_consumer LANGUAGES CXX)

# L'ancien nom d'abord : il doit trouver seul RavenCore dans la même
# installation. Le nom actuel réutilise ensuite ce qui a été chargé.
find_package(RavenEmuCores 0.1 REQUIRED)
find_package(RavenCore 0.1 REQUIRED)
cmake_path(GET RavenEmuCores_DIR PARENT_PATH package_root)
cmake_path(SET expected_dir NORMALIZE "${package_root}/RavenCore")
cmake_path(SET actual_dir NORMALIZE "${RavenCore_DIR}")
if(NOT actual_dir STREQUAL expected_dir)
    message(FATAL_ERROR "RavenEmuCores a chargé RavenCore hors de son installation : ${RavenCore_DIR}")
endif()

add_executable(ravenemu_package_consumer main.cpp)
target_link_libraries(ravenemu_package_consumer PRIVATE
    RavenEmu::ravenemu_cores RavenEmu::ravenemu_c_api)
