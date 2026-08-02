// Copyright (c) 2011-2014 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2020 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
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

/* Transaction list -- unconfirmed transaction */
#define COLOR_UNCONFIRMED QColor(126, 122, 98)
/* Transaction list -- negative amount */
#define COLOR_NEGATIVE QColor(220, 38, 38)
/* Transaction list -- bare address (without label) */
#define COLOR_BAREADDRESS QColor(126, 122, 98)
/* Transaction list -- TX status decoration - open until date */
#define COLOR_TX_STATUS_OPENUNTILDATE QColor(110, 155, 69)
/* Transaction list -- TX status decoration - default color */
#define COLOR_BLACK QColor(43, 43, 26)
/* Transaction list -- TX status decoration - conflicted */
#define COLOR_CONFLICTED QColor(248, 68, 68)
/* Transaction list -- TX status decoration - orphan (OrganicLife parchment #e3e8ce) */
#define COLOR_ORPHAN QColor(227, 232, 206)
/* Transaction list -- TX status decoration - stake (OrganicLife copper #4f7a2e) */
#define COLOR_STAKE QColor(79, 122, 46)
/* Tooltips longer than this (in characters) are converted into rich text,
   so that they can be word-wrapped.
 */
static const int TOOLTIP_WRAP_THRESHOLD = 80;

/* Maximum allowed URI length */
static const int MAX_URI_LENGTH = 255;

/* QRCodeDialog -- size of exported QR Code image */
#define EXPORT_IMAGE_SIZE 256


#define QAPP_ORG_NAME "OrganicLife"
#define QAPP_ORG_DOMAIN "organiclife.org"
#define QAPP_APP_NAME_DEFAULT "OrganicLife"
#define QAPP_APP_NAME_TESTNET "OrganicLife-testnet"

#endif // PIVX_QT_GUICONSTANTS_H
