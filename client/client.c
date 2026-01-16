#include "client.h"

// ANSI Color Codes
#define RESET   "\033[0m"
#define BOLD    "\033[1m"
#define RED     "\033[31m"
#define GREEN   "\033[32m"
#define YELLOW  "\033[33m"
#define BLUE    "\033[34m"
#define MAGENTA "\033[35m"
#define CYAN    "\033[36m"
#define WHITE   "\033[37m"
#define BG_BLUE "\033[44m"
#define BG_GREEN "\033[42m"
#define BG_RED  "\033[41m"
// ############## LLM Generated Code Begins ##############
void print_divider(int len) {
    printf(CYAN);
    for (int i = 0; i < len; i++) {
        printf("─");
    }
    printf(RESET "\n");
}

int client_listen_fd = -1;
int client_listen_port = 0;

// Utility: Connect to a server (IP, port)
int connect_to_server(char *ip, int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return -1; }

    struct sockaddr_in serv;
    memset(&serv, 0, sizeof(serv));
    serv.sin_family = AF_INET;
    serv.sin_port = htons(port);
    inet_pton(AF_INET, ip, &serv.sin_addr);

    if (connect(sock, (struct sockaddr *)&serv, sizeof(serv)) < 0) {
        perror("connect");
        close(sock);
        return -1;
    }

    return sock;
}

int create_client_listen_socket(int *out_port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("client listen socket");
        return -1;
    }

    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("client setsockopt");
        close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(0); // let OS pick

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("client bind");
        close(fd);
        return -1;
    }

    if (listen(fd, 16) < 0) {
        perror("client listen");
        close(fd);
        return -1;
    }

    socklen_t len = sizeof(addr);
    if (getsockname(fd, (struct sockaddr *)&addr, &len) == 0) {
        *out_port = ntohs(addr.sin_port);
    } else {
        perror("client getsockname");
        close(fd);
        return -1;
    }

    return fd;
}

void *client_data_listener(void *arg) {
    // unused in synchronous design
    (void)arg;
    return NULL;
}

// Handle WRITE session with Storage Server
static void handle_write_session(void) {
    struct sockaddr_in ssaddr;
    socklen_t slen = sizeof(ssaddr);
    int ss_fd = accept(client_listen_fd, (struct sockaddr *)&ssaddr, &slen);
    if (ss_fd < 0) {
        perror("client accept from SS for WRITE");
        return;
    }

    char buf[BUF_SIZE];
    
    // Wait for READY_FOR_WRITE signal from SS
    int n = recv_line(ss_fd, buf);
    if (n <= 0) {
        printf(RED "✗ ERROR: " RESET "Connection to Storage Server lost\n");
        close(ss_fd);
        return;
    }
    
    // Check if it's an error message
    if (strncmp(buf, "ERROR", 5) == 0) {
        printf(RED "✗ " RESET "%s\n", buf);
        close(ss_fd);
        return;
    }
    
    // Check for expected READY_FOR_WRITE
    if (strcmp(buf, "READY_FOR_WRITE") != 0) {
        printf(RED "✗ ERROR: " RESET "Storage Server not ready for WRITE (got: %s)\n", buf);
        close(ss_fd);
        return;
    }
    
    printf("\n" CYAN "╔═══════════════════════════════════════════════════╗\n");
    printf("║" BOLD "          WRITE SESSION STARTED" RESET CYAN "                ║\n");
    printf("╚═══════════════════════════════════════════════════╝" RESET "\n");
    printf(YELLOW "📝 Enter word updates: " RESET BOLD "<word_index> <content>" RESET "\n");
    printf(YELLOW "💾 Type " RESET BOLD GREEN "'ETIRW'" RESET YELLOW " to finish and commit changes." RESET "\n\n");
    
    // Interactive loop for word updates
    while (1) {
        printf(MAGENTA "write ▶ " RESET);
        fflush(stdout);
        
        char input[BUF_SIZE];
        if (!fgets(input, sizeof(input), stdin)) {
            break;
        }
        
        // Remove trailing newline
        size_t len = strlen(input);
        if (len > 0 && input[len-1] == '\n') {
            input[len-1] = '\0';
        }
        
        // Send input to SS
        write(ss_fd, input, strlen(input));
        write(ss_fd, "\n", 1);
        
        // Check if this was ETIRW (end command)
        if (strcmp(input, "ETIRW") == 0) {
            // Wait for final response
            n = recv_line(ss_fd, buf);
            if (n > 0) {
                if (strncmp(buf, "WRITE_COMPLETE", 14) == 0) {
                    printf(GREEN "✓ " BOLD "%s" RESET "\n", buf);
                } else {
                    printf("%s\n", buf);
                }
            }
            break;
        }
        
        // Wait for ACK from SS
        n = recv_line(ss_fd, buf);
        if (n > 0) {
            if (strcmp(buf, "ACK") == 0) {
                printf(GREEN "✓ OK" RESET "\n");
            } else {
                printf("%s\n", buf);
            }
        }
    }
    
    close(ss_fd);
}

