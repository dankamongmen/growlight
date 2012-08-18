#include "growlight.h"
#include "aggregate.h"

static const aggregate_type aggregates[] = {
	{
		.name = "mdlinear",
		.desc = "Linear disk combination (MD)",
	},{
		.name = "mdraid0",
		.desc = "Interleaved disk combination (striping) (MD)",
	},{
		.name = "mdraid1",
		.desc = "Mirroring (MD)",
	},{
		.name = "mdraid4",
		.desc = "Block striping with dedicated parity",
	},{
		.name = "mdraid5",
		.desc = "Block striping with distributed parity",
	},{
		.name = "mdraid6",
		.desc = "Block striping with dual distributed parity",
	},{
		.name = "mdraid10",
		.desc = "Interleaved mirror combination",
	},{
		.name = "zmirror",
		.desc = "Zpool with data replication (mirroring)",
	},{
		.name = "raidz1",
		.desc = "ZFS RAID with single parity",
	},{
		.name = "raidz2",
		.desc = "ZFS RAID with double parity",
	},{
		.name = "raidz3",
		.desc = "ZFS RAID with triple parity",
	},{
		.name = "zil",
		.desc = "ZFS Write-Intent Log",
	},{
		.name = "l2arc",
		.desc = "ZFS Level 2 Adaptive Replacement Cache",
	},{
		.name = "dmlinear",
		.desc = "Linear disk combination (DM)",
	},{
		.name = "dmstriped",
		.desc = "Interleaved disk combination (striping) (DM)",
	},{
		.name = "crypt",
		.desc = "Block encryption",
	},{
		.name = "dmmirror",
		.desc = "Mirroring (DM)",
	}
};

const aggregate_type *get_aggregate_types(int *count){
	*count = sizeof(aggregates) / sizeof(*aggregates);
	return aggregates;
}
