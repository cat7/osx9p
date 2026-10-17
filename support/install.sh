#!/bin/sh
# Install osx9p from a build tree: sudo support/install.sh
set -e
cd "$(dirname "$0")/.."
[ "$(id -u)" = 0 ] || { echo "run with sudo" >&2; exit 1; }

EXT=/System/Library/Extensions
SUP="/Library/Application Support/osx9p"
LD=/Library/LaunchDaemons/org.cat7.osx9p.automount.plist

kextunload -b org.cat7.osx9p >/dev/null 2>&1 || true
rm -rf "$EXT/osx9p.kext"
cp -R kext/osx9p.kext "$EXT/"
chown -R root:wheel "$EXT/osx9p.kext"
chmod -R go-w "$EXT/osx9p.kext"
touch "$EXT"

install -o root -g wheel -m 755 mount/mount_9p /sbin/mount_9p
mkdir -p "$SUP"
install -o root -g wheel -m 755 support/automount "$SUP/automount"
install -o root -g wheel -m 644 support/org.cat7.osx9p.automount.plist "$LD"

kextload "$EXT/osx9p.kext" >/dev/null 2>&1 || true
launchctl unload "$LD" >/dev/null 2>&1 || true
launchctl load "$LD"
echo "osx9p installed"
