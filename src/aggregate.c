#include "growlight.h"
#include "aggregate.h"

static const aggregate_type aggregates[] = {
	{
		.name = "mdlinear",
		.desc = "Linear disk combination (MD)",
		.mindisks = 2,
	},{
		.name = "mdraid0",
		.desc = "Interleaved disk combination (striping) (MD)",
		.mindisks = 2,
	},{
		.name = "mdraid1",
		.desc = "Mirroring (MD)",
		.mindisks = 2,
	},{
		.name = "mdraid4",
		.desc = "Block striping with dedicated parity",
		.mindisks = 3,
	},{
		.name = "mdraid5",
		.desc = "Block striping with distributed parity",
		.mindisks = 3,
	},{
		.name = "mdraid6",
		.desc = "Block striping with dual distributed parity",
		.mindisks = 4,
	},{
		.name = "mdraid10",
		.desc = "Interleaved mirror combination",
		.mindisks = 4,
	},{
		.name = "zmirror",
		.desc = "Zpool with data replication (mirroring)",
		.mindisks = 2,
	},{
		.name = "raidz1",
		.desc = "ZFS RAID with single parity",
		.mindisks = 3,
	},{
		.name = "raidz2",
		.desc = "ZFS RAID with double parity",
		.mindisks = 4,
	},{
		.name = "raidz3",
		.desc = "ZFS RAID with triple parity",
		.mindisks = 5,
	},{
		.name = "zil",
		.desc = "ZFS Write-Intent Log",
		.mindisks = 1,
	},{
		.name = "l2arc",
		.desc = "ZFS Level 2 Adaptive Replacement Cache",
		.mindisks = 1,
	},{
		.name = "dmlinear",
		.desc = "Linear disk combination (DM)",
		.mindisks = 2,
	},{
		.name = "dmstriped",
		.desc = "Interleaved disk combination (striping) (DM)",
		.mindisks = 2,
	},{
		.name = "crypt",
		.desc = "Block encryption",
		.mindisks = 1,
	},{
		.name = "dmmirror",
		.desc = "Mirroring (DM)",
		.mindisks = 2,
	}
};

const aggregate_type *get_aggregate_types(int *count){
	*count = sizeof(aggregates) / sizeof(*aggregates);
	return aggregates;
}
