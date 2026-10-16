# Cross-builds the tests for Windows. Windows CI runs them from the manifest
# installed beside them.
{ pkgs, common, src }:

let
  manifest = builtins.toFile "container-tests.json" (builtins.toJSON {
    suites = [{
      name = "subprocess_container";
      exe = "bin/logos_container_subprocess_tests.exe";
      timeout = 120;
    }];
  });
in
pkgs.stdenv.mkDerivation {
  pname = "${common.pname}-tests";
  version = common.version;

  inherit src;
  inherit (common) nativeBuildInputs buildInputs cmakeFlags meta env;

  ninjaFlags = [ "logos_container_subprocess_tests" "logos_container_test_child" ];

  installPhase = ''
    runHook preInstall
    mkdir -p $out/bin $out/share/logos-tests
    # Their DLLs are linked in beside them by the mingw fixup hook.
    cp bin/logos_container_subprocess_tests.exe bin/logos_container_test_child.exe $out/bin/
    cp ${manifest} $out/share/logos-tests/container.json
    runHook postInstall
  '';
}
