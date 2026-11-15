# Glasswork Bitmap for Nix and NixOS, with the language servers, compilers and debugger it uses for
# C, C++, Rust, Python, JavaScript and TypeScript (debugpy too, for debugging Python).
#
#   nix-build                          (from the repository folder; the program is in result/bin)
#   packaging/linux/install.sh         (on NixOS: builds this and installs it for your user)
#
# The tools go at the END of the program's PATH, so anything you install yourself (rustup, a newer
# clangd, a project's nix-shell...) is found first. The terminal inside the editor gets the same PATH.
{
  lib,
  stdenv,
  runCommand,
  cmake,
  ninja,
  pkg-config,
  makeWrapper,
  sdl3,
  curl,
  nlohmann_json,
  oniguruma,
  # Code intelligence, running and debugging
  clang-tools,
  gcc,
  gdb,
  python3,
  pyright,
  nodejs,
  typescript,
  typescript-language-server,
  tsx,
  rustc,
  cargo,
  rustfmt,
  clippy,
  rust-analyzer,
  rustPlatform,
  withLanguageTools ? true,
}:

let
  # CMake normally downloads these two; Nix builds have no network, so it is handed the very same
  # versions from nixpkgs instead (JSON 3.12.0, Oniguruma 6.9.10).
  # Python with debugpy, so F5 can stop at breakpoints in Python files.
  pythonWithDebugger = python3.withPackages (packages: [ packages.debugpy ]);

  onigurumaSource = runCommand "oniguruma-source-${oniguruma.version}" { } ''
    mkdir -p $out
    tar -xf ${oniguruma.src} --strip-components=1 -C $out
  '';

  languageTools = [
    # C and C++: clangd, the compilers (Run File, F5) and GDB (F5 debugs C, C++ and Rust)
    clang-tools
    gcc
    gdb
    # Python
    pythonWithDebugger
    pyright
    # JavaScript and TypeScript (node also runs VS Code extensions)
    nodejs
    typescript
    typescript-language-server
    tsx
    # Rust
    rustc
    cargo
    rustfmt
    clippy
    rust-analyzer
  ];

  # Last on the PATH, so your own tools come first; RUST_SRC_PATH (the standard library's source, for
  # rust-analyzer) only when you haven't set one; and the Python that has debugpy, which the editor
  # runs Python's debugger with even when your own python3 comes first on the PATH.
  wrapperArguments = lib.optionals withLanguageTools [
    "--suffix"
    "PATH"
    ":"
    (lib.makeBinPath languageTools)
    "--set-default"
    "RUST_SRC_PATH"
    "${rustPlatform.rustLibSrc}"
    "--set-default"
    "GLASSWORK_DEBUGPY_PYTHON"
    "${pythonWithDebugger}/bin/python3"
  ];
in
stdenv.mkDerivation {
  pname = "glasswork-bitmap";
  version = "1.0.0";

  src = lib.fileset.toSource {
    root = ../..;
    fileset = lib.fileset.unions [
      ../../CMakeLists.txt
      ../../src
      ../../host
      ../../third_party
      ../../packaging/linux
    ];
  };

  nativeBuildInputs = [
    cmake
    ninja
    pkg-config
    makeWrapper
  ];
  buildInputs = [
    sdl3
    curl
  ];

  cmakeFlags = [
    (lib.cmakeBool "GLASSWORK_BUILD_TESTS" false)
    (lib.cmakeBool "FETCHCONTENT_FULLY_DISCONNECTED" true)
    (lib.cmakeFeature "FETCHCONTENT_SOURCE_DIR_JSON" "${nlohmann_json.src}")
    (lib.cmakeFeature "FETCHCONTENT_SOURCE_DIR_ONIGURUMA" "${onigurumaSource}")
  ];

  # The program finds its extension host (host/) beside itself, so both live in share/ and bin/ has
  # a wrapper that adds the language tools to the PATH.
  installPhase = ''
    runHook preInstall
    mkdir -p $out/share/glasswork-bitmap $out/bin $out/share/applications $out/share/icons/hicolor/256x256/apps
    install -m 755 glasswork-bitmap $out/share/glasswork-bitmap/glasswork-bitmap
    cp -R host $out/share/glasswork-bitmap/host
    makeWrapper $out/share/glasswork-bitmap/glasswork-bitmap $out/bin/glasswork-bitmap ${lib.escapeShellArgs wrapperArguments}
    install -m 644 $src/packaging/linux/glasswork-bitmap.png $out/share/icons/hicolor/256x256/apps/glasswork-bitmap.png
    sed "s|@EXEC@|$out/bin/glasswork-bitmap|" $src/packaging/linux/glasswork-bitmap.desktop \
      > $out/share/applications/glasswork-bitmap.desktop
    runHook postInstall
  '';

  meta = {
    description = "A code editor in the style of the DOS IDEs";
    homepage = "https://github.com/natkon20043/Glasswork-Bitmap";
    license = lib.licenses.mit;
    mainProgram = "glasswork-bitmap";
    platforms = lib.platforms.linux;
  };
}
