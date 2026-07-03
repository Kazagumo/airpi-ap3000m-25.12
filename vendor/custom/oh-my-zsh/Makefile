include $(TOPDIR)/rules.mk

PKG_NAME:=oh-my-zsh
PKG_VERSION:=0.1.0
PKG_RELEASE:=1

PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)

include $(INCLUDE_DIR)/package.mk

define Package/$(PKG_NAME)
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=Oh my zsh
  DEPENDS:= +zsh
endef

define Package/$(PKG_NAME)/description
  oh my zsh and plugins
endef

define Package/$(PKG_NAME)/config
	source "$(SOURCE)/Config.in"
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	git clone https://github.com/robbyrussell/oh-my-zsh  $(PKG_BUILD_DIR)/.oh-my-zsh --depth=1
	
	[[ "$(CONFIG_ZSH_AUTOSUGGESTIONS)" == "y" ]] && git clone https://github.com/zsh-users/zsh-autosuggestions $(PKG_BUILD_DIR)/.oh-my-zsh/custom/plugins/zsh-autosuggestions --depth=1

	[[ "$(CONFIG_ZSH_SYNTAX_HIGHLIGHTING)" == "y" ]] && git clone https://github.com/zsh-users/zsh-syntax-highlighting.git $(PKG_BUILD_DIR)/.oh-my-zsh/custom/plugins/zsh-syntax-highlighting --depth=1

	[[ "$(CONFIG_ZSH_COMPLETIONS)" == "y" ]] && git clone https://github.com/zsh-users/zsh-completions $(PKG_BUILD_DIR)/.oh-my-zsh/custom/plugins/zsh-completions --depth=1
endef

define Build/Compile
endef

define Package/$(PKG_NAME)/install
	$(INSTALL_DIR) $(1)/root
	$(CP) -r $(PKG_BUILD_DIR)/.oh-my-zsh/ $(1)/root
endef

define Package/$(PKG_NAME)/postinst
#!/bin/sh
echo "Setting default shell as zsh..."
sed -i s#$(which ash)#$(which zsh)#g /etc/passwd
exit 0
endef

define Package/$(PKG_NAME)/prerm
#!/bin/sh
echo "Restoring default shell to ash..."
sed -i s#$(which zsh)#$(which ash)#g /etc/passwd
exit 0
endef

$(eval $(call BuildPackage,$(PKG_NAME)))
