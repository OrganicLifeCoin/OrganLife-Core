packages:=boost libevent gmp $(zcash_packages) libsodium
native_packages := native_rust native_meson

# Preserve the MinGW 7 ABI while fixing its C++ thread-local teardown order.
# Do not replace the runtime of newer toolchains with this compatibility build.
ifeq ($(host_os),mingw32)
mingw32_headers_major := $(shell $(host_CC) -dM -E -include _mingw.h -x c /dev/null 2>/dev/null | sed -n 's/^\#define __MINGW64_VERSION_MAJOR //p')
ifeq ($(mingw32_headers_major),7)
mingw32_packages += winpthreads
endif
endif

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
