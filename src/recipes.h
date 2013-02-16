#ifndef GROWLIGHT_RECIPES
#define GROWLIGHT_RECIPES

#ifdef __cplusplus
extern "C" {
#endif

struct device;

typedef struct recipe {
	const char *summary;	// single-word summary
	const char *longdesc;	// longer description
	struct recipe *next;
} recipe;

// Get a list of the supported recipes' descriptions
const recipe *get_recipes(void);

// Apply a recipe to a device. It cannot have a partition table or a filesystem
// signature. The string must match up with a supported recipe's summary.
int apply_recipe(struct device *,const char *);

#ifdef __cplusplus
}
#endif

#endif
