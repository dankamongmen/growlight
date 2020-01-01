# growlight by nick black (nickblack@linux.com)

[![Build Status](https://drone.dsscaw.com:4443/api/badges/dankamongmen/growlight/status.svg)](https://drone.dsscaw.com:4443/dankamongmen/growlight)

Block device manager and system installation tool.

https://nick-black.com/dankwiki/index.php/Growlight

Dependencies:

 - notcurses 0.9.9+
 - libudev 175+
 - libblkid 2.20.1+
 - libpci 3.1.9+
 - libpciaccess 0.13.1+
 - libdevmapper 1.02.74+
 - libatasmart 0.19+
 - libcryptsetup 2.0.2+
 - OpenSSL 1.0.1+
 - mkswap(8) from util-linux
 - badblocks(8), mkfs.ext4(8), mkfs.ext3(8), mkfs.ext2(8) from e2fsprogs

Kernel options:

 - CONFIG_DM_CRYPT (for device mapper encrypt aka LUKS)
 - CONFIG_MD_RAID* (for MDRAID)
 - CONFIG_MSDOS_PARTITION (for msdos partition tables)
 - CONFIG_EFI_PARTITION (for GPT partition tables)
 ... almost certainly more

Build-only dependencies:

 - xsltproc (tested with 20800/10126/815)
 - docbook-xsl (tested with 4.5)
 - autotools (tested with 1.11)
 - pkg-config (tested with 0.29)
 - cunit (tested with 2.1)
 - autoconf-archive (tested with 2018+)

Building from a git checkout:

 - autoreconf -sfi -Wall,error
 - continue from "Building from a tarball"

Building from a tarball:

 - ./configure && make check && sudo make install

## Using it

Terse help is available from the `growlight` and `growlight-readline` man
pages, or by pressing 'H' in fullscreeen mode, or type "help" in readline mode.

### User's guide

In almost all cases, growlight needs to be run as root. It will attempt to
start otherwise, but will generally be unable to discover or manipulate disks.
You'll definitely need at least `CAP_SYS_RAWIO` and `CAP_SYS_ADMIN`.

growlight's first action is to install inotify watches in several directories,
and then enumerate the current devices by walking same (`/sys/class/block`,
etc.). This way, it immediately learns of devices added or removed after
startup. growlight discovers block devices via these directories, and through
those block devices finds controllers. Controllers which do not have block
devices attaches will thus not generally be found (growlight will remain aware
of an adapter from which all devices are removed while it's running).

The highest level of structure in growlight is the controller ("controller" and
"adapter" are used interchangeably in growlight). A virtual controller is also
defined, to collect various virtual devices (especially aggregates). In the
fullscreen view, controllers are boxes labeled by their type, bus path, and
bandwidth. Below, we see a machine with one SATA SSD, a dmcrypt device mapper
block built atop that, and an unloaded SD card reader hanging off USB 3.0:

<pre>
╭──────[virtual [0]]────────────────────────────────────────────────────[-]─╮
│       dm-0┌─⇗⇨⇨⇨dm-0─────────────────────────────────────────────────────┐│
│✔        dm│ ∾∾∾∾∾∾∾∾∾∾ ext4 “grimeshome” (384.42G) at /home ∾∾∾∾∾∾∾∾∾∾∾∾ ││
│up  32.78Mi└┤Linux devmapper   n/a 391.64G  512B none  home             ?├┘│
╰───────────────────────────────────────────────────────────────────────────╯

╭──────[xhci_pci-0 (133Mbps demanded)]──────────────────────────────────[-]─╮
│        sdb  SD/MMC           1.00    0.00  512B none  n/a           PATA  │
╰─────[Southbridge device 0000:00.14.0]─────────────────────────────────────╯

╭──────[ahci-0 (6Gbps demanded)]────────────────────────────────────────[-]─╮
│        sda┌──────────────────────────────────────────────────────────────┐│
│✔solidstate│me122 ext4 at / 2333333333333 crypto_LUKS (391.65G) 333333333m││
│37° 32.78Mi└┤Samsung SSD 850  2B6Q 500.10G  512B gpt   5002d410be12f SAT3├┘│
╰─────[Southbridge device 0000:00.17.0]─────────────────────────────────────╯
</pre>

Navigate among the adapters using PgUp and PgDn. Bring up the details subscreen
with `v` to see full details about the adapter (along with other information):

<pre>
╭──────[ahci-0 (6Gbps demanded)]────────────────────────────────────────[-]─╮
│        sda┌──⇗⇨⇨⇨sda1────────────────────────────────────────────────────┐│
│✔solidstate│me122 ext4 at / 23333333333 crypto_LUKS (391.65G) 33333333333m││
│41°   104Ki└┤Samsung SSD 850  2B6Q 500.10G  512B gpt   5002d410be12f SAT3├┘│
╭─press 'v' to dismiss details────────────────────────────────────────────╮─╯
│Intel Corporation Sunrise Point-LP SATA Controller [AHCI mode]           │
│Firmware: N27ET36W (1.22 ) BIOS: LENOVO Load: 6Gbps                      │
│sda: Samsung SSD 8502B6Q (465.76GiB) S/N: S2RANX0H729580X WC+ WRV- RO-   │
│Sectors: 976773168 (512B logical / 512B physical) SAT3 (6Gbps)           │
│Partitioning: gpt I/O scheduler: [mq-deadline] none                      │
│    512MiB P₀₁ 2048→1050623 sda1 (unnamed) ef00 1MiB align               │
│ 510.98MiB vfat “GRIMESESP” at /boot/efi                                 │
╰─────────────────────────────────────────────────────────────────────────╯
</pre>

In the readline mode, adapters are listed via the `adapter` command (`-v` can
be provided to `adapter` for full details of attached devices and filesystems):

<pre>
[growlight](0)> adapter
[ahci-0] Southbridge device 0000:00.17.0
 Intel Corporation Sunrise Point-LP SATA Controller [AHCI mode]
Virtual devices
[xhci_pci-0] Southbridge device 0000:00.14.0
 Intel Corporation Sunrise Point-LP USB 3.0 xHCI Controller
[growlight](0)>
</pre>
