#ifndef GROWLIGHT_SWAP
#define GROWLIGHT_SWAP

#ifdef __cplusplus
extern "C" {
#endif

struct device;

// Create swap on the device, and use it
int swapondev(struct device *);

// Deactive the swap on this partition (if applicable)
int swapoffdev(struct device *);

// Parse /proc/swaps to detect active swap devices
int parse_swaps(void);

#ifdef __cplusplus
}
#endif

#endif
