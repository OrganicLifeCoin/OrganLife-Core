package=gmp
$(package)_version=6.3.0
$(package)_download_path=https://ftp.gnu.org/gnu/gmp
$(package)_file_name=$(package)-$($(package)_version).tar.xz
$(package)_sha256_hash=a3c2b80201b89e68616f4ad30bc66aee4927c3ce50e33929ca819d5c43538898

define $(package)_set_vars
$(package)_config_opts+=--enable-cxx --with-pic --disable-shared --enable-option-checking
$(package)_config_opts_mingw32+=--enable-mingw
# Disable assembly and fat build when cross-compiling x86_64 on Apple Silicon
$(package)_config_opts_x86_64_darwin+=--disable-assembly --disable-fat
$(package)_config_opts+=$(if $(filter aarch64,$(shell uname -m)),$(if $(filter x86_64-apple-darwin,$(host)),--disable-assembly --disable-fat,))
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
