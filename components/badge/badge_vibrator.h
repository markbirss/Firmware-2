/** @file badge_vibrator.h */
#ifndef BADGE_VIBRATOR_H
#define BADGE_VIBRATOR_H

#include <stdint.h>

/**
 * Initialize vibrator driver. (GPIO ports)
 */
extern void badge_vibrator_init(void);

/**
 * Send bit-pattern to the vibrator.
 * @note Every bit takes approx. 200ms. Lowest bit is used first.
 *
 * Code example:
 *
 *   badge_vibrator_activate(0xd);
 *   // vibrator will be on for 200ms. Then off for 200ms. Then on for 400ms.
 */
extern void badge_vibrator_activate(uint32_t pattern);

#endif // BADGE_VIBRATOR_H
