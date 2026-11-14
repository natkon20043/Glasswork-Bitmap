# nix-build builds Glasswork Bitmap with its language tools; see packaging/nix/glasswork-bitmap.nix.
{
  pkgs ? import <nixpkgs> { },
}:
pkgs.callPackage ./packaging/nix/glasswork-bitmap.nix { }
