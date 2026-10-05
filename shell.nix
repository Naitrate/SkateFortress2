# Build environment for the skate sidecar (skate-engine) on NixOS.
# The Source SDK side builds in Valve's podman sniper container instead.
{ pkgs ? import <nixpkgs> { } }:
let
  runtimeLibs = with pkgs; [
    alsa-lib
    udev
    vulkan-loader
    wayland
    libxkbcommon
    libx11
    libxcursor
    libxi
    libxrandr
  ];
in
pkgs.mkShell {
  nativeBuildInputs = with pkgs; [
    pkg-config
    clang
    llvmPackages.libclang
    # Skate 3 asset conversion (skate-engine/tools/prepare_assets.py).
    (python3.withPackages (p: [ p.numpy p.pillow ]))
  ];
  buildInputs = runtimeLibs ++ [ pkgs.wayland.dev pkgs.systemd.dev ];
  LIBCLANG_PATH = "${pkgs.llvmPackages.libclang.lib}/lib";
  LD_LIBRARY_PATH = pkgs.lib.makeLibraryPath runtimeLibs;
}
