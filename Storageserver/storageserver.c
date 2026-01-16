#include "ss.h"
#include "write_ops.h"
#include <time.h>

// ############## LLM Generated Code Begins ##############

SSConfig g_cfg;

int connect_to_nm(void) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("[SS] socket to NM");
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(g_cfg.nm_port);
    if (inet_pton(AF_INET, g_cfg.nm_ip, &addr.sin_addr) <= 0) {
        perror("[SS] inet_pton NM");
        close(sock);
        return -1;
    }

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[SS] connect NM");
        close(sock);
        return -1;
    }

    if (g_cfg.ss_ip[0] == '\0') {
        struct sockaddr_in local;
        socklen_t len = sizeof(local);
        if (getsockname(sock, (struct sockaddr *)&local, &len) == 0) {
            if (!inet_ntop(AF_INET, &local.sin_addr, g_cfg.ss_ip, sizeof(g_cfg.ss_ip))) {
                strcpy(g_cfg.ss_ip, "127.0.0.1");
            }
        } else {
            perror("[SS] getsockname");
            strcpy(g_cfg.ss_ip, "127.0.0.1");
        }
    }

    return sock;
}

void send_registration_once(void) {
    int sock = connect_to_nm();
    if (sock < 0) return;

    char msg[256];
    int n = snprintf(msg, sizeof(msg), "REGISTER_SS %s %d 0\n", g_cfg.ss_ip, g_cfg.ss_cmd_port);
    if (n < 0 || n >= (int)sizeof(msg)) {
        close(sock);
        return;
    }

    if (write(sock, msg, strlen(msg)) < 0) {
        perror("[SS] write REGISTER_SS");
        close(sock);
        return;
    }

    char buf[BUF_SIZE];
    (void)read(sock, buf, sizeof(buf) - 1);

    close(sock);
}

void *heartbeat_thread(void *arg) {
    (void)arg;
    while (1) {
        send_registration_once();
        sleep(HEARTBEAT_INTERVAL);
    }
    return NULL;
}

int connect_to_client(const char *ip, int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("[SS] socket to client");
        return -1;
    }
    
    // Set socket to non-blocking for timeout
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        perror("[SS] inet_pton client");
        close(sock);
        return -1;
    }

    int ret = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    if (ret < 0 && errno != EINPROGRESS) {
        perror("[SS] connect client");
        close(sock);
        return -1;
    }
    
    // Wait for connection with 2 second timeout
    if (errno == EINPROGRESS) {
        fd_set wfds;
        struct timeval tv;
        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);
        tv.tv_sec = 2;
        tv.tv_usec = 0;
        
        ret = select(sock + 1, NULL, &wfds, NULL, &tv);
        if (ret <= 0) {
            fprintf(stderr, "[SS] connect timeout to client %s:%d\n", ip, port);
            close(sock);
            return -1;
        }
        
        // Check if connection succeeded
        int error = 0;
        socklen_t len = sizeof(error);
        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error != 0) {
            fprintf(stderr, "[SS] connect failed to client: %s\n", strerror(error));
            close(sock);
            return -1;
        }
    }
    
    // Set back to blocking mode
    fcntl(sock, F_SETFL, flags);

    return sock;
}

int serve_read_to_client(const char *filename, const char *client_ip, int client_port) {
    int cfd = connect_to_client(client_ip, client_port);
    if (cfd < 0) return -1;

    int fd = open(filename, O_RDONLY);
    if (fd < 0) {
        const char *err = "ERROR cannot open file\n";
        write(cfd, err, strlen(err));
        const char *stop = "STOP\n";
        write(cfd, stop, strlen(stop));
        close(cfd);
        return -1;
    }

    char buf[BUF_SIZE];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        if (write(cfd, buf, n) < 0) {
            perror("[SS] write to client");
            break;
        }
    }

    close(fd);
    const char *stop = "STOP\n";
    write(cfd, stop, strlen(stop));
    close(cfd);
    return 0;
}

