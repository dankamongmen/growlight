% growlight-readline(8)
% nick black <nickblack@linux.com>
% v1.2.33

# NAME

growlight-readline - Block device and filesystem editor

# SYNOPSIS

**growlight-readline** [**-h|--help**] [**-i|--import**] [**-v|--verbose**]
 [**-V|--version**] [**-t path|--target=path**]

# DESCRIPTION

**growlight-readline** detects and describes disk pools, block devices,
partition tables, and partitions. It can partition devices, manipulate ZFS, MD,
DM, LVM and hardware RAID virtual devices, and prepare an fstab file for using
the devices to boot or in a chroot, and is fully aware of variable sector
sizes, GPT, and UEFI. **growlight-readline** facilitates use of UUID/WWN- and
HBA-based identification of block devices.

This page describes the line-based implementation. Consult **growlight(8)** for
a fullscreen **notcurses(3)**-based variant.

# OPTIONS

**-h|--help**: Print a brief usage summary, and exit.

**-i|--import**: Attempt to assemble aggregates (zpools, MD devices, etc)
based on block device scans at startup.

**-v|--verbose**: Be more verbose.

**-V|--version**: Print version information and exit.

**--notroot**: Force **growlight-readline** to start without necessary
privileges (it will usually refuse to start).

**-t path|--target=path**: Run in system installation mode, using **path**
as the temporary mountpoint for the target's root filesystem. "map" commands
will populate the hierarchy rooted at this mountpoint. System installation mode
can also be entered at run time with the "target" command. The "map" command
will not result in active mounts unless **growlight-readline** is operating in
system installation mode (they will merely be used to construct target fstab
output). Once system installation mode is entered, **growlight-readline** will
return 0 only as a result of a successful invocation of the "target finalize"
command. **path** must exist at the time of its specification.

# USAGE

**growlight-readline** implements an interactive command line
driven via GNU Readline. Commands are entered; a newline causes input to be interpreted, and
output resulting from this interpretation is displayed to the user. Unlike
programs such as **fdisk(8)** and **gdisk(8)**, **growlight-readline** acts on
commands immediately: it neither requires nor provides an explicit commit step
(though several commands do require confirmation).

An argument specified as **blockdev** can be either a
real block device, a virtual block device, or a partition. An argument
specified as **partition** must be a partition within some
kernel-recognized partition table. An argument specified as **fs**
must be a kernel-recognizable filesystem. An argument specified as **uuid**
must be 36 ASCII characters forming a valid DCE 1.1 UUID.
Devices can be specified via any appropriate identifier (label, UUID, etc.).

    **uefiboot [ -protectmbr ]**

Prepare the target for UEFI-based booting. A target root filesystem map
must be defined, and this filesystem must be mounted at the chroot's toplevel.
The filesystem must reside within a GPT partition having the EFI System Partition (ef00)
TUID of C12A7328-F81F-11D2-BA4B-00A0C93EC93B. This GPT
table must not reside on an mdadm, device-mapper, or zfs device. Preferably, no
other devices in the system are bootable, but this is not enforced.

If all these conditions are met, **GRUB2** will be installed into this filesystem,
configured to boot a Linux kernel residing at FIXME. This kernel must be
compiled with EFI stub loader support. The bootloader portion of the device's
MBR will be overwritten with zeroes, if possible, to deter attempting to
perform a BIOS/MBR boot from the device. Alternatively, the
**-protectmbr** option will install a
"protective MBR" consisting of one type 0xEE (EFI GPT) partition spanning the
disk. This might be preferable if interoperating with GPT-unaware tools.

    **biosboot**

Prepare the target for BIOS-based booting. A target root filesystem map
must be defined, and this filesystem must be mounted at the chroot's toplevel.
The filesystem must reside either within a GPT partition having the BIOS Boot
Partition (ef02) TUID of 21686148-6449-6E6F-744E-656564454649, or within a
MBR partition having type 0x83 (Linux). The partition table must not reside
on an mdadm, device-mapper, or zfs device. Preferably, no other devices in the
system are bootable, but this is not enforced.

If all these conditions are met, **GRUB2** will be installed into this filesystem
at /boot/grub, configured to boot a Linux kernel residing at FIXME. **GRUB2's**
first stage will be loaded into the bootloader portion of the device's MBR. The
partition containing **GRUB2** will be marked as **active**
(flag 0x80).

    **adapter reset adapter**
    **adapter rescan adapter**
    **adapter detail adapter**
    **adapter [ -v ]**

