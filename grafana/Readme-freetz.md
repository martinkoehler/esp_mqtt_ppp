
## Goal

* SQLite DB stays in **WAL mode** on the Fritz!Box.
* **No remote host opens the DB file.** (WAL requires all SQLite connections be on the same host, not over NFS.) ([sqlite.org][1])
* Fritz!Box exposes a **read-only HTTP endpoint** that runs `sqlite3` locally.
* Grafana reads the endpoint using **Infinity datasource** (JSON → time series). ([Grafana Labs][2])

---

## Fritz!Box setup (freetz-ng)

### 1) Start BusyBox `httpd` with CGI enabled

Your build already has:

* `httpd`
* CGI support
* basic auth support

Create folders:

```sh
mkdir -p /mod/www/cgi-bin
```

Start web server (test manually first):

```sh
busybox httpd \
  -p 8080 \
  -h /mod/www \
  -c /mod/etc/httpd.conf \
  -r "sqlite-mqtt"
```

### 2) Restrict access to LAN (recommended)

Edit `/mod/etc/httpd.conf`:

```
A:192.168.178.0/24
D:0.0.0.0/0
```

(Adjust subnet to your LAN.)

### 3) (Optional) Protect CGI with Basic Auth

Because your BusyBox has basic auth enabled, add to `/mod/etc/httpd.conf`:

```
/cgi-bin:martin:/mod/etc/httpd.passwd
```

Create `/mod/etc/httpd.passwd` on a PC (Apache htpasswd format) and copy it over.

### 4) Create the CGI endpoint

Create `/mod/www/cgi-bin/mqtt.sh`:

```sh
case "$mode" in
  num)
    $SQLITE -readonly -json \
      -cmd "PRAGMA query_only=ON;" \
      -cmd "PRAGMA busy_timeout=2000;" \
      "$DB" "
      SELECT
        ts*1000 AS time,
        topic,
        CAST(payload AS REAL) AS value
      FROM messages
      WHERE $WHERE
        AND (
             payload GLOB '-[0-9]*'
          OR payload GLOB '[0-9]*'
          OR payload GLOB '-[0-9]*.[0-9]*'
          OR payload GLOB '[0-9]*.[0-9]*'
        )
      ORDER BY ts ASC
      LIMIT $limit;
      "
    ;;
  json)
    $SQLITE -readonly -json \
      -cmd "PRAGMA query_only=ON;" \
      -cmd "PRAGMA busy_timeout=2000;" \
      "$DB" "
      SELECT ts*1000 AS time, topic, payload
      FROM messages
      WHERE $WHERE AND payload LIKE '{%}'
      ORDER BY ts DESC
      LIMIT $limit;
      "
    ;;
  *)
    echo '{"error":"mode must be num or json"}'
    ;;
esac
```

Make it executable:

```sh
chmod +x /mod/www/cgi-bin/mqtt.sh
```

### 5) Verify the endpoint

From any LAN machine:

```sh
curl 'http://fritz.box:8080/cgi-bin/mqtt.sh?mode=num&topic=obk_wr/power/get&from=0&to=2000000000&limit=3'
```

You should get JSON like:

```json
[
  {"time":1764794376000,"topic":"obk_wr/power/get","value":48.97},
  ...
]
```

---

## Grafana setup (Infinity datasource)

### 6) Install Infinity plugin

Install **yesoreyeram-infinity-datasource** in Grafana.
(Plugins → Install, or `grafana-cli plugins install yesoreyeram-infinity-datasource`.) ([Grafana Labs][2])

### 7) Add Infinity as a datasource

Configuration:

* **Type:** Infinity
* **Access:** Server (proxy)
* **Base URL:** leave empty or set to `http://fritz.box:8080/`
* **Auth:** Basic auth if you enabled it on the Fritz!Box (otherwise none)

### 8) Create the panel query for `obk_wr/power/get`

New panel → choose Infinity datasource → Query type **JSON**.

**URL:**

```
http://fritz.box:8080/cgi-bin/mqtt.sh?mode=num&topic=obk_wr/power/get&from=${__timeFrom:date:seconds}&to=${__timeTo:date:seconds}
```

Infinity time macros are preferred over Grafana’s global `${__from}`/`${__to}` and can output seconds directly. ([Grafana Labs][3])

Infinity query settings:

* **Format:** Time series ([Grafana Labs][4])
* **Time field:** `time`
* **Value field:** `value`
* **Series field:** optional (can be empty since topic is fixed)

Panel settings:

* **Visualization:** Time series
* **Unit:** Watt (W)
* **Legend:** power

Save panel/dashboard.

---

## That’s the full pipeline

* MQTT → sqlite WAL on Fritz!Box
* BusyBox CGI reads locally → JSON time series
* Grafana Infinity pulls JSON with dashboard time range macros

No NFS/SMB direct DB access, so WAL stays safe and consistent. ([sqlite.org][1])

If you want a second panel (voltage/current/etc.), you can reuse the same endpoint and just change `topic=` in the URL.

[1]: https://sqlite.org/forum/info/e90fd84ee89d028c?utm_source=chatgpt.com "SQLite User Forum: WAL on network filesystems"
[2]: https://grafana.com/docs/plugins/yesoreyeram-infinity-datasource/latest/?utm_source=chatgpt.com "Infinity data source plugin for Grafana"
[3]: https://grafana.com/docs/plugins/yesoreyeram-infinity-datasource/latest/query/macros/?utm_source=chatgpt.com "Macros | Grafana Plugins documentation"
[4]: https://grafana.com/docs/plugins/yesoreyeram-infinity-datasource/latest/json/?utm_source=chatgpt.com "JSON | Grafana Plugins documentation"
