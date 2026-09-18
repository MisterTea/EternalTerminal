include $(TOPDIR)/rules.mk

PKG_NAME:=eternalterminal
PKG_VERSION:=6.2.11
PKG_RELEASE:=1

PKG_LICENSE:=Apache-2.0
PKG_LICENSE_FILES:=LICENSE

PKG_BUILD_DEPENDS:=protobuf/host

include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/cmake.mk

define Package/eternalterminal
  SECTION:=net
  CATEGORY:=Network
  TITLE:=Eternal Terminal (Remote shell)
  URL:=https://mistertea.github.io/EternalTerminal/
  DEPENDS:=+libsodium +protobuf +libopenssl +zlib +libstdcpp
endef

define Package/eternalterminal/description
  Eternal Terminal is a remote shell that automatically reconnects without interrupting the session.
endef

CMAKE_OPTIONS += \
	-DDISABLE_VCPKG=ON \
	-DDISABLE_TELEMETRY=ON \
	-DDISABLE_SENTRY=ON \
	-DDISABLE_CRASH_LOG=ON \
	-DDISABLE_STACKTRACE=ON \
	-DCODE_COVERAGE=OFF \
	-DBUILD_TESTING=OFF \
	-DINSTALL_BASH_COMPLETION=OFF \
	-DINSTALL_ZSH_COMPLETION=OFF \
	-DProtobuf_PROTOC_EXECUTABLE=$(STAGING_DIR_HOSTPKG)/bin/protoc

ET_SRC_DIR ?=

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	for src in "$$(ET_SRC_DIR)" "$$(SOURCE_DIR)" "/builder/src" "/workspace" "$$(CURDIR)/../.." ; do \
		if [ -n "$$$$src" ] && [ -f "$$$$src/CMakeLists.txt" ]; then \
			$(CP) $$$$src/. $(PKG_BUILD_DIR)/ ; \
			break ; \
		fi ; \
	done
endef

define Package/eternalterminal/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/bin/et $(1)/usr/bin/
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/bin/etserver $(1)/usr/bin/
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/bin/etterminal $(1)/usr/bin/

	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/etserver.init $(1)/etc/init.d/etserver
endef

$(eval $(call BuildPackage,eternalterminal))
