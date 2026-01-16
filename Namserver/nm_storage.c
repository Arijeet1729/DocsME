#include "nm.h"
#include "logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <errno.h>

// ############## LLM Generated Code Begins ##############

// Global storage server list and lock
StorageServer *ss_list = NULL;
static pthread_mutex_t ss_lock = PTHREAD_MUTEX_INITIALIZER;

StorageServer *find_ss_by_ipport(const char *ip, int port) {
    pthread_mutex_lock(&ss_lock);
    StorageServer *p = ss_list;
    while (p) {
        if (strcmp(p->ip, ip) == 0 && p->ss_port == port) {
            pthread_mutex_unlock(&ss_lock);
            return p;
        }
        p = p->next;
    }
    pthread_mutex_unlock(&ss_lock);
    return NULL;
}

StorageServer *register_storage_server(const char *ip, int ss_port) {
    pthread_mutex_lock(&ss_lock);
    StorageServer *existing = ss_list;
    while (existing) {
        if (strcmp(existing->ip, ip) == 0 && existing->ss_port == ss_port) {
            existing->last_seen = time(NULL);
            pthread_mutex_unlock(&ss_lock);
            return existing;
        }
        existing = existing->next;
    }

    StorageServer *s = calloc(1, sizeof(StorageServer));
    strncpy(s->ip, ip, sizeof(s->ip)-1);
    s->ss_port = ss_port;
    s->last_seen = time(NULL);
    s->next = ss_list;
    ss_list = s;
    pthread_mutex_unlock(&ss_lock);
    log_message(LOGLVL_INFO, "Registered SS %s:%d", ip, ss_port);
    return s;
}

// Choose an SS for storing new files. Simple round-robin / first available.
StorageServer *choose_ss_for_new_file(void) {
    pthread_mutex_lock(&ss_lock);
    StorageServer *p = ss_list;
    if (!p) {
        pthread_mutex_unlock(&ss_lock);
        return NULL;
    }
    StorageServer *chosen = p; // pick head (simple)
    pthread_mutex_unlock(&ss_lock);
    return chosen;
}

// Send a single command line to a Storage Server and read back one response line.
// Returns 0 on success, -1 on network error. resp will be NUL-terminated.
int send_command_to_ss(StorageServer *ss, const char *cmd, char *resp, size_t resp_sz) {
    if (!ss || !cmd || !resp || resp_sz == 0) return -1;

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        log_message(LOGLVL_ERROR, "Failed to create socket to SS %s:%d: %s", ss->ip, ss->ss_port, strerror(errno));
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(ss->ss_port);
    if (inet_pton(AF_INET, ss->ip, &addr.sin_addr) <= 0) {
        log_message(LOGLVL_ERROR, "Invalid address for SS %s:%d", ss->ip, ss->ss_port);
        close(sock);
        return -1;
    }

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        log_message(LOGLVL_ERROR, "Failed to connect to SS %s:%d: %s", ss->ip, ss->ss_port, strerror(errno));
        close(sock);
        return -1;
    }

    size_t len = strlen(cmd);
    if (write(sock, cmd, len) < 0 || write(sock, "\n", 1) < 0) {
        log_message(LOGLVL_ERROR, "Failed to write to SS %s:%d: %s", ss->ip, ss->ss_port, strerror(errno));
        close(sock);
        return -1;
    }

    ssize_t r = read(sock, resp, resp_sz - 1);
    if (r < 0) {
        log_message(LOGLVL_ERROR, "Failed to read from SS %s:%d: %s", ss->ip, ss->ss_port, strerror(errno));
        close(sock);
        return -1;
    }
    resp[r] = '\0';

    close(sock);
    return 0;
}

// Periodic thread to purge dead SS entries
void *ss_heartbeat_monitor(void *arg) {
    (void)arg;
    while (1) {
        sleep(SS_TIMEOUT);
        pthread_mutex_lock(&ss_lock);
        StorageServer *prev = NULL, *p = ss_list;
        time_t now = time(NULL);
        while (p) {
            if (now - p->last_seen > SS_TIMEOUT) {
                log_message(LOGLVL_WARN, "SS %s:%d timed out, removing", p->ip, p->ss_port);
                StorageServer *tofree = p;
                if (prev) prev->next = p->next;
                else ss_list = p->next;
                p = p->next;
                free(tofree);
            } else {
                prev = p;
                p = p->next;
            }
        }
        pthread_mutex_unlock(&ss_lock);
    }
    return NULL;
}
// ############## LLM Generated Code Ends ##############
