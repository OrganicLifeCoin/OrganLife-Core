dnl Copyright (c) 2013-2016 The Bitcoin Core developers
dnl Distributed under the MIT software license, see the accompanying
dnl file COPYING or http://www.opensource.org/licenses/mit-license.php.

dnl Helper for cases where a Qt dependency is not met.
dnl Output: If Qt version is auto, set bitcoin_enable_qt to false. Else, exit.
AC_DEFUN([BITCOIN_QT_FAIL],[
  if test "x$bitcoin_qt_want_version" = xauto && test "x$bitcoin_qt_force" != xyes; then
    if test "x$bitcoin_enable_qt" != xno; then
      AC_MSG_WARN([$1; cteam-qt frontend will not be built])
    fi
    bitcoin_enable_qt=no
    bitcoin_enable_qt_test=no
  else
    AC_MSG_ERROR([$1])
  fi
])

AC_DEFUN([BITCOIN_QT_CHECK],[
  if test "x$bitcoin_enable_qt" != xno && test "x$bitcoin_qt_want_version" != xno; then
    true
    $1
  else
    true
    $2
  fi
])

dnl BITCOIN_QT_PATH_PROGS([FOO], [foo foo2], [/path/to/search/first], [continue if missing])
dnl Helper for finding the path of programs needed for Qt.
dnl Inputs: $1: Variable to be set
dnl Inputs: $2: List of programs to search for
dnl Inputs: $3: Look for $2 here before $PATH
dnl Inputs: $4: If "yes", don't fail if $2 is not found.
dnl Output: $1 is set to the path of $2 if found. $2 are searched in order.
AC_DEFUN([BITCOIN_QT_PATH_PROGS],[
  BITCOIN_QT_CHECK([
    if test "x$3" != x; then
      AC_PATH_PROGS([$1], [$2], [], [$3])
    else
      AC_PATH_PROGS([$1], [$2])
    fi
    if test "x$$1" = x && test "x$4" != xyes; then
      BITCOIN_QT_FAIL([$1 not found])
    fi
  ])
])

dnl Initialize Qt input.
dnl This must be called before any other BITCOIN_QT* macros to ensure that
dnl input variables are set correctly.
dnl CAUTION: Do not use this inside of a conditional.
AC_DEFUN([BITCOIN_QT_INIT],[
  dnl enable Qt support
  AC_ARG_WITH([gui],
    [AS_HELP_STRING([--with-gui@<:@=no|qt6|qt5|auto@:>@],
    [build cteam-qt GUI (default=auto)])],
    [
     bitcoin_qt_want_version=$withval
     if test "x$bitcoin_qt_want_version" = xyes; then
       bitcoin_qt_force=yes
       bitcoin_qt_want_version=auto
     fi
    ],
    [bitcoin_qt_want_version=qt6])

  AC_ARG_WITH([qt-incdir], [AS_HELP_STRING([--with-qt-incdir=INC_DIR], [specify Qt include path (overridden by pkgconfig)])], [qt_include_path=$withval], [])
  AC_ARG_WITH([qt-libdir], [AS_HELP_STRING([--with-qt-libdir=LIB_DIR], [specify Qt lib path (overridden by pkgconfig)])], [qt_lib_path=$withval], [])
  AC_ARG_WITH([qt-plugindir], [AS_HELP_STRING([--with-qt-plugindir=PLUGIN_DIR], [specify Qt plugin path (overridden by pkgconfig)])], [qt_plugin_path=$withval], [])
  AC_ARG_WITH([qt-translationdir], [AS_HELP_STRING([--with-qt-translationdir=PLUGIN_DIR], [specify Qt translation path (overridden by pkgconfig)])], [qt_translation_path=$withval], [])
  AC_ARG_WITH([qt-svgdir], [AS_HELP_STRING([--with-qt-svgdir=PLUGIN_DIR], [specify Qt svg path (overridden by pkgconfig)])], [qt_svg_path=$withval], [])
  AC_ARG_WITH([qt-bindir], [AS_HELP_STRING([--with-qt-bindir=BIN_DIR], [specify Qt bin path])], [qt_bin_path=$withval], [])

  AC_ARG_WITH([qtdbus],
    [AS_HELP_STRING([--with-qtdbus],
    [enable DBus support (default is yes if Qt is enabled and QtDBus is found)])],
    [use_dbus=$withval],
    [use_dbus=auto])

  AC_SUBST(QT_TRANSLATION_DIR,$qt_translation_path)
])

