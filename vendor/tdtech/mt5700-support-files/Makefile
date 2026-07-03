include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk
include $(INCLUDE_DIR)/package.mk

PKG_NAME:=mt5700-support-files
PKG_VERSION:=0.1.0
PKG_RELEASE:=1

define Package/$(PKG_NAME)
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=mt5700 driver and web configuration interface
  DEPENDS:=	\
  		+kmod-usb2 +kmod-usb3 \
		+kmod-usb-serial +kmod-usb-serial-option \
		+kmod-usb-net +kmod-usb-acm \
		+kmod-usb-wdm \
  		+kmod-usb-net-cdc-ether \
		+kmod-usb-net-cdc-mbim \
		+kmod-usb-net-rndis \
		+kmod-usb-net-cdc-ncm +kmod-usb-net-huawei-cdc-ncm \
		+usbutils +at-webserver +luci-app-at-webserver
endef

define Package/$(PKG_NAME)/description
	Phony package of mt5700 drivers and webui
endef

define Build/Prepare
endef

define Build/Compile
endef

define Package/$(PKG_NAME)/install
endef

$(eval $(call BuildPackage,$(PKG_NAME)))
