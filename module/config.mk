# project name
THIS = orca

# Path to top level ASF directory relative to this project directory.
PRJ_PATH = ../libavr32/asf

# Target CPU architecture: ap, ucr1, ucr2 or ucr3
ARCH = ucr2

# Target part: none, ap7xxx or uc3xxxxx
PART = uc3b0512

# Target device flash memory details
FLASH = internal@0x80000000

# Clock source to use when programming; xtal, extclk or int
PROG_CLOCK = int

# Application target name
TARGET = $(THIS).elf

# List of C source files.
CSRCS = \
	../module/main.c					\
	../src/gbuffer.c					\
	../src/sim.c						\
	../src/vmio.c						\
	../src/syscalls.c					\
	../libavr32/src/adc.c					\
	../libavr32/src/events.c				\
	../libavr32/src/fix.c					\
	../libavr32/src/font.c					\
	../libavr32/src/i2c.c					\
	../libavr32/src/init_teletype.c				\
	../libavr32/src/init_common.c				\
	../libavr32/src/region.c				\
	../libavr32/src/screen.c				\
	../libavr32/src/timers.c				\
	../libavr32/src/usb.c					\
	../libavr32/src/util.c					\
	../libavr32/src/usb/hid/hid.c				\
	../libavr32/src/usb/hid/uhi_hid.c			\
	../libavr32/src/interrupts.c				\
	../libavr32/src/random.c				\
	../libavr32/src/usb/ftdi/ftdi.c				\
	../libavr32/src/usb/ftdi/uhi_ftdi.c			\
	../libavr32/src/usb/midi/midi.c				\
	../libavr32/src/usb/midi/uhi_midi.c			\
	../libavr32/src/usb/msc/msc.c				\
	../libavr32/src/usb/cdc/cdc.c				\
	../libavr32/src/usb/cdc/uhi_cdc.c			\
	common/services/usb/class/msc/host/uhi_msc.c		\
	common/services/usb/class/msc/host/uhi_msc_mem.c	\
	common/services/storage/ctrl_access/ctrl_access.c	\
	avr32/services/fs/fat/fat.c				\
	avr32/services/fs/fat/fat_unusual.c			\
	avr32/services/fs/fat/file.c				\
	avr32/services/fs/fat/navigation.c			\
	avr32/drivers/adc/adc.c					\
	avr32/drivers/flashc/flashc.c				\
	avr32/drivers/gpio/gpio.c				\
	avr32/drivers/intc/intc.c				\
	avr32/drivers/pm/pm.c					\
	avr32/drivers/pm/pm_conf_clocks.c			\
	avr32/drivers/pm/power_clocks_lib.c			\
	avr32/drivers/spi/spi.c					\
	avr32/drivers/tc/tc.c					\
	avr32/drivers/twi/twi.c					\
	avr32/drivers/usart/usart.c				\
	avr32/drivers/usbb/usbb_host.c				\
	avr32/utils/debug/print_funcs.c				\
	common/services/usb/uhc/uhc.c				\
	common/services/clock/uc3b0_b1/sysclk.c

# List of assembler source files.
ASSRCS = \
	avr32/utils/startup/trampoline_uc3.S			\
	avr32/drivers/intc/exception.S

# List of include paths.
INC_PATH = \
	../../module						\
	../../src						\
	../src							\
	../src/usb/ftdi						\
	../src/usb/hid						\
	../src/usb/midi						\
	../src/usb/msc						\
	../src/usb/cdc						\
	../conf							\
	../conf/teletype					\
	avr32/boards						\
	avr32/drivers/cpu/cycle_counter				\
	avr32/drivers/flashc					\
	avr32/drivers/gpio					\
	avr32/drivers/intc					\
	avr32/drivers/pm					\
	avr32/drivers/spi					\
	avr32/drivers/tc					\
	avr32/drivers/twi					\
	avr32/drivers/usart					\
	avr32/drivers/usbb					\
	avr32/services/fs/fat					\
	avr32/utils						\
	avr32/utils/debug					\
	avr32/utils/preprocessor				\
	common/boards						\
	common/boards/user_board				\
	common/services/storage/ctrl_access			\
	common/services/clock					\
	common/services/delay					\
	common/services/usb/					\
	common/services/usb/uhc					\
	common/services/usb/class/hid				\
	common/services/usb/class/msc				\
	common/services/usb/class/msc/host			\
	common/utils

# Additional search paths for libraries.
LIB_PATH =

# List of libraries to use during linking.
LIBS =

# Path relative to top level directory pointing to a linker script.
LINKER_SCRIPT = ../src/link_uc3b0512.lds

# Additional options for debugging.
DBGFLAGS =

# Application optimization
OPTIMIZATION = -Os

# Extra flags to use when archiving.
ARFLAGS =

# Extra flags to use when assembling.
ASFLAGS =

# Extra flags to use when compiling.
CFLAGS = -fshort-enums -fno-common

# Extra flags to use when preprocessing.
CPPFLAGS = -D BOARD=USER_BOARD -D UHD_ENABLE

# Extra flags to use when linking
LDFLAGS = -Wl,-e,_trampoline,--defsym=__flash_nvram_size__=200K

# Pre- and post-build commands
PREBUILD_CMD =
POSTBUILD_CMD =
