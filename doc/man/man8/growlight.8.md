% growlight(8)
% nick black <nickblack@linux.com>
% v1.2.36

# NAME

growlight - Block device and filesystem editor

# SYNOPSIS

**growlight** [**-h|--help**] [**-i|--import**] [**-v|--verbose**]
 [**-V|--version**] [**--disphelp**] [**-t path|--target=path**]

# DESCRIPTION

**growlight** detects and describes disk pools, block devices, partition
tables, and partitions. It can partition devices, manipulate ZFS, MD, DM, LVM
and hardware RAID virtual devices, and prepare an fstab file for using the
devices to boot or in a chroot, and is fully aware of variable sector sizes,
GPT, and UEFI. **growlight** facilitates use of UUID/WWN- and HBA-based
identification of block devices.

This page describes the fullscreen **notcurses(3notcurses)** implementation.
Consult **growlight-readline(8)** for a line-oriented **readline(3)**
variant.

# OPTIONS

**-h|--help**: Print a brief usage summary, and exit.

**-i|--import**: Attempt to assemble aggregates (zpools, MD devices, etc)
based on block device scans at startup.

**-v|--verbose**: Be more verbose.

**-V|--version**: Print version information and exit.

**--disphelp**: Display the help subdisplay upon startup.

**--notroot**: Force **growlight** to start without necessary privileges (it
will usually refuse to start).

**-t path|--target=path**: Run in system installation mode, using **path**
as the temporary mountpoint for the target's root filesystem. "map" commands
will populate the hierarchy rooted at this mountpoint. System installation mode
can also be entered at run time with the "target" command. The "map" command
will not result in active mounts unless **growlight** is operating in system
installation mode (they will merely be used to construct target fstab output).
Once system installation mode is entered, **growlight** will return 0 only as a
result of a successful invocation of the "target finalize" command. **path**
must exist at the time of its specification.

# USAGE

Press 'H' or 'F1' to toggle a help display. This display lists all available
commands; commands which don't make sense at the moment are grayed out. Most
of these commands are also available from the menu at the top of the screen.
Press 'Alt' plus the underlined shortcut key to open a section of the menu,
or click on it with a mouse.

Press 'q' to exit the program.

The display is hierarchal, with block devices being collected under their
respective storage adapters. In addition to various physical adapters, a
"virtual" adapter is provided for e.g. aggregated devices. Move among the
adapters with Page Up and Page Down. Move among the block devices of an adapter
with up and down; move among the partitions of a block device with left and
right. Vi keys ('h'/'j'/'k'/'l') are also supported. Search with '/'; this
search will be applied to all metadata.

The 'G'rowlight menu allows toggling various subscreens, including the help,
recent diagnostics, mount points, and a details view. Only one subscreen can
be up at a time.

The 'B'lockdevs menu allows you to 'm'ake a partition table (only if the
selected block device doesn't already have one), 'r'emove a partition table
(assuming one is present), 'W'ipe a Master Boot Record (overwriting it with
zeroes), perform a 'B'ad block check, cre'A'te a new aggregate block device
(e.g. an mdadm array or ZFS zpool), modify an existing aggregate with 'z',
unbind an aggregate with 'Z', or set u'p' a loop device.

The 'P'artitions menu allows you to make a 'n'ew partition (in empty,
unallocated space, on a block device with an existing partition table),
'd'elete a partition, 's'et partition attributes, 'M'ake a filesystem, 'F'sck a
filesystem, 'w'ipe a filesystem, name a filesystem 'L'abel or name, set a
filesystem's 'U'uid, m'o'unt a filesystem, or unm'O'unt a filesystem. Most of
these latter commands can also be applied to swap devices.

When running in system installation mode (see **--target** above), the
following commands are also supported:

* moun't' target
* unmoun'T' target
* finalize UEFI with '*'
* finalize BIOS with '#'
* finalize fstab with '@'

# BUGS

Pedantic collections of ambiguous identifiers (for instance, if a label
equals another device's /dev/ name) will lead to questionable results. This
ought be fixed.

# SEE ALSO

**mount (2)**,
**swapoff (2)**,
**swapon (2)**,
**umount (2)**,
**libblkid (3)**,
**notcurses (3)**,
**fstab (5)**,
**proc (5)**,
**inotify (7)**,
**udev (7)**,
**blkid(8)**,,
**dmraid(8)**,
**dmsetup (8)**,
**growlight-readline(8)**,
**grub-install (8)**,
**grub-mkdevicemap (8)**,
**hdparm (8)**,
**losetup (8)**,
**lsblk (8)**,
**mdadm (8)**,
**mkfs (8)**,
**mount (8)**,
**parted (8)**,
**sfdisk (8)**,
**umount (8)**,
**zfs (8)**,
**zpool (8)**