int serve_stream_to_client(const char *filename, const char *client_ip, int client_port) {
    int cfd = connect_to_client(client_ip, client_port);
    if (cfd < 0) return -1;

    // Disable Nagle's algorithm for immediate transmission
    int flag = 1;
    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    FILE *fp = fopen(filename, "r");
    if (!fp) {
        const char *err = "ERROR cannot open file\n";
        write(cfd, err, strlen(err));
        const char *stop = "STOP\n";
        write(cfd, stop, strlen(stop));
        close(cfd);
        return -1;
    }

    // Stream word by word continuously (like LLM streaming)
    char word[256];
    int word_count = 0;
    
    while (fscanf(fp, "%255s", word) == 1) {
        // Send word followed by space
        if (write(cfd, word, strlen(word)) < 0) {
            perror("[SS] write to client failed");
            fclose(fp);
            close(cfd);
            return -1;
        }
        if (write(cfd, " ", 1) < 0) {
            perror("[SS] write to client failed");
            fclose(fp);
            close(cfd);
            return -1;
        }
        
        // Force immediate transmission (flush TCP buffer)
        int sync_flag = 1;
        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &sync_flag, sizeof(sync_flag));
        
        word_count++;
        
        // Small delay to simulate natural streaming (100ms = 0.1 seconds as per requirement)
        usleep(100000);
    }

    fclose(fp);
    
    // Send newline and STOP marker
    const char *newline = "\n";
    write(cfd, newline, strlen(newline));
    const char *stop = "STOP\n";
    write(cfd, stop, strlen(stop));
    close(cfd);
    
    printf("[SS] Streamed %d words from %s to %s:%d\n", word_count, filename, client_ip, client_port);
    return 0;
}

