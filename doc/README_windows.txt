OrganicLifeCoin
=====================

Intro
-----
OrganicLifeCoin is a free open source peer-to-peer electronic cash system that is
completely decentralized, without the need for a central server or trusted
parties.  Users hold the crypto keys to their own money and transact directly
with each other, with the help of a P2P network to check for double-spending.


Setup
-----
Unpack the files into a directory and run organiclife-qt.exe.

The wallet downloads and stores the history of OrganicLifeCoin transactions;
depending on the speed of your computer and network connection, the synchronization
process can take anywhere from a few hours to a day or more.

Windows requirements
--------------------
The Qt wallet requires 64-bit Windows 10 version 1809 or later, or Windows 11.
Earlier Windows versions are not supported by this release.

Installation and upgrades
-------------------------
The installer installs for the current user; administrator access is not required.
Close the wallet and daemon before upgrading. The installer includes the Qt wallet
and daemon, command-line client, and transaction tools in the daemon subfolder.
The Start menu includes normal and testnet shortcuts. Configuration can also select
the network; testnet coins do not transfer to mainnet.

Wallet safety
-------------
Wallet data and configuration normally live in %APPDATA%\OrganicLifeCoin.
Encryption is optional. Automatic backups are enabled by default, but a backup on
the same disk does not protect against disk failure. Keep a separate safe copy.
Uninstalling removes application files, not your wallet or blockchain data.
