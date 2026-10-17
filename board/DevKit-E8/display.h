#ifndef DISPLAY_H_
#define DISPLAY_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the MIPI-DSI panel scanning out the frame buffer. Returns 0 on success. */
int display_init(void);

/* Frame buffer: height rows of width pixels, bytes_per_pixel each (RGB order of the panel). */
uint8_t *display_buffer(void);
int display_width(void);
int display_height(void);
int display_bytes_per_pixel(void);

/* Push CPU writes out of the data cache so the display controller sees them. */
void display_flush(void);

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_H_ */
