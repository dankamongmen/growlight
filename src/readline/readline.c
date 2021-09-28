// copyright 2012–2021 nick black
#include <wchar.h>
#include <errno.h>
#include <unistd.h>
#include <wctype.h>
#include <assert.h>
#include <stdlib.h>
#include <signal.h>
#include <locale.h>
#include <version.h>
#include <atasmart.h>
#include <notcurses/direct.h>

#include "fs.h"
#include "mbr.h"
#include "zfs.h"
#include "swap.h"
#include "stats.h"
#include "sysfs.h"
#include "popen.h"
#include "ptypes.h"
#include "mounts.h"
#include "target.h"
#include "secure.h"
#include "ptable.h"
#include "health.h"
#include "growlight.h"

#define U64STRLEN 20    // Does not include a '\0' (18,446,744,073,709,551,616)
#define U64FMT "%-20ju"
#define U32FMT "%-10ju"
#define UUIDSTRLEN 36

// Used by quit() to communicate back to the main readline loop
static unsigned lights_off;
struct ncdirect* ncd;

#define COLOR_WHITE  0xffffff
#define COLOR_YELLOW 0xf9f1a5
#define COLOR_RED    0xe74856
#define COLOR_GREEN  0x13a10e
#define COLOR_MAGENTA 0xa4009e
#define COLOR_BLUE   0x3b78ff
#define COLOR_CYAN   0x61d6d6

static int
use_terminfo_color(unsigned rgb, int boldp){
  if(ncd){
    if(!boldp){
      ncdirect_set_styles(ncd, 0);
    }else{
      ncdirect_set_styles(ncd, NCSTYLE_BOLD);
    }
    ncdirect_set_fg_rgb(ncd, rgb);
  }
  return 0;
}

static inline int
usage(wchar_t * const *args, const char *arghelp){
  fprintf(stderr, "Usage: %ls %s\n", *args, arghelp);
  return -1;
}

static int
wmmount(device *d, const wchar_t *targ){
  char path[PATH_MAX + 1];

  if(snprintf(path, sizeof(path), "%ls", targ) >= (int)sizeof(path)){
    fprintf(stderr, "Bad path: %ls\n", targ);
    return -1;
  }
  return mmount(d, path, 0, NULL);
}

static int
wstrtoxu(const wchar_t *wstr, unsigned *ux){
  unsigned long long ull;
  char buf[BUFSIZ], *e;

  if(snprintf(buf, sizeof(buf), "%ls", wstr) >= (int)sizeof(buf)){
    fprintf(stderr, "Bad numeric value: %ls\n", wstr);
    return -1;
  }
  if(buf[0] == '-'){
    fprintf(stderr, "Negative number: %ls\n", wstr);
    return -1;
  }
  errno = 0;
  ull = strtoull(buf, &e, 16);
  if((ull == ULLONG_MAX && errno == ERANGE) || ull > UINT_MAX){
    fprintf(stderr, "Number too large: %ls\n", wstr);
    return -1;
  }
  if(e == buf){
    fprintf(stderr, "Bad numeric value: %ls\n", wstr);
    return -1;
  }
  if(*e){
    fprintf(stderr, "Invalid number: %ls\n", wstr);
    return -1;
  }
  *ux = ull;
  return 0;
}

static int
wstrtoull(const wchar_t *wstr, uintmax_t *ull){
  char buf[BUFSIZ], *e;

  if(snprintf(buf, sizeof(buf), "%ls", wstr) >= (int)sizeof(buf)){
    fprintf(stderr, "Bad numeric value: %ls\n", wstr);
    return -1;
  }
  if(buf[0] == '-'){
    fprintf(stderr, "Negative number: %ls\n", wstr);
    return -1;
  }
  errno = 0;
  *ull = strtoull(buf, &e, 0);
  if(*ull == ULLONG_MAX && errno == ERANGE){
    fprintf(stderr, "Number too large: %ls\n", wstr);
    return -1;
  }
  if(e == buf){
    fprintf(stderr, "Bad numeric value: %ls\n", wstr);
    return -1;
  }
  if(*e){
    if(e[1]){
      fprintf(stderr, "Invalid number: %ls\n", wstr);
      return -1;
    }
    switch(*e){
      case 'E': case 'e':
        *ull *= 1000000000000000000; break;
      case 'P': case 'p':
        *ull *= 1000000000000000; break;
      case 'T': case 't':
        *ull *= 1000000000000; break;
      case 'G': case 'g':
        *ull *= 1000000000; break;
      case 'M': case 'm':
        *ull *= 1000000; break;
      case 'K': case 'k':
        *ull *= 1000; break;
      default:
      fprintf(stderr, "Invalid number: %ls\n", wstr);
      return -1;
    }
  }
  return 0;
}

static int
make_wfilesystem(device *d, const wchar_t *fs, const wchar_t *name){
  char sfs[NAME_MAX], label[NAME_MAX];

  if(snprintf(sfs, sizeof(sfs), "%ls", fs) >= (int)sizeof(sfs)){
    fprintf(stderr, "Bad partition table type: %ls\n", fs);
    return -1;
  }
  if(snprintf(label, sizeof(label), "%ls", name) >= (int)sizeof(label)){
    fprintf(stderr, "Bad label: %ls\n", name);
    return -1;
  }
  return make_filesystem(d, sfs, label);
}

static controller *
lookup_wcontroller(const wchar_t *dev){
  char sdev[NAME_MAX];

  if(snprintf(sdev, sizeof(sdev), "%ls", dev) >= (int)sizeof(sdev)){
    fprintf(stderr, "Bad controller name: %ls\n", dev);
    return NULL;
  }
  return lookup_controller(sdev);
}

static device *
lookup_wdevice(const wchar_t *dev){
  char sdev[NAME_MAX];

  if(snprintf(sdev, sizeof(sdev), "%ls", dev) >= (int)sizeof(sdev)){
    fprintf(stderr, "Bad device name: %ls\n", dev);
    return NULL;
  }
  return lookup_device(sdev);
}

static int
make_partition_wtable(device *d, const wchar_t *tbl){
  char stbl[NAME_MAX];

  if(snprintf(stbl, sizeof(stbl), "%ls", tbl) >= (int)sizeof(stbl)){
    fprintf(stderr, "Bad partition table type: %ls\n", tbl);
    return -1;
  }
  return make_partition_table(d, stbl);
}

static int
prepare_wumount(device *d, const wchar_t *path){
  char spath[PATH_MAX];

  if(snprintf(spath, sizeof(spath), "%ls", path) >= (int)sizeof(spath)){
    fprintf(stderr, "Bad path: %ls\n", path);
    return -1;
  }
  return unmount(d, spath);
}

static int
prepare_wmount(device *d, const wchar_t *path, const wchar_t *ops){
  char spath[PATH_MAX], sops[PATH_MAX];

  if(snprintf(spath, sizeof(spath), "%ls", path) >= (int)sizeof(spath)){
    fprintf(stderr, "Bad path: %ls\n", path);
    return -1;
  }
  if(snprintf(sops, sizeof(sops), "%ls", ops) >= (int)sizeof(sops)){
    fprintf(stderr, "Bad filesystem options: %ls\n", ops);
    return -1;
  }
  return mmount(d, spath, 0, sops);
}

#define ZERO_ARG_CHECK(args, arghelp) \
 if(args[1]){ usage(args, arghelp); return -1 ; }

#define ONE_ARG_CHECK(args, arghelp) \
 if(!args[1] || args[2]){ usage(args, arghelp); return -1 ; }

#define TWO_ARG_CHECK(args, arghelp) \
 if(!args[1] || !args[2] || args[3]){ usage(args, arghelp); return -1 ; }

static int help(wchar_t * const *, const char *);
static int print_dev_mplex(const device *, int, int);

static int
print_mounts(const device *d){
  char buf[PREFIXSTRLEN + 1];
  int r = 0, rr;
  unsigned z;

  for(z = 0 ; z < d->mnt.count ; ++z){
    const char *size = d->mntsize ? qprefix(d->mntsize, 1, buf, 0) : "";
    r += rr = printf("%-*.*s %-5.5s %-36.36s %-6.6s %*s\n %s %s\n",
        FSLABELSIZ, FSLABELSIZ, d->label ? d->label : "n/a",
        d->mnttype, d->uuid ? d->uuid : "n/a", d->name,
        PREFIXFMT(size), d->mnt.list[z], d->mntops.list[z]);
    if(rr < 0){
      return -1;
    }
  }
  return r;
}

