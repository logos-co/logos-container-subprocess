# Common build configuration shared across all packages
{ pkgs, logosContainer }:

{
  pname = "logos-container-subprocess";
  version = "0.1.0";

  nativeBuildInputs = [
    pkgs.cmake
    pkgs.ninja
    pkgs.pkg-config
    pkgs.qt6.wrapQtAppsNoGuiHook
  ];

  # Qt6 is listed explicitly (not propagated by deps) — qtbase's setup hook
  # must be sourced after wrapQtAppsHook. Boost/spdlog/nlohmann back the
  # parent and child sides; logosContainer provides the contract + logging.
  buildInputs = [
    pkgs.qt6.qtbase
    pkgs.boost
    pkgs.nlohmann_json
    pkgs.spdlog
    pkgs.gtest
    logosContainer
  ];

  cmakeFlags = [
    "-GNinja"
    "-DLOGOS_CONTAINER_ROOT=${logosContainer}"
  ];

  env = {
    LOGOS_CONTAINER_ROOT = "${logosContainer}";
  };

  meta = with pkgs.lib; {
    description = "Subprocess container: process-isolated ModuleContainer implementation for the Logos module runtime";
    platforms = platforms.unix;
  };
}
