# Projet externe minimal : vérifie qu'une installation de RavenCore se
# consomme par find_package, sans les sources du dépôt (voir la CI).
cmake_minimum_required(VERSION 3.22.1)
project(ravenemu_package_consumer LANGUAGES CXX)

find_package(RavenCore 0.12 REQUIRED)

add_executable(ravenemu_package_consumer main.cpp)
target_link_libraries(ravenemu_package_consumer PRIVATE
    RavenCore::ravencore RavenCore::ravenemu_c_api)
