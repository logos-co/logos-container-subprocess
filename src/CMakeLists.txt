# The subprocess container: a ModuleContainer implementation that isolates each
# module in its own child process via Boost.Process v2. It launches and
# supervises the child, relays its stdout/stderr line-by-line (bounded so a
# module can't OOM the host), detects crashes, and delivers the module's auth
# token over the child's stdin (a private inherited pipe — no predictable
# filesystem path, so no socket-squat attack surface).
#
# Qt-free: the child side is just "read your token from fd 0", handled by the
# host binary in logos-liblogos, so this package no longer ships any child-side
# code. A different container (Docker, in-process) would be a sibling repo
# implementing the same logos-container contract.

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
# The subprocess container (Qt-free)
# ---------------------------------------------------------------------------
add_library(logos_container_subprocess STATIC
    subprocess_container.cpp
    subprocess_container.h
    # Provides LogosCore::makeContainer() (logos-container factory seam) so
    # consumers select this container at link time without naming it.
    subprocess_container_factory.cpp
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