dnl Find the appropriate version of Qt libraries and includes.
dnl Inputs: $1: Whether or not pkg-config should be used. yes|no. Default: yes.
dnl Outputs: See _BITCOIN_QT_FIND_LIBS_*
dnl Outputs: Sets variables for all Qt-related tools.
dnl Outputs: bitcoin_enable_qt, bitcoin_enable_qt_dbus, bitcoin_enable_qt_test
AC_DEFUN([BITCOIN_QT_CONFIGURE],[
  use_pkgconfig=$1

  if test "x$use_pkgconfig" = x; then
    use_pkgconfig=yes
  fi

  if test "x$use_pkgconfig" = xyes; then
    BITCOIN_QT_CHECK([_BITCOIN_QT_FIND_LIBS_WITH_PKGCONFIG])
  else
    BITCOIN_QT_CHECK([_BITCOIN_QT_FIND_LIBS_WITHOUT_PKGCONFIG])
  fi

  BITCOIN_QT_CHECK([
    if test "x$bitcoin_cv_qt_minimumrequired" != xyes; then
      BITCOIN_QT_FAIL([Qt version not supported])
    fi
  ])

  dnl This is ugly and complicated. Yuck. Works as follows:
  dnl For Qt5, we can check a header to find out whether Qt is build
  dnl statically. When Qt is built statically, some plugins must be linked into
  dnl the final binary as well.
  dnl With Qt5, languages moved into core and the WindowsIntegration plugin was
  dnl added.
  dnl _BITCOIN_QT_CHECK_STATIC_PLUGINS does a quick link-check and appends the
  dnl results to QT_LIBS.
  BITCOIN_QT_CHECK([
  TEMP_CPPFLAGS=$CPPFLAGS
  TEMP_CXXFLAGS=$CXXFLAGS
  CPPFLAGS="$QT_INCLUDES $QT_SVG_INCLUDES $CPPFLAGS"
  CXXFLAGS="$PIC_FLAGS $CXXFLAGS"
  _BITCOIN_QT_IS_STATIC
  if test "x$bitcoin_cv_static_qt" = xyes; then
    _BITCOIN_QT_FIND_STATIC_PLUGINS
    AC_DEFINE([QT_STATICPLUGIN], [1], [Define this symbol if Qt plugins are static])
    if test "x$QT_LIB_PREFIX" = xQt6 && type qt6_prl_libs >/dev/null 2>&1; then
      dnl Qt6 cross builds may name .prl files without the "lib" prefix (e.g.
      dnl "qjpeg.prl" instead of "libqjpeg.prl").  Try both conventions.
      qt6_try_prl() {
        if test -f "[$]1"; then qt6_prl_libs "[$]1"
        elif test -f "[$]2"; then qt6_prl_libs "[$]2"
        fi
      }
      qt6_qminimal_plugin_libs=$(qt6_try_prl "$qt_plugin_path/platforms/libqminimal.prl" "$qt_plugin_path/platforms/qminimal.prl")
      qt6_qjpeg_plugin_libs=$(qt6_try_prl "$qt_plugin_path/imageformats/libqjpeg.prl" "$qt_plugin_path/imageformats/qjpeg.prl")
      qt6_qico_plugin_libs=$(qt6_try_prl "$qt_plugin_path/imageformats/libqico.prl" "$qt_plugin_path/imageformats/qico.prl")
      qt6_qsvg_plugin_libs=$(qt6_try_prl "$qt_plugin_path/imageformats/libqsvg.prl" "$qt_plugin_path/imageformats/qsvg.prl")
      qt6_qgif_plugin_libs=$(qt6_try_prl "$qt_plugin_path/imageformats/libqgif.prl" "$qt_plugin_path/imageformats/qgif.prl")
      qt6_qsvgicon_plugin_libs=$(qt6_try_prl "$qt_plugin_path/iconengines/libqsvgicon.prl" "$qt_plugin_path/iconengines/qsvgicon.prl")
      qt6_qwindows_plugin_libs=$(qt6_try_prl "$qt_plugin_path/platforms/libqwindows.prl" "$qt_plugin_path/platforms/qwindows.prl")
      qt6_qxcb_plugin_libs=$(qt6_try_prl "$qt_plugin_path/platforms/libqxcb.prl" "$qt_plugin_path/platforms/qxcb.prl")
      qt6_qcocoa_plugin_libs=$(qt6_try_prl "$qt_plugin_path/platforms/libqcocoa.prl" "$qt_plugin_path/platforms/qcocoa.prl")
    fi

    if test "x$QT_LIB_PREFIX" = xQt6 && test -n "$qt6_qminimal_plugin_libs"; then
      _BITCOIN_QT_CHECK_STATIC_PLUGINS([Q_IMPORT_PLUGIN(QMinimalIntegrationPlugin)],[$qt6_qminimal_plugin_libs])
    else
      _BITCOIN_QT_CHECK_STATIC_PLUGINS([Q_IMPORT_PLUGIN(QMinimalIntegrationPlugin)],[-lqminimal])
    fi
    AC_DEFINE([QT_QPA_PLATFORM_MINIMAL], [1], [Define this symbol if the minimal Qt platform exists])
    if test "x$QT_LIB_PREFIX" = xQt6 && test -n "$qt6_qjpeg_plugin_libs"; then
      _BITCOIN_QT_CHECK_STATIC_PLUGINS([Q_IMPORT_PLUGIN(QJpegPlugin)],[$qt6_qjpeg_plugin_libs])
    else
      _BITCOIN_QT_CHECK_STATIC_PLUGINS([Q_IMPORT_PLUGIN(QJpegPlugin)],[-lqjpeg])
    fi
    if test "x$QT_LIB_PREFIX" = xQt6 && test -n "$qt6_qico_plugin_libs"; then
      _BITCOIN_QT_CHECK_STATIC_PLUGINS([Q_IMPORT_PLUGIN(QICOPlugin)],[$qt6_qico_plugin_libs])
    else
      _BITCOIN_QT_CHECK_STATIC_PLUGINS([Q_IMPORT_PLUGIN(QICOPlugin)],[-lqico])
    fi
    if test "x$QT_LIB_PREFIX" = xQt6 && test -n "$qt6_qsvg_plugin_libs"; then
      _BITCOIN_QT_CHECK_STATIC_PLUGINS([Q_IMPORT_PLUGIN(QSvgPlugin)],[$qt6_qsvg_plugin_libs])
    else
      _BITCOIN_QT_CHECK_STATIC_PLUGINS([Q_IMPORT_PLUGIN(QSvgPlugin)],[-lqsvg])
    fi
    if test "x$QT_LIB_PREFIX" = xQt6 && test -n "$qt6_qgif_plugin_libs"; then
      _BITCOIN_QT_CHECK_STATIC_PLUGINS([Q_IMPORT_PLUGIN(QGifPlugin)],[$qt6_qgif_plugin_libs])
    else
      _BITCOIN_QT_CHECK_STATIC_PLUGINS([Q_IMPORT_PLUGIN(QGifPlugin)],[-lqgif])
    fi
    if test "x$QT_LIB_PREFIX" = xQt6 && test -n "$qt6_qsvgicon_plugin_libs"; then
      _BITCOIN_QT_CHECK_STATIC_PLUGINS([Q_IMPORT_PLUGIN(QSvgIconPlugin)],[$qt6_qsvgicon_plugin_libs])
    else
      _BITCOIN_QT_CHECK_STATIC_PLUGINS([Q_IMPORT_PLUGIN(QSvgIconPlugin)],[-lqsvgicon])
    fi
    if test "x$TARGET_OS" = xwindows; then
      if test "x$QT_LIB_PREFIX" = xQt6 && test -n "$qt6_qwindows_plugin_libs"; then
        _BITCOIN_QT_CHECK_STATIC_PLUGINS([Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin)],[$qt6_qwindows_plugin_libs])
      else
        _BITCOIN_QT_CHECK_STATIC_PLUGINS([Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin)],[-lqwindows])
      fi
      AC_DEFINE([QT_QPA_PLATFORM_WINDOWS], [1], [Define this symbol if the Qt platform is Windows])
    elif test "x$TARGET_OS" = xlinux; then
      if test "x$QT_LIB_PREFIX" = xQt6 && test -n "$qt6_qxcb_plugin_libs"; then
        _BITCOIN_QT_CHECK_STATIC_PLUGINS([Q_IMPORT_PLUGIN(QXcbIntegrationPlugin)],[$qt6_qxcb_plugin_libs])
        AC_DEFINE([QT_QPA_PLATFORM_XCB], [1], [Define this symbol if the Qt platform is XCB])
      else
        if test -f "$qt_plugin_path/platforms/libqxcb.a" || test -f "$qt_plugin_path/platforms/libqxcb.prl" || test -f "$qt_plugin_path/platforms/qxcb.prl"; then
          _BITCOIN_QT_CHECK_STATIC_PLUGINS([Q_IMPORT_PLUGIN(QXcbIntegrationPlugin)],[-lqxcb -lxcb-static])
          AC_DEFINE([QT_QPA_PLATFORM_XCB], [1], [Define this symbol if the Qt platform is XCB])
        else
          AC_MSG_WARN([Qt XCB platform plugin not found; continuing without XCB.])
        fi
      fi
    elif test "x$TARGET_OS" = xdarwin; then
      if test "x$QT_LIB_PREFIX" = xQt6 && test -n "$qt6_qcocoa_plugin_libs"; then
        _BITCOIN_QT_CHECK_STATIC_PLUGINS([Q_IMPORT_PLUGIN(QCocoaIntegrationPlugin)],[$qt6_qcocoa_plugin_libs])
      else
        _BITCOIN_QT_CHECK_STATIC_PLUGINS([Q_IMPORT_PLUGIN(QCocoaIntegrationPlugin)],[-lqcocoa])
      fi
      AC_DEFINE([QT_QPA_PLATFORM_COCOA], [1], [Define this symbol if the Qt platform is Cocoa])
    fi
  fi
  CPPFLAGS=$TEMP_CPPFLAGS
  CXXFLAGS=$TEMP_CXXFLAGS
  ])

  if test "x$use_pkgconfig" = xyes; then
    if test "x$qt_bin_path" = x; then
      qt_bin_path="`$PKG_CONFIG --variable=host_bins ${QT_LIB_PREFIX}Core 2>/dev/null`"
      if test "x$qt_bin_path" = x; then
        qt_bin_path="`$PKG_CONFIG --variable=bindir ${QT_LIB_PREFIX}Core 2>/dev/null`"
      fi
    fi

    qt_libexec_path="`$PKG_CONFIG --variable=host_libexecs ${QT_LIB_PREFIX}Core 2>/dev/null`"
    if test "x$qt_libexec_path" = x; then
      qt_libexec_path="`$PKG_CONFIG --variable=libexecdir ${QT_LIB_PREFIX}Core 2>/dev/null`"
    fi

    if test "x$qt_libexec_path" != x; then
      case ":$qt_bin_path:" in
        *":$qt_libexec_path:"*) ;;
        *)
          if test "x$qt_bin_path" != x; then
            qt_bin_path="$qt_bin_path:$qt_libexec_path"
          else
            qt_bin_path="$qt_libexec_path"
          fi
        ;;
      esac
    fi
  fi

  if test "x$use_hardening" != xno; then
    BITCOIN_QT_CHECK([
    AC_MSG_CHECKING([whether -fPIE can be used with this Qt config])
    TEMP_CPPFLAGS=$CPPFLAGS
    TEMP_CXXFLAGS=$CXXFLAGS
    CPPFLAGS="$QT_INCLUDES $CPPFLAGS"
    CXXFLAGS="$PIE_FLAGS $CXXFLAGS"
    AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
        #include <QtCore/qconfig.h>
        #ifndef QT_VERSION
        #  include <QtCore/qglobal.h>
        #endif
      ]],
      [[
        #if defined(QT_REDUCE_RELOCATIONS)
        choke
        #endif
      ]])],
      [ AC_MSG_RESULT([yes]); QT_PIE_FLAGS=$PIE_FLAGS ],
      [ AC_MSG_RESULT([no]); QT_PIE_FLAGS=$PIC_FLAGS]
    )
    CPPFLAGS=$TEMP_CPPFLAGS
    CXXFLAGS=$TEMP_CXXFLAGS
    ])
  else
    BITCOIN_QT_CHECK([
    AC_MSG_CHECKING([whether -fPIC is needed with this Qt config])
    TEMP_CPPFLAGS=$CPPFLAGS
    CPPFLAGS="$QT_INCLUDES $CPPFLAGS"
    AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
        #include <QtCore/qconfig.h>
        #ifndef QT_VERSION
        #  include <QtCore/qglobal.h>
        #endif
      ]],
      [[
        #if defined(QT_REDUCE_RELOCATIONS)
        choke
        #endif
      ]])],
      [ AC_MSG_RESULT([no])],
      [ AC_MSG_RESULT([yes]); QT_PIE_FLAGS=$PIC_FLAGS]
    )
    CPPFLAGS=$TEMP_CPPFLAGS
    ])
  fi

  BITCOIN_QT_PATH_PROGS([MOC], [moc-qt6 moc6 moc-qt5 moc5 moc], $qt_bin_path)
  BITCOIN_QT_PATH_PROGS([UIC], [uic-qt6 uic6 uic-qt5 uic5 uic], $qt_bin_path)
  dnl Note: Use QT_RCC (not RCC) to avoid clashing with common env vars (RCC)
  dnl and toolchains that set RCC to a Windows resource compiler (rc), which
  dnl would break Qt resource compilation (qrc_*.cpp generation).
  BITCOIN_QT_PATH_PROGS([QT_RCC], [rcc-qt6 rcc6 rcc-qt5 rcc5 rcc], $qt_bin_path)
  BITCOIN_QT_PATH_PROGS([LRELEASE], [lrelease-qt6 lrelease6 lrelease-qt5 lrelease5 lrelease], $qt_bin_path)
  BITCOIN_QT_PATH_PROGS([LUPDATE], [lupdate-qt6 lupdate6 lupdate-qt5 lupdate5 lupdate],$qt_bin_path, yes)

  MOC_DEFS='-DHAVE_CONFIG_H -I$(srcdir)'
  case $host in
    *darwin*)
     BITCOIN_QT_CHECK([
       MOC_DEFS="${MOC_DEFS} -DQ_OS_MAC"
       base_frameworks="-framework Foundation -framework AppKit"
       AX_CHECK_LINK_FLAG([$base_frameworks], [QT_LIBS="$QT_LIBS $base_frameworks"], [AC_MSG_ERROR([could not find base frameworks])])
     ])
    ;;
    *mingw*)
       BITCOIN_QT_CHECK([
         AX_CHECK_LINK_FLAG([-mwindows], [QT_LDFLAGS="$QT_LDFLAGS -mwindows"], [AC_MSG_WARN([-mwindows linker support not detected])])
       ])
  esac

  dnl enable Qt support
  AC_MSG_CHECKING([whether to build ]AC_PACKAGE_NAME[ GUI])
  BITCOIN_QT_CHECK([
    bitcoin_enable_qt=yes
    bitcoin_enable_qt_test=yes
    if test "x$have_qt_test" = xno; then
      bitcoin_enable_qt_test=no
    fi
    bitcoin_enable_qt_dbus=no
    if test "x$use_dbus" != xno && test "x$have_qt_dbus" = xyes; then
      bitcoin_enable_qt_dbus=yes
    fi
    if test "x$use_dbus" = xyes && test "x$have_qt_dbus" = xno; then
      AC_MSG_ERROR([libQtDBus not found. Install libQtDBus or remove --with-qtdbus.])
    fi
    if test "x$LUPDATE" = x; then
      AC_MSG_WARN([lupdate is required to update Qt translations])
    fi
  ],[
    bitcoin_enable_qt=no
  ])
  AC_MSG_RESULT([$bitcoin_enable_qt ($QT_LIB_PREFIX)])

  AC_SUBST(QT_PIE_FLAGS)
  AC_SUBST(QT_INCLUDES)
  AC_SUBST(QT_LIBS)
  AC_SUBST(QT_LDFLAGS)
  AC_SUBST(QT_SVG_INCLUDES)
  AC_SUBST(QT_SVG_LIBS)
  AC_SUBST(QT_DBUS_INCLUDES)
  AC_SUBST(QT_DBUS_LIBS)
  AC_SUBST(QT_TEST_INCLUDES)
  AC_SUBST(QT_TEST_LIBS)
  if test "x$qt_select" = x; then
    qt_select=qt5
  fi
  QT_SELECT=$qt_select
  AC_SUBST(QT_SELECT)
  AC_SUBST(MOC_DEFS)
])

