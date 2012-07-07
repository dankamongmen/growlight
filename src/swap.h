#ifndef GROWLIGHT_SWAP
#define GROWLIGHT_SWAP

#ifdef __cplusplus
extern "C" {
#endif

struct device;
struct growlight_ui;

// Create swap on the device, and use it
int swapondev(struct device *);

// Deactive the swap on this partition (if applicable)
int swapoffdev(struct device *);

// Parse /proc/swaps to detect active swap devices
int parse_swaps(const struct growlight_ui *,const char *);

#ifdef __cplusplus
}
#endif

#endif
