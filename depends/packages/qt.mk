package=qt
$(package)_version=6.10.2
$(package)_download_path=https://download.qt.io/official_releases/qt/6.10/$($(package)_version)/single
$(package)_file_name=qt-everywhere-src-$($(package)_version).tar.xz
$(package)_sha256_hash=c3df0f0e421130cc52ed81cb712358804471ce9bd2a41d97828f9f5b1bf7fed2
$(package)_dependencies=zlib
$(package)_patches=fix_qyieldcpu_arm_acle.patch
$(package)_linux_dependencies=freetype fontconfig libxcb xcb_util xcb_util_image xcb_util_keysyms xcb_util_renderutil xcb_util_wm xcb_util_cursor xkbcommon

ifneq ($(host_arch)_$(host_os),$(build_arch)_$(build_os))
$(package)_dependencies += native_qt
endif

define $(package)_set_vars
$(package)_config_opts_release = -release
$(package)_config_opts_debug = -debug
$(package)_config_opts += -cmake-generator "Ninja"
$(package)_config_opts += -confirm-license
$(package)_config_opts += -extprefix $(host_prefix)
$(package)_config_opts += -prefix $(host_prefix)
$(package)_config_opts += -libdir $(host_prefix)/lib
$(package)_config_opts += -plugindir $(host_prefix)/plugins
$(package)_config_opts += -translationdir $(host_prefix)/translations
$(package)_config_opts += -pkg-config
$(package)_config_opts += -nomake examples
$(package)_config_opts += -nomake tests
$(package)_config_opts += -opensource
$(package)_config_opts += -skip qt3d,qt5compat,qtactiveqt,qtcoap,qtconnectivity,qtdatavis3d,qtdeclarative,qtdoc,qtgraphs,qtgrpc,qthttpserver,qtimageformats,qtlanguageserver,qtlocation,qtlottie,qtmqtt,qtmultimedia,qtnetworkauth,qtopcua,qtpositioning,qtquick3d,qtquick3dphysics,qtquickeffectmaker,qtquicktimeline,qtremoteobjects,qtscxml,qtsensors,qtserialbus,qtserialport,qtshadertools,qtspeech,qtvirtualkeyboard,qtwayland,qtwebchannel,qtwebengine,qtwebsockets,qtwebview
$(package)_config_opts += -static
$(package)_config_opts += -system-zlib
$(package)_config_opts += -qt-libpng
$(package)_config_opts += -qt-libjpeg
$(package)_config_opts += -gif
$(package)_config_opts += -no-feature-sql
$(package)_config_opts += -no-feature-zstd
$(package)_config_opts += -no-openssl
$(package)_config_opts += -no-dbus
$(package)_config_opts += -no-cups
$(package)_config_opts += -no-gtk
$(package)_config_opts += -no-glib
$(package)_config_opts += -no-icu
$(package)_config_opts += -qt-pcre
$(package)_config_opts += -qt-harfbuzz
$(package)_config_opts += -qt-freetype
$(package)_config_opts += -qt-libmd4c
$(package)_config_opts += -no-opengl
$(package)_config_opts += -no-egl
$(package)_config_opts += -no-eglfs
$(package)_config_opts += -no-gbm
$(package)_config_opts += -no-kms
$(package)_config_opts += -no-linuxfb
$(package)_config_opts += -no-libudev
$(package)_config_opts += -no-evdev
$(package)_config_opts += -no-libinput
$(package)_config_opts += -no-mtdev
$(package)_config_opts += -no-tslib

$(package)_config_opts_darwin = -no-framework
$(package)_config_opts_darwin += -opengl desktop

$(package)_config_opts_aarch64_darwin += -device-option QMAKE_APPLE_DEVICE_ARCHS=arm64
$(package)_config_opts_x86_64_darwin += -device-option QMAKE_APPLE_DEVICE_ARCHS=x86_64

