#ifndef GROWLIGHT_GPT
#define GROWLIGHT_GPT

#ifdef __cplusplus
extern "C" {
#endif

struct device;

int new_gpt(struct device *d);
int zap_gpt(struct device *d);

#ifdef __cplusplus
}
#endif

#endif