static int
print_swap(const device *p){
  char buf[PREFIXSTRLEN + 1];
  int r = 0, rr;

  assert(p->mnttype);
  qprefix(p->mntsize, 1, buf, 0),
  r += rr = printf("%-*.*s %-5.5s %-36.36s %-6.6s %*s",
      FSLABELSIZ, FSLABELSIZ, p->label ? p->label : "n/a",
      p->mnttype, p->uuid ? p->uuid : "n/a",
      p->name, PREFIXFMT(buf));
  if(rr < 0){
    return -1;
  }
  if(p->swapprio >= SWAP_MAXPRIO){
    r += rr = printf(" pri=%d\n", p->swapprio);
  }else{
    r += rr = printf("\n");
  }
  if(rr < 0){
    return -1;
  }
  return r;
}

static int
print_fs(const device *p, int descend){
  int r = 0, rr;

  use_terminfo_color(COLOR_GREEN, 1);
  if(p->mnttype == NULL){
    return 0;
  }
  if(p->swapprio != SWAP_INVALID){
    if(!descend){
      return 0;
    }
    r += rr = print_swap(p);
    if(rr < 0){
      return -1;
    }
  }
  r += rr = print_mounts(p);
  if(rr < 0){
    return -1;
  }
  return r;
}

static int
print_empty(uint64_t fsect, uint64_t lsect, size_t sectsize){
  char buf[BPREFIXSTRLEN + 1];
  int r = 0, rr;

  //assert(fsect <= lsect);
  if(sectsize == 0){
    sectsize = 1;
  }
  use_terminfo_color(COLOR_GREEN, 0);
  r += rr = printf("Unused sectors %ju:%ju (%s)\n",
                   (uintmax_t)fsect, (uintmax_t)lsect,
                   bprefix((lsect - fsect + 1) * sectsize, 1, buf, 1));
  if(rr < 0){
    return -1;
  }
  return r;
}

static int
print_partition(const device *p, int descend){
  char buf[PREFIXSTRLEN + 1];
  int r = 0, rr;

  use_terminfo_color(COLOR_BLUE, 1);
  qprefix(p->size, 1, buf, 0);
  r += rr = printf("%-10.10s %-36.36s %*s %-3.3s %ls\n",
                   p->name, p->partdev.uuid ? p->partdev.uuid : "n/a",
                   PREFIXFMT(buf),
                   partrole_str(p->partdev.ptype, p->partdev.flags),
                   p->partdev.pname ? p->partdev.pname : L"n/a");
  if(rr < 0){
    return -1;
  }
  if(!descend){
    return r;
  }
  r += rr = print_fs(p, 0);
  if(rr < 0){
    return -1;
  }
  return r;
}

static int
print_drive_stats(const device *d) {
  printf("%-10.10s %16ju %16ju %16ju %16ju\n", d->name,
    d->stats.sectors_read,
    d->statdelta.sectors_read,
    d->stats.sectors_written,
    d->statdelta.sectors_written);
  return 0;
}

static int
print_drive_stats_identified(const device *d) {
  printf("SecRead    %16ju SecReadΔ    %16ju\n"
         "SecWritten %16ju SecWrittenΔ %16ju\n",
    d->stats.sectors_read,
    d->statdelta.sectors_read,
    d->stats.sectors_written,
    d->statdelta.sectors_written);
  return 0;
}

// Yellow - hard disk
// Cyan -- SSD
// Magena -- virtual
// White -- removable
// Blue - Partition
// Green - filesystem

static int
print_drive(const device *d, int descend){
  char buf[PREFIXSTRLEN + 1];
  uint64_t sector;
  const device *p;
  int r = 0, rr;

  switch(d->layout){
  case LAYOUT_NONE:{
    if(d->blkdev.removable){
      use_terminfo_color(COLOR_WHITE, 0); // optical/usb
    }else if(d->blkdev.realdev){
      if(d->blkdev.rotation >= 0){
        use_terminfo_color(COLOR_YELLOW, 0); // disk
      }else{
        use_terminfo_color(COLOR_CYAN, 1); // ssd
      }
    }else{
      use_terminfo_color(COLOR_MAGENTA, 1); // virtual
    }
    qprefix(d->size, 1, buf, 0);
    r += rr = printf("%-10.10s %-16.16s %4.4s %*s %4uB %ls%ls%ls%ls%ls %-6.6s%-16.16s %-4.4s\n",
      d->name,
      d->model ? d->model : "n/a",
      d->revision ? d->revision : "n/a",
      PREFIXFMT(buf),
      d->physsec,
      d->blkdev.unloaded ? L"U" :
       d->blkdev.removable ? L"R" :
       d->blkdev.smart == SK_SMART_OVERALL_GOOD ? L"✔" :
       (d->blkdev.smart == SK_SMART_OVERALL_BAD_STATUS ||
         d->blkdev.smart == SK_SMART_OVERALL_BAD_SECTOR_MANY) ? L"✗" :
       d->blkdev.smart > 0 ? L"☠" :
       d->blkdev.realdev ? L"." : L"V",
      d->blkdev.rotation >= 0 ? L"O" : L".",
      d->roflag ? L"r" :
       d->blkdev.wcache ? L"W" : L".",
      d->blkdev.rwverify == RWVERIFY_SUPPORTED_ON ? L"v" :
       d->blkdev.rwverify == RWVERIFY_SUPPORTED_OFF ? L"⚠" : L".",
      d->blkdev.biosboot ? L"B" : L".",
      d->blkdev.pttable ? d->blkdev.pttable : "none",
      d->blkdev.wwn ? d->blkdev.wwn : "n/a",
      d->blkdev.realdev ? transport_str(d->blkdev.transport) : "n/a"
      );
    break;
  }case LAYOUT_MDADM:{
    use_terminfo_color(COLOR_YELLOW, 1);
    qprefix(d->size, 1, buf, 0);
    r += rr = printf("%-10.10s %-16.16s %4.4s %*s %4uB %ls%ls%ls%ls%ls %-6.6s%-16.16s %-4.4s\n",
      d->name,
      d->model ? d->model : "n/a",
      d->revision ? d->revision : "n/a",
      PREFIXFMT(buf),
      d->physsec, L"V", L"M", L".",
      d->roflag ? L"r" : L".", L".",
      d->mddev.pttable ? d->mddev.pttable : "none",
      d->mddev.mdname ? d->mddev.mdname : "n/a",
      transport_str(d->mddev.transport)
      );
    break;
  }case LAYOUT_DM:{
    use_terminfo_color(COLOR_YELLOW, 1);
    qprefix(d->size, 1, buf, 0);
    r += rr = printf("%-10.10s %-16.16s %4.4s %*s %4uB %ls%ls%ls%ls%ls %-6.6s%-16.16s %-4.4s\n",
      d->name,
      d->model ? d->model : "n/a",
      d->revision ? d->revision : "n/a",
      PREFIXFMT(buf),
      d->physsec, L"V", L"D", L".",
      d->roflag ? L"r" : L".", L".",
      "n/a",
      d->dmdev.dmname ? d->dmdev.dmname : "n/a",
      transport_str(d->dmdev.transport)
      );
    break;
  }case LAYOUT_ZPOOL:{
    use_terminfo_color(COLOR_RED, 1);
    qprefix(d->size, 1, buf, 0);
    r += rr = printf("%-10.10s %-16.16s %4ju %*s %4uB %ls%ls%ls%ls%ls %-6.6s%-16.16s %-4.4s\n",
      d->name,
      d->model ? d->model : "n/a",
      (uintmax_t)d->zpool.zpoolver,
      PREFIXFMT(buf),
      d->physsec, L"V", L"Z", L".",
      d->roflag ? L"r" : L".", L".",
      "spa",
      "n/a", // FIXME
      transport_str(d->zpool.transport)
      );
    break;
  }case LAYOUT_PARTITION:{
    return -1;
  }default:
    return -1;
  }
  if(rr < 0){
    return -1;
  }
  if(!descend){
    return r;
  }
  r += rr = print_fs(d, descend);
  if(rr < 0){
    return -1;
  }
  if(rr > 0){
    return r;
  }
  sector = 0;
  for(p = d->parts ; p ; p = p->next){
    if(sector != p->partdev.fsector){
      r += rr = print_empty(sector, p->partdev.fsector - 1, d->logsec);
      if(rr < 0){
        return -1;
      }
    }
    r += rr = print_partition(p, descend);
    if(rr < 0){
      return -1;
    }
    sector = p->partdev.lsector + 1;
  }
  if(d->size && d->logsec){
    if(sector != d->size / d->logsec){
      r += rr = print_empty(sector, d->size / d->logsec, d->logsec);
      if(rr < 0){
        return -1;
      }
    }
  }
  return r;
}

