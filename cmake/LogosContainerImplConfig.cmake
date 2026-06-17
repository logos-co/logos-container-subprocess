# CMake package config for "the container implementation installed in this prefix".
#
# Deliberately generic-named (LogosContainerImpl) so a consumer (logos-liblogos)
# selects *a* container implementation via find_package(LogosContainerImpl)
# without naming this specific one — switching containers is a matter of which
# package provides this config. It defines the imported target
# LogosContainerImpl::impl, carrying this implementation's static library, its
# public include dir, and its own transitive dependencies, so the consumer needs
# to know none of them.

include(CMakeFindDependencyMacro)
find_dependency(Boost COMPONENTS process)
find_dependency(spdlog)
find_dependency(nlohmann_json)

# This file installs at <prefix>/lib/cmake/LogosContainerImpl/ — three levels
# down from the package prefix.
get_filename_component(_logos_container_impl_prefix "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

if(NOT TARGET LogosContainerImpl::impl)
  add_library(LogosContainerImpl::impl STATIC IMPORTED)
  set_target_properties(LogosContainerImpl::impl PROPERTIES
    IMPORTED_LOCATION "${_logos_container_impl_prefix}/lib/liblogos_container_subprocess.a"
    INTERFACE_INCLUDE_DIRECTORIES "${_logos_container_impl_prefix}/include"
    INTERFACE_LINK_LIBRARIES "Boost::process;spdlog::spdlog;nlohmann_json::nlohmann_json"
  )
endif()

unset(_logos_container_impl_prefix)
