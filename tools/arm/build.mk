# Kommandozeilen-Build der Firmware (alternativ zur STM32CubeIDE)
#   make -C tools/arm          -> build/LUCEmbedded.elf/.bin/.hex
ROOT    := ../..
BUILD   := build
TARGET  := LUCEmbedded
PREFIX  ?= arm-none-eabi-
CC      := $(PREFIX)gcc
OBJCOPY := $(PREFIX)objcopy
SIZE    := $(PREFIX)size

CPU     := -mcpu=cortex-m0 -mthumb -mfloat-abi=soft
DEFS    := -DUSE_HAL_DRIVER -DSTM32F042x6
INCS    := -I$(ROOT)/Inc \
           -I$(ROOT)/Drivers/STM32F0xx_HAL_Driver/Inc \
           -I$(ROOT)/Drivers/STM32F0xx_HAL_Driver/Inc/Legacy \
           -I$(ROOT)/Middlewares/ST/STM32_USB_Device_Library/Core/Inc \
           -I$(ROOT)/Middlewares/ST/STM32_USB_Device_Library/Class/CDC/Inc \
           -I$(ROOT)/Drivers/CMSIS/Device/ST/STM32F0xx/Include \
           -I$(ROOT)/Drivers/CMSIS/Include
CFLAGS  := $(CPU) $(DEFS) $(INCS) -std=gnu11 -Os -g3 -Wall -ffunction-sections -fdata-sections -fstack-usage
LDFLAGS := $(CPU) -T$(ROOT)/STM32F042G6UX_FLASH.ld --specs=nano.specs --specs=nosys.specs \
           -Wl,--gc-sections -Wl,-Map=$(BUILD)/$(TARGET).map -Wl,--print-memory-usage

SRCS := $(filter-out $(ROOT)/Src/slcan.c $(ROOT)/Src/lin_slcan.c,$(wildcard $(ROOT)/Src/*.c)) \
        $(wildcard $(ROOT)/Drivers/STM32F0xx_HAL_Driver/Src/*.c) \
        $(wildcard $(ROOT)/Middlewares/ST/STM32_USB_Device_Library/Core/Src/*.c) \
        $(wildcard $(ROOT)/Middlewares/ST/STM32_USB_Device_Library/Class/CDC/Src/*.c)
ASMS := $(ROOT)/Startup/startup_stm32f042g6ux.s
OBJS := $(addprefix $(BUILD)/,$(notdir $(SRCS:.c=.o) $(ASMS:.s=.o)))
vpath %.c $(sort $(dir $(SRCS)))
vpath %.s $(dir $(ASMS))

all: $(BUILD)/$(TARGET).bin $(BUILD)/$(TARGET).hex
	$(SIZE) $(BUILD)/$(TARGET).elf

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/%.o: %.s | $(BUILD)
	$(CC) $(CPU) -x assembler-with-cpp -c $< -o $@
$(BUILD)/$(TARGET).elf: $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) -o $@
$(BUILD)/%.bin: $(BUILD)/%.elf
	$(OBJCOPY) -O binary $< $@
$(BUILD)/%.hex: $(BUILD)/%.elf
	$(OBJCOPY) -O ihex $< $@
$(BUILD):
	mkdir -p $@
clean:
	rm -rf $(BUILD)
.PHONY: all clean