# When cross-compiling, Qt's build needs "host tools" (moc/rcc/etc) that can run on
# the build machine. Use a native Qt (built for the build machine) installed into
# $(build_prefix) as the host-tools Qt.
#
# Also avoid overwriting native_qt's real binaries in $(build_prefix)/bin with Qt's
# wrapper scripts (notably qmake6), which can otherwise end up calling themselves.
ifneq ($(host_arch)_$(host_os),$(build_arch)_$(build_os))
$(package)_config_opts += -qt-host-path $(build_prefix)
$(package)_config_opts += -bindir $(host_prefix)/bin
else
$(package)_config_opts += -bindir $(build_prefix)/bin
endif

$(package)_config_opts_linux  = -xcb
$(package)_config_opts_aarch64_linux += -skip qttools,qttranslations

$(package)_config_opts_darwin += QT_NO_APPLE_SDK_MAX_VERSION_CHECK=ON

$(package)_config_opts_mingw32 += -no-pkg-config
endef

define $(package)_preprocess_cmds
  patch -p1 < $($(package)_patch_dir)/fix_qyieldcpu_arm_acle.patch
endef

define $(package)_extract_cmds
  mkdir -p $($(package)_extract_dir) && \
  echo "$($(package)_sha256_hash)  $($(package)_source)" | $(build_SHA256SUM) -c - && \
  tar --no-same-owner --strip-components=1 -xf $($(package)_source)
endef

