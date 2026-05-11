package=libxcb
$(package)_version=1.15
$(package)_download_path=https://xcb.freedesktop.org/dist
$(package)_file_name=$(package)-$($(package)_version).tar.xz
$(package)_sha256_hash=cc38744f817cf6814c847e2df37fcb8997357d72fa4bcbc228ae0fe47219a059
$(package)_dependencies=xcb_proto libXau libXdmcp

define $(package)_set_vars
$(package)_config_opts=--enable-static --disable-shared --disable-build-docs --without-doxygen --without-launchd
$(package)_config_opts += --disable-dependency-tracking --enable-option-checking
# Qt6 requires system xcb with extensions enabled (unlike Qt5 which bundled them
# via -qt-xcb). Enable the extensions that Qt6's xcb platform plugin needs.
# Disable only the ones that are truly unnecessary.
$(package)_config_opts += --disable-composite --disable-damage --disable-dpms
$(package)_config_opts += --disable-dri2 --disable-dri3 --disable-glx
$(package)_config_opts += --disable-present --disable-record
$(package)_config_opts += --disable-resource --disable-screensaver
$(package)_config_opts += --disable-xevie --disable-xfree86-dri
$(package)_config_opts += --disable-xinerama --disable-xprint
$(package)_config_opts += --disable-selinux --disable-xtest
$(package)_config_opts += --disable-xv --disable-xvmc
$(package)_config_opts_linux=--with-pic
endef

define $(package)_preprocess_cmds
  cp -f $(BASEDIR)/config.guess $(BASEDIR)/config.sub build-aux &&\
  sed "s/pthread-stubs//" -i configure
endef

define $(package)_config_cmds
  $($(package)_autoconf)
endef

define $(package)_build_cmds
  $(MAKE)
endef

define $(package)_stage_cmds
  $(MAKE) DESTDIR=$($(package)_staging_dir) install
endef

define $(package)_postprocess_cmds
  if [ -f lib/pkgconfig/xcb.pc ] && ! grep -q -- '-lXau' lib/pkgconfig/xcb.pc; then \
    awk '{if ($$$$1=="Libs:") {print $$$$0" -lXau -lXdmcp"} else {print}}' lib/pkgconfig/xcb.pc > lib/pkgconfig/xcb.pc.new && \
      mv lib/pkgconfig/xcb.pc.new lib/pkgconfig/xcb.pc; \
  fi; \
  rm -rf share/man share/doc lib/*.la
endef