dnl All macros below are internal and should _not_ be used from the main
dnl configure.ac.
dnl ----

dnl Internal. Check the included version of Qt against the passed minimum one.
dnl _BITCOIN_QT_VERSIONCHECK_PROGRAM([VERSION-MAJOR-NUMBER], [VERSION-MINOR-NUMBER], [VERSION-PATCH-NUMBER])
dnl Requires: INCLUDES must be populated as necessary.
m4_define([_BITCOIN_QT_VERSIONCHECK_PROGRAM],[
  AC_LANG_PROGRAM([[
    #include <QtGlobal>
  ]],[[
    #if (QT_VERSION < QT_VERSION_CHECK($1, $2, $3))
    choke
    #endif
  ]])
])

dnl Internal. Check included version of Qt against minimum specified in doc/dependencies.md
dnl Requires: INCLUDES must be populated as necessary.
dnl Output: bitcoin_cv_qt_minimumrequired=yes|no
AC_DEFUN([_BITCOIN_QT_CHECK_MINIMUM_REQUIRED],[
  AC_CACHE_CHECK([for Qt >= 5.5.1], bitcoin_cv_qt_minimumrequired,[
  AC_COMPILE_IFELSE([_BITCOIN_QT_VERSIONCHECK_PROGRAM(5,5,1)],
    [bitcoin_cv_qt_minimumrequired=yes],
    [bitcoin_cv_qt_minimumrequired=no])
])])

dnl Internal. Check if the included version of Qt is >= 5.8.
dnl Requires: INCLUDES must be populated as necessary.
dnl Output: bitcoin_cv_qt58=yes|no
AC_DEFUN([_BITCOIN_QT_CHECK_QT58],[
  AC_CACHE_CHECK([for Qt >= 5.8], bitcoin_cv_qt58,[
  AC_COMPILE_IFELSE([_BITCOIN_QT_VERSIONCHECK_PROGRAM(5,8,0)],
    [bitcoin_cv_qt58=yes],
    [bitcoin_cv_qt58=no])
])])


dnl Internal. Check if the linked version of Qt was built as static libs.
dnl Requires: Qt5.
dnl Requires: INCLUDES and LIBS must be populated as necessary.
dnl Output: bitcoin_cv_static_qt=yes|no
AC_DEFUN([_BITCOIN_QT_IS_STATIC],[
  AC_CACHE_CHECK([for static Qt], bitcoin_cv_static_qt,[
    AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
        #include <QtCore/qconfig.h>
        #ifndef QT_VERSION
        #  include <QtCore/qglobal.h>
        #endif
      ]],
      [[
        #if !defined(QT_STATIC)
        choke
        #endif
      ]])],
      [bitcoin_cv_static_qt=yes],
      [bitcoin_cv_static_qt=no])
    ])
])

