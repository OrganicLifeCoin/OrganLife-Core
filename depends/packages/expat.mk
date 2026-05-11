package=expat
$(package)_version=2.7.3
$(package)_download_path=https://github.com/libexpat/libexpat/releases/download/R_2_7_3/
$(package)_file_name=$(package)-$($(package)_version).tar.bz2
$(package)_sha256_hash=59c31441fec9a66205307749eccfee551055f2d792f329f18d97099e919a3b2f

define $(package)_set_vars
  $(package)_config_opts=--disable-shared --without-docbook
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