int handle_ss_create(const char *filename, const char *owner) {
    int fd = open(filename, O_CREAT | O_EXCL | O_WRONLY, 0644);
    if (fd < 0) return -1;
    close(fd);

    // Create sidecar metadata file <filename>.bak that will not be shown in VIEW.
    char bakname[512];
    snprintf(bakname, sizeof(bakname), "%s.bak", filename);

    int mfd = open(bakname, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (mfd >= 0) {
        char meta[512];
        time_t now = time(NULL);
        // Store owner, creation time, last_modified, last_accessed, and access info
        int n = snprintf(meta, sizeof(meta), 
                        "owner=%s\ncreated=%ld\nlast_modified=%ld\nlast_accessed=%ld\nlast_accessed_by=%s\naccess=%s (RW)\n", 
                        owner, 
                        (long)now, 
                        (long)now, 
                        (long)now,
                        owner,
                        owner);
        if (n > 0 && n < (int)sizeof(meta)) {
            (void)write(mfd, meta, (size_t)n);
        }
        close(mfd);
    }

    return 0;
}

int handle_ss_delete(const char *filename) {
    if (unlink(filename) < 0) return -1;
    return 0;
}

// Update .bak file with last access information
void update_bak_access(const char *filename, const char *username) {
    if (!filename || !username) return;
    
    char bakname[512];
    snprintf(bakname, sizeof(bakname), "%s.bak", filename);
    
    // Read existing metadata
    char owner[64] = "unknown";
    long created = 0;
    char access_info[128] = "unknown";
    
    int mfd = open(bakname, O_RDONLY);
    if (mfd >= 0) {
        char buf[1024];
        ssize_t n = read(mfd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            char *line = strtok(buf, "\n");
            while (line) {
                if (strncmp(line, "owner=", 6) == 0) {
                    strncpy(owner, line + 6, sizeof(owner) - 1);
                    owner[sizeof(owner) - 1] = '\0';
                } else if (strncmp(line, "created=", 8) == 0) {
                    created = atol(line + 8);
                } else if (strncmp(line, "access=", 7) == 0) {
                    strncpy(access_info, line + 7, sizeof(access_info) - 1);
                    access_info[sizeof(access_info) - 1] = '\0';
                }
                line = strtok(NULL, "\n");
            }
        }
        close(mfd);
    }
    
    // Write updated metadata
    mfd = open(bakname, O_WRONLY | O_TRUNC);
    if (mfd >= 0) {
        char meta[1024];
        time_t now = time(NULL);
        struct stat st;
        long last_modified = now;
        if (stat(filename, &st) == 0) {
            last_modified = (long)st.st_mtime;
        }
        
        int n = snprintf(meta, sizeof(meta), 
                        "owner=%s\ncreated=%ld\nlast_modified=%ld\nlast_accessed=%ld\nlast_accessed_by=%s\naccess=%s\n", 
                        owner, 
                        created, 
                        last_modified,
                        (long)now,
                        username,
                        access_info);
        if (n > 0 && n < (int)sizeof(meta)) {
            (void)write(mfd, meta, (size_t)n);
        }
        close(mfd);
    }
}

// Build INFO details (size, permissions, timestamps, owner) into out buffer.
int handle_ss_info(const char *filename, char *out, size_t out_sz) {
    if (!filename || !out || out_sz == 0) return -1;

    struct stat st;
    if (stat(filename, &st) < 0) {
        // File itself must exist for INFO
        return -1;
    }

    // Attempt to read sidecar .bak for logical metadata
    char bakname[512];
    snprintf(bakname, sizeof(bakname), "%s.bak", filename);

    char owner[64] = "unknown";
    long created = 0;
    long last_modified = (long)st.st_mtime;
    long last_accessed = (long)st.st_mtime;
    char last_accessed_by[64] = "unknown";
    char access_info[128] = "unknown";

    int mfd = open(bakname, O_RDONLY);
    if (mfd >= 0) {
        char buf[1024];
        ssize_t n = read(mfd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            // Parse all metadata fields
            char *line = strtok(buf, "\n");
            while (line) {
                if (strncmp(line, "owner=", 6) == 0) {
                    strncpy(owner, line + 6, sizeof(owner) - 1);
                    owner[sizeof(owner) - 1] = '\0';
                } else if (strncmp(line, "created=", 8) == 0) {
                    created = atol(line + 8);
                } else if (strncmp(line, "last_modified=", 14) == 0) {
                    last_modified = atol(line + 14);
                } else if (strncmp(line, "last_accessed=", 14) == 0) {
                    last_accessed = atol(line + 14);
                } else if (strncmp(line, "last_accessed_by=", 17) == 0) {
                    strncpy(last_accessed_by, line + 17, sizeof(last_accessed_by) - 1);
                    last_accessed_by[sizeof(last_accessed_by) - 1] = '\0';
                } else if (strncmp(line, "access=", 7) == 0) {
                    strncpy(access_info, line + 7, sizeof(access_info) - 1);
                    access_info[sizeof(access_info) - 1] = '\0';
                }
                line = strtok(NULL, "\n");
            }
        }
        close(mfd);
    }

    // Format timestamps as readable dates
    char created_str[64], modified_str[64], accessed_str[64];
    struct tm *tm_info;
    
    if (created > 0) {
        tm_info = localtime(&created);
        strftime(created_str, sizeof(created_str), "%Y-%m-%d %H:%M", tm_info);
    } else {
        snprintf(created_str, sizeof(created_str), "unknown");
    }
    
    tm_info = localtime(&last_modified);
    strftime(modified_str, sizeof(modified_str), "%Y-%m-%d %H:%M", tm_info);
    
    tm_info = localtime(&last_accessed);
    strftime(accessed_str, sizeof(accessed_str), "%Y-%m-%d %H:%M", tm_info);

    // Format INFO in a user-friendly format with --> prefix
    int written = snprintf(out, out_sz,
                           "--> File: %s\n--> Owner: %s\n--> Created: %s\n--> Last Modified: %s\n--> Size: %lld bytes\n--> Access: %s\n--> Last Accessed: %s by %s",
                           filename,
                           owner,
                           created_str,
                           modified_str,
                           (long long)st.st_size,
                           access_info,
                           accessed_str,
                           last_accessed_by);

    return (written > 0 && (size_t)written < out_sz) ? 0 : -1;
}

void *nm_command_handler(void *arg) {
    int fd = *(int *)arg;
    free(arg);

    char buf[BUF_SIZE];
    ssize_t r = read(fd, buf, sizeof(buf) - 1);
    if (r <= 0) {
        close(fd);
        return NULL;
    }
    buf[r] = '\0';
    char *nl = strchr(buf, '\n');
    if (nl) *nl = '\0';

    if (strncmp(buf, "SS_READ ", 8) == 0) {
        char filename[256], client_ip[64], username[64];
        int client_port;
        if (sscanf(buf, "SS_READ %255s %63s %d %63s", filename, client_ip, &client_port, username) == 4) {
            int rc = serve_read_to_client(filename, client_ip, client_port);
            if (rc == 0) {
                update_bak_access(filename, username);
                const char *ack = "ACK OK\n";
                write(fd, ack, strlen(ack));
            } else {
                const char *ack = "ACK ERROR read_failed\n";
                write(fd, ack, strlen(ack));
            }
        } else {
            const char *ack = "ACK ERROR bad_SS_READ_syntax\n";
            write(fd, ack, strlen(ack));
        }
        close(fd);
        return NULL;
    }

    if (strncmp(buf, "SS_STREAM ", 10) == 0) {
        char filename[256], client_ip[64], username[64];
        int client_port;
        if (sscanf(buf, "SS_STREAM %255s %63s %d %63s", filename, client_ip, &client_port, username) == 4) {
            int rc = serve_stream_to_client(filename, client_ip, client_port);
            if (rc == 0) {
                update_bak_access(filename, username);
                const char *ack = "ACK OK\n";
                write(fd, ack, strlen(ack));
            } else {
                const char *ack = "ACK ERROR stream_failed\n";
                write(fd, ack, strlen(ack));
            }
        } else {
            const char *ack = "ACK ERROR bad_SS_STREAM_syntax\n";
            write(fd, ack, strlen(ack));
        }
        close(fd);
        return NULL;
    }

    if (strncmp(buf, "SS_CREATE ", 10) == 0) {
        char filename[256], owner[64];
        if (sscanf(buf, "SS_CREATE %255s %63s", filename, owner) >= 1) {
            // Owner might be absent in older callers; fall back to "unknown".
            const char *own = (strchr(buf, ' ') ? owner : "unknown");
            int rc = handle_ss_create(filename, own);
            if (rc == 0) {
                const char *ack = "ACK OK\n";
                write(fd, ack, strlen(ack));
            } else {
                const char *ack = "ACK ERROR create_failed\n";
                write(fd, ack, strlen(ack));
            }
        } else {
            const char *ack = "ACK ERROR bad_SS_CREATE_syntax\n";
            write(fd, ack, strlen(ack));
        }
        close(fd);
        return NULL;
    }

    if (strncmp(buf, "SS_INFO ", 8) == 0) {
        char filename[256];
        if (sscanf(buf, "SS_INFO %255s", filename) == 1) {
            char info[BUF_SIZE];
            int rc = handle_ss_info(filename, info, sizeof(info));
            if (rc == 0) {
                char ack[BUF_SIZE + 32];
                int n = snprintf(ack, sizeof(ack), "ACK OK %s\n", info);
                if (n > 0 && n < (int)sizeof(ack)) {
                    write(fd, ack, (size_t)n);
                }
            } else {
                const char *ack = "ACK ERROR info_failed\n";
                write(fd, ack, strlen(ack));
            }
        } else {
            const char *ack = "ACK ERROR bad_SS_INFO_syntax\n";
            write(fd, ack, strlen(ack));
        }
        close(fd);
        return NULL;
    }

    if (strncmp(buf, "SS_DELETE ", 10) == 0) {
        char filename[256];
        if (sscanf(buf, "SS_DELETE %255s", filename) == 1) {
            int rc = handle_ss_delete(filename);
            if (rc == 0) {
                const char *ack = "ACK OK\n";
                write(fd, ack, strlen(ack));
            } else {
                const char *ack = "ACK ERROR delete_failed\n";
                write(fd, ack, strlen(ack));
            }
        } else {
            const char *ack = "ACK ERROR bad_SS_DELETE_syntax\n";
            write(fd, ack, strlen(ack));
        }
        close(fd);
        return NULL;
    }

    if (strncmp(buf, "SS_UNDO ", 8) == 0) {
        char filename[256];
        if (sscanf(buf, "SS_UNDO %255s", filename) == 1) {
            int rc = handle_ss_undo(filename);
            if (rc == 0) {
                const char *ack = "ACK OK\n";
                write(fd, ack, strlen(ack));
            } else {
                const char *ack = "ACK ERROR undo_failed\n";
                write(fd, ack, strlen(ack));
            }
        } else {
            const char *ack = "ACK ERROR bad_SS_UNDO_syntax\n";
            write(fd, ack, strlen(ack));
        }
        close(fd);
        return NULL;
    }

    if (strncmp(buf, "SS_WRITE ", 9) == 0) {
        char filename[256], client_ip[64], owner[64];
        int sentence_number, client_port;
        if (sscanf(buf, "SS_WRITE %255s %d %63s %d %63s", filename, &sentence_number, client_ip, &client_port, owner) == 5) {
            // Send ACK OK immediately so NM can respond to client
            const char *ack = "ACK OK\n";
            write(fd, ack, strlen(ack));
            close(fd);
            
            // Now handle the WRITE session in the background (this will connect to client)
            handle_ss_write(filename, sentence_number, client_ip, client_port, owner);
        } else {
            const char *ack = "ACK ERROR bad_SS_WRITE_syntax\n";
            write(fd, ack, strlen(ack));
            close(fd);
        }
        return NULL;
    }

    if (strncmp(buf, "SS_GET_CONTENT ", 15) == 0) {
        char filename[256];
        if (sscanf(buf, "SS_GET_CONTENT %255s", filename) == 1) {
            FILE *f = fopen(filename, "r");
            if (!f) {
                const char *ack = "ACK ERROR file_not_found\n";
                write(fd, ack, strlen(ack));
            } else {
                // Send ACK OK followed by file content
                const char *ack = "ACK OK\n";
                write(fd, ack, strlen(ack));
                
                // Read and send file content
                char content[BUF_SIZE];
                size_t n;
                while ((n = fread(content, 1, sizeof(content) - 1, f)) > 0) {
                    content[n] = '\0';
                    write(fd, content, n);
                }
                fclose(f);
                
                // Send END marker
                const char *end = "\nEND_CONTENT\n";
                write(fd, end, strlen(end));
            }
        } else {
            const char *ack = "ACK ERROR bad_SS_GET_CONTENT_syntax\n";
            write(fd, ack, strlen(ack));
        }
        close(fd);
        return NULL;
    }
    
    if (strncmp(buf, "SS_UPDATE_ACCESS ", 17) == 0) {
        char filename[256];
        char access_info[512];
        
        // Extract filename and access info (everything after the filename)
        char *space_pos = strchr(buf + 17, ' ');
        if (space_pos) {
            int filename_len = space_pos - (buf + 17);
            if (filename_len < 256) {
                strncpy(filename, buf + 17, filename_len);
                filename[filename_len] = '\0';
                
                // Get the access info (everything after the space)
                strncpy(access_info, space_pos + 1, sizeof(access_info) - 1);
                access_info[sizeof(access_info) - 1] = '\0';
                
                // Update the .bak file with new access info
                char bakname[512];
                snprintf(bakname, sizeof(bakname), "%s.bak", filename);
                
                // Read existing metadata
                char owner[64] = "unknown";
                long created = 0;
                long last_modified = 0;
                long last_accessed = 0;
                char last_accessed_by[64] = "unknown";
                
                int mfd = open(bakname, O_RDONLY);
                if (mfd >= 0) {
                    char buf[1024];
                    ssize_t n = read(mfd, buf, sizeof(buf) - 1);
                    if (n > 0) {
                        buf[n] = '\0';
                        char *line = strtok(buf, "\n");
                        while (line) {
                            if (strncmp(line, "owner=", 6) == 0) {
                                strncpy(owner, line + 6, sizeof(owner) - 1);
                                owner[sizeof(owner) - 1] = '\0';
                            } else if (strncmp(line, "created=", 8) == 0) {
                                created = atol(line + 8);
                            } else if (strncmp(line, "last_modified=", 14) == 0) {
                                last_modified = atol(line + 14);
                            } else if (strncmp(line, "last_accessed=", 14) == 0) {
                                last_accessed = atol(line + 14);
                            } else if (strncmp(line, "last_accessed_by=", 17) == 0) {
                                strncpy(last_accessed_by, line + 17, sizeof(last_accessed_by) - 1);
                                last_accessed_by[sizeof(last_accessed_by) - 1] = '\0';
                            }
                            line = strtok(NULL, "\n");
                        }
                    }
                    close(mfd);
                }
                
                // Write updated metadata
                mfd = open(bakname, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (mfd >= 0) {
                    char meta[1024];
                    int n = snprintf(meta, sizeof(meta), 
                                    "owner=%s\ncreated=%ld\nlast_modified=%ld\nlast_accessed=%ld\nlast_accessed_by=%s\naccess=%s\n", 
                                    owner, 
                                    created, 
                                    last_modified,
                                    last_accessed,
                                    last_accessed_by,
                                    access_info);
                    if (n > 0 && n < (int)sizeof(meta)) {
                        (void)write(mfd, meta, (size_t)n);
                    }
                    close(mfd);
                    
                    const char *ack = "ACK OK\n";
                    write(fd, ack, strlen(ack));
                } else {
                    const char *ack = "ACK ERROR could_not_update_access\n";
                    write(fd, ack, strlen(ack));
                }
            } else {
                const char *ack = "ACK ERROR filename_too_long\n";
                write(fd, ack, strlen(ack));
            }
        } else {
            const char *ack = "ACK ERROR bad_SS_UPDATE_ACCESS_syntax\n";
            write(fd, ack, strlen(ack));
        }
        close(fd);
        return NULL;
    }

    const char *ack = "ACK ERROR unknown_command\n";
    write(fd, ack, strlen(ack));
    close(fd);
    return NULL;
}

int create_listen_socket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("[SS] socket listen");
        return -1;
    }

    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("[SS] setsockopt");
        close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[SS] bind");
        close(fd);
        return -1;
    }

    if (listen(fd, 16) < 0) {
        perror("[SS] listen");
        close(fd);
        return -1;
    }

    return fd;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <ss_cmd_port> [nm_ip] [nm_port]\n", argv[0]);
        return 1;
    }

    memset(&g_cfg, 0, sizeof(g_cfg));

    g_cfg.ss_cmd_port = atoi(argv[1]);
    if (g_cfg.ss_cmd_port <= 0) {
        fprintf(stderr, "Invalid ss_cmd_port\n");
        return 1;
    }

    const char *nm_ip = (argc >= 3) ? argv[2] : "127.0.0.1";
    int nm_port = (argc >= 4) ? atoi(argv[3]) : 9000;

    strncpy(g_cfg.nm_ip, nm_ip, sizeof(g_cfg.nm_ip) - 1);
    g_cfg.nm_port = nm_port;
    g_cfg.ss_ip[0] = '\0';

    pthread_t hb;
    if (pthread_create(&hb, NULL, heartbeat_thread, NULL) != 0) {
        perror("[SS] pthread_create heartbeat");
        return 1;
    }

    int listen_fd = create_listen_socket(g_cfg.ss_cmd_port);
    if (listen_fd < 0) {
        return 1;
    }

    printf("[SS] Command listener on port %d, NM=%s:%d\n", g_cfg.ss_cmd_port, g_cfg.nm_ip, g_cfg.nm_port);

    while (1) {
        struct sockaddr_in cliaddr;
        socklen_t clilen = sizeof(cliaddr);
        int *fd = malloc(sizeof(int));
        if (!fd) {
            perror("[SS] malloc");
            continue;
        }

        *fd = accept(listen_fd, (struct sockaddr *)&cliaddr, &clilen);
        if (*fd < 0) {
            perror("[SS] accept");
            free(fd);
            continue;
        }

        pthread_t t;
        if (pthread_create(&t, NULL, nm_command_handler, fd) != 0) {
            perror("[SS] pthread_create command");
            close(*fd);
            free(fd);
            continue;
        }
        pthread_detach(t);
    }

    close(listen_fd);
    return 0;
}
// ############## LLM Generated Code Ends ##############
