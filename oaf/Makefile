include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=oaf
include $(INCLUDE_DIR)/package.mk

RSTRIP:=:

define KernelPackage/oaf
  SUBMENU:=Netfilter Extensions
  TITLE:=OpenAppFilter netfilter modules
  FILES:=$(PKG_BUILD_DIR)/oaf.ko $(PKG_BUILD_DIR)/oaf_qos.ko
  AUTOLOAD:=$(call AutoLoad,90,oaf oaf_qos)
  DEPENDS:=+kmod-ipt-conntrack
  KCONFIG:=
endef

define KernelPackage/oaf/description
  OpenAppFilter application filter and per-application QoS classifier
endef

KCFLAGS := -Wno-error=missing-prototypes \
           -Wno-error=missing-declarations \
           -Wno-error=unused-variable \
           -Wno-error=declaration-after-statement \
           -Wno-error=implicit-fallthrough \
           -Wno-error=missing-braces \
           -Wno-error=parentheses \
           -Wno-error=format \
           -Wno-frame-larger-than

MAKE_OPTS:= \
	$(KERNEL_MAKE_FLAGS) \
	M="$(PKG_BUILD_DIR)" \
	KCFLAGS="$(KCFLAGS)" \
	$(EXTRA_KCONFIG)

define Build/Compile
	$(MAKE) -C "$(LINUX_DIR)" \
		$(MAKE_OPTS) \
		modules
endef

$(eval $(call KernelPackage,oaf))
