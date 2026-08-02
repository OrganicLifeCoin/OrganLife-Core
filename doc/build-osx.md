macOS Build Instructions and Notes
====================================
The commands in this guide should be executed in a Terminal application.
The built-in one is located in `/Applications/Utilities/Terminal.app`.

Preparation
-----------
Install the macOS command line tools:

`xcode-select --install`

When the popup appears, click `Install`.

Then install [Homebrew](https://brew.sh).

Dependencies
----------------------

Minimum requirements (for the recommended `depends/` build):

    brew install autoconf automake libtool pkg-config python3 rust

If you want to build against system libraries instead of `depends/`, you'll need a larger set of packages (Boost, Qt6/Qt5, libevent, etc.). See [dependencies.md](dependencies.md).

If you want to build the disk image with `make deploy` (.dmg / optional), you need RSVG:

    brew install librsvg

and [`macdeployqtplus`](../contrib/macdeploy/README.md) dependencies:
```shell
pip3 install ds_store mac_alias
```

Berkeley DB
-----------
It is recommended to use Berkeley DB 4.8. If you have to build it yourself,
you can use [the installation script included in contrib/](/contrib/install_db4.sh)
like so:

```shell
./contrib/install_db4.sh .
```

from the root of the repository.

**Note**: You only need Berkeley DB if the wallet is enabled (see [*Disable-wallet mode*](/doc/build-osx.md#disable-wallet-mode)).

Build OrganicLife Core
------------------------

Option 1 (recommended): `depends/` (static, reproducible):

        ./build-depends.sh

Option 2: classic Autotools build:

1. Clone the OrganicLife Core source code:

        git clone <your-repo-url>
        cd <repo>

2.  Build OrganicLife Core:

        ./autogen.sh
        ./configure
        make

3.  It is recommended to build and run the unit tests:

        ./params/install-params.sh
        make check

4.  You can also create a .dmg that contains the .app bundle (optional):

        make deploy

Disable-wallet mode
--------------------
**Note:** This functionality is not yet completely implemented, and compilation using the below option will currently fail.

When the intention is to run only a P2P node without a wallet, OrganicLife Core may be compiled in
disable-wallet mode with:

    ./configure --disable-wallet

In this case there is no dependency on Berkeley DB 4.8.

Running
-------

OrganicLife Core is now available at `./src/organiclifed`

Before running, you may create an empty configuration file:

    mkdir -p "/Users/${USER}/Library/Application Support/OrganicLife"

    touch "/Users/${USER}/Library/Application Support/OrganicLife/organiclife.conf"

    chmod 600 "/Users/${USER}/Library/Application Support/OrganicLife/organiclife.conf"

The first time you run organiclifed, it will start downloading the blockchain. This process could take many hours, or even days on slower than average systems.

You can monitor the download process by looking at the debug.log file:

    tail -f "$HOME/Library/Application Support/OrganicLife/debug.log"

Other commands:
-------

    ./src/organiclifed -daemon # Starts the daemon.
    ./src/organiclife-cli --help # Outputs a list of command-line options.
    ./src/organiclife-cli help # Outputs a list of RPC commands when the daemon is running.

Notes
-----

* Tested on modern macOS on both Apple Silicon and Intel.

* Building with downloaded Qt binaries is not officially supported. See the notes in [#7714](https://github.com/bitcoin/bitcoin/issues/7714)
