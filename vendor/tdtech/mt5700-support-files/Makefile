include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

# Include unified version
include ../../version.mk

PKG_NAME:=mt5700-support-files
PKG_VERSION:=0.1.0
PKG_RELEASE:=1

include $(INCLUDE_DIR)/package.mk

define Package/$(PKG_NAME)
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=QModem scripts
  DEPENDS:=	\
  		+kmod-usb2 +kmod-usb3 \
		+kmod-usb-serial +kmod-usb-serial-option \
		+kmod-usb-net +kmod-usb-acm \
		+kmod-usb-wdm \
  		+kmod-usb-net-cdc-ether \
		+kmod-usb-net-cdc-mbim \
		+kmod-usb-net-rndis \
		+kmod-usb-net-cdc-ncm +kmod-usb-net-huawei-cdc-ncm \
		+usbutils +at-webserver +luci-app-at-webserver\
endef

define Package/$(PKG_NAME)/description
	mt5700 essential files
endef

define Build/Prepare
endef

define Build/Compile
endef

define Package/$(PKG_NAME)/install
		$(CP) ./files/* $(1)/
endef

$(eval $(call BuildPackage,$(PKG_NAME)))
