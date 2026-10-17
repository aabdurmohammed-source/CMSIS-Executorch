#ifndef MIC_H_
#define MIC_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Starts background capture from the on-board PDM mics. Returns 0 on success. */
int mic_init(void);

/* Non-blocking. 1 = a new block arrived and *level holds its RMS loudness
 * (int16 sample units), 0 = nothing new yet, -1 = the block had an error. */
int mic_poll(float *level);

uint32_t mic_block_count(void);

#ifdef __cplusplus
}
#endif

#endif /* MIC_H_ */