// Accept a single connection from a Storage Server and stream until STOP
static void receive_from_ss_once(void) {
    struct sockaddr_in ssaddr;
    socklen_t slen = sizeof(ssaddr);
    int ss_fd = accept(client_listen_fd, (struct sockaddr *)&ssaddr, &slen);
    if (ss_fd < 0) {
        perror("client accept from SS");
        return;
    }

    char buf[BUF_SIZE];
    while (1) {
        int n = recv_line(ss_fd, buf);
        if (n <= 0) break;

        // Handle case where data and STOP arrive in the same chunk (e.g. "BRUHHHHSTOP")
        char *stop_pos = strstr(buf, "STOP");
        if (stop_pos) {
            *stop_pos = '\0';
            if (buf[0] != '\0') {
                printf(CYAN "%s" RESET, buf);
                fflush(stdout);
            }
            break;
        }

        printf("%s", buf);
        fflush(stdout);
    }

    // Ensure prompt starts on a new line after streamed data
    printf("\n");
    close(ss_fd);
}

// Accept connection from SS and display streaming content word-by-word in real-time
static void receive_stream_from_ss(void) {
    struct sockaddr_in ssaddr;
    socklen_t slen = sizeof(ssaddr);
    int ss_fd = accept(client_listen_fd, (struct sockaddr *)&ssaddr, &slen);
    if (ss_fd < 0) {
        perror("client accept from SS");
        return;
    }

    char ch;
    char word[256];
    int word_idx = 0;
    int stop_check_idx = 0;
    const char *stop_marker = "STOP";
    
    while (1) {
        ssize_t n = read(ss_fd, &ch, 1);
        if (n <= 0) {
            // Connection lost - display any remaining word
            if (word_idx > 0) {
                word[word_idx] = '\0';
                printf("%s", word);
                fflush(stdout);
            }
            printf("\n" RED "✗ ERROR: " RESET "Storage Server connection lost during streaming\n");
            break;
        }

        // Check for STOP marker
        if (ch == stop_marker[stop_check_idx]) {
            stop_check_idx++;
            if (stop_check_idx == 4) {  // Found "STOP"
                // Display any remaining word before stopping
                if (word_idx > 0) {
                    word[word_idx] = '\0';
                    printf("%s", word);
                    fflush(stdout);
                }
                break;
            }
        } else {
            // Reset STOP check if mismatch
            if (stop_check_idx > 0) {
                // We had partial STOP match, add those chars to word
                for (int i = 0; i < stop_check_idx; i++) {
                    if (word_idx < 255) {
                        word[word_idx++] = stop_marker[i];
                    }
                }
                stop_check_idx = 0;
            }
        }

        // Skip STOP marker characters from being added to word
        if (stop_check_idx > 0) {
            continue;
        }

        // Check if character is a word delimiter (space, newline, tab)
        if (ch == ' ' || ch == '\n' || ch == '\t') {
            if (word_idx > 0) {
                // We have a complete word - display it
                word[word_idx] = '\0';
                printf("%s", word);
                fflush(stdout);
                
                // Add 0.1 second delay after each word
                usleep(100000);
                
                // Print the delimiter (space/newline)
                printf("%c", ch);
                fflush(stdout);
                
                // Reset word buffer
                word_idx = 0;
            } else {
                // Empty word, just print the delimiter
                printf("%c", ch);
                fflush(stdout);
            }
        } else {
            // Accumulate character into current word
            if (word_idx < 255) {
                word[word_idx++] = ch;
            }
        }
    }

    printf("\n");
    close(ss_fd);
}

// =====================================================
// Communication Helpers
// =====================================================
void send_line(int sock, const char *msg) {
    write(sock, msg, strlen(msg));
}

int recv_line(int sock, char *buffer) {
    int idx = 0;
    while (idx < BUF_SIZE - 1) {
        char c;
        int n = read(sock, &c, 1);
        if (n <= 0) {
            if (idx == 0) return -1; // no data at all
            break; // return what we have
        }
        if (c == '\n') break;
        buffer[idx++] = c;
    }
    buffer[idx] = '\0';
    return idx;
}