static int
print_zpool(const device *d, int descend){
  char buf[PREFIXSTRLEN + 1];
  int r = 0, rr;

  if(d->layout != LAYOUT_ZPOOL){
    return 0;
  }
  qprefix(d->size, 1, buf, 0);
  r += rr = printf("%-10.10s %-36.36s %*s %4uB ZFS%2ju %5lu %-6.6s\n",
      d->name,
      d->uuid ? d->uuid : "n/a",
      PREFIXFMT(buf), d->physsec, d->zpool.zpoolver,
      d->zpool.disks, d->zpool.level ? d->zpool.level : "n/a"
      );
  if(rr < 0){
    return -1;
  }
  if(!descend){
    return r;
  }
  return r;
}

static int
print_dm(const device *d, int prefix, int descend){
  char buf[PREFIXSTRLEN + 1];
  const mdslave *md;
  int r = 0, rr;

  if(d->layout != LAYOUT_DM){
    return 0;
  }
  use_terminfo_color(COLOR_YELLOW, 1);
  qprefix(d->size, 1, buf, 0);
  r += rr = printf("%-*.*s%-10.10s %-36.36s %*s %4uB %-6.6s%5lu %-6.6s\n",
      prefix, prefix, "",
      d->name,
      d->uuid ? d->uuid : "n/a",
      PREFIXFMT(buf), d->physsec, "n/a",
      d->dmdev.disks, d->dmdev.level ? d->dmdev.level : "n/a"
      );
  if(rr < 0){
    return -1;
  }
  if(!descend){
    return r;
  }
  for(md = d->dmdev.slaves ; md ; md = md->next){
    device *s = lookup_device(md->name);

    if(s){
      r += rr = print_dev_mplex(s, 1, descend);
      if(rr < 0){
        return -1;
      }
      if(strcmp(md->name, s->name)){
        const device *p;

        for(p = s->parts ; p ; p = p->next){
          if(strcmp(md->name, p->name)){
            continue;
          }
          r += rr = print_partition(p, descend);
          if(rr < 0){
            return -1;
          }
        }
      }
    }

  }
  return r;
}

static int
print_mdadm(const device *d, int prefix, int descend){
  char buf[PREFIXSTRLEN + 1];
  const mdslave *md;
  int r = 0, rr;

  if(d->layout != LAYOUT_MDADM){
    return 0;
  }
  use_terminfo_color(COLOR_YELLOW, 1);
  qprefix(d->size, 1, buf, 0);
  r += rr = printf("%-*.*s%-10.10s %-36.36s %*s %4uB %-6.6s%5lu %-6.6s\n",
      prefix, prefix, "",
      d->name,
      d->uuid ? d->uuid : "n/a",
      PREFIXFMT(buf),
      d->physsec, "n/a",
      d->mddev.disks, d->mddev.level ? d->mddev.level : "n/a");
  if(rr < 0){
    return -1;
  }
  if(!descend){
    return r;
  }
  for(md = d->mddev.slaves ; md ; md = md->next){
    device *s = lookup_device(md->name);

    if(s){
      r += rr = print_dev_mplex(s, 1, descend);
      if(rr < 0){
        return -1;
      }
      if(strcmp(md->name, s->name)){
        const device *p;

        for(p = s->parts ; p ; p = p->next){
          if(strcmp(md->name, p->name)){
            continue;
          }
          r += rr = print_partition(p, descend);
          if(rr < 0){
            return -1;
          }
        }
      }
    }

  }
  return r;
}

static int
print_dev_mplex(const device *d, int prefix, int descend){
  switch(d->layout){
    case LAYOUT_NONE:
      return print_drive(d, descend);
    case LAYOUT_PARTITION:
      return print_partition(d, descend);
    case LAYOUT_MDADM:
      return print_mdadm(d, prefix, descend);
    case LAYOUT_DM:
      return print_dm(d, prefix, descend);
    case LAYOUT_ZPOOL:
      return print_zpool(d, descend);
    default:
      return -1;
  }
}

static int
print_controller(const controller *c, int descend){
  int r = 0, rr;
  device *d;

  use_terminfo_color(COLOR_WHITE, 1);
  switch(c->bus){
    case BUS_PCIe:
      if(c->pcie.lanes_neg == 0){
        r += rr = printf("[%s] Onboard %04x:%02x.%02x.%x\n ",
          c->ident, c->pcie.domain, c->pcie.bus,
          c->pcie.dev, c->pcie.func);
      }else{
        char buf[PREFIXSTRLEN + 1];

        r += rr = printf("[%s] PCI Express %04x:%02x.%02x.%x (gen %s x%u, %sbps)\n ",
          c->ident, c->pcie.domain, c->pcie.bus,
          c->pcie.dev, c->pcie.func,
          pcie_gen(c->pcie.gen), c->pcie.lanes_neg,
          qprefix(c->bandwidth, 1, buf, 1));
      }
      break;
    case BUS_VIRTUAL:
      rr = 0;
      break;
    case BUS_UNKNOWN:
      rr = 0;
      break;
    default:
      fprintf(stderr, "Unknown bus type: %d\n", c->bus);
      return -1;
  }
  if(rr < 0){
    return -1;
  }
  r += rr = printf("%s\n", c->name);
  if(rr < 0){
    return -1;
  }
  if(!descend){
    return r;
  }
  for(d = c->blockdevs ; d ; d = d->next){
    r += rr = print_drive(d, descend);
    if(rr < 0){
      return -1;
    }
  }
  return r;
}

static int
detail_controller(const controller *c){
  int r, rr;

  if((r = print_controller(c, 0)) < 0){
    return r;
  }
  r += rr = printf("Firmware: %s\n", c->fwver ? c->fwver : "Unknown / inapplicable");
  if(rr < 0){
    return -1;
  }
  return 0;
}

static int
adapter(wchar_t * const *args, const char *arghelp){
  const controller *ci;
  int descend;

  if(args[1] == NULL){
    descend = 0;
  }else if(wcscmp(args[1], L"-v") == 0 && args[2] == NULL){
    descend = 1;
  }else{
    controller *c;

    if(args[2] && !args[3]){
      if((c = lookup_wcontroller(args[2])) == NULL){
        return -1;
      }
      if(wcscmp(args[1], L"reset") == 0){
        if(reset_controller(c)){
          return -1;
        }
        return 0;
      }else if(wcscmp(args[1], L"rescan") == 0){
        if(rescan_controller(c)){
          return -1;
        }
        return 0;
      }else if(wcscmp(args[1], L"detail") == 0){
        if(detail_controller(c)){
          return -1;
        }
        return 0;
      }
    }
    usage(args, arghelp);
    return -1;
  }
  for(ci = get_controllers() ; ci ; ci = ci->next){
    if(print_controller(ci, descend) < 0){
      return -1;
    }
  }
  return 0;
}

// Walk the block devices, evaluating *fxn on each. The return value will be
// accumulated in r, unless -1 is ever returned, in which case we abort
// immediately and return -1.
static int
walk_devices(int (*fxn)(const device *, int), int descend){
  const controller *c;
  int rr, r = 0;

  for(c = get_controllers() ; c ; c = c->next){
    const device *d;

    for(d = c->blockdevs ; d ; d = d->next){
      const device *p;

      r += rr = fxn(d, descend);
      if(rr < 0){
        return -1;
      }
      for(p = d->parts ; p ; p = p->next){
        r += rr = fxn(p, descend);
        if(rr < 0){
          return -1;
        }
      }
    }
  }
  return 0;
}

static int
zpool(wchar_t * const *args, const char *arghelp){
  int descend;

  if(args[1] == NULL){
    descend = 0;
  }else if(wcscmp(args[1], L"-v") == 0 && args[2] == NULL){
    descend = 1;
  }else{
    if(vpopen_drain("zpool", args + 1)){
      usage(args, arghelp);
      return -1;
    }
    return 0;
  }
  printf("%-10.10s %-36.36s %*s %5.5s %-6.6s%-6.6s%-6.6s\n",
         "Device", "UUID", PREFIXFMT("Bytes"), "AShft", "Fmt", "Disks", "Level");
  if(walk_devices(print_zpool, descend)){
    return -1;
  }
  return 0;
}

