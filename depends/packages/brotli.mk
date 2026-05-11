package=brotli
$(package)_version=1.1.0
$(package)_download_path=https://github.com/google/brotli/archive/refs/tags
$(package)_file_name=v$($(package)_version).tar.gz
$(package)_sha256_hash=e720a6ca29428b803f4ad165371771f5398faba397edf6778837a18599ea13ff
$(package)_build_subdir=build

define $(package)_preprocess_cmds
  mkdir -p build
endef

define $(package)_config_cmds
  cmake -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX:PATH=$(host_prefix) \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DBUILD_SHARED_LIBS=OFF \
    -DBROTLI_DISABLE_TESTS=ON \
    -DBROTLI_DISABLE_INSTALL=OFF \
    -DCMAKE_C_COMPILER="$(firstword $($(package)_cc))" \
    -DCMAKE_C_FLAGS="$(wordlist 2,$(words $($(package)_cc)),$($(package)_cc)) $($(package)_cflags) $($(package)_cppflags)" \
    -DCMAKE_CXX_COMPILER="$(firstword $($(package)_cxx))" \
    -DCMAKE_CXX_FLAGS="$(wordlist 2,$(words $($(package)_cxx)),$($(package)_cxx)) $($(package)_cxxflags) $($(package)_cppflags)" \
    -DCMAKE_FIND_ROOT_PATH="$(host_prefix)" \
    -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
    -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
    -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
    -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY \
    ..
endef

define $(package)_build_cmds
  ninja $(if $(JOBS),-j$(JOBS),)
endef

define $(package)_stage_cmds
  DESTDIR=$($(package)_staging_dir) ninja install
endef

define $(package)_postprocess_cmds
  rm -f lib/*.la
endef
