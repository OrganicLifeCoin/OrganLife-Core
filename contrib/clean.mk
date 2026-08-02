# Minimal, standalone cleanup makefile.
# Use this when the autotools-generated Makefile tries to re-run ./configure
# before executing a clean target.
#
# Usage:
#   make -f contrib/clean.mk clean-all
#   make -f contrib/clean.mk clean-all-hard
#   make -f contrib/clean.mk clobber
#
# Notes:
# - This removes build artifacts only (not your datadir, wallets, blocks, etc).
# - `clean-all-hard`/`clobber` also deletes `depends/sources/` (forces re-download).

.PHONY: clean-all clean-all-hard clobber

	RM_TARGETS = \
	autom4te.cache \
	build \
	dist \
	target \
	vcpkg \
	vcpkg_installed \
		OrganicLife.app \
		*.dmg \
	background.tiff background.tiff.png background.tiff@2x.png \
	cache test/cache test/tmp coverage_percent.txt test_organiclife.coverage total.coverage \
	depends/work depends/built depends/SDKs \
	depends/*-*-*/

clean-all:
	@echo "[INFO] Removing build artifacts..."
	@set -e; \
	rm -rf $(RM_TARGETS) 2>/dev/null || true; \
	# If anything is still around, it's usually because some files were created as root (sudo). \
	# Retry with sudo when available. \
	if [ -d depends/work ] || [ -d depends/built ] || ls -d depends/*-*-* >/dev/null 2>&1; then \
		echo "[WARN] Some artifacts could not be removed (likely permissions)."; \
		if command -v sudo >/dev/null 2>&1; then \
			echo "[INFO] Retrying with sudo..."; \
			sudo rm -rf $(RM_TARGETS) || true; \
		fi; \
	fi; \
	if [ -d depends/work ] || [ -d depends/built ] || ls -d depends/*-*-* >/dev/null 2>&1; then \
		echo "[ERROR] Still not fully cleaned. Try:"; \
		echo "       sudo make -f contrib/clean.mk clean-all"; \
		exit 1; \
	fi; \
	echo "[OK] Done."

clean-all-hard: clean-all
	@echo "[INFO] Removing depends sources (forces re-download next build)..."
	rm -rf depends/sources
	@echo "[OK] Done."

clobber: clean-all-hard
