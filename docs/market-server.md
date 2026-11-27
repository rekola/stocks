# Market-data server: setting it up

`market-ingest` loads external datasets (now HistData.com's 1-minute quotes, as the history) into a database, by hand, and `market-server` serves what it holds over a read-only HTTP
API for the desktop application. What they are and how they work is in the README ("Market-data server") and, as planned, in
[plans/server.md](../plans/server.md); this is how to set them up on a machine, step by step.

For a machine of its own that serves the local network: Ubuntu 24.04 (or like it), its address 192.168.2.10, MariaDB on the same machine as the ingest and the
server, which is why the database user is `market`@`localhost` and nothing but the machine itself can reach the database. Change the address, the paths
and the password to yours. Commands with `sudo` are run by an administrator; the programs themselves run as a user of their own.

**1. Install what it needs and build the two programs.**

    sudo apt install build-essential cmake ninja-build pkg-config git libcurl4-openssl-dev libunistring-dev libcairo2-dev libfontconfig1-dev \
                     mariadb-server libmariadb-dev
    git clone https://github.com/rekola/stocks && cd stocks
    cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build --target market-ingest market-server
    sudo install build/market-ingest build/market-server /usr/local/bin/

Only the two programs are built (the window application is not needed on the server). `libmariadb-dev` is what makes `"type": "mariadb"` work; without
it the build has SQLite only and `"mariadb"` is refused with a message when a program starts.

**2. Make the database.** The programs make their own tables (and start them over if their schema version changes), so the user needs all rights on its
schema, and the server needs them as much as the ingest does: there is no read-only user yet.

    sudo systemctl enable --now mariadb
    sudo mariadb <<'SQL'
    CREATE DATABASE IF NOT EXISTS market CHARACTER SET utf8mb4;
    CREATE USER IF NOT EXISTS 'market'@'localhost' IDENTIFIED BY 'CHOOSE-A-LONG-PASSWORD';
    GRANT ALL PRIVILEGES ON market.* TO 'market'@'localhost';
    SQL

Leave MariaDB listening on the machine only (its default, `bind-address = 127.0.0.1`): the clients of the data talk to the server's HTTP API, never to the
database. Check the login: `mariadb -umarket -p market -e 'select 1'`.

**3. Make a user and the directories.** The raw cache holds the only copy of the 1-minute data, so give it a place that is backed up.

    sudo useradd --system --home-dir /var/lib/market --shell /usr/sbin/nologin market
    sudo install -d -o market -g market /var/lib/market /var/lib/market/histdata
    sudo install -d -o root -g market -m 750 /etc/market

**4. Write the settings.** `/etc/market/market.json` (the format is in `src/market/Config.h`):

    {
      "database": { "type": "mariadb", "host": "localhost", "user": "market", "name": "market" },
      "datasets": { "histdata": { "cache_dir": "/var/lib/market/histdata" } },
      "server":   { "listen": "192.168.2.10", "port": 8080, "refresh_seconds": 60 }
    }

The password is not in that file: the environment variable `MARKET_DB_PASSWORD` takes its place. Put it in a file only the service user and root can read:

    sudo install -o root -g market -m 640 /dev/null /etc/market/market.env
    echo 'MARKET_DB_PASSWORD=CHOOSE-A-LONG-PASSWORD' | sudo tee /etc/market/market.env >/dev/null
    sudo chgrp market /etc/market/market.json && sudo chmod 640 /etc/market/market.json

`listen` is the address the server binds: `192.168.2.10` serves the network that address is on and nothing else, `0.0.0.0` every interface of the machine,
`127.0.0.1` (the default) only the machine itself. **There is no authentication**: anyone who can reach the port can read everything the server has. Keep it
on the local network, and open the port to that network only:

    sudo ufw allow from 192.168.2.0/24 to any port 8080 proto tcp     # if ufw is the firewall, and the network is 192.168.2.0/24

`refresh_seconds` is how often the server reads again what the ingest has written since (1 or more).

**5. Load a first instrument, to see that it works.** The ingest and the server are run as the `market` user, with the password from the file:

    sudo -u market bash -c 'set -a; . /etc/market/market.env; market-ingest histdata eurusd --level 5m --config /etc/market/market.json'

It downloads one file (the latest month of EURUSD, two requests and a pause of two seconds), keeps the zip in `/var/lib/market/histdata`, makes the
instrument and its bars, and prints a line for each resolution (bars, first and last bar). The exit code is 0 only if nothing failed.

**6. Load everything.** `all` is every pair HistData lists that has data this year (61 of 66 at the time of writing), at every resolution:

    sudo -u market bash -c 'set -a; . /etc/market/market.env; market-ingest histdata all --config /etc/market/market.json'

The first run downloads about 1,400 files at two seconds a request and takes more than an hour, so run it where a closed terminal does not end it (`tmux`, or
`sudo systemd-run --uid=market --property=EnvironmentFile=/etc/market/market.env market-ingest histdata all --config /etc/market/market.json`, whose output is
in `journalctl`). It can be stopped and started again: what was downloaded is in the cache, and what was written is not written twice. A later run of the same command
makes no request for a file it has, and only asks again for the month that was not over when it was fetched (once it is over) and for the months of this year that
`--refresh` names.

The ingest is **not scheduled**: the history is loaded by hand, once (and again only if you want what HistData has published since), and the new data comes from
another source. Nothing in this guide starts it by itself.

**7. Start the server and look at it.**

    sudo -u market bash -c 'set -a; . /etc/market/market.env; market-server --config /etc/market/market.json'

and from another machine of the network:

    curl http://192.168.2.10:8080/health                       # {"instruments":61,"status":"ok"}
    curl http://192.168.2.10:8080/instruments
    curl 'http://192.168.2.10:8080/summary?codes=eurusd,xauusd'

`--port` and `--listen` on the command line override the file. Anything but GET is refused (405), and there is no endpoint for raw bars.

**8. Keep the server running.** A unit for it, `/etc/systemd/system/market-server.service`:

    [Unit]
    Description=Market data API
    After=network-online.target mariadb.service
    Wants=network-online.target

    [Service]
    User=market
    EnvironmentFile=/etc/market/market.env
    ExecStart=/usr/local/bin/market-server --config /etc/market/market.json
    Restart=on-failure
    RestartSec=5

    [Install]
    WantedBy=multi-user.target

    sudo systemctl daemon-reload
    sudo systemctl enable --now market-server.service
    systemctl status market-server; journalctl -u market-server -f

The server reads the database again every `refresh_seconds`, so what an ingest run writes is served within that time, with no restart. An ingest run that ends with an
exit code other than 0 failed in something it says (a period it could not get, a write that was lost, no pair with data at all).

**9. Back it up.** The cache is the only copy of the 1-minute data and nothing deletes from it; the database can be made again from it (an ingest of
`all` then reads the cache and asks the site for little), but dump it anyway: `mariadb-dump market > market.sql`. If a new version of the program has another
schema, the stores start over on their own and the next ingest fills them again.

**If it does not work.** `cannot connect to localhost as market`: the user, the password in `market.env` or MariaDB itself. `cannot listen on 192.168.2.10:8080`:
the address is not this machine's, or the port is taken. A client on the network gets no answer: the firewall, or `listen` is `127.0.0.1`. `this build has no
"mariadb" database`: the programs were built without `libmariadb-dev`; build again. An ingest that ends with "every instrument came out inactive": the site did not
answer (or did not give a token), not a market that stopped; see the lines before it and try again later.
