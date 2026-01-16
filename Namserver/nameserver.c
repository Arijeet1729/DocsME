// nameserver.c
// Name Server main and client-handling logic.
// Compile: gcc -pthread -o nameserver nameserver.c nm_storage.c nm_files.c

// ############## LLM Generated Code Begins ##############
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <time.h>
#include <errno.h>

#include "nm.h"
#include "logger.h"

typedef struct {
    int client_fd;
    char addr_str[64];
} ClientConn;

ClientInfo *client_list = NULL;
pthread_mutex_t client_lock = PTHREAD_MUTEX_INITIALIZER;

ssize_t send_line(int fd, const char *s) {
    size_t n = strlen(s);
    ssize_t w = write(fd, s, n);
    if (w < 0) return w;
    w = write(fd, "\n", 1);
    return w < 0 ? -1 : (ssize_t)(n + 1);
}

void register_client_info(int fd, const char *ip, const char *username, int client_port) {
    pthread_mutex_lock(&client_lock);
    ClientInfo *p = client_list;
    while (p) {
        if (p->fd == fd) {
            strncpy(p->username, username, sizeof(p->username)-1);
            strncpy(p->ip, ip, sizeof(p->ip)-1);
            p->client_port = client_port;
            p->last_seen = time(NULL);
            pthread_mutex_unlock(&client_lock);
            return;
        }
        p = p->next;
    }

    ClientInfo *c = calloc(1, sizeof(ClientInfo));
    c->fd = fd;
    strncpy(c->username, username, sizeof(c->username)-1);
    strncpy(c->ip, ip, sizeof(c->ip)-1);
    c->client_port = client_port;
    c->last_seen = time(NULL);
    c->next = client_list;
    client_list = c;
    pthread_mutex_unlock(&client_lock);
}

ClientInfo *find_client_by_fd(int fd) {
    pthread_mutex_lock(&client_lock);
    ClientInfo *p = client_list;
    while (p) {
        if (p->fd == fd) {
            pthread_mutex_unlock(&client_lock);
            return p;
        }
        p = p->next;
    }
    pthread_mutex_unlock(&client_lock);
    return NULL;
}

void remove_client_info(int fd) {
    pthread_mutex_lock(&client_lock);
    ClientInfo *prev = NULL, *p = client_list;
    while (p) {
        if (p->fd == fd) {
            if (prev) prev->next = p->next;
            else client_list = p->next;
            free(p);
            break;
        }
        prev = p;
        p = p->next;
    }
    pthread_mutex_unlock(&client_lock);
}

