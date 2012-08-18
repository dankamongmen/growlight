#include "growlight.h"
#include "aggregate.h"

static const aggregate_type aggregates[] = {
	{
		.name = "linear",
		.desc = "Linear disk combination",
	},{
		.name = "raid0",
		.desc = "Interleaved disk combination (striping)",
	},{
		.name = "raid1",
		.desc = "Mirroring",
	},{
		.name = "raid4",
		.desc = "Block striping with dedicated parity",
	},{
		.name = "raid5",
		.desc = "Block striping with distributed parity",
	},{
		.name = "raid6",
		.desc = "Block striping with dual distributed parity",
	},{
		.name = "raid10",
		.desc = "Interleaved mirror combination",
	},
};

const aggregate_type *get_aggregate_types(int *count){
	*count = sizeof(aggregates) / sizeof(*aggregates);
	return aggregates;
}