Passed no arguments, **adapter** lists each
HBA (Host Bus Adapter) device known to the system, including those not
actively used. Passed "-v", attached devices of each HBA will also be listed.
For each adapter listed, the first token is an identifier suitable for use with
other **adapter** subcommands (it is typically the
device driver's module name suffixed by a small integer, but the exact form
is intentionally left implementation-defined). The "reset" subcommand resets
the HBA, if it supports this functionality. The "rescan" subcommand causes the
kernel to scan the HBA for newly connected devices. Both operations are
performed via the Linux kernel's sysfs filesystem. "detail" will display
detailed information about the adapter.

    **blockdev rescan blockdev**
    **blockdev badblocks blockdev [ rw ]**
    **blockdev wipebiosboot blockdev**
    **blockdev ataerase blockdev**
    **blockdev rmtable blockdev**
    **blockdev mktable [ blockdev tabletype ]**
    **blockdev detail blockdev**
    **blockdev [ -v ]**

Passed no arguments, **blockdev** concisely lists
all block devices recognized by the system. Passed "-v", partitions and
filesystems present on a given block device will also be listed. The "rescan"
command causes the kernel to reanalyze the device's geometry and partition tables.
Any changes will be propagated to **growlight-readline**. "badblocks"
runs a non-destructive bad block check on the device. If provided "rw", "badblocks"
will perform a destructive, lenghtier, more strenuous read-write check. "wipebiosboot"
writes zeroes to the BIOS bootcode section of a disk (the first 446 bytes of
the first sector), hopefully ensuring that no attempt will be made to perform
a BIOS-type boot from the device. "ataerase" uses the ATA Secure Erase functionality
of the disk, if supported, to restore the device to factory settings. This can
lead to noticeably improved performance from used Solid State Devices (SSDs).
"rmtable" will attempt to write zeros over all partition table structures such
that **libblkid(3)** does not recognize the disk as being
partitioned. "mktable" will create a partition table of the provided type; with
no arguments, supported partition table types are listed. "detail" will display
detailed information about the block device.

    **partition del partition**
    **partition add blockdev size name type**
    **partition setuuid partition uuid**
    **partition setname partition name**
    **partition settype [ partition type ]**
    **partition setflag [ partition on|off flag ]**
    **partition [ -v ]**

Passed no arguments, **partition** concisely lists all
detected partitions. Passed "-v", data present on a given partition will also
be listed. "del" attempts to convert a partition to unallocated space. "add"
attempts to carve a partition of the specified size, type and name from
the specified block device's free space. "size" may be either a single number,
representing a size in bytes, or a range indicated by one or two numbers
separated by a colon. A range with no first number specifies "empty space up
until this sector." A range with no second number specifies "empty space following
this sector." A range with two numbers indicates "the specified range", and must
be wholly contained within free space. A size
of 0 indicates "all space available." When a size is used instead of a sector
range, the space is taken from the largest free space. "setuuid" attempts to set the partition
GUID (**not** the Type UUID) to uuid. "setname" attempts to
set the partition label to name. With no arguments, "settype" lists the types
supported by various partitioning schemes. Otherwise, it attempts to set the
specified type on the specified partition. With no arguments, "setflag" lists
the partition flags supported by this system. Otherwise, it attempts to set or
unset the specified flag on the specified partition.

    **fs mkfs [ blockdev fstype label ]**
    **fs wipefs blockdev**
    **fs fsck blockdev**
    **fs setuuid blockdev uuid**
    **fs setlabel blockdev label**
    **fs loop file mountpoint mnttype options**
    **fs mount blockdev mountpoint mnttype options**
    **fs umount blockdev**
    **fs**

Passed no arguments, **fs** concisely lists all
detected possible filesystems, irrespective of mount status or viability. The
"mkfs" subcommand will create a filesystem on the specified block device of the
specified type, having the specified label (possibly truncated). "mkfs" with no
arguments lists supported filesystem types. "wipefs" will attempt to destroy
the specified filesystem's superblocks, to the degree that
**libblkid(3)** does not detect them. "setuuid" will set the
UUID of the filesystem on blockdev, assuming that filesystem supports UUIDs.
"setlabel" will set the volume label of the filesystem on blockdev, assuming
that filesystem supports volume labels. "mount" will attempt to mount the block
device as a mnttype-type filesystem at mountpoint. This mount will not be added
to either the host or target's /etc/fstab, and will thus not persist across
reboots. "loop" will attempt to mount the file as a mnttype-type filesystem at
mountpoint using a loop device. "umount" will attempt to unmount the filesystem
underlain by blockdev. "fsck" will check the filesystem for correctness.

    **swap on swapdev label [ uuid ]**
    **swap off swapdev**
    **swap**