// Worker thread for each connected client (or SS)
void *connection_handler(void *arg) {
    int fd = ((ClientConn *)arg)->client_fd;
    char addr[64];
    strncpy(addr, ((ClientConn *)arg)->addr_str, sizeof(addr)-1);
    free(arg);

    char buf[BUF_SZ];
    ssize_t r;

    // Basic loop: read lines and respond
    while ((r = read(fd, buf, sizeof(buf)-1)) > 0) {
        ClientInfo* ci = find_client_by_fd(fd);
        if (ci) {
            ci->last_seen = time(NULL);
        }
        buf[r] = '\0';
        // trim newline
        char *nl = strchr(buf, '\n');
        if (nl) *nl = '\0';

        if (strncmp(buf, "REGISTER_CLIENT", 15) == 0) {
            // format: REGISTER_CLIENT <username> <client_port>
            char username[64]; int client_port = 0;
            int cnt = sscanf(buf, "REGISTER_CLIENT %63s %d", username, &client_port);
            if (cnt >= 1) {
                if (cnt < 2) client_port = 0;
                register_client_info(fd, addr, username, client_port);
                // Add user to persistent registry
                add_user_to_registry(username, addr);
                char reply[128];
                snprintf(reply, sizeof(reply), "OK Registered client %s", username);
                send_line(fd, reply);
                log_message(LOGLVL_INFO, "Client registered: user='%s' ip='%s' port=%d", username, addr, client_port);
                continue;
            } else {
                log_message(LOGLVL_WARN, "Bad REGISTER_CLIENT request from %s", addr);
                send_line(fd, "ERR bad REGISTER_CLIENT format");
                continue;
            }
        }

        if (strncmp(buf, "REGISTER_SS", 11) == 0) {
            // format: REGISTER_SS <ip> <ss_port>
            char ip[64]; int ss_port;
            if (sscanf(buf, "REGISTER_SS %63s %d", ip, &ss_port) == 2) {
                register_storage_server(ip, ss_port);
                log_message(LOGLVL_INFO, "Storage server registered: ip='%s' port=%d", ip, ss_port);
                send_line(fd, "OK Registered SS");
                continue;
            } else {
                log_message(LOGLVL_WARN, "Bad REGISTER_SS request from %s", addr);
                send_line(fd, "ERR bad REGISTER_SS format");
                continue;
            }
        }

        // VIEW or VIEW -a, VIEW -l, VIEW -al
        if (strncmp(buf, "VIEW", 4) == 0) {
            // Check for -a flag (either as "-a" or within "-al" or "-la")
            int all = (strstr(buf, "-a") != NULL) || (strstr(buf, "a") != NULL && strchr(buf, '-') != NULL);
            // Check for -l flag (either as "-l" or within "-al" or "-la")
            int detailed = (strstr(buf, "-l") != NULL) || (strstr(buf, "l") != NULL && strchr(buf, '-') != NULL);
            // More robust: check if 'a' or 'l' appears after any '-'
            char *dash = strchr(buf, '-');
            if (dash) {
                all = (strchr(dash, 'a') != NULL);
                detailed = (strchr(dash, 'l') != NULL);
            }
            log_message(LOGLVL_INFO, "Request from %s: VIEW (all=%d, detailed=%d)", addr, all, detailed);
            handle_view(fd, all, detailed);
            continue;
        }

        // INFO <filename>
        if (strncmp(buf, "INFO ", 5) == 0) {
            char filename[256];
            if (sscanf(buf, "INFO %255s", filename) == 1) {
                log_message(LOGLVL_INFO, "Request from %s: INFO %s", addr, filename);
                handle_info_request(fd, filename);
            } else {
                log_message(LOGLVL_WARN, "Bad INFO request from %s: %s", addr, buf);
                send_line(fd, "ERR bad INFO syntax");
            }
            continue;
        }

        // CREATE <filename> <owner>
        if (strncmp(buf, "CREATE ", 7) == 0) {
            char filename[256], owner[64];
            if (sscanf(buf, "CREATE %255s %63s", filename, owner) >= 1) {
                log_message(LOGLVL_INFO, "Request from %s: CREATE %s (owner=%s)", addr, filename, owner);
                StorageServer *ss = choose_ss_for_new_file();
                if (!ss) {
                    send_line(fd, "ERROR No storage servers available");
                } else {
                    // First ask SS to create the file physically.
                    char cmd[BUF_SZ];
                    char resp[BUF_SZ];
                    snprintf(cmd, sizeof(cmd), "SS_CREATE %s %s", filename, owner);
                    if (send_command_to_ss(ss, cmd, resp, sizeof(resp)) < 0) {
                        send_line(fd, "ERROR Could not contact storage server");
                    } else if (strncmp(resp, "ACK OK", 6) != 0) {
                        send_line(fd, "ERROR Storage server create failed");
                    } else {
                        // On success, update NM metadata.
                        if (create_file(filename, owner, ss) == 0)
                            send_line(fd, "OK File created");
                        else
                            send_line(fd, "ERROR File already exists");
                    }
                }
            } else {
                log_message(LOGLVL_WARN, "Bad CREATE request from %s: %s", addr, buf);
                send_line(fd, "ERR bad CREATE syntax");
            }
            continue;
        }

        // DELETE <filename>
        if (strncmp(buf, "DELETE ", 7) == 0) {
            char filename[256];
            if (sscanf(buf, "DELETE %255s", filename) == 1) {
                log_message(LOGLVL_INFO, "Request from %s: DELETE %s", addr, filename);
                FileEntry *f = find_file(filename);
                if (!f) {
                    send_line(fd, "ERROR File not found");
                } else if (!f->ss) {
                    send_line(fd, "ERROR File has no storage server");
                } else {
                    char cmd[BUF_SZ];
                    char resp[BUF_SZ];
                    snprintf(cmd, sizeof(cmd), "SS_DELETE %s", filename);
                    if (send_command_to_ss(f->ss, cmd, resp, sizeof(resp)) < 0) {
                        send_line(fd, "ERROR Could not contact storage server");
                    } else if (strncmp(resp, "ACK OK", 6) != 0) {
                        send_line(fd, "ERROR Storage server delete failed");
                    } else {
                        if (delete_file(filename) == 0)
                            send_line(fd, "OK File deleted");
                        else
                            send_line(fd, "ERROR File not found");
                    }
                }
            } else {
                log_message(LOGLVL_WARN, "Bad DELETE request from %s: %s", addr, buf);
                send_line(fd, "ERR bad DELETE syntax");
            }
            continue;
        }

        // READ <filename> -> redirect to SS
        if (strncmp(buf, "READ ", 5) == 0) {
            char filename[256];
            if (sscanf(buf, "READ %255s", filename) == 1) {
                log_message(LOGLVL_INFO, "Request from %s: READ %s", addr, filename);
                handle_read_request(fd, filename, buf);
            } else {
                log_message(LOGLVL_WARN, "Bad READ request from %s: %s", addr, buf);
                send_line(fd, "ERR bad READ syntax");
            }
            continue;
        }

        // STREAM <filename> -> redirect to SS for word-by-word streaming
        if (strncmp(buf, "STREAM ", 7) == 0) {
            char filename[256];
            if (sscanf(buf, "STREAM %255s", filename) == 1) {
                log_message(LOGLVL_INFO, "Request from %s: STREAM %s", addr, filename);
                handle_stream_request(fd, filename);
            } else {
                log_message(LOGLVL_WARN, "Bad STREAM request from %s: %s", addr, buf);
                send_line(fd, "ERR bad STREAM syntax");
            }
            continue;
        }

        // WRITE <filename> <sentence_number> -> redirect to SS for word-level editing
        if (strncmp(buf, "WRITE ", 6) == 0) {
            char filename[256];
            int sentence_number;
            if (sscanf(buf, "WRITE %255s %d", filename, &sentence_number) == 2) {
                log_message(LOGLVL_INFO, "Request from %s: WRITE %s (sentence=%d)", addr, filename, sentence_number);
                handle_write_request(fd, filename, sentence_number);
            } else {
                log_message(LOGLVL_WARN, "Bad WRITE request from %s: %s", addr, buf);
                send_line(fd, "ERR bad WRITE syntax");
            }
            continue;
        }

        // LIST -> show all registered users
        if (strncmp(buf, "LIST", 4) == 0 && (buf[4] == '\0' || buf[4] == '\n' || buf[4] == ' ')) {
            log_message(LOGLVL_INFO, "Request from %s: LIST", addr);
            handle_list_users(fd);
            continue;
        }

        // ADDACCESS -R/-W <filename> <username> -> add read/write access
        if (strncmp(buf, "ADDACCESS ", 10) == 0) {
            char flag[8], filename[256], username[64];
            if (sscanf(buf, "ADDACCESS %7s %255s %63s", flag, filename, username) == 3) {
                ClientInfo *ci = find_client_by_fd(fd);
                if (!ci || ci->username[0] == '\0') {
                    send_line(fd, "ERROR You must be registered to manage access");
                } else {
                    int read_access = 0, write_access = 0;
                    if (strcmp(flag, "-R") == 0) {
                        read_access = 1;
                    } else if (strcmp(flag, "-W") == 0) {
                        read_access = 1;
                        write_access = 1;
                    } else {
                        send_line(fd, "ERROR Invalid flag. Use -R for read or -W for write access");
                        continue;
                    }
                    handle_addaccess_request(fd, filename, username, read_access, write_access, ci->username);
                }
            } else {
                send_line(fd, "ERR bad ADDACCESS syntax");
            }
            continue;
        }

        // REMACCESS <filename> <username> -> remove all access
        if (strncmp(buf, "REMACCESS ", 10) == 0) {
            char filename[256], username[64];
            if (sscanf(buf, "REMACCESS %255s %63s", filename, username) == 2) {
                ClientInfo *ci = find_client_by_fd(fd);
                if (!ci || ci->username[0] == '\0') {
                    send_line(fd, "ERROR You must be registered to manage access");
                } else {
                    handle_remaccess_request(fd, filename, username, ci->username);
                }
            } else {
                send_line(fd, "ERR bad REMACCESS syntax");
            }
            continue;
        }

        // EXEC <filename> -> execute file content as shell commands
        if (strncmp(buf, "EXEC ", 5) == 0) {
            char filename[256];
            if (sscanf(buf, "EXEC %255s", filename) == 1) {
                log_message(LOGLVL_INFO, "Request from %s: EXEC %s", addr, filename);
                handle_exec_request(fd, filename);
            } else {
                log_message(LOGLVL_WARN, "Bad EXEC request from %s: %s", addr, buf);
                send_line(fd, "ERR bad EXEC syntax");
            }
            continue;
        }

        // UNDO <filename> -> undo last change to file
        if (strncmp(buf, "UNDO ", 5) == 0) {
            char filename[256];
            if (sscanf(buf, "UNDO %255s", filename) == 1) {
                log_message(LOGLVL_INFO, "Request from %s: UNDO %s", addr, filename);
                FileEntry *f = find_file(filename);
                if (!f) {
                    send_line(fd, "ERROR File not found");
                } else if (!f->ss) {
                    send_line(fd, "ERROR File has no storage server");
                } else {
                    char cmd[BUF_SZ];
                    char resp[BUF_SZ];
                    snprintf(cmd, sizeof(cmd), "SS_UNDO %s", filename);
                    if (send_command_to_ss(f->ss, cmd, resp, sizeof(resp)) < 0) {
                        send_line(fd, "ERROR Could not contact storage server");
                    } else if (strncmp(resp, "ACK OK", 6) != 0) {
                        send_line(fd, "ERROR Storage server undo failed");
                    } else {
                        send_line(fd, "OK Undo successful");
                    }
                }
            } else {
                log_message(LOGLVL_WARN, "Bad UNDO request from %s: %s", addr, buf);
                send_line(fd, "ERR bad UNDO syntax");
            }
            continue;
        }

        // CACHE_STATS -> show cache performance statistics
        if (strncmp(buf, "CACHE_STATS", 11) == 0) {
            log_message(LOGLVL_INFO, "Request from %s: CACHE_STATS", addr);
            cache_print_stats();
            send_line(fd, "OK Cache stats printed to server console");
            continue;
        }

        // Unknown
        log_message(LOGLVL_WARN, "Unknown command from %s: '%s'", addr, buf);
        send_line(fd, "ERR Unknown command");
    }

    if (r == 0) {
        log_message(LOGLVL_INFO, "Connection closed from %s", addr);
    } else {
        log_message(LOGLVL_ERROR, "Read error from %s: %s", addr, strerror(errno));
    }

    remove_client_info(fd);
    close(fd);
    return NULL;
}

