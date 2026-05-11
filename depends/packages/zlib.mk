package=zlib
$(package)_version=1.3.1
$(package)_download_path=https://github.com/madler/zlib/releases/download/v$($(package)_version)
$(package)_file_name=$(package)-$($(package)_version).tar.xz
$(package)_sha256_hash=38ef96b8dfe510d42707d9c781877914792541133e1870841463bfa73f883e32

define $(package)_set_vars
$(package)_build_opts= CC="$($(package)_cc)"
# Add -Wno-deprecated-non-prototype to fix build on newer clang (macOS 15+/Xcode 16+)
# zlib 1.2.13 uses old K&R style function definitions
$(package)_build_opts+=CFLAGS="$($(package)_cflags) $($(package)_cppflags) -fPIC -Wno-deprecated-non-prototype"
$(package)_build_opts+=RANLIB="$($(package)_ranlib)"
$(package)_build_opts+=AR="$($(package)_ar)"
$(package)_build_opts_darwin+=AR="$($(package)_libtool)"
$(package)_build_opts_darwin+=ARFLAGS="-o"
endef

define $(package)_config_cmds
  CC="$($(package)_cc)" AR="$($(package)_ar)" RANLIB="$($(package)_ranlib)" \
    ./configure --static --prefix=$(host_prefix)
endef

define $(package)_build_cmds
  $(MAKE) $($(package)_build_opts) libz.a
endef

define $(package)_stage_cmds
  $(MAKE) DESTDIR=$($(package)_staging_dir) install $($(package)_build_opts)
endef
