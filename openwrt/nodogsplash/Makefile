#
# Copyright (C) 2013 OpenWrt.org
#
# This is free software, licensed under the GNU General Public License v2.
# See /LICENSE for more information.
#

include $(TOPDIR)/rules.mk

PKG_NAME:=lazooosplash
PKG_FIXUP:=autoreconf
PKG_VERSION:=0.2
PKG_RELEASE:=2

PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)/
PKG_SOURCE:=$(PKG_NAME)-$(PKG_VERSION).tar.gz
PKG_SOURCE_URL:=git://github.com/wifiLazooo/LazoooSplash.git
PKG_SOURCE_VERSION:=development
PKG_SOURCE_SUBDIR:=$(PKG_NAME)-$(PKG_VERSION)
PKG_BUILD_PARALLEL:=1

include $(INCLUDE_DIR)/package.mk


define Package/nodogsplash
	SUBMENU:=Captive Portals
	SECTION:=net
	CATEGORY:=Network
	DEPENDS:=+libpthread +iptables-mod-ipopt
	TITLE:=Open public network gateway daemon
	URL:=https://github.com/nodogsplash/nodogsplash
endef

define Package/nodogsplash/description
	Nodogsplash offers a simple way to open a free hotspot providing
	restricted access to an internet connection.
endef

define Package/nodogsplash/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/nodogsplash $(1)/usr/bin/
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/ndsctl $(1)/usr/bin/

	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) files/nodogsplash.init $(1)/etc/init.d/nodogsplash

endef

define Package/nodogsplash/conffiles
/etc/nodogsplash/nodogsplash.conf
endef

$(eval $(call BuildPackage,nodogsplash))

