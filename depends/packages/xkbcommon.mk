package=xkbcommon
$(package)_version=1.7.0
$(package)_download_path=https://xkbcommon.org/download
$(package)_file_name=libxkbcommon-$($(package)_version).tar.xz
$(package)_sha256_hash=65782f0a10a4b455af9c6baab7040e2f537520caa2ec2092805cdfd36863b247
$(package)_build_subdir=build
$(package)_dependencies=libxcb native_meson

define $(package)_preprocess_cmds
  mkdir -p build
endef

ifneq ($(host),$(build))
define $(package)_config_cmds
  printf '%s\n' \
    "[binaries]" \
    "c = '$(host)-gcc'" \
    "cpp = '$(host)-g++'" \
    "ar = '$(host)-ar'" \
    "strip = '$(host)-strip'" \
    "pkgconfig = 'pkg-config'" \
    "" \
    "[host_machine]" \
    "system = 'linux'" \
    "cpu_family = 'aarch64'" \
    "cpu = 'aarch64'" \
    "endian = 'little'" \
    "" \
    "[properties]" \
    "needs_exe_wrapper = true" \
    "sys_root = '$(host_prefix)'" \
    "c_args = ['-I$(host_prefix)/include']" \
    "cpp_args = ['-I$(host_prefix)/include']" \
    "c_link_args = ['-L$(host_prefix)/lib']" \
    "cpp_link_args = ['-L$(host_prefix)/lib']" \
    > $(host_prefix)/meson-cross.txt; \
  pyver="`python3 -c 'import sys; print(\"%s.%s\" % (sys.version_info[0], sys.version_info[1]))'`"; \
  export PKG_CONFIG_PATH="$(host_prefix)/lib/pkgconfig:$(host_prefix)/share/pkgconfig"; \
  export PKG_CONFIG_LIBDIR="$(host_prefix)/lib/pkgconfig"; \
  PYTHONPATH="$(build_prefix)/local/lib/python$$$$pyver/dist-packages:$(build_prefix)/local/lib/python$$$$pyver/site-packages" \
    python3 -m mesonbuild.mesonmain setup .. \
    --cross-file $(host_prefix)/meson-cross.txt \
    --prefix $(host_prefix) \
    --libdir lib \
    --buildtype release \
    -Ddefault_library=static \
    -Denable-x11=true \
    -Denable-wayland=false \
    -Denable-docs=false \
    -Denable-tools=false \
    -Denable-xkbregistry=false \
    -Dprefer_static=true
endef
else
define $(package)_config_cmds
  pyver="`python3 -c 'import sys; print(\"%s.%s\" % (sys.version_info[0], sys.version_info[1]))'`"; \
  export PKG_CONFIG_PATH="$(host_prefix)/lib/pkgconfig:$(host_prefix)/share/pkgconfig"; \
  export PKG_CONFIG_LIBDIR="$(host_prefix)/lib/pkgconfig"; \
  CC="$(firstword $($(package)_cc))" \
  CXX="$(firstword $($(package)_cxx))" \
  CFLAGS="$(wordlist 2,$(words $($(package)_cc)),$($(package)_cc)) $($(package)_cflags) $($(package)_cppflags)" \
  CXXFLAGS="$(wordlist 2,$(words $($(package)_cxx)),$($(package)_cxx)) $($(package)_cxxflags) $($(package)_cppflags)" \
  PYTHONPATH="$(build_prefix)/local/lib/python$$$$pyver/dist-packages:$(build_prefix)/local/lib/python$$$$pyver/site-packages" \
    python3 -m mesonbuild.mesonmain setup .. \
    --prefix $(host_prefix) \
    --libdir lib \
    --buildtype release \
    -Ddefault_library=static \
    -Denable-x11=true \
    -Denable-wayland=false \
    -Denable-docs=false \
    -Denable-tools=false \
    -Denable-xkbregistry=false \
    -Dprefer_static=true
endef
endif

define $(package)_build_cmds
  ninja $(if $(JOBS),-j$(JOBS),)
endef

define $(package)_stage_cmds
  pyver="`python3 -c 'import sys; print(\"%s.%s\" % (sys.version_info[0], sys.version_info[1]))'`"; \
  PYTHONPATH="$(build_prefix)/local/lib/python$$$$pyver/dist-packages:$(build_prefix)/local/lib/python$$$$pyver/site-packages" \
    DESTDIR=$($(package)_staging_dir) python3 -m mesonbuild.mesonmain install --no-rebuild
endef

define $(package)_postprocess_cmds
  rm -f lib/*.la
endef
