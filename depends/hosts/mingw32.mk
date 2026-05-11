mingw32_CFLAGS=-pipe
mingw32_CXXFLAGS=$(mingw32_CFLAGS)

mingw32_release_CFLAGS=-O2
mingw32_release_CXXFLAGS=$(mingw32_release_CFLAGS)

mingw32_debug_CFLAGS=-O1
mingw32_debug_CXXFLAGS=$(mingw32_debug_CFLAGS)

mingw32_debug_CPPFLAGS=-D_GLIBCXX_DEBUG -D_GLIBCXX_DEBUG_PEDANTIC

mingw32_thread_model:=$(shell $(host)-g++ -v 2>&1 | sed -n 's/^Thread model: //p' | head -n 1)
ifneq (,$(findstring posix,$(mingw32_thread_model)))
mingw32_CFLAGS += -pthread
mingw32_CXXFLAGS += -pthread
mingw32_LDFLAGS += -pthread
endif
