package=winpthreads
$(package)_version=7.0.0
$(package)_download_path=https://github.com/mingw-w64/mingw-w64/archive/refs/tags
$(package)_download_file=v$($(package)_version).tar.gz
$(package)_file_name=mingw-w64-$($(package)_version).tar.gz
$(package)_sha256_hash=0033d29eea0eb6e98d5112bf52122edadb828aed8481a08e67467082cbdf5bf7
$(package)_patches=tls-destructor-order.patch
$(package)_build_subdir=mingw-w64-libraries/winpthreads

define $(package)_set_vars
$(package)_config_opts=--disable-shared --enable-static
endef

define $(package)_preprocess_cmds
  cd $($(package)_build_subdir) && patch -p1 < $($(package)_patch_dir)/tls-destructor-order.patch
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
