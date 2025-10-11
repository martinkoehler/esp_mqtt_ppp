// mqtt_to_sqlite.c
// Build-time deps: libmosquitto-dev, libsqlite3-dev
// Runtime deps: libmosquitto, sqlite3
//
// Behavior:
// - Connects to MQTT broker at 192.168.4.1
// - Subscribes to "obk/#" (covers your OpenBeken topics like obk681BA32A/...)
// - Inserts every message into SQLite (mqtt_messages.db)
// - On disconnect, runs NETWORK_FIX_SCRIPT (default ./handle_network_error.sh) and retries with backoff
//
// Env overrides (optional):
//   MQTT_BROKER      (default "192.168.4.1")
//   MQTT_PORT        (default "1883")
//   MQTT_CLIENT_ID   (default auto-generated)
//   MQTT_TOPIC       (default "#")
//   MQTT_DB_PATH     (default "./mqtt_messages.db")
//   NETWORK_FIX_SCRIPT (default "./handle_network_error.sh")
//   RECONNECT_MIN_S  (default "2")
//   RECONNECT_MAX_S  (default "60")

#define _POSIX_C_SOURCE 200809L

#include <mosquitto.h>
#include <sqlite3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

static volatile sig_atomic_t g_should_stop = 0;

static sqlite3 *g_db = NULL;
static sqlite3_stmt *g_stmt_insert = NULL;
static struct mosquitto *g_mosq = NULL;

static char g_broker_host[128] = "192.168.4.1";
static int  g_broker_port = 1883;
static char g_topic[128] = "#";
static char g_db_path[256] = "./mqtt_messages.db";
static char g_netfix_script[256] = "./handle_network_error.sh";
static int  g_reconnect_min = 2;
static int  g_reconnect_max = 60;

// Throttle running the repair script (avoid spamming)
static time_t g_last_script_run = 0;
static const int SCRIPT_MIN_INTERVAL_SEC = 20;

// --- helpers

static const char *env_or_default(const char *name, const char *defval) {
    const char *v = getenv(name);
    return (v && *v) ? v : defval;
}

static int env_or_default_int(const char *name, int defval) {
    const char *v = getenv(name);
    if (!v || !*v) return defval;
    char *end = NULL;
    long x = strtol(v, &end, 10);
    if (end == v || *end) return defval;
    if (x < 0 || x > 1000000) return defval;
    return (int)x;
}

static void install_sig_handlers(void);

static void on_signal(int sig) {
    (void)sig;
    g_should_stop = 1;
    if (g_mosq) mosquitto_disconnect(g_mosq);
}

static void log_ts(const char *level, const char *msg) {
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    fprintf(stderr, "[%s] [%s] %s\n", buf, level, msg);
}

static int run_network_repair_script(void) {
    time_t now = time(NULL);
    if (now - g_last_script_run < SCRIPT_MIN_INTERVAL_SEC) {
        log_ts("INFO", "Skipping network repair script (throttled)");
        return 0;
    }
    g_last_script_run = now;

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "sh -c '%s'", g_netfix_script);

    fprintf(stderr, "Running network repair script: %s\n", cmd);
    int rc = system(cmd);
    if (rc == -1) {
        perror("system");
        return -1;
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "Network repair script exit code: %d", WEXITSTATUS(rc));
        log_ts("INFO", msg);
    }
    return 0;
}

// --- SQLite

static int db_init(const char *path) {
    int rc = sqlite3_open(path, &g_db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "sqlite3_open failed: %s\n", sqlite3_errmsg(g_db));
        return -1;
    }

    // Fast-ish settings for low-end devices; safe for single-writer
    sqlite3_exec(g_db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(g_db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);
    sqlite3_exec(g_db, "PRAGMA temp_store=MEMORY;", NULL, NULL, NULL);

    const char *ddl =
        "CREATE TABLE IF NOT EXISTS messages ("
        "  id       INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  ts       INTEGER NOT NULL,"         // Unix epoch seconds
        "  topic    TEXT    NOT NULL,"
        "  payload  TEXT    NOT NULL,"
        "  qos      INTEGER NOT NULL,"
        "  retain   INTEGER NOT NULL"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_messages_ts ON messages(ts);"
        "CREATE INDEX IF NOT EXISTS idx_messages_topic ON messages(topic);";

    char *errmsg = NULL;
    rc = sqlite3_exec(g_db, ddl, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "sqlite3_exec(DDL) failed: %s\n", errmsg);
        sqlite3_free(errmsg);
        return -1;
    }

    const char *ins =
        "INSERT INTO messages (ts, topic, payload, qos, retain) "
        "VALUES (?, ?, ?, ?, ?);";

    rc = sqlite3_prepare_v2(g_db, ins, -1, &g_stmt_insert, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "sqlite3_prepare_v2 failed: %s\n", sqlite3_errmsg(g_db));
        return -1;
    }

    return 0;
}

static void db_close(void) {
    if (g_stmt_insert) {
        sqlite3_finalize(g_stmt_insert);
        g_stmt_insert = NULL;
    }
    if (g_db) {
        sqlite3_close(g_db);
        g_db = NULL;
    }
}

