TARGET = ge_patch
OBJS = main.o gu.o vertex_restore.o uv_inset.o guard_band.o exports.o
GP_TRACE ?= 0
GP_LOG ?= 0
GP_PROFILE ?= 0
CFLAGS = -DGP_TRACE=$(GP_TRACE) -DGP_LOG=$(GP_LOG) -DGP_PROFILE=$(GP_PROFILE) -O2 -G0 -Wall -Wextra -fshort-wchar -fno-pic -mno-check-zero-division
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)
BUILD_PRX = 1
PRX_EXPORTS = exports.exp
USE_KERNEL_LIBC = 1
USE_KERNEL_LIBS = 1
LIBS = -lpspsystemctrl_kernel -lpsppower_driver

# Local build only. Installing onto a mounted card is an explicit separate step.
PSPSDK := $(shell psp-config --pspsdk-path)
ifeq ($(strip $(PSPSDK)),)
$(error psp-config missing: add $$PSPDEV/bin to PATH; see README.md)
endif
include $(PSPSDK)/lib/build_prx.mak
