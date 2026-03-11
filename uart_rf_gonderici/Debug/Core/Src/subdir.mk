################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (13.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../Core/Src/c25519.c \
../Core/Src/dma.c \
../Core/Src/entropy.c \
../Core/Src/f25519.c \
../Core/Src/gpio.c \
../Core/Src/iwdg.c \
../Core/Src/main.c \
../Core/Src/sender_fw_update.c \
../Core/Src/sender_normal_mode.c \
../Core/Src/sender_rf_link.c \
../Core/Src/sender_state.c \
../Core/Src/sender_uart_debug.c \
../Core/Src/si4432.c \
../Core/Src/spi.c \
../Core/Src/stm32f0xx_hal_msp.c \
../Core/Src/stm32f0xx_it.c \
../Core/Src/syscalls.c \
../Core/Src/sysmem.c \
../Core/Src/system_stm32f0xx.c \
../Core/Src/tim.c \
../Core/Src/usart.c 

OBJS += \
./Core/Src/c25519.o \
./Core/Src/dma.o \
./Core/Src/entropy.o \
./Core/Src/f25519.o \
./Core/Src/gpio.o \
./Core/Src/iwdg.o \
./Core/Src/main.o \
./Core/Src/sender_fw_update.o \
./Core/Src/sender_normal_mode.o \
./Core/Src/sender_rf_link.o \
./Core/Src/sender_state.o \
./Core/Src/sender_uart_debug.o \
./Core/Src/si4432.o \
./Core/Src/spi.o \
./Core/Src/stm32f0xx_hal_msp.o \
./Core/Src/stm32f0xx_it.o \
./Core/Src/syscalls.o \
./Core/Src/sysmem.o \
./Core/Src/system_stm32f0xx.o \
./Core/Src/tim.o \
./Core/Src/usart.o 

C_DEPS += \
./Core/Src/c25519.d \
./Core/Src/dma.d \
./Core/Src/entropy.d \
./Core/Src/f25519.d \
./Core/Src/gpio.d \
./Core/Src/iwdg.d \
./Core/Src/main.d \
./Core/Src/sender_fw_update.d \
./Core/Src/sender_normal_mode.d \
./Core/Src/sender_rf_link.d \
./Core/Src/sender_state.d \
./Core/Src/sender_uart_debug.d \
./Core/Src/si4432.d \
./Core/Src/spi.d \
./Core/Src/stm32f0xx_hal_msp.d \
./Core/Src/stm32f0xx_it.d \
./Core/Src/syscalls.d \
./Core/Src/sysmem.d \
./Core/Src/system_stm32f0xx.d \
./Core/Src/tim.d \
./Core/Src/usart.d 


# Each subdirectory must supply rules for building sources it contributes
Core/Src/%.o Core/Src/%.su Core/Src/%.cyclo: ../Core/Src/%.c Core/Src/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m0 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32F030x8 -c -I../Core/Inc -I../Drivers/STM32F0xx_HAL_Driver/Inc -I../Drivers/STM32F0xx_HAL_Driver/Inc/Legacy -I../Drivers/CMSIS/Device/ST/STM32F0xx/Include -I../Drivers/CMSIS/Include -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfloat-abi=soft -mthumb -o "$@"

clean: clean-Core-2f-Src

clean-Core-2f-Src:
	-$(RM) ./Core/Src/c25519.cyclo ./Core/Src/c25519.d ./Core/Src/c25519.o ./Core/Src/c25519.su ./Core/Src/dma.cyclo ./Core/Src/dma.d ./Core/Src/dma.o ./Core/Src/dma.su ./Core/Src/entropy.cyclo ./Core/Src/entropy.d ./Core/Src/entropy.o ./Core/Src/entropy.su ./Core/Src/f25519.cyclo ./Core/Src/f25519.d ./Core/Src/f25519.o ./Core/Src/f25519.su ./Core/Src/gpio.cyclo ./Core/Src/gpio.d ./Core/Src/gpio.o ./Core/Src/gpio.su ./Core/Src/iwdg.cyclo ./Core/Src/iwdg.d ./Core/Src/iwdg.o ./Core/Src/iwdg.su ./Core/Src/main.cyclo ./Core/Src/main.d ./Core/Src/main.o ./Core/Src/main.su ./Core/Src/sender_fw_update.cyclo ./Core/Src/sender_fw_update.d ./Core/Src/sender_fw_update.o ./Core/Src/sender_fw_update.su ./Core/Src/sender_normal_mode.cyclo ./Core/Src/sender_normal_mode.d ./Core/Src/sender_normal_mode.o ./Core/Src/sender_normal_mode.su ./Core/Src/sender_rf_link.cyclo ./Core/Src/sender_rf_link.d ./Core/Src/sender_rf_link.o ./Core/Src/sender_rf_link.su ./Core/Src/sender_state.cyclo ./Core/Src/sender_state.d ./Core/Src/sender_state.o ./Core/Src/sender_state.su ./Core/Src/sender_uart_debug.cyclo ./Core/Src/sender_uart_debug.d ./Core/Src/sender_uart_debug.o ./Core/Src/sender_uart_debug.su ./Core/Src/si4432.cyclo ./Core/Src/si4432.d ./Core/Src/si4432.o ./Core/Src/si4432.su ./Core/Src/spi.cyclo ./Core/Src/spi.d ./Core/Src/spi.o ./Core/Src/spi.su ./Core/Src/stm32f0xx_hal_msp.cyclo ./Core/Src/stm32f0xx_hal_msp.d ./Core/Src/stm32f0xx_hal_msp.o ./Core/Src/stm32f0xx_hal_msp.su ./Core/Src/stm32f0xx_it.cyclo ./Core/Src/stm32f0xx_it.d ./Core/Src/stm32f0xx_it.o ./Core/Src/stm32f0xx_it.su ./Core/Src/syscalls.cyclo ./Core/Src/syscalls.d ./Core/Src/syscalls.o ./Core/Src/syscalls.su ./Core/Src/sysmem.cyclo ./Core/Src/sysmem.d ./Core/Src/sysmem.o ./Core/Src/sysmem.su ./Core/Src/system_stm32f0xx.cyclo ./Core/Src/system_stm32f0xx.d ./Core/Src/system_stm32f0xx.o ./Core/Src/system_stm32f0xx.su ./Core/Src/tim.cyclo ./Core/Src/tim.d ./Core/Src/tim.o ./Core/Src/tim.su ./Core/Src/usart.cyclo ./Core/Src/usart.d ./Core/Src/usart.o ./Core/Src/usart.su

.PHONY: clean-Core-2f-Src

