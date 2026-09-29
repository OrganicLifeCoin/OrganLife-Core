#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
installer="$root/share/setup.nsi.in"
grep -Fxq 'Unicode true' "$installer"
grep -Fq '${IfNot} ${AtLeastBuild} 17763' "$installer"
grep -Fq 'SetErrorLevel 1633' "$installer"
grep -Fq 'File @abs_top_srcdir@/release/@BITCOIN_TX_NAME@@EXEEXT@' "$installer"
grep -Fq "UninstallString '\"\$INSTDIR\\uninstall.exe\"'" "$installer"
grep -Fq 'Delete /REBOOTOK "$INSTDIR\daemon\@BITCOIN_TX_NAME@@EXEEXT@"' "$installer"
if grep -Eq 'RMDir /r|File /r|example.invalid' "$installer"; then
    echo 'Installer must not recursively remove user files or bundle development documentation' >&2
    exit 1
fi
awk '/Function un.onInit/{inside=1; next} inside && /SetRegView 64/{found=1} inside && /FunctionEnd/{exit !found}' "$installer"
grep -Fq 'BITCOIN_WIN_INSTALLER=OrganicLifeCoin-$(PACKAGE_VERSION)-win64-setup$(EXEEXT)' "$root/Makefile.am"
grep -Fq '$(INSTALL_STRIP_PROGRAM) $(BITCOIN_TX_BIN)' "$root/Makefile.am"
if grep -Fq 'echo error: could not build $@' "$root/Makefile.am"; then
    echo 'Installer compiler failure must fail the build' >&2
    exit 1
fi
echo 'Windows installer safety checks passed'