int main(int argc, char **argv) {
    int listen_port = 9000;
    if (argc >= 2) listen_port = atoi(argv[1]);

    // Initialize LRU cache for efficient O(1) file lookups
    cache_init();

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { log_message(LOGLVL_ERROR, "Failed to create socket: %s", strerror(errno)); exit(1); }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in serv;
    serv.sin_family = AF_INET;
    serv.sin_addr.s_addr = INADDR_ANY;
    serv.sin_port = htons(listen_port);

    if (bind(listen_fd, (struct sockaddr *)&serv, sizeof(serv)) < 0) {
        log_message(LOGLVL_ERROR, "Failed to bind to port %d: %s", listen_port, strerror(errno));
        exit(1);
    }
    if (listen(listen_fd, BACKLOG) < 0) {
        log_message(LOGLVL_ERROR, "Failed to listen on port %d: %s", listen_port, strerror(errno));
        exit(1);
    }

    log_message(LOGLVL_INFO, "Name Server listening on port %d", listen_port);

    // Load persistent registries
    load_user_registry();
    load_file_registry();
    load_access_registry();  // Load after file registry so files exist

    pthread_t hb_thread;
    pthread_create(&hb_thread, NULL, ss_heartbeat_monitor, NULL);

    while (1) {
        struct sockaddr_in cliaddr;
        socklen_t clilen = sizeof(cliaddr);
        int client_fd = accept(listen_fd, (struct sockaddr *)&cliaddr, &clilen);
        if (client_fd < 0) {
            log_message(LOGLVL_ERROR, "Failed to accept connection: %s", strerror(errno));
            continue;
        }

        char addrstr[64];
        inet_ntop(AF_INET, &cliaddr.sin_addr, addrstr, sizeof(addrstr));
        log_message(LOGLVL_INFO, "Accepted connection from %s:%d", addrstr, ntohs(cliaddr.sin_port));

        ClientConn *cc = malloc(sizeof(ClientConn));
        cc->client_fd = client_fd;
        strncpy(cc->addr_str, addrstr, sizeof(cc->addr_str)-1);

        pthread_t t;
        pthread_create(&t, NULL, connection_handler, cc);
        pthread_detach(t);
    }

    close(listen_fd);
    return 0;
}
// ############## LLM Generated Code Ends ##############
