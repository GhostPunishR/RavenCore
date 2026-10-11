# Composants matériels CGB déjà extraits, portés par leur propre cible.
#
# Deux dépendances de sens opposé se croisent ici, et elles sont toutes les
# deux légitimes : `gb_raven_core` porte encore l'implémentation unifiée
# DMG/CGB et consomme donc les composants extraits, tandis que
# `gbc_raven_core` est la façade publique du cœur couleur et s'appuie sur
# cette implémentation. Les faire porter par une seule cible imposerait le
# cycle `gb_raven_core -> gbc_raven_core -> gb_raven_core`.
#
# La cible est INTERFACE tant que les composants extraits ne sont que des
# en-têtes. Quand l'un d'eux gagnera un `.cpp` — le PPU, les DMA et le port
# série sont les prochains sur la liste d'extraction — il suffira de la passer
# en STATIC avec ses sources : aucune autre ligne ne bouge et, surtout, aucun
# cycle n'apparaît. C'est précisément ce que l'inclusion par chemin relatif
# depuis `cores/gb` rendait impossible.
add_library(gbc_hardware INTERFACE)
target_include_directories(gbc_hardware INTERFACE include)
target_link_libraries(gbc_hardware INTERFACE ravenemu_common)

add_library(gbc_raven_core STATIC src/core.cpp)
target_include_directories(gbc_raven_core PUBLIC include)
target_link_libraries(gbc_raven_core PUBLIC gb_raven_core gbc_hardware)
set_target_properties(gbc_raven_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(RAVENEMU_BUILD_TESTS)
    add_executable(gbc_raven_core_tests tests/gbc_hardware_test.cpp)
    target_include_directories(gbc_raven_core_tests PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/../gb/src
        ${CMAKE_CURRENT_LIST_DIR}/../common/tests/support
    )
    target_link_libraries(gbc_raven_core_tests PRIVATE gbc_raven_core)
    add_test(NAME gbc_raven_core COMMAND gbc_raven_core_tests)
endif()
