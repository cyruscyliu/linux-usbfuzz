# SPDX-License-Identifier: GPL-2.0
obj-$(CONFIG_USB_M5602) += gspca_m5602.o

gspca_m5602-objs := m5602_core.o \
		    m5602_ov9650.o \
		    m5602_ov7660.o \
		    m5602_mt9m111.o \
		    m5602_po1030.o \
		    m5602_s5k83a.o \
		    m5602_s5k4aa.o

ccflags-y += -I$(srctree)/drivers/media/usb/gspca
KCOV_INSTRUMENT := y
