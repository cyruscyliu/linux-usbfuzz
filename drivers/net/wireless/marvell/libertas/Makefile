# SPDX-License-Identifier: GPL-2.0
libertas-y += cfg.o
libertas-y += cmd.o
libertas-y += cmdresp.o
libertas-y += debugfs.o
libertas-y += ethtool.o
libertas-y += main.o
libertas-y += rx.o
libertas-y += tx.o
libertas-y += firmware.o
libertas-$(CONFIG_LIBERTAS_MESH) += mesh.o

usb8xxx-objs += if_usb.o
libertas_cs-objs += if_cs.o
libertas_sdio-objs += if_sdio.o
libertas_spi-objs += if_spi.o

obj-$(CONFIG_LIBERTAS)     += libertas.o
obj-$(CONFIG_LIBERTAS_USB) += usb8xxx.o
obj-$(CONFIG_LIBERTAS_CS)  += libertas_cs.o
obj-$(CONFIG_LIBERTAS_SDIO) += libertas_sdio.o
obj-$(CONFIG_LIBERTAS_SPI) += libertas_spi.o

KCOV_INSTRUMENT := y
