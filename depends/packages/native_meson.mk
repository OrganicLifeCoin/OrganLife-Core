package=native_meson
$(package)_version=1.5.0
$(package)_download_path=https://github.com/mesonbuild/meson/releases/download/$($(package)_version)
$(package)_file_name=meson-$($(package)_version).tar.gz
$(package)_sha256_hash=45d7b8653c1e5139df35b33be2dd5b2d040c5b2c6129f9a7c890d507e33312b8

define $(package)_build_cmds
  python3 setup.py build
endef

define $(package)_stage_cmds
  python3 setup.py install --prefix=$(build_prefix) --root=$($(package)_staging_dir)
endef

define $(package)_postprocess_cmds
  rm -f lib/*.la
endef