static void db_insert_message(const char *topic, const void *payload, int payloadlen, int qos, int retain) {
    if (!g_stmt_insert) return;

    time_t now = time(NULL);

    // Bind and execute
    sqlite3_reset(g_stmt_insert);
    sqlite3_clear_bindings(g_stmt_insert);

    sqlite3_bind_int64(g_stmt_insert, 1, (sqlite3_int64)now);
    sqlite3_bind_text (g_stmt_insert, 2, topic, -1, SQLITE_TRANSIENT);

    // Ensure payload is stored as UTF-8 text; if not, we still store raw bytes interpreted as text.
    // If you prefer BLOB, change the column type and bind with sqlite3_bind_blob.
    char *pl = NULL;
    if (payload && payloadlen >= 0) {
        pl = (char*)malloc((size_t)payloadlen + 1);
        if (pl) {
            memcpy(pl, payload, (size_t)payloadlen);
            pl[payloadlen] = '\0';
        }
    }
    sqlite3_bind_text (g_stmt_insert, 3, pl ? pl : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int  (g_stmt_insert, 4, qos);
    sqlite3_bind_int  (g_stmt_insert, 5, retain);

    int rc = sqlite3_step(g_stmt_insert);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "sqlite3_step(insert) failed: %s\n", sqlite3_errmsg(g_db));
    }

    if (pl) free(pl);
}

// --- MQTT callbacks

static void handle_connect(struct mosquitto *mosq, void *obj, int rc) {
    (void)obj;
    char buf[128];
    snprintf(buf, sizeof(buf), "Connected with rc=%d", rc);
    log_ts("INFO", buf);

    if (rc == 0) {
        int s = mosquitto_subscribe(mosq, NULL, g_topic, 0);
        if (s != MOSQ_ERR_SUCCESS) {
            fprintf(stderr, "mosquitto_subscribe failed: %s\n", mosquitto_strerror(s));
        } else {
            fprintf(stderr, "Subscribed to %s\n", g_topic);
        }
    }
}

static void handle_disconnect(struct mosquitto *mosq, void *obj, int rc) {
    (void)mosq; (void)obj;
    char buf[128];
    snprintf(buf, sizeof(buf), "Disconnected (rc=%d). Attempting repair + reconnect…", rc);
    log_ts("WARN", buf);

    run_network_repair_script();
    // Reconnect attempts are handled by mosquitto_loop_forever with reconnect_delay_set
}

static void handle_message(struct mosquitto *mosq, void *obj, const struct mosquitto_message *msg) {
    (void)mosq; (void)obj;
    if (!msg) return;

    db_insert_message(msg->topic,
                      msg->payload,
                      msg->payloadlen,
                      msg->qos,
                      msg->retain);

    // Optional: small console trace for visibility
    fprintf(stderr, "MSG %s => %.*s\n", msg->topic, msg->payloadlen, (const char*)msg->payload);
}

// --- main

static void install_sig_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

int main(void) {
    // Read env overrides
    snprintf(g_broker_host, sizeof(g_broker_host), "%s", env_or_default("MQTT_BROKER", "192.168.4.1"));
    g_broker_port = env_or_default_int("MQTT_PORT", 1883);
    snprintf(g_topic, sizeof(g_topic), "%s", env_or_default("MQTT_TOPIC", "#"));
    snprintf(g_db_path, sizeof(g_db_path), "%s", env_or_default("MQTT_DB_PATH", "./mqtt_messages.db"));
    snprintf(g_netfix_script, sizeof(g_netfix_script), "%s", env_or_default("NETWORK_FIX_SCRIPT", "./handle_network_error.sh"));
    g_reconnect_min = env_or_default_int("RECONNECT_MIN_S", 2);
    g_reconnect_max = env_or_default_int("RECONNECT_MAX_S", 60);

    install_sig_handlers();

    if (db_init(g_db_path) != 0) {
        fprintf(stderr, "Failed to init DB at %s\n", g_db_path);
        return 1;
    }

    mosquitto_lib_init();

    // Client ID (optional env)
    const char *cid_env = getenv("MQTT_CLIENT_ID");
    char cid_buf[64];
    if (!cid_env || !*cid_env) {
        snprintf(cid_buf, sizeof(cid_buf), "mqtt2sqlite-%ld", (long)getpid());
        cid_env = cid_buf;
    }

    g_mosq = mosquitto_new(cid_env, true, NULL);
    if (!g_mosq) {
        fprintf(stderr, "mosquitto_new failed\n");
        db_close();
        mosquitto_lib_cleanup();
        return 1;
    }

    mosquitto_connect_callback_set(g_mosq, handle_connect);
    mosquitto_disconnect_callback_set(g_mosq, handle_disconnect);
    mosquitto_message_callback_set(g_mosq, handle_message);

    // Automatic reconnect with backoff
    mosquitto_reconnect_delay_set(g_mosq, true, g_reconnect_min, g_reconnect_max);

    int rc = mosquitto_connect(g_mosq, g_broker_host, g_broker_port, /*keepalive*/ 30);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "Initial connect failed: %s\n", mosquitto_strerror(rc));
        run_network_repair_script();
        // We still proceed to loop_forever; it will attempt reconnects.
    }

    // This handles reconnects automatically thanks to reconnect_delay_set()
    rc = mosquitto_loop_forever(g_mosq, -1, 1);

    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "mosquitto_loop_forever ended: %s\n", mosquitto_strerror(rc));
    }

    mosquitto_destroy(g_mosq);
    mosquitto_lib_cleanup();
    db_close();

    return 0;
}