static int
zfs(wchar_t * const *args, const char *arghelp){
  if(args[1] == NULL){
    usage(args, arghelp);
    return -1;
  }
  if(vpopen_drain("zfs", args + 1)){
    return -1;
  }
  return 0;
}

static int
dm(wchar_t * const *args, const char *arghelp){
  int descend;

  if(args[1] == NULL){
    descend = 0;
  }else if(wcscmp(args[1], L"-v") == 0 && args[2] == NULL){
    descend = 1;
  }else{
    if(vpopen_drain("dmsetup", args + 1)){
      usage(args, arghelp);
      return -1;
    }
    return 0;
  }
  /*
  printf("%-10.10s %-36.36s %*s %5.5s %-6.6s%-6.6s%-6.6s\n",
      "Device", "UUID", "Bytes", "PSect", "Table", "Disks", "Level");
  if(walk_devices(print_zpool, descend)){
    return -1;
  }
  */
  fprintf(stderr, "Sorry, not yet implemented %d\n", descend); // FIXME
  return -1;
}

static int
mdadm(wchar_t * const *args, const char *arghelp){
  const controller *c;
  int descend;

  if(args[1] == NULL){
    descend = 0;
  }else if(wcscmp(args[1], L"-v") == 0 && args[2] == NULL){
    descend = 1;
  }else{
    if(vpopen_drain("mdadm", args + 1)){
      usage(args, arghelp);
      return -1;
    }
    usage(args, arghelp);
    return -1;
  }
  printf("%-10.10s %-36.36s %*s %5.5s %-6.6s%-6.6s%-6.6s\n",
      "Device", "UUID", PREFIXFMT("Bytes"), "PSect", "Table", "Disks", "Level");
  for(c = get_controllers() ; c ; c = c->next){
    device *d;

    if(c->bus != BUS_VIRTUAL){
      continue;
    }
    for(d = c->blockdevs ; d ; d = d->next){
      if(d->layout == LAYOUT_MDADM){
        if(print_mdadm(d, 0, descend) < 0){
          return -1;
        }
      }else if(d->layout == LAYOUT_DM){
        if(print_dm(d, 0, descend) < 0){
          return -1;
        }
      }
    }
  }
  return 0;
}

static int
print_tabletypes(void){
  int z, count, rr, r = 0;
  pttable_type *pty;

  if((pty = get_ptable_types(&count)) == NULL){
    return -1;
  }
  for(z = 0 ; z < count ; ++z){
    r += rr = printf("%-4.4s %s\n", pty[z].name, pty[z].desc);
    if(rr < 0){
      return -1;
    }
  }
  free_ptable_types(pty, count);
  return r;
}

static int
print_fstypes(void){
  int z, count, rr, r = 0;
  pttable_type *pty;

  if((pty = get_fs_types(&count)) == NULL){
    return -1;
  }
  for(z = 0 ; z < count ; ++z){
    r += rr = printf("%-11.11s %s\n", pty[z].name, pty[z].desc);
    if(rr < 0){
      return -1;
    }
  }
  free_ptable_types(pty, count);
  return r;
}

static inline int
blockdev_dump(int descend){
  const controller *c;

  printf("%-10.10s %-16.16s %4.4s %*s %5.5s Flags %-6.6s%-16.16s %-4.4s\n",
      "Device", "Model", "Rev", PREFIXFMT("Bytes"), "PSect", "Table", "WWN", "PHY");
  for(c = get_controllers() ; c ; c = c->next){
    const device *d;

    for(d = c->blockdevs ; d ; d = d->next){
      if(print_drive(d, descend) < 0){
        return -1;
      }
    }
  }
  use_terminfo_color(COLOR_WHITE, 1);
  printf("\n\tFlags:\t(R)emovable, (U)nloaded, (V)irtual, (M)dadm, (Z)pool, \n"
    "\t\t(D)M, r(O)tational, (r)ead-only, (W)ritecache enabled, \n"
    "\t\t(B)IOS bootable, v/⚠: Read-Write-Verify, ✓/✗/☠: SMART status\n");
  return 0;
}

static inline int
blockdev_details(const device *d){
  char buf[BUFSIZ];
  unsigned z;

  if(print_drive(d, 1) < 0){
    return -1;
  }
  printf("\n");
  use_terminfo_color(COLOR_YELLOW, 1);
  print_drive_stats_identified(d);
  use_terminfo_color(COLOR_WHITE, 1);
  printf("Logical sector size: %u Physical: %u\n", d->logsec, d->physsec);
  printf("I/O scheduler: %s\n", d->sched ? d->sched : "N/A");
  if(d->layout == LAYOUT_NONE){
    if(d->blkdev.biossha1){
      if(printf("\nBIOS boot SHA-1: ") < 0){
        return -1;
      }
      for(z = 0 ; z < 19 ; ++z){
        if(printf("%02x:", ((const unsigned char *)d->blkdev.biossha1)[z]) < 0){
          return -1;
        }
      }
      if(printf("%02x\n", ((const unsigned char *)d->blkdev.biossha1)[z]) < 0){
        return -1;
      }
    }
    printf("Serial number: %s\n", d->blkdev.serial ? d->blkdev.serial : "n/a");
    printf("Transport: %s\n", transport_str(d->blkdev.transport));
    if(d->blkdev.transport == DIRECT_NVME){
      if(snprintf(buf, sizeof(buf), "nvme id-ctrl /dev/%s", d->name) >= (int)sizeof(buf)){
        return -1;
      }
    }else{ // probably shouldn't for e.g. USB? maybe should? FIXME
      if(snprintf(buf, sizeof(buf), "hdparm -I /dev/%s", d->name) >= (int)sizeof(buf)){
        return -1;
      }
    }
  }else if(d->layout == LAYOUT_MDADM){
    if(snprintf(buf, sizeof(buf), "mdadm --detail /dev/%s", d->name) >= (int)sizeof(buf)){
      return -1;
    }
  }else if(d->layout == LAYOUT_DM){
    if(snprintf(buf, sizeof(buf), "dmsetup info /dev/%s", d->name) >= (int)sizeof(buf)){
      return -1;
    }
  }else if(d->layout == LAYOUT_ZPOOL){
    if(snprintf(buf, sizeof(buf), "zpool status %s", d->name) >= (int)sizeof(buf)){
      return -1;
    }
  }else{
    return 0;
  }
  if(popen_drain(buf)){
    return -1;
  }
  return 0;
}

static int
blockdev(wchar_t * const *args, const char *arghelp){
  device *d;

  if(args[1] == NULL){
    return blockdev_dump(0);
  }
  if(args[2] == NULL){
    if(wcscmp(args[1], L"-v") == 0){
      return blockdev_dump(1);
    }else if(wcscmp(args[1], L"mktable") == 0){
      if(print_tabletypes() < 0){
        return -1;
      }
      return 0;
    }
    usage(args, arghelp);
    return -1;
  }
  // Everything else has a required device argument
  if((d = lookup_wdevice(args[2])) == NULL){
    return -1;
  }
  if(wcscmp(args[1], L"rescan") == 0){
    if(args[3]){
      usage(args, arghelp);
      return -1;
    }
    if(rescan_blockdev(d)){
      return -1;
    }
    return 0;
  }else if(wcscmp(args[1], L"badblocks") == 0){
    unsigned rw = 0;

    if(args[3]){
      if(args[4] || wcscmp(args[3], L"rw")){
        usage(args, arghelp);
        return -1;
      }
      rw = 1;
    }
    return badblock_scan(d, rw);
  }else if(wcscmp(args[1], L"rmtable") == 0){
    if(args[3]){
      usage(args, arghelp);
      return -1;
    }
    return wipe_ptable(d, NULL);
  }else if(wcscmp(args[1], L"wipebiosboot") == 0){
    if(args[3]){
      usage(args, arghelp);
      return -1;
    }
    return wipe_biosboot(d);
  }else if(wcscmp(args[1], L"wipedosmbr") == 0){
    if(args[3]){
      usage(args, arghelp);
      return -1;
    }
    return wipe_dosmbr(d);
  }else if(wcscmp(args[1], L"ataerase") == 0){
    if(args[3]){
      usage(args, arghelp);
      return -1;
    }
    return ata_secure_erase(d);
  }else if(wcscmp(args[1], L"detail") == 0){
    if(args[3]){
      usage(args, arghelp);
      return -1;
    }
    return blockdev_details(d);
  }else if(wcscmp(args[1], L"mktable") == 0){
    if(args[3] == NULL || args[4]){
      usage(args, arghelp);
      return -1;
    }
    make_partition_wtable(d, args[3]);
    return 0;
  }
  usage(args, arghelp);
  return -1;
}