dnl Internal. Check if the link-requirements for static plugins are met.
dnl Requires: INCLUDES and LIBS must be populated as necessary.
dnl Inputs: $1: A series of Q_IMPORT_PLUGIN().
dnl Inputs: $2: The libraries that resolve $1.
dnl Output: QT_LIBS is prepended or configure exits.
AC_DEFUN([_BITCOIN_QT_CHECK_STATIC_PLUGINS],[
  dnl Copy M4 arg $2 into a shell variable. Important: use "$2" (M4 expansion),
  dnl not "[$]2" (shell positional parameter), otherwise configure can end up
  dnl checking something unrelated like "qmake6".
  plugin_libs="$2"
  dnl Some Qt6 .prl files can leak Unix libs into cross builds. Strip them here
  dnl as a last line of defense (qt6_prl_libs already tries to do this).
  if test "x$TARGET_OS" = xwindows; then
    changequote({,})
    plugin_libs=$(echo "$plugin_libs" | tr '\r\n\t' '   ' | sed 's|[^ ]*/objects-[^ ]*||g')
    changequote([,])
    dnl Use word-boundary matching to avoid stripping -lm from -lmpr etc.
    plugin_libs=" $plugin_libs "
    plugin_libs=$(echo "$plugin_libs" | sed 's/ -ldl / /g; s/ -lrt / /g; s/ -lresolv / /g; s/ -lm / /g; s/ -pthread / /g; s/ -lpthread / /g')
    plugin_libs=$(echo "$plugin_libs" | sed 's/^ *//; s/ *$//')
  fi

  AC_MSG_CHECKING([for static Qt plugins: ]$plugin_libs)
  CHECK_STATIC_PLUGINS_TEMP_LIBS="$LIBS"
  dnl Put QT_LIBS first so any required -L paths come before -l* entries in $2.
  dnl This matters for Qt6 .prl-derived plugin deps which can include e.g. "-lz".
  dnl For MinGW cross builds, wrap static archives in a start/end group to avoid
  dnl order-dependent resolution issues with many interdependent static libs.
  if test "x$TARGET_OS" = xwindows; then
    LIBS="-Wl,--start-group $QT_LIBS $plugin_libs -Wl,--end-group $LIBS"
  else
    LIBS="$QT_LIBS $plugin_libs $LIBS"
  fi
  AC_LINK_IFELSE([AC_LANG_PROGRAM([[
    #define QT_STATICPLUGIN
    #include <QtPlugin>
    $1]],
    [[return 0;]])],
    [AC_MSG_RESULT([yes]); QT_LIBS="$plugin_libs $QT_LIBS"],
    [AC_MSG_RESULT([no]); BITCOIN_QT_FAIL([Could not resolve: ]$plugin_libs)])
  LIBS="$CHECK_STATIC_PLUGINS_TEMP_LIBS"
])

dnl Internal. Find paths necessary for linking Qt static plugins
dnl Inputs: qt_plugin_path. optional.
dnl Outputs: QT_LIBS is appended
AC_DEFUN([_BITCOIN_QT_FIND_STATIC_PLUGINS],[
    if test "x$use_pkgconfig" = xyes && test "x$qt_plugin_path" = x; then
      qt_plugin_path="`$PKG_CONFIG --variable=prefix ${QT_LIB_PREFIX}Core 2>/dev/null`/plugins"
    fi
    if test "x$qt_plugin_path" != x; then
      QT_LIBS="$QT_LIBS -L$qt_plugin_path/platforms -L$qt_plugin_path/imageformats -L$qt_plugin_path/iconengines"
      if test -d "$qt_plugin_path/accessible"; then
        QT_LIBS="$QT_LIBS -L$qt_plugin_path/accessible"
      fi
     if test "x$use_pkgconfig" = xyes; then
     : dnl
     m4_ifdef([PKG_CHECK_MODULES],[
       if test x$bitcoin_cv_qt58 = xno; then
         PKG_CHECK_MODULES([QTPLATFORM], [Qt5PlatformSupport], [QT_LIBS="$QTPLATFORM_LIBS $QT_LIBS"])
       else
         PKG_CHECK_MODULES([QTFONTDATABASE], [Qt5FontDatabaseSupport], [QT_LIBS="-lQt5FontDatabaseSupport $QT_LIBS"], [:])
         PKG_CHECK_MODULES([QTEVENTDISPATCHER], [Qt5EventDispatcherSupport], [QT_LIBS="-lQt5EventDispatcherSupport $QT_LIBS"], [:])
         PKG_CHECK_MODULES([QTTHEME], [Qt5ThemeSupport], [QT_LIBS="-lQt5ThemeSupport $QT_LIBS"], [:])
         PKG_CHECK_MODULES([QTDEVICEDISCOVERY], [Qt5DeviceDiscoverySupport], [QT_LIBS="-lQt5DeviceDiscoverySupport $QT_LIBS"], [:])
         PKG_CHECK_MODULES([QTACCESSIBILITY], [Qt5AccessibilitySupport], [QT_LIBS="-lQt5AccessibilitySupport $QT_LIBS"], [:])
         PKG_CHECK_MODULES([QTFB], [Qt5FbSupport], [QT_LIBS="-lQt5FbSupport $QT_LIBS"], [:])
       fi
       if test "x$TARGET_OS" = xlinux; then
         if test "x$QT_LIB_PREFIX" = xQt5; then
           PKG_CHECK_MODULES([QTXCBQPA], [Qt5XcbQpa], [QT_LIBS="$QTXCBQPA_LIBS $QT_LIBS"])
         fi
	       elif test "x$TARGET_OS" = xdarwin; then
	         PKG_CHECK_MODULES([QTCLIPBOARD], [Qt5ClipboardSupport], [QT_LIBS="-lQt5ClipboardSupport $QT_LIBS"], [:])
	         PKG_CHECK_MODULES([QTGRAPHICS], [Qt5GraphicsSupport], [QT_LIBS="-lQt5GraphicsSupport $QT_LIBS"], [:])
	         PKG_CHECK_MODULES([QTCGL], [Qt5CglSupport], [QT_LIBS="-lQt5CglSupport $QT_LIBS"], [:])
	         QT_LIBS="$QT_LIBS -framework Carbon -framework QuartzCore -framework CoreVideo -framework IOSurface"
	       fi

       # Some Qt static builds (notably from this repo's depends system) don't ship pkg-config
       # files for the private *Support libraries, but still install the libraries themselves.
       # Ensure these are linked when present so static plugin link tests can succeed.
       if test "x$QT_LIB_PREFIX" = xQt5 && test "x$bitcoin_cv_qt58" = xyes; then
         qt5_libdir="`$PKG_CONFIG --variable=libdir ${QT_LIB_PREFIX}Core 2>/dev/null`"
         if test "x$qt5_libdir" != x; then
           for qt5_support_lib in FontDatabaseSupport EventDispatcherSupport ThemeSupport DeviceDiscoverySupport AccessibilitySupport FbSupport ClipboardSupport GraphicsSupport EdidSupport ServiceSupport; do
             if test -f "$qt5_libdir/lib${QT_LIB_PREFIX}${qt5_support_lib}.a" || test -f "$qt5_libdir/lib${QT_LIB_PREFIX}${qt5_support_lib}.dylib" || test -f "$qt5_libdir/lib${QT_LIB_PREFIX}${qt5_support_lib}.so"; then
               QT_LIBS="-l${QT_LIB_PREFIX}${qt5_support_lib} $QT_LIBS"
             fi
           done
         fi
       fi
     ])
     else
       if test "x$TARGET_OS" = xwindows; then
         if test "x$QT_LIB_PREFIX" = xQt5; then
           AC_CACHE_CHECK([for Qt >= 5.6], bitcoin_cv_need_platformsupport,[
             AC_COMPILE_IFELSE([_BITCOIN_QT_VERSIONCHECK_PROGRAM(5,6,0)],
             [bitcoin_cv_need_platformsupport=yes],
             [bitcoin_cv_need_platformsupport=no])
           ])
           if test "x$bitcoin_cv_need_platformsupport" = xyes; then
             if test x$bitcoin_cv_qt58 = xno; then
               BITCOIN_QT_CHECK(AC_CHECK_LIB([${QT_LIB_PREFIX}PlatformSupport], [main], [], [BITCOIN_QT_FAIL([lib$QT_LIB_PREFIXPlatformSupport not found])]))
             else
               BITCOIN_QT_CHECK(AC_CHECK_LIB([${QT_LIB_PREFIX}FontDatabaseSupport], [main], [], [BITCOIN_QT_FAIL([lib$QT_LIB_PREFIXFontDatabaseSupport not found])]))
               BITCOIN_QT_CHECK(AC_CHECK_LIB([${QT_LIB_PREFIX}EventDispatcherSupport], [main], [], [BITCOIN_QT_FAIL([lib$QT_LIB_PREFIXEventDispatcherSupport not found])]))
               BITCOIN_QT_CHECK(AC_CHECK_LIB([${QT_LIB_PREFIX}ThemeSupport], [main], [], [BITCOIN_QT_FAIL([lib$QT_LIB_PREFIXThemeSupport not found])]))
               BITCOIN_QT_CHECK(AC_CHECK_LIB([${QT_LIB_PREFIX}FbSupport], [main], [], [BITCOIN_QT_FAIL([lib$QT_LIB_PREFIXFbSupport not found])]))
               BITCOIN_QT_CHECK(AC_CHECK_LIB([${QT_LIB_PREFIX}DeviceDiscoverySupport], [main], [], [BITCOIN_QT_FAIL([lib$QT_LIB_PREFIXDeviceDiscoverySupport not found])]))
               BITCOIN_QT_CHECK(AC_CHECK_LIB([${QT_LIB_PREFIX}AccessibilitySupport], [main], [], [BITCOIN_QT_FAIL([lib$QT_LIB_PREFIXAccessibilitySupport not found])]))
               QT_LIBS="$QT_LIBS -lversion -ldwmapi -luxtheme -lkernel32 -luser32 -lgdi32 -lcomdlg32 -lwinspool -lshell32 -lole32 -loleaut32 -luuid -lws2_32 -ladvapi32 -lcrypt32 -liphlpapi -lshlwapi"
             fi
           fi
         else
           # Qt6 Windows specific system libraries
           QT_LIBS="$QT_LIBS -lversion -ldwmapi -luxtheme -lkernel32 -luser32 -lgdi32 -lcomdlg32 -lwinspool -lshell32 -lole32 -loleaut32 -luuid -lws2_32 -ladvapi32 -lcrypt32 -liphlpapi -lshlwapi -ld3d11 -ldxgi -ldxguid -ld3d12 -ldwrite -ld2d1 -lsynchronization -lmpr -luserenv -lauthz -lnetapi32 -lwinmm"
         fi
       fi
     fi
   fi
])

