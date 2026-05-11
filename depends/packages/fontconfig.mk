package=fontconfig
$(package)_version=2.15.0
$(package)_download_path=https://www.freedesktop.org/software/fontconfig/release/
$(package)_file_name=$(package)-$($(package)_version).tar.xz
$(package)_sha256_hash=63a0658d0e06e0fa886106452b58ef04f21f58202ea02a94c39de0d3335d7c0e
$(package)_dependencies=freetype expat

define $(package)_set_vars
  # Build a static fontconfig for inclusion in static Qt builds.
  # Use system runtime locations so the resulting binaries can discover fonts on the target.
  $(package)_config_opts=--disable-docs --disable-shared --disable-libxml2 --disable-iconv
  $(package)_config_opts += --sysconfdir=/etc --localstatedir=/var
  $(package)_config_opts += --disable-dependency-tracking --enable-option-checking
  $(package)_config_opts_linux=--with-pic
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
  rm lib/*.la
endef
