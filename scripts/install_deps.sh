#!/bin/bash
# TieredVol — Install dependencies, build, and optionally install
#
# Usage:
#   ./scripts/install_deps.sh          # Install deps + build
#   ./scripts/install_deps.sh --install # Install deps + build + sudo make install
#   ./scripts/install_deps.sh --test   # Install deps + build + run tests

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
DO_INSTALL=0
DO_TEST=0

for arg in "$@"; do
    case "$arg" in
        --install) DO_INSTALL=1 ;;
        --test)    DO_TEST=1 ;;
        --help|-h)
            echo "Usage: $0 [--install] [--test]"
            echo "  (no args)  Install dependencies and build"
            echo "  --install  Also run sudo make install"
            echo "  --test     Also run make test"
            exit 0
            ;;
    esac
done

# Detect distro
if [[ -f /etc/os-release ]]; then
    . /etc/os-release
    DISTRO="${ID:-unknown}"
else
    DISTRO="unknown"
fi

echo "=========================================="
echo "  TieredVol Dependency Installer"
echo "=========================================="
echo "  Detected: ${PRETTY_NAME:-$DISTRO}"
echo ""

install_debian() {
    echo "Installing packages for Debian/Ubuntu..."
    sudo apt update -qq
    sudo apt install -y \
        gcc make \
        linux-headers-$(uname -r) \
        lvm2 nvme-cli fio \
        bc python3
}

install_fedora() {
    echo "Installing packages for Fedora/RHEL..."
    sudo dnf install -y \
        gcc make \
        kernel-devel \
        lvm2 nvme-cli fio \
        bc python3
}

install_arch() {
    echo "Installing packages for Arch..."
    sudo pacman -S --needed --noconfirm \
        gcc make \
        linux-headers \
        lvm2 nvme-cli fio \
        bc python
}

case "$DISTRO" in
    ubuntu|debian|linuxmint|pop) install_debian ;;
    fedora|rhel|centos|rocky|alma) install_fedora ;;
    arch|manjaro|endeavouros) install_arch ;;
    *)
        echo "WARNING: Unknown distro '$DISTRO'. Trying Debian/Ubuntu..."
        install_debian
        ;;
esac

echo ""
echo "Building TieredVol (kernel module)..."
cd "$PROJECT_DIR"
make clean
make -j"$(nproc)" 2>&1 | sed 's/^/  /'

echo ""
echo "=========================================="
echo "  Build successful!"
echo "=========================================="
echo ""
echo "  Artifact:"
ls -la driver/tieredvol.ko 2>/dev/null | awk '{print "    " $NF " (" $5 " bytes)"}'
echo ""

if [[ $DO_INSTALL -eq 1 ]]; then
    echo "Installing kernel module..."
    sudo make install
    echo ""
    echo "Installed! Load with:"
    echo "  sudo modprobe tieredvol"
fi

if [[ $DO_TEST -eq 1 ]]; then
    echo "Running tests..."
    make test 2>&1 | sed 's/^/  /'
fi

echo ""
echo "Quick start:"
echo "  # 1. 準備 config（格式見 docs/CONFIG.md）：/etc/tieredvol/<name>.conf"
echo "  S=\$(python3 -c \"import configparser;c=configparser.ConfigParser();c.read('/etc/tieredvol/fastpool.conf');print(int(c['weighted_striping']['seg0_end'])//512)\")"
echo "  echo \"0 \$S tieredvol /etc/tieredvol/fastpool.conf\" | sudo dmsetup create fastpool"
echo "  sudo fio --filename=/dev/mapper/fastpool --rw=write --bs=1M --size=2G --direct=1 --ioengine=libaio --iodepth=32"