dnl Internal. Find Qt libraries using pkg-config.
dnl Outputs: All necessary QT_* variables are set.
dnl Outputs: have_qt_test and have_qt_dbus are set (if applicable) to yes|no.
AC_DEFUN([_BITCOIN_QT_FIND_LIBS_WITH_PKGCONFIG],[
  m4_ifdef([PKG_CHECK_MODULES],[
    qt_select=qt5

    if test "x$bitcoin_qt_want_version" = xqt6; then
      qt_select=qt6
    elif test "x$bitcoin_qt_want_version" = xauto; then
      PKG_CHECK_EXISTS([Qt6Core], [qt_select=qt6], [:])
      dnl If pkg-config data isn't available (common for static Qt6 depends builds),
      dnl probe qmake to see if a Qt6 installation exists.
      if test "x$qt_select" != xqt6; then
        QMAKE_PROBE=
        if test "x$qt_bin_path" != x; then
          AC_PATH_PROGS([QMAKE_PROBE], [qmake6 qmake], [], [$qt_bin_path])
        else
          AC_PATH_PROGS([QMAKE_PROBE], [qmake6 qmake])
        fi
	        if test "x$QMAKE_PROBE" != x; then
	          qt_ver=
	          qt_ver_out="conftest.qt_qmake_qt_version"
	          rm -f "$qt_ver_out"
	          bitcoin_qt_guard=no
	          case "$host" in
	            aarch64*-linux*)
	              if test "x$build" != "x$host"; then bitcoin_qt_guard=yes; fi
	            ;;
	          esac
	          if test "x$bitcoin_qt_guard" = xyes; then
	            dnl Guard against broken wrapper scripts that call themselves.
	            qt_probe_head="`LC_ALL=C head -n 1 \"$QMAKE_PROBE\" 2>/dev/null`"
	            case "$qt_probe_head" in
	              '#!'*)
	                if grep -F "target_qt.conf" "$QMAKE_PROBE" >/dev/null 2>&1; then
	                  QMAKE_PROBE=
	                elif grep -F "$QMAKE_PROBE" "$QMAKE_PROBE" >/dev/null 2>&1; then
	                  QMAKE_PROBE=
	                fi
	              ;;
	            esac
	          fi
	          qt_ver_pid=
	          if test "x$QMAKE_PROBE" != x; then
	            if test "x$bitcoin_qt_guard" != xyes; then
	              qt_ver="`$QMAKE_PROBE -query QT_VERSION 2>/dev/null`"
	              rm -f "$qt_ver_out"
	            else
	              if command -v setsid >/dev/null 2>&1; then
	                ( setsid "$QMAKE_PROBE" -query QT_VERSION >"$qt_ver_out" 2>/dev/null ) &
	              else
	                ( "$QMAKE_PROBE" -query QT_VERSION >"$qt_ver_out" 2>/dev/null ) &
	              fi
	              qt_ver_pid=$!
	              qt_ver_wait=0
	              while kill -0 $qt_ver_pid 2>/dev/null; do
	                qt_ver_wait=$((qt_ver_wait + 1))
	                if test $qt_ver_wait -ge 5; then
	                  kill -TERM -$qt_ver_pid 2>/dev/null
	                  kill $qt_ver_pid 2>/dev/null
	                  wait $qt_ver_pid 2>/dev/null
	                  rm -f "$qt_ver_out"
	                  qt_ver_pid=
	                  break
	                fi
	                sleep 1
	              done
	              if test -n "$qt_ver_pid"; then
	                wait $qt_ver_pid 2>/dev/null
	                qt_ver="$(cat "$qt_ver_out" 2>/dev/null | tr -d '\r\n')"
	                rm -f "$qt_ver_out"
	              fi
	            fi
	          else
	            rm -f "$qt_ver_out"
	          fi
          case "$qt_ver" in
            6.*) qt_select=qt6 ;;
          esac
        fi
      fi
    fi

    if test "x$qt_select" = xqt6; then
      QT_LIB_PREFIX=Qt6
      BITCOIN_QT_CHECK([
        PKG_CHECK_MODULES([QT6], [Qt6Core Qt6Gui Qt6Network Qt6Widgets], [QT_INCLUDES="$QT6_CFLAGS"; QT_LIBS="$QT6_LIBS" have_qt=yes],[have_qt=no])

        if test "x$have_qt" = xyes; then
          PKG_CHECK_MODULES([QT_SVG], [${QT_LIB_PREFIX}Svg], [QT_SVG_INCLUDES="$QT_SVG_CFLAGS"; QT_LIBS="$QT_LIBS $QT_SVG_LIBS"], [], [BITCOIN_QT_FAIL([QtSvg not found])])
          PKG_CHECK_MODULES([QT_CONCURRENT], [${QT_LIB_PREFIX}Concurrent], [QT_INCLUDES="$QT_INCLUDES $QT_CONCURRENT_CFLAGS"; QT_LIBS="$QT_LIBS $QT_CONCURRENT_LIBS"], [], [BITCOIN_QT_FAIL([QtConcurrent not found])])
          PKG_CHECK_MODULES([QT_OPENGL], [${QT_LIB_PREFIX}OpenGL], [have_qt_opengl=yes], [have_qt_opengl=no])
          PKG_CHECK_MODULES([QT_OPENGLWIDGETS], [${QT_LIB_PREFIX}OpenGLWidgets], [have_qt_openglwidgets=yes], [have_qt_openglwidgets=no])
          if test "x$have_qt_opengl" = xyes && test "x$have_qt_openglwidgets" = xyes; then
            QT_INCLUDES="$QT_INCLUDES $QT_OPENGL_CFLAGS $QT_OPENGLWIDGETS_CFLAGS -DUSE_GPU_TOPBAR_OVERLAY"
            QT_LIBS="$QT_LIBS $QT_OPENGL_LIBS $QT_OPENGLWIDGETS_LIBS"
          fi
          PKG_CHECK_MODULES([QT_TEST], [${QT_LIB_PREFIX}Test], [QT_TEST_INCLUDES="$QT_TEST_CFLAGS"; have_qt_test=yes], [have_qt_test=no])
          if test "x$use_dbus" != xno; then
            PKG_CHECK_MODULES([QT_DBUS], [${QT_LIB_PREFIX}DBus], [QT_DBUS_INCLUDES="$QT_DBUS_CFLAGS"; have_qt_dbus=yes], [have_qt_dbus=no])
          fi
          AC_CACHE_CHECK([for Qt >= 6.2.0], bitcoin_cv_qt_minimumrequired,
            PKG_CHECK_EXISTS(Qt6Core < 6.2.0, [bitcoin_cv_qt_minimumrequired=no], [bitcoin_cv_qt_minimumrequired=yes])
          )
          bitcoin_cv_qt58=yes
        else
          dnl Qt6 static builds from this repo's depends system do not ship pkg-config .pc files.
          dnl Fall back to qmake + .prl parsing to find include/link flags.
          _BITCOIN_QT6_FIND_LIBS_WITH_QMAKE
        fi
      ])
    else
      QT_LIB_PREFIX=Qt5
      BITCOIN_QT_CHECK([
        PKG_CHECK_MODULES([QT5], [Qt5Core Qt5Gui Qt5Network Qt5Widgets], [QT_INCLUDES="$QT5_CFLAGS"; QT_LIBS="$QT5_LIBS" have_qt=yes],[have_qt=no])

        if test "x$have_qt" != xyes; then
          have_qt=no
          BITCOIN_QT_FAIL([Qt dependencies not found])
        fi
      ])
      BITCOIN_QT_CHECK([
        PKG_CHECK_MODULES([QT_SVG], [${QT_LIB_PREFIX}Svg], [QT_SVG_INCLUDES="$QT_SVG_CFLAGS"; QT_LIBS="$QT_LIBS $QT_SVG_LIBS"], [], [BITCOIN_QT_FAIL([QtSvg not found])])
      ])
      BITCOIN_QT_CHECK([
        PKG_CHECK_MODULES([QT_CONCURRENT], [${QT_LIB_PREFIX}Concurrent], [QT_INCLUDES="$QT_INCLUDES $QT_CONCURRENT_CFLAGS"; QT_LIBS="$QT_LIBS $QT_CONCURRENT_LIBS"], [], [BITCOIN_QT_FAIL([QtConcurrent not found])])
      ])
      BITCOIN_QT_CHECK([
        PKG_CHECK_MODULES([QT_TEST], [${QT_LIB_PREFIX}Test], [QT_TEST_INCLUDES="$QT_TEST_CFLAGS"; have_qt_test=yes], [have_qt_test=no])
        if test "x$use_dbus" != xno; then
          PKG_CHECK_MODULES([QT_DBUS], [${QT_LIB_PREFIX}DBus], [QT_DBUS_INCLUDES="$QT_DBUS_CFLAGS"; have_qt_dbus=yes], [have_qt_dbus=no])
        fi
      ])
      BITCOIN_QT_CHECK([
        AC_CACHE_CHECK([for Qt >= 5.5.1], bitcoin_cv_qt_minimumrequired,
          PKG_CHECK_EXISTS(Qt5Core < 5.5.1, [bitcoin_cv_qt_minimumrequired=no], [bitcoin_cv_qt_minimumrequired=yes])
        )
      ])
      BITCOIN_QT_CHECK([
        AC_CACHE_CHECK([for Qt >= 5.8.0], bitcoin_cv_qt58,
          PKG_CHECK_EXISTS(Qt5Core < 5.8.0, [bitcoin_cv_qt58=no], [bitcoin_cv_qt58=yes])
        )
      ])
    fi
  ])
  true; dnl
])

