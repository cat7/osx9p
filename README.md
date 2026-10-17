# osx9p

Shared folders for PowerPC Mac OS X guests in QEMU: a kernel extension
that mounts a `virtio-9p-pci` share as a volume.

- Mac OS X 10.4 and 10.5 (PowerPC)
- 9P2000.L over the virtio 1.0 PCI interface
- Finder info and resource forks are kept as the host's
  `com.apple.FinderInfo` / `com.apple.ResourceFork` extended attributes

## QEMU

    -fsdev local,id=share0,path=/path/to/folder,security_model=none,macos_compat=on
    -device virtio-9p-pci,fsdev=share0,mount_tag=Shared

`macos_compat` needs a QEMU with Mac OS 9 guest support in 9p (it also
lets Mac OS 9 guests with classicvirtio use the same share).

## Building

On Mac OS X 10.4 with Xcode 2.5 (10.4u SDK):

    make

This produces `kext/osx9p.kext` and `mount/mount_9p`. The same build loads
on 10.5.

## Use

    sudo cp -R kext/osx9p.kext /tmp/ && sudo chown -R root:wheel /tmp/osx9p.kext
    sudo kextload /tmp/osx9p.kext
    mkdir /Volumes/Shared
    mount/mount_9p -o volume=Shared Shared /Volumes/Shared

`mount_9p [-o volume=NAME,rdonly,nobrowse,owners,chatty] TAG DIR`

## Limitations

- Advisory locks (`flock`, `fcntl`) hold between guest processes only;
  the host does not see them.
- File IDs (needed by Carbon on 10.4) are kept in memory: a file that has
  not been seen since the mount cannot be found by ID alone.

## License

BSD, see LICENSE. Based on Mac9P by Federico G. Benavento.
