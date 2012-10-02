#include "zfs.h"
#include "mdadm.h"
#include "popen.h"
#include "crypt.h"
#include "growlight.h"
#include "aggregate.h"

static int
make_crypt(const char *name __attribute__ ((unused)),char * const *argv,int argc){
	device *d;

	if(argc != 1){
		diag("Wrong number of devices (%d != 1) for LUKS\n",argc);
		return -1;
	}
	if((d = lookup_device(*argv)) == NULL){
		return -1;
	}
	return cryptondev(d);
}

static const aggregate_type aggregates[] = {
	{
		.name = "mdlinear",
		.desc = "Linear disk combination (MD)",
		.mindisks = 2,
		.maxfaulted = -1,
	},{
		.name = "mdddf",
		.desc = "SNIA Data Disk Format container",
		.mindisks = 2,
		.maxfaulted = 0,
	},{
		.name = "mdimsm",
		.desc = "Intel® Matrix Storage Manager container",
		.mindisks = 2,
		.maxfaulted = 0,
	},{
		.name = "mdcontain",
		.desc = "Linear disk combination with metadata",
		.mindisks = 2,
		.maxfaulted = 0,
	},{
		.name = "mdraid0",
		.desc = "Interleaved disk combination (striping) (MD)",
		.mindisks = 2,
		.maxfaulted = 0,
		.makeagg = make_mdraid0,
		.defname = "SprezzaRAID0",
	},{
		.name = "mdraid1",
		.desc = "Mirroring (MD)",
		.mindisks = 2,
		.maxfaulted = 1,
		.makeagg = make_mdraid1,
		.defname = "SprezzaRAID1",
	},{
		.name = "mdraid4",
		.desc = "Block striping with dedicated parity",
		.mindisks = 3,
		.maxfaulted = 1,
		.makeagg = make_mdraid4,
		.defname = "SprezzaRAID4",
	},{
		.name = "mdraid5",
		.desc = "Block striping with distributed parity",
		.mindisks = 3,
		.maxfaulted = 1,
		.makeagg = make_mdraid5,
		.defname = "SprezzaRAID5",
	},{
		.name = "mdraid6",
		.desc = "Block striping with 2x distributed parity",
		.mindisks = 4,
		.maxfaulted = 2,
		.makeagg = make_mdraid6,
		.defname = "SprezzaRAID6",
	},{
		.name = "mdraid10",
		.desc = "Interleaved mirror combination",
		.mindisks = 4,
		.maxfaulted = 1, // FIXME technically 1 from each mirror
		.makeagg = make_mdraid10,
		.defname = "SprezzaRAID10",
	},{
		.name = "zmirror",
		.desc = "Zpool with data replication (mirroring)",
		.mindisks = 2,
		.maxfaulted = 0,
		.makeagg = make_zmirror,
		.defname = "SprezZMirror",
	},{
		.name = "raidz1",
		.desc = "ZFS RAID with distributed parity",
		.mindisks = 3,
		.maxfaulted = 0,
		.makeagg = make_raidz1,
		.defname = "SprezZRAID",
	},{
		.name = "raidz2",
		.desc = "ZFS RAID with 2x distributed parity",
		.mindisks = 4,
		.maxfaulted = 0,
		.makeagg = make_raidz2,
		.defname = "SprezZRAID2",
	},{
		.name = "raidz3",
		.desc = "ZFS RAID with 3x distributed parity",
		.mindisks = 5,
		.maxfaulted = 0,
		.makeagg = make_raidz3,
		.defname = "SprezZRAID3",
	},{
		.name = "zil",
		.desc = "ZFS Write-Intent Log",
		.mindisks = 1,
		.maxfaulted = 0,
	},{
		.name = "l2arc",
		.desc = "ZFS Level 2 Adaptive Replacement Cache",
		.mindisks = 1,
		.maxfaulted = 0,
	},{
		.name = "dmlinear",
		.desc = "Linear disk combination (DM)",
		.mindisks = 2,
		// .maxfaulted // FIXME need investigate
	},{
		.name = "dmstriped",
		.desc = "Interleaved disk combination (striping) (DM)",
		.mindisks = 2,
		.maxfaulted = 0,
	},{
		.name = "dmcrypt",
		.desc = "LUKS block encryption (DM)",
		.mindisks = 1,
		.maxfaulted = 0,
		.makeagg = make_crypt,
	},{
		.name = "dmmirror",
		.desc = "Mirroring (DM)",
		.mindisks = 2,
		.maxfaulted = 1,
	}
};

const aggregate_type *get_aggregate(const char *name){
	unsigned z;

	for(z = 0 ; z < sizeof(aggregates) / sizeof(*aggregates) ; ++z){
		if(strcmp(aggregates[z].name,name) == 0){
			return &aggregates[z];
		}
	}
	return NULL;
}

const aggregate_type *get_aggregate_types(int *count){
	*count = sizeof(aggregates) / sizeof(*aggregates);
	return aggregates;
}

int assemble_aggregates(void){
	int zpool = 0,mdraid = 0;
	const controller *c;

	// FIXME maybe only do it if there are both aggregable devices and
	// they're not currently slaves of anything?
	for(c = get_controllers() ; c ; c = c->next){
		const device *d;

		for(d = c->blockdevs ; d ; d = d->next){
			const device *p;

			if(d->mnttype){
				if(zpool_p(d->mnttype)){
					zpool = 1;
				}else if(mdraid_p(d->mnttype)){
					mdraid = 1;
				}
			}
			for(p = d->parts ; p ; p = p->next){
				if(p->mnttype){
					if(zpool_p(p->mnttype)){
						zpool = 1;
					}else if(mdraid_p(p->mnttype)){
						mdraid = 1;
					}
				}
			}
		}
	}
	if(zpool){
		diag("Scanning for zpools...\n");
		vspopen_drain("zpool import -a");
	}
	if(mdraid){
		diag("Scanning for MD devices...\n");
		vspopen_drain("mdadm --assemble --scan");
	}
	return 0;
}
