#!/bin/sh
# Install osx9p from a build tree: sudo support/install.sh
set -e
cd "$(dirname "$0")/.."
[ "$(id -u)" = 0 ] || { echo "run with sudo" >&2; exit 1; }

EXT=/System/Library/Extensions
SUP="/Library/Application Support/osx9p"
LD=/Library/LaunchDaemons/org.cat7.osx9p.automount.plist
SI=/Library/StartupItems/osx9p

case $(uname -r) in
7.*)	KEXT=panther/osx9p.kext ;;	# Mac OS X 10.3
*)	KEXT=kext/osx9p.kext ;;
esac

kextunload -b org.cat7.osx9p >/dev/null 2>&1 || true
rm -rf "$EXT/osx9p.kext"
cp -R "$KEXT" "$EXT/"
chown -R root:wheel "$EXT/osx9p.kext"
chmod -R go-w "$EXT/osx9p.kext"
touch "$EXT"

install -o root -g wheel -m 755 mount/mount_9p /sbin/mount_9p
mkdir -p "$SUP"
install -o root -g wheel -m 755 support/automount "$SUP/automount"

kextload "$EXT/osx9p.kext" >/dev/null 2>&1 || true
case $(uname -r) in
7.*)
	rm -rf "$SI"
	mkdir -p "$SI"
	install -o root -g wheel -m 755 support/StartupItem/osx9p "$SI/osx9p"
	install -o root -g wheel -m 644 support/StartupItem/StartupParameters.plist "$SI/"
	"$SUP/automount"
	;;
*)
	install -o root -g wheel -m 644 support/org.cat7.osx9p.automount.plist "$LD"
	launchctl unload "$LD" >/dev/null 2>&1 || true
	launchctl load "$LD"
	;;
esac
echo "osx9p installed"