// =====================================================
// Handle direct connection to Storage Server
// For READ, WRITE, STREAM
// =====================================================
void handle_storage_server(char *ss_ip, int ss_port, char *nm_response) {
    int ss = connect_to_server(ss_ip, ss_port);
    if (ss < 0) {
        printf(RED "✗ ERROR: " RESET "Could not connect to Storage Server\n");
        return;
    }

    // forward the request nm_response to SS
    send_line(ss, nm_response);

    char buf[BUF_SIZE];

    while (1) {
        int n = recv_line(ss, buf);
        if (n <= 0) break;

        // check STOP packet (as per spec)
        if (strncmp(buf, "STOP", 4) == 0) break;

        printf("%s", buf);
        fflush(stdout);
    }

    close(ss);
}

// =====================================================
// Main Client Loop
// =====================================================
int main(int argc, char *argv[]) {
    char username[64];
    char nm_ip[64] = "127.0.0.1";
    int nm_port = 9000;
    
    if (argc >= 2) {
        strncpy(nm_ip, argv[1], sizeof(nm_ip) - 1);
        nm_ip[sizeof(nm_ip) - 1] = '\0';
    }
    if (argc >= 3) {
        nm_port = atoi(argv[2]);
    }

    printf("\n" CYAN "╔═══════════════════════════════════════════════════╗\n");
    printf("║" BOLD "     DISTRIBUTED FILE SYSTEM - CLIENT" RESET CYAN "         ║\n");
    printf("╚═══════════════════════════════════════════════════╝" RESET "\n\n");
    printf(BOLD BLUE "👤 Enter username: " RESET);
    scanf("%s", username);

    client_listen_fd = create_client_listen_socket(&client_listen_port);
    if (client_listen_fd < 0) {
        fprintf(stderr, RED "✗ Failed to create client listen socket\n" RESET);
        exit(1);
    }

    // connect to Name Server
    int nm_sock = connect_to_server(nm_ip, nm_port);
    if (nm_sock < 0) {
        printf(RED "✗ Failed to connect to Name Server." RESET "\n");
        exit(1);
    }

    // Register client with NM
    char reg_msg[128];
    sprintf(reg_msg, "REGISTER_CLIENT %s %d\n", username, client_listen_port);
    send_line(nm_sock, reg_msg);

    char buf[BUF_SIZE];
    recv_line(nm_sock, buf);

    if (strncmp(buf, "OK", 2) == 0) {
        printf(GREEN "✓ Name Server: " RESET "%s\n\n", buf);
    } else {
        printf(YELLOW "⚠ Name Server: " RESET "%s\n\n", buf);
    }

    getchar(); // clear input

    // ============================
    // Interactive Loop
    // ============================
    while (1) {
        char cmd[MAX_CMD];
        printf("\n" BOLD CYAN "client ▶ " RESET);
        fflush(stdout);

        if (!fgets(cmd, MAX_CMD, stdin)) break;

        if (strncmp(cmd, "EXIT", 4) == 0) break;

        // send command to name server
        if (strncmp(cmd, "CREATE ", 7) == 0) {
            char filename[256];
            if (sscanf(cmd + 7, "%255s", filename) == 1) {
                char create_msg[512];
                snprintf(create_msg, sizeof(create_msg), "CREATE %s %s\n", filename, username);
                send_line(nm_sock, create_msg);
            } else {
                // fallback: send as-is if we cannot parse filename
                send_line(nm_sock, cmd);
            }
        } else {
            send_line(nm_sock, cmd);
        }

        // Special handling for LIST which returns multiple lines terminated by END_LIST
        if (strncmp(cmd, "LIST", 4) == 0) {
            while (1) {
                int n = recv_line(nm_sock, buf);
                if (n <= 0) {
                    printf(RED "\n✗ Connection to Name Server lost." RESET "\n");
                    goto done;
                }
                if (strcmp(buf, "END_LIST") == 0) {
                    break;
                }
                if (strncmp(buf, "Registered Users:", 17) == 0) {
                    printf("\n" BOLD CYAN "%s" RESET "\n", buf);
                } else if (strncmp(buf, "Total:", 6) == 0) {
                    printf(BOLD GREEN "%s" RESET "\n", buf);
                } else {
                    printf("  %s\n", buf);
                }
            }
            continue;
        }

        // Special handling for INFO which returns multiple lines terminated by END_INFO
        if (strncmp(cmd, "INFO", 4) == 0) {
            while (1) {
                int n = recv_line(nm_sock, buf);
                if (n <= 0) {
                    printf(RED "\n✗ Connection to Name Server lost." RESET "\n");
                    goto done;
                }
                if (strcmp(buf, "END_INFO") == 0) {
                    break;
                }
                // Print ERROR or INFO lines
                if (strncmp(buf, "ERROR", 5) == 0) {
                    printf(RED "✗ %s" RESET "\n", buf);
                    break;
                } else if (strncmp(buf, "ERR", 3) == 0) {
                    printf(RED "✗ %s" RESET "\n", buf);
                    break;
                } else if (strncmp(buf, "-->", 3) == 0) {
                    printf(CYAN "%s" RESET "\n", buf);
                }
            }
            continue;
        }

        // Special handling for VIEW which returns multiple lines terminated by END_VIEW
        if (strncmp(cmd, "VIEW", 4) == 0) {
            int is_detailed = (strstr(cmd, "-l") != NULL || strstr(cmd, "-al") != NULL || strstr(cmd, "-la") != NULL);
            char header[BUF_SIZE];
            int n = recv_line(nm_sock, header);
            if (n <= 0) {
                printf(RED "\n✗ Connection to Name Server lost." RESET "\n");
                goto done;
            }

            if (is_detailed) {
                printf("\n");
                print_divider(120);
                printf(BOLD CYAN "%-25s %-22s %-22s %-15s %-30s\n" RESET, "Filename", "Creation Time", "Last Access Time", "Owner", "Storage");
                print_divider(120);
            } else {
                printf("\n" BOLD CYAN "%s" RESET "\n", header);
            }

            while (1) {
                n = recv_line(nm_sock, buf);
                if (n <= 0) {
                    printf(RED "\n✗ Connection to Name Server lost." RESET "\n");
                    goto done;
                }
                if (strcmp(buf, "END_VIEW") == 0) {
                    break;
                }

                if (is_detailed) {
                    char filename[64], owner[64], creation_str[64], access_str[64];
                    char storage_str[64];
                    sscanf(buf, "%s %s %s %s %s", filename, creation_str, access_str, owner, storage_str);
                    printf("%-25s %-22s %-22s %-15s %-30s\n", filename, creation_str, access_str, owner, storage_str);
                } else {
                    printf(YELLOW "  ▸ " RESET "%s\n", buf);
                }
            }

            if (is_detailed) {
                print_divider(120);
            }
            continue;
        }

        // Special handling for EXEC which returns multiple lines terminated by END_EXEC
        if (strncmp(cmd, "EXEC", 4) == 0) {
            recv_line(nm_sock, buf); // consume header
            while (1) {
                int n = recv_line(nm_sock, buf);
                if (n <= 0) {
                    printf(RED "\n✗ Connection to Name Server lost." RESET "\n");
                    goto done;
                }
                if (strncmp(buf, "END_EXEC", 8) == 0) {
                    printf(GREEN "✓ " RESET "%s\n", buf);
                    break;
                }
                printf(YELLOW "  ▸ " RESET "%s\n", buf);
            }
            continue;
        }

        // read response
        int n = recv_line(nm_sock, buf);
        if (n <= 0) {
            printf(RED "\n✗ Connection to Name Server lost." RESET "\n");
            break;
        }

        // For READ, NM responds with OK/ERROR; if OK, then SS will connect to us.
        if (strncmp(buf, "OK READ started", 15) == 0) {
            // Then synchronously accept data from SS
            receive_from_ss_once();
            continue;
        }

        // For WRITE, NM responds with OK/ERROR; if OK, then SS will connect to us.
        if (strncmp(buf, "OK WRITE started", 16) == 0) {
            // Print NM response first
            printf("%s\n", buf);
            // Then synchronously handle WRITE session with SS
            handle_write_session();
            continue;
        }

        // For STREAM, NM responds with OK/ERROR; if OK, then SS will connect to us.
        if (strncmp(buf, "OK STREAM started", 17) == 0) {
            // Print NM response first
            printf("%s\n", buf);
            printf(CYAN "╔═══════════════════════════════════════════════════╗\n");
            printf("║" BOLD "              STREAMING CONTENT" RESET CYAN "                ║\n");
            printf("╚═══════════════════════════════════════════════════╝" RESET "\n");
            // Then synchronously accept streamed data from SS with real-time display
            receive_stream_from_ss();
            continue;
        }

        if (strncmp(buf, "OK File created", 15) == 0) {
            printf(GREEN "✓ " BOLD "File Created Successfully!" RESET "\n");
        } else if (strncmp(buf, "OK File deleted", 15) == 0) {
            char filename[64];
            sscanf(cmd, "DELETE %s", filename);
            printf(GREEN "✓ " BOLD "File '%s' deleted successfully!" RESET "\n", filename);
        } else if (strncmp(buf, "OK", 2) == 0) {
            printf(GREEN "✓ " RESET "%s\n", buf);
        } else if (strncmp(buf, "ERROR", 5) == 0) {
            printf(RED "✗ " RESET "%s\n", buf);
        } else if (strncmp(buf, "ERR", 3) == 0) {
            printf(RED "✗ " RESET "%s\n", buf);
        } else {
            printf("%s\n", buf);
        }
    }
done:
    close(nm_sock);
    return 0;
}
// ############## LLM Generated Code Ends ##############
