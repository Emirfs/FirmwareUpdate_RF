# Flash Usage Report (Bootloader)

## Build Context
- Target: `alici_cihaz/Debug/alici_cihaz.elf`
- Tool outputs used:
  - `arm-none-eabi-size`
  - `arm-none-eabi-size -A`
  - `alici_cihaz/Debug/alici_cihaz.map`
  - `arm-none-eabi-nm --print-size --size-sort --radix=d`

## Flash Capacity and Occupancy
- Flash region (linker/map):
  - Origin: `0x08000000`
  - End: `0x08008000`
  - Capacity: `32768` bytes (`32 KB`)
- Used end address: `0x08005D8C`
- Used bytes: `23948`
- Free bytes: `8820`
- Utilization: `73.08%`

## Section Breakdown (Flash-resident)
- `.isr_vector`: `188`
- `.text`: `22800`
- `.rodata`: `940`
- `.init_array`: `4`
- `.fini_array`: `4`
- `.data` load image in flash: `12`
- Total flash occupancy: `23948`

## Top Flash Consumers (Functions)
- `3984` - `Bootloader_Main` (`16.64%` of used flash)
- `1596` - `HAL_RCC_OscConfig` (`6.66%`)
- `1168` - `InvCipher` (`4.88%`)
- `972` - `sha256_block` (`4.06%`)
- `964` - `HAL_SPI_TransmitReceive` (`4.03%`)
- `736` - `HAL_GPIO_Init` (`3.07%`)
- `702` - `HAL_SPI_Transmit` (`2.93%`)
- `676` - `HAL_SPI_Receive` (`2.82%`)
- `544` - `KeyExpansion` (`2.27%`)
- `452` - `SI4432_Init` (`1.89%`)
- `412` - `HAL_RCC_ClockConfig` (`1.72%`)
- `412` - `HAL_RCCEx_PeriphCLKConfig` (`1.72%`)
- `408` - `MX_GPIO_Init` (`1.70%`)
- `372` - `SHA256_Final` (`1.55%`)
- `368` - `HAL_SPI_Init` (`1.54%`)

## Top Read-Only Data (`.rodata`)
- `256` - `K`
- `256` - `rsbox`
- `256` - `sbox`
- `32` - `DEFAULT_AUTH_KEY`
- `32` - `HMAC_MAC_KEY`
- `32` - `DEFAULT_AES_KEY`
- `16` - `AHBPrescTable`
- `16` - `DEFAULT_AUTH_PASSWORD`

## Notes
- The heaviest part is still bootloader control flow + HAL clock/SPI + crypto.
- `TIM/RTC/I2C` symbol families are not present in the final linked flash image after recent cleanup, so they are effectively `0` in this binary.