dnl Internal. Find Qt libraries without using pkg-config. Version is deduced
dnl from the discovered headers.
dnl Inputs: bitcoin_qt_want_version (from --with-gui=). The version to use.
dnl         If "auto", the version will be discovered by _BITCOIN_QT_CHECK_MINIMUM_REQUIRED
dnl         and _BITCOIN_QT_CHECK_QT58.
dnl Outputs: All necessary QT_* variables are set.
dnl Outputs: have_qt_test and have_qt_dbus are set (if applicable) to yes|no.
AC_DEFUN([_BITCOIN_QT_FIND_LIBS_WITHOUT_PKGCONFIG],[
  TEMP_CPPFLAGS="$CPPFLAGS"
  TEMP_CXXFLAGS="$CXXFLAGS"
  CXXFLAGS="$PIC_FLAGS $CXXFLAGS"
  TEMP_LIBS="$LIBS"
  if test "x$bitcoin_qt_want_version" = xqt6; then
    BITCOIN_QT_CHECK([
      qt_select=qt6
      QT_LIB_PREFIX=Qt6
      _BITCOIN_QT6_FIND_LIBS_WITH_QMAKE
    ])
  else
  BITCOIN_QT_CHECK([
    if test "x$qt_include_path" != x; then
      QT_INCLUDES="-I$qt_include_path -I$qt_include_path/QtCore -I$qt_include_path/QtGui -I$qt_include_path/QtWidgets -I$qt_include_path/QtNetwork -I$qt_include_path/QtTest -I$qt_include_path/QtDBus -I$qt_include_path/QtConcurrent -I$qt_include_path/QtSvg -I$qt_include_path/QtCharts"
      CPPFLAGS="$QT_INCLUDES $CPPFLAGS"
    fi
  ])

  BITCOIN_QT_CHECK([AC_CHECK_HEADER([QtPlugin], [], [BITCOIN_QT_FAIL([QtCore headers missing])])])
  BITCOIN_QT_CHECK([AC_CHECK_HEADER([QApplication], [], [BITCOIN_QT_FAIL([QtGui headers missing])])])
  BITCOIN_QT_CHECK([AC_CHECK_HEADER([QLocalSocket], [], [BITCOIN_QT_FAIL([QtNetwork headers missing])])])
  BITCOIN_QT_CHECK([AC_CHECK_HEADER([QtConcurrent], [], [BITCOIN_QT_FAIL([QtConcurrent headers missing])])])

  BITCOIN_QT_CHECK([
    if test "x$bitcoin_qt_want_version" = xauto; then
      _BITCOIN_QT_CHECK_MINIMUM_REQUIRED
      _BITCOIN_QT_CHECK_QT58
    fi
    QT_LIB_PREFIX=Qt5
  ])

  BITCOIN_QT_CHECK([
    LIBS=
    if test "x$qt_lib_path" != x; then
      LIBS="$LIBS -L$qt_lib_path"
    fi

    if test "x$TARGET_OS" = xwindows; then
      AC_CHECK_LIB([imm32], [main], [], [BITCOIN_QT_FAIL([libimm32 not found])])
    fi
  ])

  BITCOIN_QT_CHECK(AC_CHECK_LIB([z], [main], [], [AC_MSG_WARN([zlib not found. Assuming Qt has it built-in])]))
  BITCOIN_QT_CHECK(AC_SEARCH_LIBS([jpeg_create_decompress], [qtjpeg jpeg], [], [AC_MSG_WARN([libjpeg not found. Assuming Qt has it built-in])]))
  if test x$bitcoin_cv_qt58 = xno; then
    BITCOIN_QT_CHECK(AC_SEARCH_LIBS([png_error], [qtpng png], [], [AC_MSG_WARN([libpng not found. Assuming Qt has it built-in])]))
    BITCOIN_QT_CHECK(AC_SEARCH_LIBS([pcre16_exec], [qtpcre pcre16], [], [AC_MSG_WARN([libpcre16 not found. Assuming Qt has it built-in])]))
  else
    BITCOIN_QT_CHECK(AC_SEARCH_LIBS([png_error], [qtlibpng png], [], [AC_MSG_WARN([libpng not found. Assuming Qt has it built-in])]))
    BITCOIN_QT_CHECK(AC_SEARCH_LIBS([pcre2_match_16], [qtpcre2 libqtpcre2], [], [AC_MSG_WARN([libqtpcre2 not found. Assuming Qt has it built-in])]))
  fi
  BITCOIN_QT_CHECK(AC_SEARCH_LIBS([hb_ot_tags_from_script], [qtharfbuzzng qtharfbuzz harfbuzz], [], [AC_MSG_WARN([libharfbuzz not found. Assuming Qt has it built-in or support is disabled])]))
  BITCOIN_QT_CHECK(AC_CHECK_LIB([${QT_LIB_PREFIX}Core], [main], [], [BITCOIN_QT_FAIL([lib${QT_LIB_PREFIX}Core not found])]))
  BITCOIN_QT_CHECK(AC_CHECK_LIB([${QT_LIB_PREFIX}Gui], [main], [], [BITCOIN_QT_FAIL([lib${QT_LIB_PREFIX}Gui not found])]))
  BITCOIN_QT_CHECK(AC_CHECK_LIB([${QT_LIB_PREFIX}Network], [main], [], [BITCOIN_QT_FAIL([lib${QT_LIB_PREFIX}Network not found])]))
  BITCOIN_QT_CHECK(AC_CHECK_LIB([${QT_LIB_PREFIX}Widgets], [main], [], [BITCOIN_QT_FAIL([lib${QT_LIB_PREFIX}Widgets not found])]))
  BITCOIN_QT_CHECK(AC_CHECK_LIB([${QT_LIB_PREFIX}Concurrent], [main], [], [BITCOIN_QT_FAIL([lib${QT_LIB_PREFIX}Concurrent not found])]))
  QT_LIBS="$LIBS"
  LIBS="$TEMP_LIBS"

  BITCOIN_QT_CHECK([
    LIBS=
    if test "x$qt_lib_path" != x; then
      LIBS="-L$qt_lib_path"
    fi
    AC_CHECK_LIB([${QT_LIB_PREFIX}Svg], [main], [], [BITCOIN_QT_FAIL([lib${QT_LIB_PREFIX}Svg not found])])
    AC_CHECK_HEADER([QtSvg], [], [BITCOIN_QT_FAIL([lib${QT_LIB_PREFIX}Svg not found])])
    QT_LIBS="$QT_LIBS $LIBS"
    QT_SVG_LIBS="$LIBS"
    LIBS=
    if test "x$qt_lib_path" != x; then
      LIBS="-L$qt_lib_path"
    fi
    AC_CHECK_LIB([${QT_LIB_PREFIX}Test], [main], [], [have_qt_test=no])
    AC_CHECK_HEADER([QTest], [], [have_qt_test=no])
    QT_TEST_LIBS="$LIBS"
    if test "x$use_dbus" != xno; then
      LIBS=
      if test "x$qt_lib_path" != x; then
        LIBS="-L$qt_lib_path"
      fi
      AC_CHECK_LIB([${QT_LIB_PREFIX}DBus], [main], [], [have_qt_dbus=no])
      AC_CHECK_HEADER([QtDBus], [], [have_qt_dbus=no])
      QT_DBUS_LIBS="$LIBS"
    fi
  ])
  fi
  CPPFLAGS="$TEMP_CPPFLAGS"
  CXXFLAGS="$TEMP_CXXFLAGS"
  LIBS="$TEMP_LIBS"
])

