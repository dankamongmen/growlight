% growlight(8)
% nick black <nickblack@Linux.com>
% v1.2.10

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
