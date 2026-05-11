build_darwin_CC:=$(shell xcrun -f clang) -isysroot$(shell xcrun --show-sdk-path)
build_darwin_CXX:=$(shell xcrun -f clang++) -isysroot$(shell xcrun --show-sdk-path)
build_darwin_AR:=$(shell xcrun -f ar)
build_darwin_RANLIB:=$(shell xcrun -f ranlib)
build_darwin_STRIP:=$(shell xcrun -f strip)
build_darwin_OTOOL:=$(shell xcrun -f otool)
build_darwin_NM:=$(shell xcrun -f nm)
build_darwin_INSTALL_NAME_TOOL:=$(shell xcrun -f install_name_tool)
build_darwin_SHA256SUM=shasum -a 256
build_darwin_DOWNLOAD=curl --location --fail --connect-timeout $(DOWNLOAD_CONNECT_TIMEOUT) --retry $(DOWNLOAD_RETRIES) -o

# darwin host on darwin builder. overrides darwin host preferences.
# Note: when building x86_64 on Apple Silicon without Rosetta, clang will default
# to arm64. Force the host architecture here so outputs land in the correct
# depends prefix (and avoid mixing architectures in the same tree).
darwin_arch:=$(host_arch)
ifeq ($(darwin_arch),aarch64)
darwin_arch:=arm64
endif
darwin_CC=$(shell xcrun -f clang) -arch $(darwin_arch) -mmacosx-version-min=$(OSX_MIN_VERSION) -isysroot$(shell xcrun --show-sdk-path)
darwin_CXX:=$(shell xcrun -f clang++) -arch $(darwin_arch) -mmacosx-version-min=$(OSX_MIN_VERSION) -stdlib=libc++ -isysroot$(shell xcrun --show-sdk-path)
darwin_AR:=$(shell xcrun -f ar)
darwin_RANLIB:=$(shell xcrun -f ranlib)
darwin_STRIP:=$(shell xcrun -f strip)
darwin_LIBTOOL:=$(shell xcrun -f libtool)
darwin_OTOOL:=$(shell xcrun -f otool)
darwin_NM:=$(shell xcrun -f nm)
darwin_INSTALL_NAME_TOOL:=$(shell xcrun -f install_name_tool)
darwin_native_binutils=
darwin_native_toolchain=
