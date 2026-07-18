include $(TOPDIR)/rules.mk

PKG_NAME:=at-webserver
PKG_VERSION:=0.1.0
PKG_RELEASE:=1

PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)

include $(INCLUDE_DIR)/package.mk

define Package/at-webserver
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=MT5700 AT WebSocket server in C
  DEPENDS:= +libpthread +libc +kmod-usb-net-cdc-mbim
endef

define Package/at-webserver/description
  C implementation of the MT5700 AT WebSocket backend for OpenWrt,
  bundled with the /5700 frontend assets and helper CGI scripts.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) -r ./src $(PKG_BUILD_DIR)/
	$(CP) ./Makefile.native $(PKG_BUILD_DIR)/Makefile
endef

define Build/Compile
	$(MAKE) -C $(PKG_BUILD_DIR) CC="$(TARGET_CC)" CFLAGS="$(TARGET_CFLAGS) -pthread"
endef

define Package/at-webserver/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/at-server-c $(1)/usr/bin/at-server-c
	
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/etc/init.d/at-webserver $(1)/etc/init.d/
	
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/etc/config/at-webserver $(1)/etc/config/
	
	$(INSTALL_DIR) $(1)/www/5700
	$(CP) -r ./files/www/5700/* $(1)/www/5700/
	
	$(INSTALL_DIR) $(1)/www/cgi-bin
	$(INSTALL_BIN) ./files/www/cgi-bin/at-ws-info $(1)/www/cgi-bin/
	$(INSTALL_BIN) ./files/www/cgi-bin/at-log-clear $(1)/www/cgi-bin/
endef

define Package/at-webserver/conffiles
/etc/config/at-webserver
endef

$(eval $(call BuildPackage,at-webserver))
