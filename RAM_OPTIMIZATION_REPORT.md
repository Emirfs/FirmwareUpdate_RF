# RAM/Flash Optimization Report (Bootloader)

## Scope
- Target: `alici_cihaz` bootloader build (`Debug`)
- Goal: reduce RAM usage first, remove unused components safely.

## Baseline (Before)
- RAM (from `arm-none-eabi-size`): `data=12`, `bss=5936`
- FLASH (from `arm-none-eabi-size`): `text=28024`, `data=12`
- Total reserved RAM by linker-relevant sections:
  - `.data`: 12
  - `.bss`: 816
  - `._user_heap_stack`: 5120

## Changes Applied
1. Removed unused TIM/RTC init calls from boot path.
   - File: `alici_cihaz/Core/Src/main.c`
   - Removed: `MX_TIM17_Init`, `MX_TIM6_Init`, `MX_TIM16_Init`, `MX_TIM3_Init`, `MX_RTC_Init`
2. Removed TIM17 HAL IRQ dependency (kept empty handler).
   - File: `alici_cihaz/Core/Src/stm32f0xx_it.c`
   - Removed: `extern TIM_HandleTypeDef htim17`, `HAL_TIM_IRQHandler(&htim17)`
3. Reduced firmware assembly buffer to exact protocol size.
   - File: `alici_cihaz/Core/Src/boot_flow.c`
   - `fw_assembly_buf[200]` -> `fw_assembly_buf[FW_FULL_PACKET_SIZE]` (148 bytes)
4. Reduced linker reserved heap/stack.
   - File: `alici_cihaz/STM32F030CCTX_FLASH.ld`
   - `_Min_Heap_Size: 0x800 -> 0x0`
   - `_Min_Stack_Size: 0xC00 -> 0x800`

## Result (After)
- RAM: `data=12`, `bss=2488`
- FLASH: `text=23936`, `data=12`

## Delta
- RAM saved: `5936 - 2488 = 3448 bytes`
- FLASH saved: `28024 - 23936 = 4088 bytes`

## RAM Breakdown After
- `.data`: 12
- `.bss`: 440
- `._user_heap_stack`: 2048

Top `.bss` symbols after optimization:
- `fw_assembly_buf`: 148
- `hspi2`: 100
- `rf_rx_buf`: 64
- `AES_KEY`: 32
- `pFlash`: 32
- `object.0`: 24
- `hiwdg`: 16

Removed from final binary RAM footprint:
- `htim3`, `htim6`, `htim16`, `htim17` (4 x 72)
- `hrtc` (32)

## Notes
- `Bootloader_Main` stack usage report (`.su`) shows 1008 bytes static usage; 2KB stack reserve keeps safety margin.
- `make all` in this shell may return a non-fatal `Error -1` on secondary output step, but `alıcı_cihaz.elf` is produced and size measurements above are from that ELF.

## Next Candidate Optimizations
1. Move build profile from `-O0` to `-Os`/`-O2` for production.
2. If status LED is not required in production, trim NeoPixel path to save additional RAM/FLASH.
3. Re-check stack margin on real hardware under worst-case RF/update path and lower `_Min_Stack_Size` only if safe.