static int
print_partition_attributes(void){
  printf("GPT flags:\n");
  printf("\t0x%016llx %s\n", 0x0000000000000001llu, "Required partition");
  printf("\t0x%016llx %s\n", 0x0000000000000002llu, "Hide from EFI");
  printf("\t0x%016llx %s\n", 0x0000000000000004llu, "Legacy BIOS bootable");
  printf("\t0x%016llx %s\n", 0x1000000000000000llu, "Read-only");
  printf("\t0x%016llx %s\n", 0x2000000000000000llu, "Shadow copy");
  printf("\t0x%016llx %s\n", 0x4000000000000000llu, "Hidden");
  printf("\t0x%016llx %s\n", 0x8000000000000000llu, "No automount");
  printf("MBR flags:\n");
  printf("\t0x%02x %s\n", 0x80u, "Bootable");
  return 0;
}

static int
print_partition_types(void){
  const ptype *pt;

  for(pt = ptypes ; pt->name ; ++pt){
    if(printf("%04x %-37.37s", pt->code, pt->name) < 0){
      return -1;
    }
    // FIXME if all zeros, don't print gpt guid
    char gstr[GUIDSTRLEN + 1];

    guidstr(pt->gpt_guid, gstr);
    if(printf(" %s", gstr) < 0){
      return -1;
    }
    if(printf("\n") < 0){
      return -1;
    }
  }
  return 0;
}

// Allows:
//
//  a number by itself. this ought be considered a size in bytes. written to
//    **size only. *fsec and *lsec are set to NULL.
//  a number followed by a colon, indicating the specified sector through the
//      end of its empty space, and written to **fsec. *size and *lsec = NULL.
//  a colon followed by a number, indicating the largest contiguous free space
//      possibly ending with that sector, and written to **lsec.
//  two numbers separated by a colon, indicating a precise range.
static int
extract_partition_spec(const wchar_t *spec, uintmax_t **size,
        uintmax_t **fsec, uintmax_t **lsec){
  wchar_t *sep;

  if((sep = wcschr(spec, L':')) == NULL){
    *fsec = *lsec = NULL;
    if(wstrtoull(spec, *size)){
      return -1;
    }
    return 0;
  }
  if(spec == sep){
    *size = *fsec = NULL;
    if(wstrtoull(sep + 1, *lsec)){
      return -1;
    }
    return 0;
  }else if(sep[1] == L'\0'){
    *size = *lsec = NULL;
    if(wstrtoull(spec, *fsec)){
      return -1;
    }
    return 0;
  }
  *size = NULL;
  if(wstrtoull(spec, *fsec)){
    return -1;
  }
  if(wstrtoull(sep + 1, *lsec)){
    return -1;
  }
  return 0;
}

static int
partition(wchar_t * const *args, const char *arghelp){
  const controller *c;
  int descend;

  if(args[1] == NULL){
    descend = 0;
  }else if(wcscmp(args[1], L"-v") == 0 && args[2] == NULL){
    descend = 1;
  }else{
    device *d;

    if(wcscmp(args[1], L"setflag") == 0){
      uintmax_t ull;
      unsigned val;

      if(!args[2]){
        print_partition_attributes();
        return 0;
      }
      if((d = lookup_wdevice(args[2])) == NULL){
        usage(args, arghelp);
        return -1;
      }
      if(!args[3] || !args[4] || args[5]){
        usage(args, arghelp);
        return -1;
      }else if(wstrtoull(args[4], &ull)){
        usage(args, arghelp);
        return -1;
      }else if(ull > (1ull << 63) || ull == 0){
        usage(args, arghelp);
        return -1;
      }else if( (ull & (ull - 1u)) ){ // ought be power of 2
        usage(args, arghelp);
        return -1;
      }else if(wcscasecmp(args[3], L"on") == 0){
        val = 1;
      }else if(wcscasecmp(args[3], L"off") == 0){
        val = 0;
      }else{
        usage(args, arghelp);
        return -1;
      }
      if(partition_set_flag(d, ull, val)){
        return -1;
      }
      return 0;
    }else if(wcscmp(args[1], L"settype") == 0){
      unsigned ull;

      if(!args[2]){
        if(print_partition_types() < 0){
          return -1;
        }
        return 0;
      }
      if((d = lookup_wdevice(args[2])) == NULL){
        usage(args, arghelp);
        return -1;
      }
      if(!args[3] || args[4]){
        usage(args, arghelp);
        return -1;
      }else if(wstrtoxu(args[3], &ull)){
        usage(args, arghelp);
        return -1;
      }else if(ull > 0xffff || ull == 0){
        usage(args, arghelp);
        return -1;
      }
      if(partition_set_code(d, ull)){
        return -1;
      }
      return 0;
    }
    if(args[2] == NULL){ // the remainder always have an arg
      usage(args, arghelp);
      return -1;
    }
    if((d = lookup_wdevice(args[2])) == NULL){
      usage(args, arghelp);
      return -1;
    }
    if(wcscmp(args[1], L"add") == 0){
      uintmax_t fsec, lsec, size, *f, *l, *s;
      unsigned code;

      // target dev == 2, 3 == sectors, 4 == name, 5 == type
      if(!args[3] || !args[4] || !args[5] || args[6]){
        usage(args, arghelp);
        return -1;
      }
      f = &fsec;
      l = &lsec;
      s = &size;
      if(extract_partition_spec(args[3], &s, &f, &l)){
        usage(args, arghelp);
        return -1;
      }
      if(wstrtoxu(args[5], &code)){
        usage(args, arghelp);
        return -1;
      }
      if(s){
        fprintf(stderr, "Size-based creation is not yet implemented FIXME\n");
        return -1;
      }else{
        if(add_partition(d, args[4], fsec, lsec, code)){
          return -1;
        }
      }
      return 0;
    }else if(wcscmp(args[1], L"del") == 0){
      if(args[3]){
        usage(args, arghelp);
        return -1;
      }
      if(wipe_partition(d)){
        return -1;
      }
      return 0;
    }else if(wcscmp(args[1], L"setname") == 0){
      if(!args[3] || args[4]){
        usage(args, arghelp);
        return -1;
      }
      if(name_partition(d, args[3])){
        return -1;
      }
      return 0;
    }else if(wcscmp(args[1], L"setuuid") == 0){
      if(!args[3] || args[4]){
        usage(args, arghelp);
        return -1;
      }
      if(uuid_partition(d, args[3])){
        return -1;
      }
      return 0;
    }
    usage(args, arghelp);
    return -1;
  }
  printf("%-10.10s %-36.36s %*s %-4.4s %s\n",
      "Partition", "UUID", PREFIXFMT("Bytes"), "Role", "Name");
  for(c = get_controllers() ; c ; c = c->next){
    const device *d;

    for(d = c->blockdevs ; d ; d = d->next){
      const device *p;

      for(p = d->parts ; p ; p = p->next){
        if(print_partition(p, descend) < 0){
          return -1;
        }
      }
    }
  }
  return 0;
}

static int
mounts(wchar_t * const *args, const char *arghelp){
  const controller *c;

  ZERO_ARG_CHECK(args, arghelp);
  printf("%-*.*s %-5.5s %-36.36s %s %*s\n",
      FSLABELSIZ, FSLABELSIZ, "Label",
      "Type", "UUID", "Device", PREFIXFMT("Bytes"));
  for(c = get_controllers() ; c ; c = c->next){
    const device *d;

    for(d = c->blockdevs ; d ; d = d->next){
      const device *p;

      if(print_mounts(d) < 0){
        return -1;
      }
      for(p = d->parts ; p ; p = p->next){
        if(print_mounts(p) < 0){
          return -1;
        }
      }
    }
  }
  return 0;
}

static int
unmap(wchar_t * const *args, const char *arghelp){
  device *d;

  TWO_ARG_CHECK(args, arghelp);
  if((d = lookup_wdevice(args[1])) == NULL){
    return -1;
  }
  if(args[2][0] != L'/'){
    fprintf(stderr, "Not an absolute path: %ls\n", args[2]);
    return -1;
  }
  if(prepare_wumount(d, args[1])){
    return -1;
  }
  return 0;
}

