
Debian
====================
This directory contains files used to package organiclifed/organiclife-qt
for Debian-based Linux systems. If you compile organiclifed/organiclife-qt yourself, there are some useful files here.

## pivx: URI support ##


organiclife-qt.desktop  (Gnome / Open Desktop)
To install:

	sudo desktop-file-install organiclife-qt.desktop
	sudo update-desktop-database

If you build yourself, you will either need to modify the paths in
the .desktop file or copy or symlink your organiclife-qt binary to `/usr/bin`
and the `../../share/pixmaps/pivx128.png` to `/usr/share/pixmaps`

organiclife-qt.protocol (KDE)

