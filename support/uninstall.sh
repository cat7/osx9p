#!/bin/sh
# Remove an osx9p installed from its package.
[ "$(id -u)" = 0 ] || { echo "run with sudo" >&2; exit 1; }
LD=/Library/LaunchDaemons/org.cat7.osx9p.automount.plist

mount -t 9p | sed -n 's/^.* on \(.*\) (.*$/\1/p' | while read -r d; do
	umount "$d" 2>/dev/null || diskutil unmount "$d"
	rmdir "$d" 2>/dev/null
done
launchctl unload "$LD" 2>/dev/null
rm -f "$LD" /sbin/mount_9p
rm -rf /Library/StartupItems/osx9p
rm -rf "/Library/Application Support/osx9p"
kextunload -b org.cat7.osx9p 2>/dev/null
rm -rf /System/Library/Extensions/osx9p.kext
touch /System/Library/Extensions
rm -rf /Library/Receipts/osx9p.pkg /Library/Receipts/boms/org.cat7.osx9p.pkg.bom
pkgutil --forget org.cat7.osx9p.pkg >/dev/null 2>&1
echo "osx9p removed"