static int
map(wchar_t * const *args, const char *arghelp){
  device *d;

  if(!args[1]){
    char *s;

    if((s = dump_targets()) == NULL){
      return -1;
    }
    if(printf("%s", s) < 0){
      free(s);
      return -1;
    }
    free(s);
    return 0;
  }
  if(!args[2] || !args[3] || args[4]){
    usage(args, arghelp);
    return -1;
  }
  if((d = lookup_wdevice(args[1])) == NULL){
    return -1;
  }
  if(args[2][0] != L'/'){
    fprintf(stderr, "Not an absolute path: %ls\n", args[2]);
    return -1;
  }
  if(prepare_wmount(d, args[2], args[3])){
    return -1;
  }
  return 0;
}

static int
print_swaps(const device *d, int descend){
  char buf[PREFIXSTRLEN + 1];
  int rr, r = 0;

  if(descend){
    fprintf(stderr, "Can't descend for swap!\n");
  }
  if(d->swapprio == SWAP_INVALID){
    return 0;
  }
  qprefix(d->mntsize, 1, buf, 0);
  r += rr = printf("%-*.*s %-5d %-36.36s %s %*s\n",
      FSLABELSIZ, FSLABELSIZ, d->label ? d->label : "n/a",
      d->swapprio, d->uuid ? d->uuid : "n/a",
      d->name, PREFIXFMT(buf));
  if(rr < 0){
    return -1;
  }
  return r;
}

static inline int
fs_dump(int descend){
  printf("%-*.*s %-5.5s %-36.36s %s %*s\n",
         FSLABELSIZ, FSLABELSIZ, "Label",
         "Type", "UUID", "Device", PREFIXFMT("Bytes"));
  if(walk_devices(print_fs, descend)){
    return -1;
  }
  return 0;
}

static int
fs(wchar_t * const *args, const char *arghelp){
  device *d;

  if(args[1] == NULL){
    return fs_dump(0);
  }
  if(args[2] == NULL){
    if(wcscmp(args[1], L"mkfs") == 0){
      if(print_fstypes() < 0){
        return -1;
      }
      return 0;
    }
    usage(args, arghelp);
    return -1;
  }
// Everything else has a required device argument
  if((d = lookup_wdevice(args[2])) == NULL){
    return -1;
  }
  if(wcscmp(args[1], L"mkfs") == 0){
    if(!args[3] || !args[4] || args[5]){
      usage(args, arghelp);
      return -1;
    }
    if(make_wfilesystem(d, args[3], args[4])){
      return -1;
    }
    return 0;
  }else if(wcscmp(args[1], L"wipefs") == 0){
    if(args[3]){
      usage(args, arghelp);
      return -1;
    }
    if(wipe_filesystem(d)){
      return -1;
    }
    return 0;
  }else if(wcscmp(args[1], L"fsck") == 0){
    if(args[3]){
      usage(args, arghelp);
      return -1;
    }
    if(check_partition(d)){
      return -1;
    }
    return 0;
  }else if(wcscmp(args[1], L"umount") == 0){
    if(args[3]){
      usage(args, arghelp);
      return -1;
    }
    if(unmount(d, NULL)){
      return -1;
    }
    return 0;
  }else if(wcscmp(args[1], L"setuuid") == 0){
    if(!args[3] || args[4]){
      usage(args, arghelp);
      return -1;
    }
    // FIXME
    return 0;
  }else if(wcscmp(args[1], L"setlabel") == 0){
    if(!args[3] || args[4]){
      usage(args, arghelp);
      return -1;
    }
    // FIXME
    return 0;
  }else if(wcscmp(args[1], L"mount") == 0){
    if(!args[3] || args[4]){
      usage(args, arghelp);
      return -1;
    }
    wmmount(d, args[3]);
    return 0;
  }else if(wcscmp(args[1], L"loop") == 0){
    if(!args[3] || args[4]){
      usage(args, arghelp);
      return -1;
    }
    // FIXME
    return 0;
  }
  usage(args, arghelp);
  return -1;
}

static int
swap(wchar_t * const *args, const char *arghelp){
  device *d;
  if(!args[1]){
    if(printf("%-*.*s %-5.5s %-36.36s %s %*s\n", FSLABELSIZ, FSLABELSIZ,
          "Label", "Prio", "UUID", "Device", PREFIXFMT("Bytes")) < 0){
      return -1;
    }
    if(walk_devices(print_swaps, 0)){
      return -1;
    }
    return 0;
  }
  TWO_ARG_CHECK(args, arghelp);
  if((d = lookup_wdevice(args[2])) == NULL){
    return -1;
  }
  if(wcscmp(args[1], L"on") == 0){
    if(swapondev(d)){
      return -1;
    }
  }else if(wcscmp(args[1], L"off") == 0){
    if(swapoffdev(d)){
      return -1;
    }
  }else{
    usage(args, arghelp);
    return -1;
  }
  return 0;
}

static int
benchmark(wchar_t * const *args, const char *arghelp){
  device *d;

  ONE_ARG_CHECK(args, arghelp);
  if((d = lookup_wdevice(args[1])) == NULL){
    return -1;
  }
  if(benchmark_blockdev(d)){
    return -1;
  }
  return 0;
}

static int
troubleshoot(wchar_t * const *args, const char *arghelp){
  ZERO_ARG_CHECK(args, arghelp);
  fprintf(stderr, "Sorry, not yet implemented\n");
  // FIXME things to do:
  // FIXME check PCIe bandwidth against SATA bandwidth
  // FIXME check for proper alignment of partitions
  // FIXME check for msdos, apm or bsd partition tables
  // FIXME check for filesystems without noatime
  // FIXME check for SSD erase block size alignment
  // FIXME check for GPT partition table validity
  return -1;
}

static device *
get_target_root(void){
  const controller *c;

  if(growlight_target == NULL){
    fprintf(stderr, "No target is defined\n");
    return NULL;
  }
  for(c = get_controllers() ; c ; c = c->next){
    device *d, *p;

    for(d = c->blockdevs ; d ; d = d->next){
      if(string_included_p(&d->mnt, growlight_target)){
        return d;
      }
      for(p = d->parts ; p ; p = p->next){
        if(string_included_p(&p->mnt, growlight_target)){
          return d;
        }
      }
    }
  }
  return NULL;
}

static int
uefiboot(wchar_t * const *args, const char *arghelp){
  device *dev;

  ZERO_ARG_CHECK(args, arghelp);
  if((dev = get_target_root()) == NULL){
    return -1;
  }
  if(prepare_uefi_boot(dev)){
    return -1;
  }
  return 0;
}

static int
biosboot(wchar_t * const *args, const char *arghelp){
  device *dev;

  ZERO_ARG_CHECK(args, arghelp);
  if((dev = get_target_root()) == NULL){
    return -1;
  }
  if(prepare_bios_boot(dev)){
    return -1;
  }
  return 0;
}

static int
grubmap(wchar_t * const *args, const char *arghelp){
  ZERO_ARG_CHECK(args, arghelp);

  if(popen_drain("grub-mkdevicemap -m /dev/stdout")){
    return -1;
  }
  return 0;
}

static void
free_tokes(wchar_t **tokes){
  wchar_t **toke;

  if(tokes){
    for(toke = tokes ; *toke ; ++toke){
      free(*toke);
    }
    free(tokes);
  }
}

static int
tokenize(const char *line, wchar_t ***tokes){
  unsigned inquotes = 0;
  const char *s = NULL;
  unsigned wchars = 0;
  mbstate_t ps, sps;
  size_t len, conv;
  int t = 0;

  memset(&sps, 0, sizeof(sps));
  memset(&ps, 0, sizeof(ps));
  len = strlen(line);
  *tokes = NULL;
  do{
    wchar_t w, *n, **tmp;

    if((conv = mbrtowc(&w, line, len, &ps)) == (size_t)-1){
      free_tokes(*tokes);
      fprintf(stderr, "Error converting multibyte: %s\n", line);
      break;
    }
    if(conv == (size_t)-2){
      // FIXME len ought be expanded? character didn't terminate
      conv = 0;
      w = L'\0';
    }
    line += conv;
    len -= conv;
    if(s == NULL){
      if(conv){
        if(iswspace(w)){
          continue;
        }
        if(w == L'"'){
          inquotes = 1;
        }
      }
      s = line - conv;
      sps = ps;
      wchars = 1;
    }else{
      const char *olds;

      if(!conv){
        if(inquotes){
          fprintf(stderr, "Unterminated quotes in %s\n", line);
          free_tokes(*tokes);
          return -1;
        }
      }else{
        if(iswgraph(w)){
          if(w == L'"'){
            inquotes = !inquotes;
          }
          ++wchars;
          continue;
        }else if(iswspace(w)){
          if(inquotes){
            ++wchars;
            continue;
          }
        }
      }
      assert(wchars);
      if((n = malloc(sizeof(*n) * (wchars + 1))) == NULL){
        free_tokes(*tokes);
        return -1;
      }
      olds = s;
      if(mbsrtowcs(n, &s, wchars, &sps) != wchars){
        fprintf(stderr, "Couldn't convert %s\n", olds);
        free(n);
        free_tokes(*tokes);
        return -1;
      }
      n[wchars] = L'\0';
      // Use t + 2 because we must have space for a final NULL
      if((tmp = realloc(*tokes, sizeof(**tokes) * (t + 2))) == NULL){
        free(n);
        free_tokes(*tokes);
        return -1;
      }
      *tokes = tmp;
      (*tokes)[t++] = n;
      wchars = 0;
      s = NULL;
    }
  }while(conv);
  if(t){
    (*tokes)[t] = NULL;
  }
  return t;
}

