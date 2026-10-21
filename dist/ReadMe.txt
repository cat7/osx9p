osx9p: shared folders for Mac OS X in QEMU

osx9p mounts the folders QEMU shares with virtio-9p as volumes in
Mac OS X 10.3, 10.4 and 10.5 on PowerPC. Finder info (type, creator,
labels) and resource forks are kept on the host, so Classic and Carbon
files survive the trip.

Install
  Open osx9p.pkg and follow the Installer. No restart is needed: each
  share appears in the Finder and under /Volumes, named after its QEMU
  mount tag, and is mounted again at every startup.

QEMU options
  -fsdev local,id=share0,path=/path/to/folder,security_model=none,macos_compat=on
  -device virtio-9p-pci,fsdev=share0,mount_tag=Shared

Mounting by hand
  mount_9p [-o volume=NAME,rdonly,nobrowse,owners,chatty] TAG DIR

Uninstall
  Open "Uninstall osx9p.command" on this CD, or run
  sudo "/Library/Application Support/osx9p/uninstall.sh"

Mac OS 9 guests use the classicvirtio driver instead; nothing on this
CD is needed for them.

Limits
  Advisory locks hold between programs in the guest only.
  On 10.3 the Finder cannot eject a share (use: umount /Volumes/NAME),
  and Move to Trash deletes the item at once.

Source and license (BSD): https://github.com/cat7/osx9p
