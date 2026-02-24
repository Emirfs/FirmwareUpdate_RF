/**
 * @file    neopixel.c
 * @brief   WS2812 NeoPixel LED driver – bit-bang implementation for STM32F0 @
 * 48 MHz
 *
 *   "0" bit: T0H ~0.40 us HIGH,  T0L ~0.85 us LOW
 *   "1" bit: T1H ~0.80 us HIGH,  T1L ~0.45 us LOW
 *   Reset :  >50 us LOW
 *
 * At 48 MHz each CPU cycle is 20.83 ns.
 *   T0H ≈ 19 cycles,  T0L ≈ 41 cycles
 *   T1H ≈ 38 cycles,  T1L ≈ 22 cycles
 */

#include "neopixel.h"

#pragma GCC optimize("O3")

/* ---------- Pixel buffer (GRB order, MSB first) ---------- */
static uint8_t pixel_buf[NEOPIXEL_NUM_LEDS * 3]; /* G-R-B per LED */

/* ---------- Low-level helpers ---------- */

/* Force the compiler to keep these as real inline NOPs */
#define NOP1 __asm volatile("nop")
#define NOP4                                                                   \
  do {                                                                         \
    NOP1;                                                                      \
    NOP1;                                                                      \
    NOP1;                                                                      \
    NOP1;                                                                      \
  } while (0)
#define NOP10                                                                  \
  do {                                                                         \
    NOP4;                                                                      \
    NOP4;                                                                      \
    NOP1;                                                                      \
    NOP1;                                                                      \
  } while (0)

/**
 * Send a single byte (MSB first) over the NeoPixel data line.
 * Must be called with interrupts disabled.
 */
static void neopixel_send_byte(uint8_t byte) {
  /* Use BSRR for single-cycle set/reset on Cortex-M0            */
  /* GPIO BSRR: lower 16 bits = SET, upper 16 bits = RESET       */
  volatile uint32_t *bsrr = &NEOPIXEL_PORT->BSRR;
  uint32_t pin_set = (uint32_t)NEOPIXEL_PIN;         /* set   */
  uint32_t pin_reset = (uint32_t)NEOPIXEL_PIN << 16; /* reset */

  for (int8_t bit = 7; bit >= 0; bit--) {
    if (byte & (1 << bit)) {
      /* ---- Send "1" bit ---- */
      *bsrr = pin_set;
      /* T1H ≈ 0.80 µs → ~38 cycles (subtract overhead ≈ 8) → 30 NOPs */
      NOP10;
      NOP10;
      NOP10;

      *bsrr = pin_reset;
      /* T1L ≈ 0.45 µs → ~22 cycles (subtract overhead ≈ 8) → 14 NOPs */
      NOP10;
      NOP4;
    } else {
      /* ---- Send "0" bit ---- */
      *bsrr = pin_set;
      /* T0H ≈ 0.40 µs → ~19 cycles (subtract overhead ≈ 8) → 11 NOPs */
      NOP10;
      NOP1;

      *bsrr = pin_reset;
      /* T0L ≈ 0.85 µs → ~41 cycles (subtract overhead ≈ 8) → 33 NOPs */
      NOP10;
      NOP10;
      NOP10;
      NOP1;
      NOP1;
      NOP1;
    }
  }
}

/* ---------- Public API ---------- */

void NeoPixel_Init(void) {
  GPIO_InitTypeDef gpio = {0};
  gpio.Pin = NEOPIXEL_PIN;
  gpio.Mode = GPIO_MODE_OUTPUT_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(NEOPIXEL_PORT, &gpio);

  /* Make sure pin starts LOW (reset state for WS2812). */
  HAL_GPIO_WritePin(NEOPIXEL_PORT, NEOPIXEL_PIN, GPIO_PIN_RESET);

  /* Clear the buffer and push to LEDs */
  NeoPixel_Clear();
  NeoPixel_Show();
}

void NeoPixel_SetColor(uint8_t led, uint8_t r, uint8_t g, uint8_t b) {
  if (led >= NEOPIXEL_NUM_LEDS)
    return;
  uint16_t idx = (uint16_t)led * 3;
  pixel_buf[idx + 0] = g; /* WS2812 expects GRB order */
  pixel_buf[idx + 1] = r;
  pixel_buf[idx + 2] = b;
}

void NeoPixel_SetAll(uint8_t r, uint8_t g, uint8_t b) {
  for (uint8_t i = 0; i < NEOPIXEL_NUM_LEDS; i++)
    NeoPixel_SetColor(i, r, g, b);
}

void NeoPixel_Clear(void) {
  for (uint8_t i = 0; i < sizeof(pixel_buf); i++)
    pixel_buf[i] = 0;
}

void NeoPixel_Show(void) {
  __disable_irq();

  for (uint8_t i = 0; i < sizeof(pixel_buf); i++)
    neopixel_send_byte(pixel_buf[i]);

  __enable_irq();

  /* Reset pulse: data line LOW for ≥ 50 µs.
   * Pin is already LOW after last bit; just wait.  */
  /* Use a rough loop: 50 µs × 48 MHz = 2400 cycles */
  for (volatile uint32_t t = 0; t < 800; t++)
    __asm volatile("nop");
}
