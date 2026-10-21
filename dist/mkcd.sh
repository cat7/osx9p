#!/bin/sh
# Guest additions CD: an Installer package for 10.3 to 10.5 on a hybrid
# CD image. Run after make, as root: sudo dist/mkcd.sh [out.iso]
set -e
cd "$(dirname "$0")/.."
[ "$(id -u)" = 0 ] || { echo "run with sudo" >&2; exit 1; }
[ -d kext/osx9p.kext ] && [ -d panther/osx9p.kext ] && [ -x mount/mount_9p ] ||
    { echo "run make first" >&2; exit 1; }

VER=$(sed -n '/CFBundleShortVersionString/{n;s/.*<string>\(.*\)<\/string>.*/\1/p;}' kext/Info.plist)
MAJ=${VER%%.*}
MIN=$(echo "$VER" | cut -d. -f2)
OUT=${1:-osx9p-$VER.iso}
W=$(mktemp -d /tmp/osx9p-cd.XXXXXX)
trap 'rm -rf "$W"' EXIT

# installed files, owned and moded as on a stock system
R=$W/root
SUP="$R/Library/Application Support/osx9p"
mkdir -p "$R/System/Library/Extensions" "$R/sbin" "$SUP" "$R/Library/LaunchDaemons"
cp -R kext/osx9p.kext "$R/System/Library/Extensions/"
cp mount/mount_9p "$R/sbin/"
cp support/automount support/uninstall.sh "$SUP/"
cp support/org.cat7.osx9p.automount.plist "$R/Library/LaunchDaemons/"
# for 10.3, which postflight puts in place there
mkdir -p "$SUP/Panther" "$SUP/StartupItem"
cp -R panther/osx9p.kext "$SUP/Panther/"
cp support/StartupItem/osx9p support/StartupItem/StartupParameters.plist "$SUP/StartupItem/"
find "$R" -type d -exec chmod 755 {} \;
find "$R" -type f -exec chmod 644 {} \;
chmod 755 "$R/sbin/mount_9p" "$SUP/automount" "$SUP/uninstall.sh" "$SUP/StartupItem/osx9p"
chown -R root:wheel "$R"
chown root:admin "$R" "$R/Library" "$R/Library/Application Support"
chmod 1775 "$R" "$R/Library"
chmod 775 "$R/Library/Application Support"

CD=$W/cd
P=$CD/osx9p.pkg/Contents
mkdir -p "$P/Resources/English.lproj"
(cd "$R" && pax -w -x cpio .) | gzip -9 > "$P/Archive.pax.gz"
mkbom "$R" "$P/Archive.bom"
KB=$(du -sk "$R" | cut -f1)
sed -e "s/@VERSION@/$VER/g" -e "s/@MAJOR@/$MAJ/g" -e "s/@MINOR@/$MIN/g" \
    -e "s/@SIZE@/$KB/g" dist/Info.plist > "$P/Info.plist"
printf 'pmkrpkg1' > "$P/PkgInfo"
printf 'major: %s\nminor: %s\n' "$MAJ" "$MIN" > "$P/Resources/package_version"
sed "s/@VERSION@/$VER/g" dist/Description.plist > "$P/Resources/English.lproj/Description.plist"
cp dist/ReadMe.txt "$P/Resources/English.lproj/ReadMe.txt"
cp LICENSE "$P/Resources/English.lproj/License.txt"
cp dist/preflight dist/postflight "$P/Resources/"
chmod 755 "$P/Resources/preflight" "$P/Resources/postflight"

cp dist/ReadMe.txt "$CD/ReadMe.txt"
cp dist/Uninstall.command "$CD/Uninstall osx9p.command"
chmod 755 "$CD/Uninstall osx9p.command"

rm -f "$OUT"
hdiutil makehybrid -quiet -hfs -iso -joliet -hfs-volume-name "osx9p $VER" \
    -iso-volume-name OSX9P -joliet-volume-name OSX9P -o "$OUT" "$CD"
[ -n "$SUDO_USER" ] && chown "$SUDO_USER" "$OUT"
echo "$OUT"