Passed no arguments, **swap** concisely lists
each swap device known to the system, including those not actively used. The
"on" subcommand attempts to create and use a swap device on **swapdev**. This
command will fail if **swapdev** is referenced by any map, mount, md device, dm
device, or zpool, if **swapdev** is not writable, or on any
**mkswap(8)** or **swapon(2)**
error. The "off" subcommand attempts to halt an active swap device (it does not
write to the swap device itself, and thus the swap device remains usable). It
will fail if the device is not an active swap, or on any **swapoff(2)**
failure.

    **mdadm arguments**
    **mdadm [ -v ]**

Passed no arguments, **mdadm** concisely lists
each MD (Multiple Device, aka the Linux kernel's mdraid driver) device
currently available to the system. Passed "-v", each MD device's components
will also be listed. Otherwise, arguments will be sanitized and passed to the
external **mdadm(8)** tool. Note that this
behavior is subject to change as MD is more fully integrated.

    **dmsetup arguments**

Arguments are sanitized and passed to the external
**dmsetup(8)** tool. Note that this behavior is
subject to change as DM is more fully integrated.

    **zpool arguments**
    **zpool [ -v ]**

Passed no arguments, **zpool** concisely lists
each zpool (ZFS storage pool) device currently available to the system. Passed
"-v", each zpool's components will also be listed. Otherwise, arguments will be
sanitized and passed to the external **zpool(8)**
tool. Note that this behavior is subject to change as ZFS is more fully
integrated.


    **zfs arguments**

Arguments are sanitized and passed to the external
**zfs(8)** tool. This command is not expected to
be retained once ZFS is fully integrated.

    **target set path**
    **target unset**
    **target finalize**
    **target**

The **set** subcommand sets the target (see the **--target**
option). If a target has already been set, the command will fail. If **path**
does not exist in the current filesystem, the command will fail. If **path**
exists, but is not a directory, the command will fail. Called with **unset**,
undefines the target and all mappings therein. If there is no target defined,
the command will fail. Called with **finalize**,
the target's /etc/fstab will be written out. Called with no arguments, lists
the current target (if one exists).

    **map [ mountdev mountpoint options ]**
    **map**

Provided no arguments, **map** dumps the
target's /etc/fstab as currently envisioned. Otherwise, attempt to mount
mountdev at mountpoint (relative to the target root) as a filesystem of appropriate
type, using the specified mount options (see **mount(8)**).
If the mount is successful, the mapping will be added to the target /etc/fstab
(though this will not be written out until **target finalize**
is run). The **map** command is unavailable unless in
system preparation mode (see **target**).

    **unmap mountpoint**

**unmap** removes the target map, and unmounts the associated mount. The
mapping will no longer be carried into the target's /etc/fstab. The mountpoint
is relative to the target root.

    **mounts**

Displays all currently-mounted filesystems. Accepts no arguments.

    **grubmap**

Invokes **grub-mkdevicemap** to display **GRUB2's** calculated device map. This
will not overwrite any existing device map, but is included merely for
diagnostic purposes. Accepts no arguments.

    **benchmark blockdev**

Run a simple, non-destructive benchmark on the block device. Currently,
this is implemented via **hdparm -t**.

    **troubleshoot**

Look for problems, both physical and logical, in the storage setup. This
command's behavior is not yet well-defined FIXME.
    
    **version**

Displays the **&dhpackage;** banner and version, and the version of various
tools/libraries. **version** accepts no arguments.

    **diags [ count ]**

Dump up through count diagnostic messages from the logging ringbuffer to stdout.
Provided no parameter, all available diagnostic messages will be printed. Timestamps
are printed along with each message.

    **help command**
    **help**

Invoked upon a **command**, **help** displays that command's synopsis.
Otherwise, **help** lists a synopsis of each command.

    **quit**

Exits the program. See the **--target** command line option and **target**
command to understand the conditions for a successful return during
installation mode.
**quit** accepts no arguments.

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
**growlight(8)**,
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