define $(package)_config_cmds
  rm -rf CMakeCache.txt CMakeFiles && \
  export PKG_CONFIG_SYSROOT_DIR=/ && \
  cmake_pkg_config_args="" && \
  if test "$(host_arch)_$(host_os)" != "$(build_arch)_$(build_os)"; then \
    export QT_HOST_PATH="$(build_prefix)"; \
  fi && \
  if echo "$(host)" | grep -q "linux"; then \
    cmake_pkg_config_args="-DPKG_CONFIG_ARGN=--static"; \
    if test "$(host_arch)_$(host_os)" != "$(build_arch)_$(build_os)"; then \
      export PKG_CONFIG_LIBDIR="$(host_prefix)/lib/pkgconfig"; \
      export PKG_CONFIG_PATH="$(host_prefix)/lib/pkgconfig:$(host_prefix)/share/pkgconfig"; \
    else \
      unset PKG_CONFIG_LIBDIR && \
      sys_pc="/usr/lib/pkgconfig:/usr/share/pkgconfig:/usr/local/lib/pkgconfig" && \
      for d in /usr/lib/*/pkgconfig; do \
        if [ -d "$$d" ]; then sys_pc="$$sys_pc:$$d"; fi; \
      done && \
      export PKG_CONFIG_PATH="$(host_prefix)/lib/pkgconfig:$(host_prefix)/share/pkgconfig:$$sys_pc"; \
    fi; \
  else \
    export PKG_CONFIG_LIBDIR="$(host_prefix)/lib/pkgconfig"; \
  fi && \
  export QT_MAC_SDK_NO_VERSION_CHECK=1 && \
  if echo "$(host)" | grep -q "mingw32"; then \
    echo 'set(CMAKE_SYSTEM_NAME Windows)' > $(host_prefix)/mingw-toolchain.cmake && \
    echo 'set(CMAKE_SYSTEM_PROCESSOR x86_64)' >> $(host_prefix)/mingw-toolchain.cmake && \
    echo 'set(CMAKE_C_COMPILER $(host)-gcc)' >> $(host_prefix)/mingw-toolchain.cmake && \
    echo 'set(CMAKE_CXX_COMPILER $(host)-g++)' >> $(host_prefix)/mingw-toolchain.cmake && \
    echo 'set(CMAKE_RC_COMPILER $(host)-windres)' >> $(host_prefix)/mingw-toolchain.cmake && \
    echo 'set(CMAKE_FIND_ROOT_PATH $(host_prefix))' >> $(host_prefix)/mingw-toolchain.cmake && \
    echo 'set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)' >> $(host_prefix)/mingw-toolchain.cmake && \
    echo 'set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)' >> $(host_prefix)/mingw-toolchain.cmake && \
    echo 'set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)' >> $(host_prefix)/mingw-toolchain.cmake && \
    echo 'set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)' >> $(host_prefix)/mingw-toolchain.cmake && \
    cd qtbase && \
    ./configure -top-level $($(package)_config_opts) -- -DCMAKE_TOOLCHAIN_FILE=$(host_prefix)/mingw-toolchain.cmake -DFEATURE_system_doubleconversion=OFF -DQT_HOST_PATH=$(build_prefix) -DQt6HostInfo_DIR=$(build_prefix)/lib/cmake/Qt6HostInfo -DCMAKE_PREFIX_PATH=$(build_prefix)/lib/cmake; \
  elif echo "$(host)" | grep -q "linux" && test "$(host_arch)_$(host_os)" != "$(build_arch)_$(build_os)"; then \
    echo 'set(CMAKE_SYSTEM_NAME Linux)' > $(host_prefix)/linux-toolchain.cmake && \
    echo 'set(CMAKE_SYSTEM_PROCESSOR $(host_arch))' >> $(host_prefix)/linux-toolchain.cmake && \
    echo 'set(CMAKE_C_COMPILER $(host)-gcc)' >> $(host_prefix)/linux-toolchain.cmake && \
    echo 'set(CMAKE_CXX_COMPILER $(host)-g++)' >> $(host_prefix)/linux-toolchain.cmake && \
    echo 'set(CMAKE_FIND_ROOT_PATH $(host_prefix))' >> $(host_prefix)/linux-toolchain.cmake && \
    echo 'set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)' >> $(host_prefix)/linux-toolchain.cmake && \
    echo 'set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)' >> $(host_prefix)/linux-toolchain.cmake && \
    echo 'set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)' >> $(host_prefix)/linux-toolchain.cmake && \
    echo 'set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)' >> $(host_prefix)/linux-toolchain.cmake && \
    cd qtbase && \
    ./configure -top-level $($(package)_config_opts) -- -DCMAKE_TOOLCHAIN_FILE=$(host_prefix)/linux-toolchain.cmake -DFEATURE_system_doubleconversion=OFF -DTEST_xcb_syslibs=ON -DQT_HOST_PATH=$(build_prefix) -DQt6HostInfo_DIR=$(build_prefix)/lib/cmake/Qt6HostInfo -DCMAKE_PREFIX_PATH=$(build_prefix)/lib/cmake $${cmake_pkg_config_args}; \
  else \
    cd qtbase && \
    ./configure -top-level $($(package)_config_opts) -- -DFEATURE_system_doubleconversion=OFF $(if $(findstring linux,$(host)),-DTEST_xcb_syslibs=ON,) $${cmake_pkg_config_args} $(if $(findstring apple-darwin,$(host)),-DCMAKE_OSX_ARCHITECTURES=$(if $(filter aarch64,$(host_arch)),arm64,$(host_arch)),); \
  fi
endef

define $(package)_build_cmds
  ninja $(if $(JOBS),-j$(JOBS),)
endef

define $(package)_stage_cmds
  DESTDIR=$($(package)_staging_dir) ninja install
endef

define $(package)_postprocess_cmds
  if find lib plugins mkspecs -type f -name '*.prl' -print -o -type f -name '*.pri' -print 2>/dev/null | grep -q .; then \
    find lib plugins mkspecs -type f -name '*.prl' -print0 -o -type f -name '*.pri' -print0 | \
      xargs -0 sed -i.old \
        -e 's/ -lbrotlidec / -lbrotlidec -lbrotlicommon /g' \
        -e 's/-lbrotlidec;/ -lbrotlidec;-lbrotlicommon;/g' || true; \
  fi; \
  if [ -f plugins/platforms/libqxcb.prl ] && ! grep -q 'libxcb-util' plugins/platforms/libqxcb.prl; then \
    sed -i.old \
      -e 's| -ldl| $(host_prefix)/lib/libxcb-util.a -ldl|g' \
      -e 's|;-ldl|;$(host_prefix)/lib/libxcb-util.a;-ldl|g' \
      plugins/platforms/libqxcb.prl; \
  fi; \
  find lib plugins mkspecs -type f -name '*.old' -delete 2>/dev/null || true; \
	  rm -f lib/lib*.la
endef
