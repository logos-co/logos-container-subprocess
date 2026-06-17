# Builds tests
{ pkgs, common, build, logosContainer }:

pkgs.stdenv.mkDerivation {
  pname = "${common.pname}-tests";
  version = common.version;

  inherit (build) src;
  inherit (common) buildInputs meta env;

  nativeBuildInputs = common.nativeBuildInputs
    ++ pkgs.lib.optionals pkgs.stdenv.isDarwin [ pkgs.darwin.cctools ]
    ++ pkgs.lib.optionals pkgs.stdenv.isLinux [ pkgs.patchelf ];

  cmakeFlags = common.cmakeFlags;

  configurePhase = ''
    runHook preConfigure

    cp -r ${build}/* .
    chmod -R u+w .

    cmake -B build -S ${build.src} \
      -GNinja \
      -DLOGOS_CONTAINER_ROOT=${logosContainer} \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=$out

    runHook postConfigure
  '';

  buildPhase = ''
    runHook preBuild

    cd build
    ninja logos_container_subprocess_tests

    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall

    mkdir -p $out/bin
    cp bin/logos_container_subprocess_tests $out/bin/

    mkdir -p $out/lib
    cp -r lib/* $out/lib/ || true

    ${pkgs.lib.optionalString pkgs.stdenv.isLinux ''
      patchelf --set-rpath "$out/lib:${pkgs.boost}/lib:${pkgs.gtest}/lib:${pkgs.spdlog}/lib:${pkgs.fmt}/lib:${pkgs.stdenv.cc.cc.lib}/lib" $out/bin/logos_container_subprocess_tests || true
    ''}

    runHook postInstall
  '';
}
