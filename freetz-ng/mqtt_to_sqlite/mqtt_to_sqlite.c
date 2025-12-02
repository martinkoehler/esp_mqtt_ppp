// mqtt_to_sqlite.c
// Robust MQTT->SQLite collector that never exits on network errors.
// Changes vs. previous:
//  - Manual loop with mosquitto_loop() and explicit reconnect logic.
//  - Sleep after running network repair script.
//  - Exponential backoff between reconnect attempts.
//  - Exits ONLY on SIGINT/SIGTERM.

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
#include <sys/wait.h>

static volatile sig_atomic_t g_should_stop = 0;

static sqlite3 *g_db = NULL;
static sqlite3_stmt *g_stmt_insert = NULL;
static struct mosquitto *g_mosq = NULL;

static char g_broker_host[128] = "192.168.178.50";
static int  g_broker_port = 1883;
static char g_topic[128] = "#";  // subscribe to all (your request)
static char g_db_path[256] = "./mqtt_messages.db";
static char g_netfix_script[256] = "./handle_network_error.sh";

static int  g_reconnect_min = 2;    // seconds
static int  g_reconnect_max = 60;   // seconds
static int  g_sleep_after_script = 5; // seconds

static time_t g_last_script_run = 0;
static const int SCRIPT_MIN_INTERVAL_SEC = 20;

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

static void log_ts(const char *level, const char *msg) {
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    fprintf(stderr, "[%s] [%s] %s\n", buf, level, msg);
}

static int g_log_inserts = -1; // -1 = uninitialized

static void init_log_inserts(void) {
    if (g_log_inserts == -1) {
        const char *v = getenv("MQTT_LOG_INSERTS");
        g_log_inserts = (v && *v && strcmp(v, "0") != 0) ? 1 : 0;
    }
}

static void print_inserted_message(time_t ts, const char *topic, const void *payload, int payloadlen, int qos, int retain) {
    init_log_inserts();
    if (!g_log_inserts) return;

    enum { MAX_SHOW = 256 };
    char show[MAX_SHOW + 1];
    int n = (payloadlen < MAX_SHOW) ? payloadlen : MAX_SHOW;
    for (int i = 0; i < n; ++i) {
        unsigned char c = ((const unsigned char*)payload)[i];
        // keep readable; escape control chars
        show[i] = (c >= 32 && c <= 126) ? (char)c : '.';
    }
    show[n] = '\0';

    char tbuf[64];
    struct tm tm;
    localtime_r(&ts, &tm);
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tm);

    fprintf(stderr,
            "[%s] [DB+] topic='%s' qos=%d retain=%d payload_len=%d payload='%s'%s\n",
            tbuf, topic, qos, retain, payloadlen, show,
            (payloadlen > MAX_SHOW ? "…" : ""));
}

static void on_signal(int sig) {
    (void)sig;
    g_should_stop = 1;
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

    char msg[512];
    snprintf(msg, sizeof(msg), "Running network repair script: %s", g_netfix_script);
    log_ts("WARN", msg);

    int rc = system(cmd);
    if (rc == -1) {
        perror("system");
        return -1;
    } else {
        snprintf(msg, sizeof(msg), "Network repair script exit code: %d", WEXITSTATUS(rc));
        log_ts("INFO", msg);
    }
    return 0;
}

/* ---------- SQLite ---------- */

static int db_init(const char *path) {
    int rc = sqlite3_open(path, &g_db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "sqlite3_open failed: %s\n", sqlite3_errmsg(g_db));
        return -1;
    }
    // Tolerant pragmas for low-end devices and concurrent readers like Grafana.
    sqlite3_exec(g_db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(g_db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);
    sqlite3_exec(g_db, "PRAGMA temp_store=MEMORY;", NULL, NULL, NULL);

    const char *ddl =
        "CREATE TABLE IF NOT EXISTS messages ("
        "  id       INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  ts       INTEGER NOT NULL,"
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
    if (g_stmt_insert) { sqlite3_finalize(g_stmt_insert); g_stmt_insert = NULL; }
    if (g_db) { sqlite3_close(g_db); g_db = NULL; }
}

static void db_insert_message(const char *topic, const void *payload, int payloadlen, int qos, int retain) {
    if (!g_stmt_insert) return;
    time_t now = time(NULL);

    sqlite3_reset(g_stmt_insert);
    sqlite3_clear_bindings(g_stmt_insert);

    sqlite3_bind_int64(g_stmt_insert, 1, (sqlite3_int64)now);
    sqlite3_bind_text (g_stmt_insert, 2, topic, -1, SQLITE_TRANSIENT);

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
    } else {
        print_inserted_message(now, topic, payload, payloadlen, qos, retain);
    }

    if (pl) free(pl);
}

/* ---------- MQTT callbacks (lightweight; no exits) ---------- */

