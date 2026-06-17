# Installs the logos-container-subprocess headers
{ pkgs, common, src }:

pkgs.stdenv.mkDerivation {
  pname = "${common.pname}-headers";
  version = common.version;

  inherit src;
  inherit (common) meta;

  dontBuild = true;
  dontConfigure = true;

  installPhase = ''
    runHook preInstall

    # Install headers in logos_container_subprocess subdirectory
    mkdir -p $out/include/logos_container_subprocess
    cp src/*.h $out/include/logos_container_subprocess/

    runHook postInstall
  '';
}
