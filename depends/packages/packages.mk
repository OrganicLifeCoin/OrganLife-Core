packages:=boost libevent gmp $(zcash_packages) libsodium
native_packages := native_rust native_meson

qt_packages = qrencode zlib

qt_linux_packages:=qt expat freetype fontconfig brotli xproto xcb_proto libXau libXdmcp libxcb xcb_util xcb_util_image xcb_util_keysyms xcb_util_renderutil xcb_util_wm xcb_util_cursor xkbcommon
qt_aarch64_linux_packages:=xproto xcb_proto libXau libXdmcp libxcb xcb_util xcb_util_image xcb_util_keysyms xcb_util_renderutil xcb_util_wm xcb_util_cursor xkbcommon

qt_darwin_packages=qt
qt_mingw32_packages=qt

qt_native_packages =
ifneq ($(host_arch)_$(host_os),$(build_arch)_$(build_os))
qt_native_packages = native_qt
endif

wallet_packages=bdb

zmq_packages=zeromq

upnp_packages=miniupnpc
natpmp_packages=libnatpmp

darwin_native_packages = native_ds_store native_mac_alias

$(host_arch)_$(host_os)_native_packages += native_b2

ifneq ($(build_os),darwin)
darwin_native_packages += native_cctools native_libdmg-hfsplus
endif
