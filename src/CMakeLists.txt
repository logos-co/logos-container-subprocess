# The subprocess container: a ModuleContainer implementation that isolates each
# module in its own child process. Two compiled units, split by their dependency
# surface:
#
#   logos_container_subprocess       parent side — Boost.Process lifecycle +
#                                    the auth-token sender. Qt-free.
#   logos_container_subprocess_host  child side — receives the auth token over a
#                                    QLocalServer Unix socket. Links Qt::Network.
#
# Both depend only on the logos-container contract (the ModuleContainer
# interface, the ModuleDescriptor value types, and the shared module-name
# allowlist). A different container (Docker, in-process) would be a sibling
# repo implementing the same contract.

# ---------------------------------------------------------------------------
# logos-container contract (header-only, imported as an INTERFACE target)
# ---------------------------------------------------------------------------
if(NOT DEFINED LOGOS_CONTAINER_ROOT OR NOT EXISTS "${LOGOS_CONTAINER_ROOT}/include")
  message(FATAL_ERROR "LOGOS_CONTAINER_ROOT not set or ${LOGOS_CONTAINER_ROOT}/include missing")
endif()

add_library(logos_container INTERFACE IMPORTED)
set_target_properties(logos_container PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES "${LOGOS_CONTAINER_ROOT}/include"
  INTERFACE_LINK_LIBRARIES "nlohmann_json::nlohmann_json"
)

# ---------------------------------------------------------------------------
# Parent side — the ModuleContainer (Qt-free)
# ---------------------------------------------------------------------------
add_library(logos_container_subprocess STATIC
    subprocess_container.cpp
    subprocess_container.h
)

target_link_libraries(logos_container_subprocess PUBLIC
    logos_container
    Boost::process
    spdlog::spdlog
)

target_include_directories(logos_container_subprocess PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include/logos_container_subprocess>
)

# ---------------------------------------------------------------------------
# Child side — the token receiver used by a module host (Qt::Network)
# ---------------------------------------------------------------------------
add_library(logos_container_subprocess_host STATIC
    token_receiver.cpp
    token_receiver.h
)

target_link_libraries(logos_container_subprocess_host PUBLIC
    logos_container
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Network
    spdlog::spdlog
)

target_include_directories(logos_container_subprocess_host PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include/logos_container_subprocess>
)