dnl Internal. Find Qt6 libraries using qmake + .prl parsing (for static builds without pkg-config).
dnl Requires: QT_LIB_PREFIX=Qt6.
dnl Outputs: QT_INCLUDES, QT_LIBS, QT_SVG_INCLUDES, QT_TEST_INCLUDES, have_qt_test, have_qt_dbus,
dnl          bitcoin_cv_qt_minimumrequired, bitcoin_cv_qt58 and have_qt.
AC_DEFUN([_BITCOIN_QT6_FIND_LIBS_WITH_QMAKE],[
  QMAKE=
  if test "x$qt_bin_path" != x; then
    AC_PATH_PROGS([QMAKE], [qmake6 qmake], [], [$qt_bin_path])
  else
    AC_PATH_PROGS([QMAKE], [qmake6 qmake])
  fi
  if test "x$QMAKE" = x; then
    BITCOIN_QT_FAIL([qmake not found (required for Qt6 builds without pkg-config)])
  fi

  qt6_prefix="`$QMAKE -query QT_INSTALL_PREFIX 2>/dev/null`"
  qt6_headers="`$QMAKE -query QT_INSTALL_HEADERS 2>/dev/null`"
  qt6_libs="`$QMAKE -query QT_INSTALL_LIBS 2>/dev/null`"
  qt6_plugins="`$QMAKE -query QT_INSTALL_PLUGINS 2>/dev/null`"
  qt6_bins="`$QMAKE -query QT_INSTALL_BINS 2>/dev/null`"
  qt6_libexecs="`$QMAKE -query QT_INSTALL_LIBEXECS 2>/dev/null`"
  if test -z "$qt6_prefix" || test -z "$qt6_headers" || test -z "$qt6_libs"; then
    BITCOIN_QT_FAIL([could not query Qt install paths via qmake])
  fi

  if test "x$qt_bin_path" = x && test -n "$qt6_bins"; then
    qt_bin_path="$qt6_bins"
  fi
  dnl Qt6 often installs moc/uic/rcc under libexec rather than bin. Make sure we search it too.
  if test -n "$qt6_libexecs"; then
    case ":$qt_bin_path:" in
      *":$qt6_libexecs:"*) ;;
      *)
        if test "x$qt_bin_path" = x; then
          qt_bin_path="$qt6_libexecs"
        else
          qt_bin_path="$qt_bin_path:$qt6_libexecs"
        fi
      ;;
    esac
  fi
  if test "x$qt_include_path" = x; then
    qt_include_path="$qt6_headers"
  fi
  if test "x$qt_lib_path" = x; then
    qt_lib_path="$qt6_libs"
  fi
  if test "x$qt_plugin_path" = x && test -n "$qt6_plugins"; then
    qt_plugin_path="$qt6_plugins"
  fi

      qt6_prl_libs() {
        prl_file="[$]1"
        test -f "$prl_file" || return 0
        prl_libs=$(sed -n 's/^QMAKE_PRL_LIBS *= *//p' "$prl_file" | head -n 1)
        prl_libs=$(printf "%s" "$prl_libs" | sed "s|\\$\\$\\@<:@QT_INSTALL_LIBS@:>@|$qt6_libs|g; s|\\$\\$\\@<:@QT_INSTALL_PREFIX@:>@|$qt6_prefix|g; s|\\$\\$\\@<:@QT_INSTALL_PLUGINS@:>@|$qt6_plugins|g")
        dnl Use changequote to protect sed regex from M4 processing
        changequote({,})
        prl_libs=$(echo "$prl_libs" | sed -E 's/(^|[[:space:]])-l([^[:space:]]+)\.a($|[[:space:]])/\1-l\2\3/g')
        changequote([,])

        dnl Filter out Unix-only libraries when cross-compiling for Windows.
        if test "x$TARGET_OS" = xwindows; then
          dnl Qt6 static builds sometimes list resource object files under QT_INSTALL_LIBS
          dnl (e.g. "$$[QT_INSTALL_LIBS]/objects-Release/.../*.o"). In cross builds these can
          dnl accidentally be built for the build machine (ELF) and then break our link tests.
          dnl Prefer linking the static libraries themselves and ignore these auxiliary objects.
          changequote({,})
          prl_libs=$(echo "$prl_libs" | sed 's|/objects-[^ ]*/[^ ]*\\.o||g')
          changequote([,])
          prl_libs=" $prl_libs "
          prl_libs=$(echo "$prl_libs" | sed 's/ -ldl / /g; s/ -lrt / /g; s/ -lresolv / /g; s/ -lm / /g; s/ -pthread / /g; s/ -lpthread / /g')
          prl_libs=$(echo "$prl_libs" | sed 's/^ *//; s/ *$//')
          prl_libs=$(echo "$prl_libs" | sed 's/-lxkbcommon//g; s/-lX11//g; s/-lXext//g; s/-lXrender//g; s/-lXinerama//g; s/-lXi//g; s/-lXcursor//g; s/-lXfixes//g; s/-lXrandr//g; s/-lXdamage//g; s/-lXcomposite//g; s/-lXss//g')
        fi

    printf "%s" "$prl_libs"
  }

  QT_INCLUDES="-I$qt_include_path -I$qt_include_path/QtCore -I$qt_include_path/QtGui -I$qt_include_path/QtWidgets -I$qt_include_path/QtNetwork -I$qt_include_path/QtConcurrent -I$qt_include_path/QtSvg -I$qt_include_path/QtTest -I$qt_include_path/QtCharts"
  QT_SVG_INCLUDES="-I$qt_include_path/QtSvg"
  QT_TEST_INCLUDES="-I$qt_include_path/QtTest"

  if test ! -f "$qt_lib_path/lib${QT_LIB_PREFIX}Core.a" && test ! -f "$qt_lib_path/lib${QT_LIB_PREFIX}Core.dylib" && test ! -f "$qt_lib_path/lib${QT_LIB_PREFIX}Core.so"; then
    BITCOIN_QT_FAIL([lib${QT_LIB_PREFIX}Core not found])
  fi
  if test ! -f "$qt_lib_path/lib${QT_LIB_PREFIX}Widgets.a" && test ! -f "$qt_lib_path/lib${QT_LIB_PREFIX}Widgets.dylib" && test ! -f "$qt_lib_path/lib${QT_LIB_PREFIX}Widgets.so"; then
    BITCOIN_QT_FAIL([lib${QT_LIB_PREFIX}Widgets not found])
  fi

  dnl Link the Qt6 module libraries themselves (static builds need explicit -lQt6Network, etc).
  dnl Then append each module's .prl dependency list (frameworks, bundled libs, resource objects).
  QT_LIBS="-L$qt_lib_path -l${QT_LIB_PREFIX}Svg -l${QT_LIB_PREFIX}Concurrent -l${QT_LIB_PREFIX}Widgets -l${QT_LIB_PREFIX}Network -l${QT_LIB_PREFIX}Gui -l${QT_LIB_PREFIX}Core"
  qt6_widgets_prl_libs=$(qt6_prl_libs "$qt_lib_path/lib${QT_LIB_PREFIX}Widgets.prl")
  qt6_network_prl_libs=$(qt6_prl_libs "$qt_lib_path/lib${QT_LIB_PREFIX}Network.prl")
  qt6_concurrent_prl_libs=$(qt6_prl_libs "$qt_lib_path/lib${QT_LIB_PREFIX}Concurrent.prl")
  qt6_svg_prl_libs=$(qt6_prl_libs "$qt_lib_path/lib${QT_LIB_PREFIX}Svg.prl")
  QT_LIBS="$QT_LIBS $qt6_widgets_prl_libs $qt6_network_prl_libs $qt6_concurrent_prl_libs $qt6_svg_prl_libs"
  if test "x$TARGET_OS" = xwindows; then
    QT_LIBS=" $QT_LIBS "
    QT_LIBS=$(echo "$QT_LIBS" | sed 's/ -ldl / /g; s/ -lrt / /g; s/ -lresolv / /g; s/ -lm / /g; s/ -pthread / /g; s/ -lpthread / /g')
    QT_LIBS=$(echo "$QT_LIBS" | sed 's/^ *//; s/ *$//')
  fi

  have_qt_test=yes
  if test ! -f "$qt_lib_path/lib${QT_LIB_PREFIX}Test.a" && test ! -f "$qt_lib_path/lib${QT_LIB_PREFIX}Test.dylib" && test ! -f "$qt_lib_path/lib${QT_LIB_PREFIX}Test.so"; then
    have_qt_test=no
  else
    dnl Qt .prl files contain link dependencies but not the module library itself.
    dnl Ensure we link Qt6Test plus its dependency list.
    QT_TEST_LIBS="-L$qt_lib_path -l${QT_LIB_PREFIX}Test $(qt6_prl_libs "$qt_lib_path/lib${QT_LIB_PREFIX}Test.prl")"
    if test "x$TARGET_OS" = xwindows; then
      QT_TEST_LIBS=" $QT_TEST_LIBS "
      QT_TEST_LIBS=$(echo "$QT_TEST_LIBS" | sed 's/ -ldl / /g; s/ -lrt / /g; s/ -lresolv / /g; s/ -lm / /g; s/ -pthread / /g; s/ -lpthread / /g')
      QT_TEST_LIBS=$(echo "$QT_TEST_LIBS" | sed 's/^ *//; s/ *$//')
    fi
  fi

  have_qt_dbus=no
  if test "x$use_dbus" != xno; then
    if test -f "$qt_lib_path/lib${QT_LIB_PREFIX}DBus.a" || test -f "$qt_lib_path/lib${QT_LIB_PREFIX}DBus.dylib" || test -f "$qt_lib_path/lib${QT_LIB_PREFIX}DBus.so"; then
      have_qt_dbus=yes
      QT_DBUS_INCLUDES="-I$qt_include_path/QtDBus"
      dnl Same as QtTest: link the module itself plus dependencies.
      QT_DBUS_LIBS="-L$qt_lib_path -l${QT_LIB_PREFIX}DBus $(qt6_prl_libs "$qt_lib_path/lib${QT_LIB_PREFIX}DBus.prl")"
      if test "x$TARGET_OS" = xwindows; then
        QT_DBUS_LIBS=" $QT_DBUS_LIBS "
        QT_DBUS_LIBS=$(echo "$QT_DBUS_LIBS" | sed 's/ -ldl / /g; s/ -lrt / /g; s/ -lresolv / /g; s/ -lm / /g; s/ -pthread / /g; s/ -lpthread / /g')
        QT_DBUS_LIBS=$(echo "$QT_DBUS_LIBS" | sed 's/^ *//; s/ *$//')
      fi
    fi
  fi

  TEMP_CPPFLAGS_QT6="$CPPFLAGS"
  CPPFLAGS="$QT_INCLUDES $CPPFLAGS"
  AC_CACHE_CHECK([for Qt >= 6.2.0], bitcoin_cv_qt_minimumrequired,[
    AC_COMPILE_IFELSE([_BITCOIN_QT_VERSIONCHECK_PROGRAM(6,2,0)],
      [bitcoin_cv_qt_minimumrequired=yes],
      [bitcoin_cv_qt_minimumrequired=no])
  ])
  CPPFLAGS="$TEMP_CPPFLAGS_QT6"

  bitcoin_cv_qt58=yes
  have_qt=yes
])