static void
do_logo(void){
  if(ncd){
    int v = ncdirect_render_image(ncd, GROWLIGHT_SHARE "/growlight.jpg",
                                  NCALIGN_CENTER, NCBLIT_DEFAULT, NCSCALE_SCALE_HIRES);
    if(v >= 0){
      return;
    }
  }
  use_terminfo_color(COLOR_RED, 1);
  printf("+++++++++++++++++++++++++++++++++++++++++++++++++############++++++++++++++++++\n"
"++++++++++++++++++++++++++++++++++++++++++++++++++###########++++++++++++++++++\n"
"++++++++++++++++++++++++++++++++++++++++++++++++++###########++++++++++++++++++\n"
"++++++++++++++++++++++++++++++++++++++++++++++++++###########++++++++++++++++++\n"
"+++++++++++++++++++++++++++++++++++++++++++++++++++##########++++++''''++++++++\n"
"+++++++++++++++++++++++++++++++++'+++++++++++++''+++#########+++++'''''''''''''\n"
"+++++++++++++++++++++++++++++++++'++''+++++++++''+++###++####+++++''''+';;';;''\n"
"+++++++++++++++++++++++++++++++++'++''+++++++++''+++###++####+++++'''';''++'';;\n"
"+++++++++++++++++++++++++++++++++'+++++++++++++''+##+##++####+++++''';+;+;'+++'\n"
"+++++#'+++#++#++#+++#++++++++++#+'+++#++#+++''+''++####++#@@#+++++'''';+';''';+\n"
"+''''''#+''''''''''++''++''++'''+'++''#+'''''++''''++##++++##+++++'''+';+';+;'+\n"
"+''##''++''+++''++''+''++''+#''++'++''+''++''++'''+'+##++#####++++'''+:'+';'''+\n"
"+''++''++''++#''++''+'''+'''#''++'++''+''+#+'++''++''##++#####++++'''+'''''''''\n"
"+''++''++''+++'++++'++''''''''+++'++''+''++''++''++#+##++#####+++++''++'''+''''\n"
"+''''''++'++++''++''++''''''''+++'++''+#'''''++''+++'##++#####+++++''''''+'''''\n"
"+''+++#++'++++''''''+++''++'''+++'++''+''+++#++''+++'##++##@##+++++''''''''''''\n"
"+'''''++#'++++''''''+++''++''+++#'++''#'''''+++''+++'###++++###++++''''''''''''\n"
"+''''''+#+#++++#+++++++++++++#++#+##+++'''''''+++++#+#+########+++++'''''''''''\n"
"+''++''#+++++++++++++++++++#+++++++++++''#+#''++++++++++#######+++++'''''''''''\n"
"+''++''++++++++++++++++++++++++++++++++''++'''+++++++++##########++++''''''''''\n"
"++'''''#++++++++++++++++++++++++++++++++'''''++++++++++##########++++''++++++''\n"
"++#++++++++++++++++++++++++++++++++++++++++#++++++++++++#########++++'+''''''++\n");
}

static int
version(wchar_t * const *args, const char *arghelp){
  int ret = 0;

  ZERO_ARG_CHECK(args, arghelp);
  do_logo();
  use_terminfo_color(COLOR_WHITE, 1);
  ret |= popen_drain("mkswap --version");
  printf("\n");
  ret |= popen_drain("grub-mkdevicemap --version");
  if(print_zfs_version(stdout) < 0){
    ret |= -1;
  }
  printf("\n");
  use_terminfo_color(COLOR_RED, 1);
  printf("%s %s\n", PACKAGE, VERSION);
  return ret;
}

static int
target(wchar_t * const *args, const char *arghelp){
  if(args[1] == NULL){
    const char *t = get_target();

    if(t == NULL){
      if(printf("No target is defined\n") < 0){
        return -1;
      }
    }else{
      if(printf("%s\n", t) < 0){
        return -1;
      }
    }
    return 0;
  }
  if(wcscmp(args[1], L"set") == 0){
    char targ[PATH_MAX];

    if(args[2] == NULL || args[3]){
      usage(args, arghelp);
      return -1;
    }
    if(snprintf(targ, sizeof(targ), "%ls", args[2]) >= (int)sizeof(targ)){
      fprintf(stderr, "Bad target specification: %ls\n", args[2]);
      usage(args, arghelp);
      return -1;
    }
    if(set_target(targ)){
      return -1;
    }
    return 0;
  }else if(wcscmp(args[1], L"unset") == 0){
    if(args[2]){
      usage(args, arghelp);
      return -1;
    }
    if(set_target(NULL)){
      return -1;
    }
    return 0;
  }else if(wcscmp(args[1], L"finalize") == 0){
    if(args[2]){
      usage(args, arghelp);
      return -1;
    }
    if(finalize_target()){
      return -1;
    }
    return 0;
  }
  usage(args, arghelp);
  return -1;
}

static int
diags(wchar_t * const *args, const char *arghelp){
  logent logs[MAXIMUM_LOG_ENTRIES];
  unsigned idx;
  int z;

  idx = sizeof(logs) / sizeof(*logs);
  if(args[1]){
    if(!args[2]){
      uintmax_t ull;

      if(wstrtoull(args[1], &ull)){
        usage(args, arghelp);
        return -1;
      }
      if(ull > idx || ull == 0){
        fprintf(stderr, "Request no more than %u log records, and no fewer than 1\n", idx);
        return -1;
      }
      idx = ull;
    }else{
      usage(args, arghelp);
      return -1;
    }
  }
  if((z = get_logs(idx, logs)) < 0){
    return -1;
  }
  while(z--){
    char tbuf[26]; // see ctime_r(3)

    if(ctime_r(&logs[z].when, tbuf) == NULL){
      fprintf(stderr, "Bad timestamp at index %d! %s\n", z, logs[z].msg);
    }else{
      tbuf[strlen(tbuf) - 1] = ' '; // kill newline
      printf("%s%s", tbuf, logs[z].msg);
    }
    free(logs[z].msg);
  }
  fflush(stdout);
  return 0;
}

static int
stats(wchar_t * const *args, const char *arghelp){
  const controller *c;

  ZERO_ARG_CHECK(args, arghelp);
  use_terminfo_color(COLOR_WHITE, 1);
  printf("Device         Sectors read          SRead Δ  Sectors written       SWritten Δ\n");
  use_terminfo_color(COLOR_BLUE, 1);
  for(c = get_controllers() ; c ; c = c->next){
    const device *d;

    for(d = c->blockdevs ; d ; d = d->next){
      if(print_drive_stats(d) < 0){
        return -1;
      }
    }
  }
  return 0;
}

static int
quit(wchar_t * const *args, const char *arghelp){
  ZERO_ARG_CHECK(args, arghelp);
  lights_off = 1;
  return 0;
}

