# SPDX-License-Identifier: GPL-2.0
#
# Realtek RTL8197F (RTL819x) switch-core CPU-port Ethernet driver
#

obj-$(CONFIG_NET_RTL819X) += rtl819x.o

rtl819x-y := rtl819x-eth.o rtl819x_swnic.o rtl865x_asichal.o
# Hardware NAT offload is being rebuilt on ndo_setup_tc(TC_SETUP_FT); the file
# still carries the ASIC table programming that rewrite reuses.
rtl819x-$(CONFIG_RTL819X_HWNAT) += rtl819x_hwnat.o
