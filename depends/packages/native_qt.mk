package=native_qt
$(package)_version=6.10.2
$(package)_download_path=https://download.qt.io/official_releases/qt/6.10/$($(package)_version)/single
$(package)_file_name=qt-everywhere-src-$($(package)_version).tar.xz
$(package)_sha256_hash=c3df0f0e421130cc52ed81cb712358804471ce9bd2a41d97828f9f5b1bf7fed2
$(package)_patches=fix_qyieldcpu_arm_acle.patch

define $(package)_set_vars
$(package)_config_opts += -cmake-generator "Ninja"
$(package)_config_opts += -confirm-license
$(package)_config_opts += -opensource
$(package)_config_opts += -prefix $(build_prefix)
$(package)_config_opts += -static
$(package)_config_opts += -release
$(package)_config_opts_darwin += -no-framework
$(package)_config_opts += -nomake examples
$(package)_config_opts += -nomake tests
$(package)_config_opts += -no-dbus
$(package)_config_opts += -no-icu
$(package)_config_opts += -no-opengl
$(package)_config_opts += -no-openssl
$(package)_config_opts += -no-cups
$(package)_config_opts += -no-gtk
$(package)_config_opts += -no-glib
$(package)_config_opts += -no-feature-sql
$(package)_config_opts += -no-feature-zstd
$(package)_config_opts += -no-feature-backtrace
$(package)_config_opts += -qt-pcre
$(package)_config_opts += -qt-freetype
$(package)_config_opts += -qt-libpng
$(package)_config_opts += -qt-libjpeg
$(package)_config_opts += -qt-zlib
$(package)_config_opts += -qt-libmd4c
$(package)_config_opts += -skip qt3d,qt5compat,qtactiveqt,qtcharts,qtcoap,qtconnectivity,qtdatavis3d,qtdeclarative,qtdoc,qtgraphs,qtgrpc,qthttpserver,qtimageformats,qtlanguageserver,qtlocation,qtlottie,qtmqtt,qtmultimedia,qtnetworkauth,qtopcua,qtpositioning,qtquick3d,qtquick3dphysics,qtquickeffectmaker,qtquicktimeline,qtremoteobjects,qtscxml,qtsensors,qtserialbus,qtserialport,qtshadertools,qtspeech,qtsvg,qtvirtualkeyboard,qtwayland,qtwebchannel,qtwebengine,qtwebsockets,qtwebview
endef

define $(package)_extract_cmds
  mkdir -p $($(package)_extract_dir) && \
  echo "$($(package)_sha256_hash)  $($(package)_source)" | $(build_SHA256SUM) -c - && \
  tar --no-same-owner --strip-components=1 -xf $($(package)_source)
endef

define $(package)_preprocess_cmds
  patch -p1 < $($(package)_patch_dir)/fix_qyieldcpu_arm_acle.patch
endef

define $(package)_config_cmds
  cd qtbase && \
  ./configure -top-level $($(package)_config_opts) -- \
    -DQT_NO_APPLE_SDK_MAX_VERSION_CHECK=ON \
    -DFEATURE_backtrace=OFF \
    -DFEATURE_libudev=OFF \
    -DTEST_librt=OFF \
    -DCMAKE_DISABLE_FIND_PACKAGE_WrapBacktrace=ON \
    -DCMAKE_DISABLE_FIND_PACKAGE_Libdrm=ON \
    -DCMAKE_DISABLE_FIND_PACKAGE_gbm=ON
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
  find lib plugins mkspecs -type f -name '*.old' -delete 2>/dev/null || true; \
  if echo "$(host)" | grep -q "^aarch64-.*linux"; then \
    if test -f bin/qmake6 && test -r bin/qmake6; then \
      head="`LC_ALL=C head -n 1 bin/qmake6 2>/dev/null`"; \
      case "$$head" in \
        '#!'*) \
          if grep -F "target_qt.conf" bin/qmake6 >/dev/null 2>&1; then \
            if test -x bin/qmake; then \
              rm -f bin/qmake6 && ln -s qmake bin/qmake6; \
            else \
              echo "ERROR: native_qt produced a recursive bin/qmake6 wrapper and bin/qmake is missing."; \
              exit 1; \
            fi; \
          fi; \
        ;; \
      esac; \
    fi; \
  fi && \
  rm -f lib/cmake/Qt6/Qt6Config.cmake && \
  rm -f lib/cmake/Qt6/Qt6ConfigVersion.cmake && \
  rm -f lib/cmake/Qt6/Qt6ConfigVersionImpl.cmake && \
  rm -f lib/cmake/Qt6/Qt6Targets.cmake && \
  cd lib/cmake && \
  for d in *; do \
    if [ -d "$$d" ] && echo "$$d" | grep -qE '^Qt6[A-Z]' && ! echo "$$d" | grep -q 'Tools' && ! echo "$$d" | grep -q 'BuildInternals'; then \
      rm -rf "$$d"; \
    fi; \
  done
endef
