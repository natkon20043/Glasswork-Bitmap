#!/bin/sh
# Builds Glasswork Bitmap and installs it for the current user:
#   ~/.local/share/glasswork-bitmap/   the program and its extension host
#   ~/.local/bin/glasswork-bitmap      a command that starts it
#   ~/.local/share/applications/       a menu entry, with its icon
#
# On NixOS (or with --nix) it is built with Nix instead (default.nix), together with the language
# servers, compilers and debugger it uses for C, C++, Rust, Python, JavaScript and TypeScript. The
# build is linked from ~/.local/share/glasswork-bitmap/package, which also keeps Nix's garbage
# collector from removing it.
#
#   packaging/linux/install.sh            build and install
#   packaging/linux/install.sh --nix      build and install with Nix, outside NixOS too
#   packaging/linux/install.sh --remove   uninstall (settings in ~/.config/glasswork-bitmap stay)
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
data=${XDG_DATA_HOME:-$HOME/.local/share}
dest="$data/glasswork-bitmap"
bin="$HOME/.local/bin"

if [ "${1:-}" = "--remove" ]; then
    rm -rf "$dest" "$bin/glasswork-bitmap" "$data/applications/glasswork-bitmap.desktop" \
        "$data/icons/hicolor/256x256/apps/glasswork-bitmap.png"
    command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$data/applications" || true
    echo "Glasswork Bitmap was removed."
    exit 0
fi

use_nix=false
if [ -e /etc/NIXOS ] || [ "${1:-}" = "--nix" ]; then
    use_nix=true
fi

cd "$root"
if $use_nix; then
    # Build first, so a failed build leaves the installed copy alone.
    package=$(nix-build "$root/default.nix" --no-out-link)
    rm -rf "$dest"
    mkdir -p "$dest" "$bin" "$data/applications" "$data/icons/hicolor/256x256/apps"
    nix-store --add-root "$dest/package" --realise "$package" >/dev/null
    program="$dest/package/bin/glasswork-bitmap"
    ln -sf "$program" "$bin/glasswork-bitmap"
else
    # Under WSL, keep Windows folders out of CMake's search.
    case "$(uname -r)" in
        *microsoft*|*WSL*) PATH=/usr/local/bin:/usr/bin:/bin:/usr/local/sbin:/usr/sbin:/sbin ;;
    esac
    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DGLASSWORK_BUILD_TESTS=OFF
    cmake --build build

    mkdir -p "$dest" "$bin" "$data/applications" "$data/icons/hicolor/256x256/apps"
    program="$dest/glasswork-bitmap"
    install -m 755 build/glasswork-bitmap "$program"
    rm -rf "$dest/host"
    cp -R host "$dest/host"
    ln -sf "$program" "$bin/glasswork-bitmap"
fi
install -m 644 packaging/linux/glasswork-bitmap.png "$data/icons/hicolor/256x256/apps/glasswork-bitmap.png"
sed "s|@EXEC@|$program|" packaging/linux/glasswork-bitmap.desktop > "$data/applications/glasswork-bitmap.desktop"
command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$data/applications" || true

echo "Glasswork Bitmap is installed in $dest"
case ":$PATH:" in
    *":$bin:"*) echo "Run it with: glasswork-bitmap [folder]" ;;
    *) echo "Run it with: $bin/glasswork-bitmap [folder]  (add $bin to your PATH to drop the folder)" ;;
esac
