#ifndef CAMERA_H_
#define CAMERA_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the MT9M114 (MIPI CSI2) through the CPI driver. Returns 0 on success. */
int camera_init(void);

/* Captures one frame into the camera buffer. 0 = ok, 1 = frame with error events,
 * negative = failed / timed out. Needs the DWT cycle counter running. */
int camera_capture(void);

uint32_t camera_last_events(void);

/* RGB565 frame, width x height pixels, 2 bytes each */
const uint8_t *camera_frame(void);
int camera_width(void);
int camera_height(void);

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_H_ */