static void handle_connect(struct mosquitto *mosq, void *obj, int rc) {
    (void)obj;
    (void)mosq;

    enum { TOPIC_PREVIEW = 120 };
    char topic_preview[TOPIC_PREVIEW + 1];
    size_t tlen = strlen(g_topic);
    if (tlen > TOPIC_PREVIEW) {
        memcpy(topic_preview, g_topic, TOPIC_PREVIEW);
        topic_preview[TOPIC_PREVIEW] = '\0';
    } else {
        memcpy(topic_preview, g_topic, tlen + 1);
    }

    char buf[256];
    snprintf(buf, sizeof(buf),
             "Connected (rc=%d), subscribing to '%s'%s",
             rc,
             topic_preview,
             (tlen > TOPIC_PREVIEW ? "…" : ""));
    log_ts("INFO", buf);

    if (rc == 0) {
        int s = mosquitto_subscribe(mosq, NULL, g_topic, 0);
        if (s != MOSQ_ERR_SUCCESS) {
            fprintf(stderr, "mosquitto_subscribe failed: %s\n", mosquitto_strerror(s));
        }
    }
}


static void handle_disconnect(struct mosquitto *mosq, void *obj, int rc) {
    (void)mosq; (void)obj;
    char buf[160];
    snprintf(buf, sizeof(buf), "Disconnected (rc=%d). Will try to recover…", rc);
    log_ts("WARN", buf);
    // NOTE: We do NOT exit here; the main loop will handle reconnects.
}

static void handle_message(struct mosquitto *mosq, void *obj, const struct mosquitto_message *msg) {
    (void)mosq; (void)obj;
    if (!msg) return;
    db_insert_message(msg->topic, msg->payload, msg->payloadlen, msg->qos, msg->retain);
}

/* ---------- util ---------- */

static void install_sig_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

/* ---------- main ---------- */

int main(void) {
    // Env overrides
    snprintf(g_broker_host, sizeof(g_broker_host), "%s", env_or_default("MQTT_BROKER", "192.168.178.50"));
    g_broker_port = env_or_default_int("MQTT_PORT", 1883);
    snprintf(g_topic, sizeof(g_topic), "%s", env_or_default("MQTT_TOPIC", "#")); // your note
    snprintf(g_db_path, sizeof(g_db_path), "%s", env_or_default("MQTT_DB_PATH", "./mqtt_messages.db"));
    snprintf(g_netfix_script, sizeof(g_netfix_script), "%s", env_or_default("NETWORK_FIX_SCRIPT", "./handle_network_error.sh"));
    g_reconnect_min = env_or_default_int("RECONNECT_MIN_S", 2);
    g_reconnect_max = env_or_default_int("RECONNECT_MAX_S", 60);
    g_sleep_after_script = env_or_default_int("RETRY_SLEEP_AFTER_SCRIPT_S", 5);

    install_sig_handlers();

    if (db_init(g_db_path) != 0) {
        fprintf(stderr, "Failed to init DB at %s\n", g_db_path);
        return 1;
    }

    mosquitto_lib_init();

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

    // Configure internal backoff (not strictly needed in manual loop but ok)
    mosquitto_reconnect_delay_set(g_mosq, true, g_reconnect_min, g_reconnect_max);

    // Initial connect (non-fatal on failure)
    int rc = mosquitto_connect(g_mosq, g_broker_host, g_broker_port, 30);
    if (rc != MOSQ_ERR_SUCCESS) {
        char buf[160];
        snprintf(buf, sizeof(buf), "Initial connect failed: %s", mosquitto_strerror(rc));
        log_ts("WARN", buf);
    }

    // Manual loop: keep going until signaled to stop.
    int backoff = g_reconnect_min;
    while (!g_should_stop) {
        rc = mosquitto_loop(g_mosq, /*timeout_ms*/ 1000, /*max_packets*/ 1);
        if (rc == MOSQ_ERR_SUCCESS) {
            // Healthy loop iteration.
            backoff = g_reconnect_min; // reset backoff on success
            continue;
        }

        // Any error: connection likely lost or not yet established.
        char buf[200];
        snprintf(buf, sizeof(buf), "mosquitto_loop error: %s", mosquitto_strerror(rc));
        log_ts("WARN", buf);

        // Try to repair network; then sleep a bit for routes/ppp to settle.
        run_network_repair_script();

        if (g_sleep_after_script > 0) {
            char m2[120];
            snprintf(m2, sizeof(m2), "Sleeping %d s after repair script…", g_sleep_after_script);
            log_ts("INFO", m2);
            for (int i = 0; i < g_sleep_after_script && !g_should_stop; ++i) sleep(1);
        }

        // Reconnect loop with exponential backoff
        while (!g_should_stop) {
            int rc2 = mosquitto_reconnect(g_mosq);
            if (rc2 == MOSQ_ERR_SUCCESS) {
                log_ts("INFO", "Reconnected successfully.");
                backoff = g_reconnect_min;
                break; // back to main loop; subscribe happens in on_connect
            }

            snprintf(buf, sizeof(buf), "Reconnect failed: %s. Retrying in %d s…",
                     mosquitto_strerror(rc2), backoff);
            log_ts("WARN", buf);

            for (int i = 0; i < backoff && !g_should_stop; ++i) sleep(1);
            if (backoff < g_reconnect_max) {
                backoff *= 2;
                if (backoff > g_reconnect_max) backoff = g_reconnect_max;
            }
        }
    }

    log_ts("INFO", "Shutting down…");
    mosquitto_disconnect(g_mosq);
    mosquitto_destroy(g_mosq);
    mosquitto_lib_cleanup();
    db_close();
    return 0;
}

