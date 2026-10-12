# Ancien nom du paquet RavenCore, gardé jusqu'à la version 1.0 pour les projets
# existants. Il charge RavenCore depuis la même installation ; les cibles
# RavenEmu:: sont les mêmes.
if(NOT RavenEmuCores_FIND_QUIETLY)
    message(DEPRECATION
        "Le paquet RavenEmuCores s'appelle désormais RavenCore : remplacez "
        "find_package(RavenEmuCores) par find_package(RavenCore). Les cibles "
        "RavenEmu:: ne changent pas.")
endif()
include(CMakeFindDependencyMacro)
find_dependency(RavenCore ${RavenEmuCores_VERSION} EXACT CONFIG
    PATHS "${CMAKE_CURRENT_LIST_DIR}/../RavenCore" NO_DEFAULT_PATH)
