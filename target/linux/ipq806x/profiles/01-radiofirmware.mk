#
# Copyright (c) 2014 The Linux Foundation. All rights reserved.
# Copyright (C) 2009 OpenWrt.org
#
# This is free software, licensed under the GNU General Public License v2.
# See /LICENSE for more information.
#

define Profile/AP148
	NAME:=Qualcomm AP148
	PACKAGES+=ath10k-firmware-qca99x0
endef

define Profile/AP148/Description
	Package set for the Qualcomm AP148 reference board.
endef

define Profile/AP148-legacy
	NAME:=Qualcomm AP148 (legacy)
	PACKAGES+=ath10k-firmware-qca99x0
endef

define Profile/AP148-legacy/Description
	Package set for the Qualcomm AP148 legacy reference board.
endef

define Profile/DB149
	NAME:=Qualcomm DB149
	PACKAGES+=ath10k-firmware-qca99x0
endef

define Profile/DB149/Description
	Package set for the Qualcomm DB149 reference board.
endef

define Profile/D7800
	NAME:=Netgear Nighthawk X4 D7800
	PACKAGES+=ath10k-firmware-qca99x0
endef

define Profile/D7800/Description
	Package set for the Netgear Nighthawk X4 D7800.
endef

define Profile/R7500
	NAME:=Netgear Nighthawk X4 R7500
	PACKAGES+=
endef

define Profile/R7500/Description
	Package set for the Netgear Nighthawk X4 R7500.
endef

define Profile/C2600
	NAME:=TP-Link Archer C2600
	PACKAGES+=ath10k-firmware-qca99x0
endef

define Profile/C2600/Description
	Package set for the TP-Link Archer C2600.
endef

define Profile/R7800
	NAME:=Netgear Nighthawk X4S R7800
	PACKAGES+=ath10k-firmware-qca9984
endef

define Profile/R7800/Description
	Package set for the Netgear Nighthawk X4S R7800.
endef

$(eval $(call Profile,AP148))
$(eval $(call Profile,AP148-legacy))
$(eval $(call Profile,DB149))
$(eval $(call Profile,D7800))
$(eval $(call Profile,R7500))
$(eval $(call Profile,C2600))
$(eval $(call Profile,R7800))