static const struct fxn {
  const wchar_t *cmd;
  int (*fxn)(wchar_t * const *, const char *);
  const char *arghelp;
} fxns[] = {
#define FXN(x, args) { .cmd = L###x, .fxn = x, .arghelp = args, }
  FXN(adapter, "[ \"reset\" adapter ]\n"
      "                 | [ \"rescan\" adapter ]\n"
      "                 | [ \"detail\" adapter ]\n"
      "                 | [ -v ] no arguments to list all host bus adapters"),
  FXN(blockdev, "[ \"rescan\" blockdev ]\n"
      "                 | [ \"badblocks\" blockdev [ \"rw\" ] ]\n"
      "                 | [ \"wipebiosboot\" blockdev ]\n"
      "                 | [ \"wipedosmbr\" blockdev ]\n"
      "                 | [ \"ataerase\" blockdev ]\n"
      "                 | [ \"rmtable\" blockdev ]\n"
      "                 | [ \"mktable\" [ blockdev tabletype ] ]\n"
      "                    | no arguments to list supported table types\n"
      "                 | [ \"detail\" blockdev ]\n"
      "                 | [ -v ] no arguments to list all blockdevs"),
  FXN(partition, "[ \"del\" partition ]\n"
      "                 | [ \"add\" blockdev size/range name type ]\n"
      "                    size: a single number, interpreted as bytes\n"
      "                    range: num:num, num: or :num, interpreted as sectors\n"
      "                 | [ \"setuuid\" partition uuid ]\n"
      "                 | [ \"setname\" partition name ]\n"
      "                 | [ \"settype\" [ partition type ] ]\n"
      "                    | no arguments to list supported types\n"
      "                 | [ \"setflag\" [ partition \"on\"|\"off\" flag ] ]\n"
      "                    | no arguments to list supported flags\n"
      "                 | [ -v ] no arguments to list all partitions"),
  FXN(fs, "[ \"mkfs\" [ partition fstype name ] ]\n"
      "                 | no arguments to list supported fs types\n"
      "                 | [ \"fsck\" ks ]\n"
      "                 | [ \"wipefs\" fs ]\n"
      "                 | [ \"setuuid\" fs uuid ]\n"
      "                 | [ \"setlabel\" fs label ]\n"
      "                 | [ \"loop\" file mountpoint type options ]\n"
      "                 | [ \"mount\" blockdev mountpoint type options ]\n"
      "                 | [ \"umount\" blockdev ]\n"
      "                 | no arguments to list all filesystems"),
  FXN(swap, "[ \"on\"|\"off\" swapdevice ]\n"
      "                 | no arguments to list all swaps"),
  FXN(mdadm, "[ arguments passed directly through to mdadm(8) ]\n"
      "                 | [ -v ] no arguments to list all md devices"),
  FXN(dm, "[ arguments passed directly through to dmsetup(8) ]\n"
      "                 | [ -v ] no arguments to list all devicemaps"),
  FXN(zpool, "[ arguments passed directly through to zpool(8) ]\n"
      "                 | [ -v ] no arguments to list all zpools"),
  FXN(zfs, "arguments passed directly through to zfs(8)"),
  FXN(target, "[ \"set\" path ]\n"
      "                 | [ \"unset\" ]\n"
      "                 | [ \"finalize\" ]\n"
      "                 | no arguments prints target"),
  FXN(map, "[ mountdev mountpoint options ]\n"
      "                 | no arguments prints target fstab"),
  FXN(unmap, "mountpoint"),
  FXN(stats, ""),
  FXN(mounts, ""),
  FXN(uefiboot, "root fs map must be defined in GPT partition"),
  FXN(biosboot, "root fs map must be defined in GPT/MBR partition"),
  FXN(diags, "[ count ]"),
  FXN(grubmap, ""),
  FXN(benchmark, "blockdev"),
  FXN(troubleshoot, ""),
  FXN(version, ""),
  FXN(help, "[ command ]"),
  FXN(quit, ""),
  { .cmd = NULL, .fxn = NULL, .arghelp = NULL, },
#undef FXN
};

static int
help(wchar_t * const *args, const char *arghelp){
  const struct fxn *fxn;

  if(args[1] == NULL){
    use_terminfo_color(COLOR_WHITE, 1);
    printf("%-15.15s %s\n", "Command", "Arguments");
    for(fxn = fxns ; fxn->cmd ; ++fxn){
      printf("%-15.15ls %s\n", fxn->cmd, fxn->arghelp);
    }
  }else if(args[2] == NULL){
    for(fxn = fxns ; fxn->cmd ; ++fxn){
      if(wcscmp(fxn->cmd, args[1]) == 0){
        use_terminfo_color(COLOR_WHITE, 1);
        printf("%15.15ls %s\n", args[1], fxn->arghelp);
        return 0;
      }
    }
    fprintf(stderr, "Unknown command: %ls\n", args[1]);
    return -1;
  }else{
    usage(args, arghelp);
    return -1;
  }
  return 0;
}

#define RL_START "\x01" // RL_PROMPT_START_IGNORE
#define RL_END "\x02"  // RL_PROMPT_END_IGNORE
static int
tty_ui(void){
  static char prompt[80] = RL_START "\033[0;35m" RL_END
                           "[" RL_START "\033[0;36m" RL_END
                           PACKAGE RL_START "\033[0;35m" RL_END
                           "]" RL_START "\033[1;32m" RL_END
                           "(0)> " RL_START "\033[1;37m" RL_END;
  char *l;

  while( (l = ncdirect_readline(ncd, prompt)) ){
    const struct fxn *fxn;
    wchar_t **tokes;
    int z;

    fflush(stdout);
    z = tokenize(l, &tokes);
    free(l);
    if(z <= 0){
      continue;
    }
    for(fxn = fxns ; fxn->cmd ; ++fxn){
      if(wcscasecmp(fxn->cmd, tokes[0])){
        continue;
      }
      break;
    }
    if(fxn->fxn){
      use_terminfo_color(COLOR_WHITE, 1);
      lock_growlight();
      z = fxn->fxn(tokes, fxn->arghelp);
      unlock_growlight();
      if(z < 0){
        printf("\n");
      }
      use_terminfo_color(COLOR_WHITE, 0);
    }else{
      fprintf(stderr, "Unknown command: %ls\n", tokes[0]);
      z = -1;
    }
    free_tokes(tokes);
    if(z){
      snprintf(prompt, sizeof(prompt), RL_START "\033[0;35m" RL_END
          "[" RL_START "\033[0;36m" RL_END
          PACKAGE RL_START "\033[0;35m" RL_END
          "]" RL_START "\033[1;31m" RL_END
          "(%d)" RL_START "\033[1;32m" RL_END
          "> " RL_START "\033[1;37m" RL_END, z);
    }else{
      snprintf(prompt, sizeof(prompt), RL_START "\033[0;35m" RL_END
          "[" RL_START "\033[0;36m" RL_END
          PACKAGE RL_START "\033[0;35m" RL_END
          "]" RL_START "\033[1;32m" RL_END
          "(0)" RL_START "\033[1;32m" RL_END
          "> " RL_START "\033[1;37m" RL_END);
    }
    if(lights_off){
      return 0;
    }
  }
  printf("\n");
  return 0;
}
#undef RL_END
#undef RL_START

static void
vdiag(const char *fmt, va_list va){
  vfprintf(stderr, fmt, va);
  raise(SIGWINCH); // get prompt reprinted
}

/*static void
diag(const char *fmt, ...){
  va_list va;

  va_start(va, fmt);
  vdiag(fmt, va);
  va_end(va);
}*/

static void *
block_event(device *d, void *v){
  assert(d);
  (void)d;
  return v;
}

static void *new_adapter(controller *c, void *v){ (void)c; return v; }
static void adapter_free(void *cv){ (void)cv; }
static void block_free(void *cv, void *bv){ (void)cv; (void)bv; }

static void
vinfo(const char *text, ...){
  va_list v;

  fprintf(stdout, "\n");
  va_start(v, text);
  vfprintf(stdout, text, v);
  va_end(v);
  fprintf(stdout, "\n\n");
}

int main(int argc, char * const *argv){
  const glightui ui = {
    .vdiag = vdiag,
    .boxinfo = vinfo,
    .adapter_event = new_adapter,
    .block_event = block_event,
    .adapter_free = adapter_free,
    .block_free = block_free,
  };

  if(setlocale(LC_ALL, "") == NULL){
    fprintf(stderr, "Warning: couldn't load locale\n");
    //return EXIT_FAILURE;
  }
  if(growlight_init(argc, argv, &ui, NULL)){
    return EXIT_FAILURE;
  }
  if(isatty(STDOUT_FILENO)){
    const uint64_t flags = NCDIRECT_OPTION_INHIBIT_SETLOCALE;
    if((ncd = ncdirect_init(NULL, NULL, flags)) == NULL){
      fprintf(stderr, "Couldn't set up notcurses\n");
      growlight_stop();
      return EXIT_FAILURE;
    }
  }
  if(tty_ui()){
    growlight_stop();
    ncdirect_stop(ncd);
    return EXIT_FAILURE;
  }
  if(growlight_stop()){
    ncdirect_stop(ncd);
    return EXIT_FAILURE;
  }
  if(ncdirect_stop(ncd)){
    fprintf(stderr, "Couldn't reset terminal\n");
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
