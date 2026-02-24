/**
 * @file    neopixel.h
 * @brief   WS2812 NeoPixel LED driver for STM32F0 (bit-bang on PB0)
 */
#ifndef NEOPIXEL_H
#define NEOPIXEL_H

#include "main.h"
#include <stdint.h>

/* ---------- Configuration ---------- */
#define NEOPIXEL_NUM_LEDS 3
#define NEOPIXEL_PIN NEOPIXEL_BTN_Pin
#define NEOPIXEL_PORT NEOPIXEL_BTN_GPIO_Port

/* ---------- Public API ---------- */

/**
 * @brief  Initialize the NeoPixel data pin and clear all LEDs.
 */
void NeoPixel_Init(void);

/**
 * @brief  Set the color of a single LED (buffered, call NeoPixel_Show to
 * apply).
 * @param  led  LED index (0 .. NEOPIXEL_NUM_LEDS-1)
 * @param  r    Red   (0-255)
 * @param  g    Green (0-255)
 * @param  b    Blue  (0-255)
 */
void NeoPixel_SetColor(uint8_t led, uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief  Set all LEDs to the same color (buffered).
 */
void NeoPixel_SetAll(uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief  Turn off all LEDs (buffered).
 */
void NeoPixel_Clear(void);

/**
 * @brief  Push the color buffer to the LED strip.
 *         Disables interrupts briefly for bit-bang timing.
 */
void NeoPixel_Show(void);

#endif /* NEOPIXEL_H */
