package=xcb_proto
$(package)_version=1.15
$(package)_download_path=https://xcb.freedesktop.org/dist
$(package)_file_name=xcb-proto-$($(package)_version).tar.xz
$(package)_sha256_hash=d34c3b264e8365d16fa9db49179cfa3e9952baaf9275badda0f413966b65955f

# xcb-proto 1.10 ships an Automake `py-compile` helper that uses the removed
# Python stdlib module `imp` (fails on Python 3.12+). We don't need `.pyc`
# files in the depends output (we delete them in postprocess), so make this a
# no-op to keep builds working on modern distros (e.g. Ubuntu 24.04).
define $(package)_preprocess_cmds
  printf '%s\n' '#!/usr/bin/env sh' 'exit 0' > py-compile && chmod +x py-compile
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
  find -name "*.pyc" -delete && \
  find -name "*.pyo" -delete
endef
