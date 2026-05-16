// Copyright (c) 2011-2014 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2020 The PIVX Core developers
// Copyright (c) 2026 The CTEAM Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef PIVX_QT_GUICONSTANTS_H
#define PIVX_QT_GUICONSTANTS_H

/* Milliseconds between model updates */
static const int MODEL_UPDATE_DELAY = 1000;

/* AskPassphraseDialog -- Maximum passphrase length */
static const int MAX_PASSPHRASE_SIZE = 1024;

/* Pivx GUI -- Size of icons in status bar */
static const int STATUSBAR_ICONSIZE = 16;

static const bool DEFAULT_SPLASHSCREEN = true;

/* Invalid field background style */
#define STYLE_INVALID "background:#FF8080"

/* Brown palette candidate keeps previous values nearby for quick review:
   previous primary #F24A09, accent #FF8A3D, navy #22254A, neutral #7D7877, silver #DFE1E2. */

/* Transaction list -- unconfirmed transaction */
#define COLOR_UNCONFIRMED QColor(138, 118, 103)
/* Transaction list -- negative amount */
#define COLOR_NEGATIVE QColor(220, 38, 38)
/* Transaction list -- bare address (without label) */
#define COLOR_BAREADDRESS QColor(138, 118, 103)
/* Transaction list -- TX status decoration - open until date */
#define COLOR_TX_STATUS_OPENUNTILDATE QColor(201, 130, 61)
/* Transaction list -- TX status decoration - default color */
#define COLOR_BLACK QColor(58, 36, 24)
/* Transaction list -- TX status decoration - conflicted */
#define COLOR_CONFLICTED QColor(248, 68, 68)
/* Transaction list -- TX status decoration - orphan (CTEAM parchment #E8DCCF) */
#define COLOR_ORPHAN QColor(232, 220, 207)
/* Transaction list -- TX status decoration - stake (CTEAM copper #9C4E1A) */
#define COLOR_STAKE QColor(156, 78, 26)
/* Tooltips longer than this (in characters) are converted into rich text,
   so that they can be word-wrapped.
 */
static const int TOOLTIP_WRAP_THRESHOLD = 80;

/* Maximum allowed URI length */
static const int MAX_URI_LENGTH = 255;

/* QRCodeDialog -- size of exported QR Code image */
#define EXPORT_IMAGE_SIZE 256


#define QAPP_ORG_NAME "CTEAM"
#define QAPP_ORG_DOMAIN "cteam.org"
#define QAPP_APP_NAME_DEFAULT "CTEAM"
#define QAPP_APP_NAME_TESTNET "CTEAM-testnet"

#endif // PIVX_QT_GUICONSTANTS_H
