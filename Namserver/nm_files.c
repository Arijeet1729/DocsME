#include "nm.h"
#include "logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/wait.h>

// ############## LLM Generated Code Begins ##############

// Global file hash table and lock
FileEntry *file_ht[FILE_HT_SIZE];
UserEntry *user_registry = NULL;
pthread_mutex_t user_registry_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t file_lock = PTHREAD_MUTEX_INITIALIZER;

// Global LRU cache for efficient O(1) lookups
static LRUCache lru_cache;
static pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;

// Hash helpers for file table
static unsigned file_hash(const char *name) {
    unsigned h = 5381;
    const unsigned char *p = (const unsigned char *)name;
    while (*p) {
        h = ((h << 5) + h) + *p; // djb2
        p++;
    }
    return h % FILE_HT_SIZE;
}

// Cache hash function (separate from file hash for better distribution)
static unsigned cache_hash(const char *name) {
    unsigned h = 5381;
    const unsigned char *p = (const unsigned char *)name;
    while (*p) {
        h = ((h << 5) + h) + *p;
        p++;
    }
    return h % CACHE_SIZE;
}

// Initialize LRU cache - O(1)
void cache_init(void) {
    pthread_mutex_lock(&cache_lock);
    memset(&lru_cache, 0, sizeof(LRUCache));
    lru_cache.head = NULL;
    lru_cache.tail = NULL;
    lru_cache.size = 0;
    lru_cache.hits = 0;
    lru_cache.misses = 0;
    for (int i = 0; i < CACHE_SIZE; i++) {
        lru_cache.cache_map[i] = NULL;
    }
    pthread_mutex_unlock(&cache_lock);
    log_message(LOGLVL_INFO, "Initialized LRU cache with size %d", CACHE_SIZE);
}

// Remove node from doubly linked list - O(1)
static void remove_node(CacheNode *node) {
    if (node->prev) {
        node->prev->next = node->next;
    } else {
        lru_cache.head = node->next;
    }
    if (node->next) {
        node->next->prev = node->prev;
    } else {
        lru_cache.tail = node->prev;
    }
}

// Add node to front of list (most recently used) - O(1)
static void add_to_front(CacheNode *node) {
    node->next = lru_cache.head;
    node->prev = NULL;
    if (lru_cache.head) {
        lru_cache.head->prev = node;
    }
    lru_cache.head = node;
    if (!lru_cache.tail) {
        lru_cache.tail = node;
    }
}

// Get from cache - O(1) average case
FileEntry *cache_get(const char *key) {
    pthread_mutex_lock(&cache_lock);
    unsigned idx = cache_hash(key);
    CacheNode *node = lru_cache.cache_map[idx];
    
    // Search in the bucket's chain
    while (node) {
        if (strcmp(node->key, key) == 0) {
            // Cache hit - move to front
            lru_cache.hits++;
            remove_node(node);
            add_to_front(node);
            FileEntry *result = node->value;
            pthread_mutex_unlock(&cache_lock);
            return result;
        }
        node = node->next;
    }
    
    // Cache miss
    lru_cache.misses++;
    pthread_mutex_unlock(&cache_lock);
    return NULL;
}

// Put into cache - O(1) average case
void cache_put(const char *key, FileEntry *value) {
    pthread_mutex_lock(&cache_lock);
    unsigned idx = cache_hash(key);
    
    // Check if key already exists
    CacheNode *node = lru_cache.cache_map[idx];
    while (node) {
        if (strcmp(node->key, key) == 0) {
            // Update existing entry
            node->value = value;
            remove_node(node);
            add_to_front(node);
            pthread_mutex_unlock(&cache_lock);
            return;
        }
        node = node->next;
    }
    
    // Create new node
    CacheNode *new_node = (CacheNode *)calloc(1, sizeof(CacheNode));
    if (!new_node) {
        pthread_mutex_unlock(&cache_lock);
        return;
    }
    strncpy(new_node->key, key, MAX_NAME - 1);
    new_node->value = value;
    
    // If cache is full, evict LRU (tail)
    if (lru_cache.size >= CACHE_SIZE) {
        CacheNode *lru = lru_cache.tail;
        if (lru) {
            // Remove from hash map
            unsigned lru_idx = cache_hash(lru->key);
            CacheNode *map_node = lru_cache.cache_map[lru_idx];
            CacheNode *map_prev = NULL;
            while (map_node) {
                if (map_node == lru) {
                    if (map_prev) {
                        map_prev->next = map_node->next;
                    } else {
                        lru_cache.cache_map[lru_idx] = map_node->next;
                    }
                    break;
                }
                map_prev = map_node;
                map_node = map_node->next;
            }
            remove_node(lru);
            free(lru);
            lru_cache.size--;
        }
    }
    
    // Add to front of list
    add_to_front(new_node);
    
    // Add to hash map (chain if collision)
    new_node->next = lru_cache.cache_map[idx];
    if (lru_cache.cache_map[idx]) {
        lru_cache.cache_map[idx]->prev = new_node;
    }
    lru_cache.cache_map[idx] = new_node;
    
    lru_cache.size++;
    pthread_mutex_unlock(&cache_lock);
}

// Invalidate cache entry - O(1) average case
void cache_invalidate(const char *key) {
    pthread_mutex_lock(&cache_lock);
    unsigned idx = cache_hash(key);
    CacheNode *node = lru_cache.cache_map[idx];
    CacheNode *prev = NULL;
    
    while (node) {
        if (strcmp(node->key, key) == 0) {
            // Remove from hash map
            if (prev) {
                prev->next = node->next;
            } else {
                lru_cache.cache_map[idx] = node->next;
            }
            // Remove from LRU list
            remove_node(node);
            free(node);
            lru_cache.size--;
            pthread_mutex_unlock(&cache_lock);
            return;
        }
        prev = node;
        node = node->next;
    }
    pthread_mutex_unlock(&cache_lock);
}

// Print cache statistics
void cache_print_stats(void) {
    pthread_mutex_lock(&cache_lock);
    unsigned long total = lru_cache.hits + lru_cache.misses;
    double hit_rate = total > 0 ? (100.0 * lru_cache.hits / total) : 0.0;
    log_message(LOGLVL_INFO, "Cache Stats - Hits: %lu, Misses: %lu, Hit Rate: %.2f%%, Size: %d/%d",
           lru_cache.hits, lru_cache.misses, hit_rate, lru_cache.size, CACHE_SIZE);
    pthread_mutex_unlock(&cache_lock);
}

// File hash table helpers with O(1) cache lookup
FileEntry *find_file(const char *fname) {
    // Try cache first - O(1) average case
    FileEntry *cached = cache_get(fname);
    if (cached) {
        return cached;  // Cache hit - immediate return
    }
    
    // Cache miss - search hash table - O(1) average case
    unsigned idx = file_hash(fname);
    pthread_mutex_lock(&file_lock);
    FileEntry *p = file_ht[idx];
    while (p) {
        if (strcmp(p->name, fname) == 0) {
            pthread_mutex_unlock(&file_lock);
            // Add to cache for future lookups
            cache_put(fname, p);
            return p;
        }
        p = p->next;
    }
    pthread_mutex_unlock(&file_lock);
    return NULL;
}

int create_file(const char *fname, const char *owner, StorageServer *ss) {
    unsigned idx = file_hash(fname);
    pthread_mutex_lock(&file_lock);
    FileEntry *p = file_ht[idx];
    while (p) {
        if (strcmp(p->name, fname) == 0) {
            pthread_mutex_unlock(&file_lock);
            return -1;
        }
        p = p->next;
    }

    FileEntry *f = calloc(1, sizeof(FileEntry));
    if (!f) {
        pthread_mutex_unlock(&file_lock);
        return -1;
    }
    strncpy(f->name, fname, MAX_NAME-1);
    strncpy(f->owner, owner, sizeof(f->owner)-1);
    f->ss = ss;
    f->size_bytes = 0;
    time_t now = time(NULL);
    f->creation_time = now;
    f->last_access = now;
    f->access_list = NULL;  // Initialize access list
    f->next = file_ht[idx];
    file_ht[idx] = f;
    pthread_mutex_unlock(&file_lock);

    printf("[NM] Created file %s owned by %s on SS %s:%d\n", fname, owner, ss->ip, ss->ss_port);
    
    // Save file registry after creating file
    save_file_registry();
    
    // Add to cache immediately for fast subsequent access
    cache_put(fname, f);

    log_message(LOGLVL_INFO, "Created file '%s' owned by '%s' on SS %s:%d", fname, owner, ss->ip, ss->ss_port);
    return 0;
}

int delete_file(const char *fname) {
    unsigned idx = file_hash(fname);
    pthread_mutex_lock(&file_lock);
    FileEntry *prev = NULL, *p = file_ht[idx];
    while (p) {
        if (strcmp(p->name, fname) == 0) {
            if (prev) prev->next = p->next;
            else file_ht[idx] = p->next;
            
            // Clean up access list before freeing file entry
            cleanup_file_access(p);
            free(p);
            pthread_mutex_unlock(&file_lock);
            
            // Save file registry after deleting file
            save_file_registry();
            // Invalidate cache entry
            cache_invalidate(fname);
            
            return 0;
        }
        prev = p;
        p = p->next;
    }
    pthread_mutex_unlock(&file_lock);
    return -1;
}

// Build VIEW response
// Flags: list_all (-a flag), detailed (-l flag)
void handle_view(int client_fd, int list_all, int detailed) {
    char line[BUF_SZ];
    
    // Get current user's info to filter files by access
    ClientInfo *ci = find_client_by_fd(client_fd);
    const char *username = ci ? ci->username : NULL;
    
    // Send header
    if (detailed) {
        send_line(client_fd, "FILENAME CREATION_TIME LAST_ACCESS OWNER STORAGE");
    } else {
        send_line(client_fd, "FILES:");
    }
    
    pthread_mutex_lock(&file_lock);
    for (int i = 0; i < FILE_HT_SIZE; i++) {
        FileEntry *p = file_ht[i];
        while (p) {
            // Filter by access: if not list_all, only show files owned by or accessible to current user
            if (!list_all && username) {
                // Check if user owns the file
                int is_owner = (strcmp(p->owner, username) == 0);
                
                // Check if user has been granted access
                int has_access = 0;
                AccessEntry *ace = p->access_list;
                while (ace) {
                    if (strcmp(ace->username, username) == 0) {
                        has_access = 1;
                        break;
                    }
                    ace = ace->next;
                }
                
                // Skip if user neither owns nor has access to the file
                if (!is_owner && !has_access) {
                    p = p->next;
                    continue;
                }
            }
            
            if (detailed) {
                char creation_str[64];
                struct tm *tm_info = localtime(&p->creation_time);
                strftime(creation_str, sizeof(creation_str), "%Y-%m-%d_%H:%M:%S", tm_info);
                
                char access_str[64];
                tm_info = localtime(&p->last_access);
                strftime(access_str, sizeof(access_str), "%Y-%m-%d_%H:%M:%S", tm_info);
                
                char storage_str[64];
                if (p->ss) {
                    snprintf(storage_str, sizeof(storage_str), "%s:%d", p->ss->ip, p->ss->ss_port);
                } else {
                    snprintf(storage_str, sizeof(storage_str), "-");
                }
                
                snprintf(line, sizeof(line), "%s %s %s %s %s",
                        p->name, creation_str, access_str, p->owner, storage_str);
            } else {
                if (list_all) {
                    snprintf(line, sizeof(line), "%s", p->name);
                } else {
                    snprintf(line, sizeof(line), "%s", p->name);
                }
            }
            send_line(client_fd, line);
            
            // Show access permissions if detailed view and file has access list
            if (detailed && p->access_list) {
                AccessEntry *ace = p->access_list;
                while (ace) {
                    char access_line[BUF_SZ];
                    snprintf(access_line, sizeof(access_line), "  └─ %s: %s access", 
                            ace->username, 
                            ace->write_access ? "READ+WRITE" : "READ");
                    send_line(client_fd, access_line);
                    ace = ace->next;
                }
            }
            
            p = p->next;
        }
    }
    pthread_mutex_unlock(&file_lock);
    
    // Send an explicit terminator so clients know VIEW output is complete.
    send_line(client_fd, "END_VIEW");
}

// Redirect client to appropriate SS for operations like READ/STREAM/WRITE
// Here we support READ and STREAM redirection (via SS_READ/SS_STREAM and client listener).
void handle_read_request(int client_fd, const char *filename, const char *original_cmd) {
    (void)original_cmd; // unused in new design
    FileEntry *f = find_file(filename);
    if (!f || !f->ss) {
        send_line(client_fd, "ERROR File not found or no storage server");
        return;
    }
    ClientInfo *ci = find_client_by_fd(client_fd);
    if (!ci || ci->client_port <= 0) {
        send_line(client_fd, "ERROR Client not registered with port");
        return;
    }
    
    // Check read access
    if (!check_file_access(filename, ci->username, 0)) {
        send_line(client_fd, "ERROR Access denied: You don't have read permission for this file");
        return;
    }

    char cmd[BUF_SZ];
    char resp[BUF_SZ];
    snprintf(cmd, sizeof(cmd), "SS_READ %s %s %d %s", filename, ci->ip, ci->client_port, ci->username);
    if (send_command_to_ss(f->ss, cmd, resp, sizeof(resp)) < 0) {
        send_line(client_fd, "ERROR Could not contact storage server");
    } else if (strncmp(resp, "ACK OK", 6) != 0) {
        send_line(client_fd, "ERROR Storage server read failed");
    } else {
        // Update last access time
        pthread_mutex_lock(&file_lock);
        f->last_access = time(NULL);
        pthread_mutex_unlock(&file_lock);
        save_file_registry();
        
        send_line(client_fd, "OK READ started");
    }
}

// Handle STREAM request: redirect client to SS for word-by-word streaming
void handle_stream_request(int client_fd, const char *filename) {
    FileEntry *f = find_file(filename);
    if (!f || !f->ss) {
        send_line(client_fd, "ERROR File not found or no storage server");
        return;
    }
    ClientInfo *ci = find_client_by_fd(client_fd);
    if (!ci || ci->client_port <= 0) {
        send_line(client_fd, "ERROR Client not registered with port");
        return;
    }
    
    // Check read access
    if (!check_file_access(filename, ci->username, 0)) {
        send_line(client_fd, "ERROR Access denied: You don't have read permission for this file");
        return;
    }

    char cmd[BUF_SZ];
    char resp[BUF_SZ];
    snprintf(cmd, sizeof(cmd), "SS_STREAM %s %s %d %s", filename, ci->ip, ci->client_port, ci->username);
    if (send_command_to_ss(f->ss, cmd, resp, sizeof(resp)) < 0) {
        send_line(client_fd, "ERROR Could not contact storage server");
    } else if (strncmp(resp, "ACK OK", 6) != 0) {
        send_line(client_fd, "ERROR Storage server stream failed");
    } else {
        // Update last access time
        pthread_mutex_lock(&file_lock);
        f->last_access = time(NULL);
        pthread_mutex_unlock(&file_lock);
        save_file_registry();
        
        send_line(client_fd, "OK STREAM started");
    }
}

// Handle WRITE request: redirect client to SS for word-level editing
void handle_write_request(int client_fd, const char *filename, int sentence_number) {
    log_message(LOGLVL_DEBUG, "WRITE request for %s sentence %d", filename, sentence_number);
    
    FileEntry *f = find_file(filename);
    if (!f || !f->ss) {
        log_message(LOGLVL_WARN, "File not found or no storage server for %s", filename);
        send_line(client_fd, "ERROR File not found or no storage server");
        return;
    }
    
    ClientInfo *ci = find_client_by_fd(client_fd);
    if (!ci || ci->client_port <= 0) {
        log_message(LOGLVL_WARN, "Client not registered with port (port=%d)", ci ? ci->client_port : -1);
        send_line(client_fd, "ERROR Client not registered with port");
        return;
    }
    
    // Check write access
    if (!check_file_access(filename, ci->username, 1)) {
        printf("[NM] Access denied: %s doesn't have write permission for %s\n", ci->username, filename);
        send_line(client_fd, "ERROR Access denied: You don't have write permission for this file");
        return;
    }

    log_message(LOGLVL_DEBUG, "Sending SS_WRITE to %s:%d for client %s:%d", f->ss->ip, f->ss->ss_port, ci->ip, ci->client_port);

    char cmd[BUF_SZ];
    char resp[BUF_SZ];
    snprintf(cmd, sizeof(cmd), "SS_WRITE %s %d %s %d %s", filename, sentence_number, ci->ip, ci->client_port, ci->username);
    
    log_message(LOGLVL_DEBUG, "Command to SS: %s", cmd);
    
    if (send_command_to_ss(f->ss, cmd, resp, sizeof(resp)) < 0) {
        log_message(LOGLVL_ERROR, "Could not contact storage server %s:%d", f->ss->ip, f->ss->ss_port);
        send_line(client_fd, "ERROR Could not contact storage server");
    } else {
        log_message(LOGLVL_DEBUG, "SS response: %s", resp);
        if (strncmp(resp, "ACK OK", 6) != 0) {
            log_message(LOGLVL_ERROR, "Storage server write failed for file %s", filename);
            send_line(client_fd, "ERROR Storage server write failed");
        } else {
            // Update last access time
            pthread_mutex_lock(&file_lock);
            f->last_access = time(NULL);
            pthread_mutex_unlock(&file_lock);
            save_file_registry();
            
            log_message(LOGLVL_DEBUG, "Sending OK WRITE started to client");
            send_line(client_fd, "OK WRITE started");
            log_message(LOGLVL_DEBUG, "Successfully sent OK WRITE started to client");
        }
    }
    log_message(LOGLVL_DEBUG, "handle_write_request completed");
}

// Handle LIST command: show all registered users (persistent + online)
void handle_list_users(int client_fd) {
    log_message(LOGLVL_DEBUG, "LIST request - showing all registered users");
    
    send_line(client_fd, "Registered Users:");
    
    pthread_mutex_lock(&user_registry_lock);
    
    // Show all users from persistent registry
    UserEntry *entry = user_registry;
    int user_count = 0;
    
    // Get current online users for status
    extern ClientInfo *client_list;
    
    while (entry) {
        char user_info[512];
        char time_str[64];
        struct tm *tm_info = localtime(&entry->last_seen);
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);
        
        // Check if user is currently online
        int is_online = 0;
        ClientInfo *ci = client_list;
        while (ci) {
            if (strcmp(ci->username, entry->username) == 0) {
                is_online = 1;
                break;
            }
            ci = ci->next;
        }
        
        snprintf(user_info, sizeof(user_info), "- %s (last IP: %s, last seen: %s) %s", 
                entry->username, entry->last_ip, time_str, is_online ? "[ONLINE]" : "[OFFLINE]");
        send_line(client_fd, user_info);
        user_count++;
        
        entry = entry->next;
    }
    
    pthread_mutex_unlock(&user_registry_lock);
    
    if (user_count == 0) {
        send_line(client_fd, "No users have ever registered");
    } else {
        char summary[128];
        snprintf(summary, sizeof(summary), "Total: %d user%s registered", user_count, user_count == 1 ? "" : "s");
        send_line(client_fd, summary);
    }
    
    send_line(client_fd, "END_LIST");
}

// Handle INFO <filename>: ask the responsible SS for metadata and forward it.
void handle_info_request(int client_fd, const char *filename) {
    FileEntry *f = find_file(filename);
    if (!f || !f->ss) {
        send_line(client_fd, "ERROR File not found or no storage server");
        return;
    }

    char cmd[BUF_SZ];
    char resp[BUF_SZ];
    snprintf(cmd, sizeof(cmd), "SS_INFO %s", filename);
    if (send_command_to_ss(f->ss, cmd, resp, sizeof(resp)) < 0) {
        send_line(client_fd, "ERROR Could not contact storage server");
        return;
    }

    // Expected formats:
    //   ACK OK --> File: ...\n--> Owner: ...\n...
    //   ACK ERROR ...
    if (strncmp(resp, "ACK OK ", 7) == 0) {
        // Skip the leading "ACK OK " prefix and send each line separately
        char *info = resp + 7;
        
        // Send each line of the INFO output
        char *line = strtok(info, "\n");
        while (line) {
            send_line(client_fd, line);
            line = strtok(NULL, "\n");
        }
        // Send end marker
        send_line(client_fd, "END_INFO");
    } else if (strncmp(resp, "ACK ERROR", 9) == 0) {
        send_line(client_fd, "ERROR Storage server INFO failed");
    } else {
        send_line(client_fd, "ERROR Malformed response from storage server");
    }
}

// Handle EXEC <filename>: fetch file content from SS and execute as shell commands on NM
void handle_exec_request(int client_fd, const char *filename) {
    FileEntry *f = find_file(filename);
    if (!f || !f->ss) {
        send_line(client_fd, "ERROR File not found or no storage server");
        return;
    }
    
    // Check read access
    ClientInfo *ci = find_client_by_fd(client_fd);
    if (!ci || !check_file_access(filename, ci->username, 0)) {
        send_line(client_fd, "ERROR Access denied: You don't have read permission for this file");
        return;
    }

    // Connect to Storage Server to get file content
    int ss_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (ss_sock < 0) {
        send_line(client_fd, "ERROR Could not create socket");
        return;
    }

    struct sockaddr_in ss_addr;
    memset(&ss_addr, 0, sizeof(ss_addr));
    ss_addr.sin_family = AF_INET;
    ss_addr.sin_port = htons(f->ss->ss_port);
    inet_pton(AF_INET, f->ss->ip, &ss_addr.sin_addr);

    if (connect(ss_sock, (struct sockaddr *)&ss_addr, sizeof(ss_addr)) < 0) {
        close(ss_sock);
        send_line(client_fd, "ERROR Could not connect to storage server");
        return;
    }

    // Request file content
    char cmd[BUF_SZ];
    snprintf(cmd, sizeof(cmd), "SS_GET_CONTENT %s\n", filename);
    write(ss_sock, cmd, strlen(cmd));

    // Read response
    char resp[BUF_SZ];
    int n = read(ss_sock, resp, sizeof(resp) - 1);
    if (n <= 0) {
        close(ss_sock);
        send_line(client_fd, "ERROR Failed to read from storage server");
        return;
    }
    resp[n] = '\0';

    // Check if ACK OK
    if (strncmp(resp, "ACK OK\n", 7) != 0) {
        close(ss_sock);
        send_line(client_fd, "ERROR Storage server could not read file");
        return;
    }

    // Read file content (skip "ACK OK\n")
    char file_content[BUF_SZ * 4];
    size_t content_len = 0;
    char *content_start = resp + 7;
    size_t initial_len = n - 7;
    
    if (initial_len > 0) {
        memcpy(file_content, content_start, initial_len);
        content_len = initial_len;
    }

    // Read remaining content
    while (content_len < sizeof(file_content) - 1) {
        n = read(ss_sock, file_content + content_len, sizeof(file_content) - content_len - 1);
        if (n <= 0) break;
        content_len += n;
        
        // Check for END_CONTENT marker
        file_content[content_len] = '\0';
        if (strstr(file_content, "END_CONTENT")) break;
    }
    close(ss_sock);

    // Remove END_CONTENT marker
    char *end_marker = strstr(file_content, "\nEND_CONTENT");
    if (end_marker) *end_marker = '\0';
    file_content[content_len] = '\0';

    // Execute file content as shell commands on Name Server
    send_line(client_fd, "EXEC OUTPUT:");
    
    FILE *pipe = popen(file_content, "r");
    if (!pipe) {
        send_line(client_fd, "ERROR Failed to execute commands");
        return;
    }

    // Read and send command output to client
    char output[BUF_SZ];
    while (fgets(output, sizeof(output), pipe)) {
        // Remove trailing newline for send_line (it adds its own)
        size_t len = strlen(output);
        if (len > 0 && output[len-1] == '\n') {
            output[len-1] = '\0';
        }
        send_line(client_fd, output);
    }

    int status = pclose(pipe);
    
    // Send completion message
    char completion[128];
    snprintf(completion, sizeof(completion), "END_EXEC (exit code: %d)", WEXITSTATUS(status));
    send_line(client_fd, completion);
}

// =====================================================
// Persistent User Registry Functions
// =====================================================

// Load user registry from file
void load_user_registry(void) {
    FILE *fp = fopen("user_registry.dat", "r");
    if (!fp) {
        printf("[NM] No existing user registry file found\n");
        return;
    }
    
    pthread_mutex_lock(&user_registry_lock);
    
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        char username[64], ip[64];
        time_t first_registered, last_seen;
        
        if (sscanf(line, "%63s %63s %ld %ld", username, ip, &first_registered, &last_seen) == 4) {
            UserEntry *entry = malloc(sizeof(UserEntry));
            if (entry) {
                strncpy(entry->username, username, sizeof(entry->username) - 1);
                entry->username[sizeof(entry->username) - 1] = '\0';
                strncpy(entry->last_ip, ip, sizeof(entry->last_ip) - 1);
                entry->last_ip[sizeof(entry->last_ip) - 1] = '\0';
                entry->first_registered = first_registered;
                entry->last_seen = last_seen;
                entry->next = user_registry;
                user_registry = entry;
            }
        }
    }
    
    fclose(fp);
    pthread_mutex_unlock(&user_registry_lock);
    printf("[NM] User registry loaded\n");
}

// Save user registry to file
void save_user_registry(void) {
    FILE *fp = fopen("user_registry.dat", "w");
    if (!fp) {
        printf("[NM] Failed to save user registry\n");
        return;
    }
    
    pthread_mutex_lock(&user_registry_lock);
    
    UserEntry *entry = user_registry;
    while (entry) {
        fprintf(fp, "%s %s %ld %ld\n", entry->username, entry->last_ip, 
                entry->first_registered, entry->last_seen);
        entry = entry->next;
    }
    
    fclose(fp);
    pthread_mutex_unlock(&user_registry_lock);
}

// Add user to registry or update existing entry
void add_user_to_registry(const char *username, const char *ip) {
    if (!username || !ip) return;
    
    pthread_mutex_lock(&user_registry_lock);
    
    // Check if user already exists
    UserEntry *entry = user_registry;
    while (entry) {
        if (strcmp(entry->username, username) == 0) {
            // Update existing entry
            strncpy(entry->last_ip, ip, sizeof(entry->last_ip) - 1);
            entry->last_ip[sizeof(entry->last_ip) - 1] = '\0';
            entry->last_seen = time(NULL);
            pthread_mutex_unlock(&user_registry_lock);
            save_user_registry();
            return;
        }
        entry = entry->next;
    }
    
    // Create new entry
    entry = malloc(sizeof(UserEntry));
    if (entry) {
        strncpy(entry->username, username, sizeof(entry->username) - 1);
        entry->username[sizeof(entry->username) - 1] = '\0';
        strncpy(entry->last_ip, ip, sizeof(entry->last_ip) - 1);
        entry->last_ip[sizeof(entry->last_ip) - 1] = '\0';
        entry->first_registered = time(NULL);
        entry->last_seen = time(NULL);
        entry->next = user_registry;
        user_registry = entry;
    }
    
    pthread_mutex_unlock(&user_registry_lock);
    save_user_registry();
}

// Check if user exists in registry
int is_user_in_registry(const char *username) {
    if (!username) return 0;
    
    pthread_mutex_lock(&user_registry_lock);
    
    UserEntry *entry = user_registry;
    while (entry) {
        if (strcmp(entry->username, username) == 0) {
            pthread_mutex_unlock(&user_registry_lock);
            return 1;
        }
        entry = entry->next;
    }
    
    pthread_mutex_unlock(&user_registry_lock);
    return 0;
}

// =====================================================
// Access Control Functions
// =====================================================

// Check if user has access to file (0 = no access, 1 = has access)
int check_file_access(const char *filename, const char *username, int need_write) {
    if (!filename || !username) return 0;
    
    FileEntry *file = find_file(filename);
    if (!file) return 0;
    
    // Owner always has full access
    if (strcmp(file->owner, username) == 0) {
        return 1;
    }
    
    // Check access list
    AccessEntry *entry = file->access_list;
    while (entry) {
        if (strcmp(entry->username, username) == 0) {
            if (need_write) {
                return entry->write_access;
            } else {
                return entry->read_access;
            }
        }
        entry = entry->next;
    }
    
    return 0;  // No access found
}

// Add or update access for a user
void add_file_access(FileEntry *file, const char *username, int read_access, int write_access) {
    if (!file || !username) return;
    
    // Check if user already has an entry
    AccessEntry *entry = file->access_list;
    while (entry) {
        if (strcmp(entry->username, username) == 0) {
            // Update existing entry
            entry->read_access = read_access;
            entry->write_access = write_access;
            return;
        }
        entry = entry->next;
    }
    
    // Create new entry
    entry = malloc(sizeof(AccessEntry));
    if (entry) {
        strncpy(entry->username, username, sizeof(entry->username) - 1);
        entry->username[sizeof(entry->username) - 1] = '\0';
        entry->read_access = read_access;
        entry->write_access = write_access;
        entry->next = file->access_list;
        file->access_list = entry;
    }
}

// Remove access for a user
void remove_file_access(FileEntry *file, const char *username) {
    if (!file || !username) return;
    
    AccessEntry *prev = NULL;
    AccessEntry *entry = file->access_list;
    
    while (entry) {
        if (strcmp(entry->username, username) == 0) {
            if (prev) {
                prev->next = entry->next;
            } else {
                file->access_list = entry->next;
            }
            free(entry);
            return;
        }
        prev = entry;
        entry = entry->next;
    }
}

// Clean up all access entries for a file (used when deleting file)
void cleanup_file_access(FileEntry *file) {
    if (!file) return;
    
    AccessEntry *entry = file->access_list;
    while (entry) {
        AccessEntry *next = entry->next;
        free(entry);
        entry = next;
    }
    file->access_list = NULL;
}

// Format access list into a string for storage in .bak file
char *format_access_list(FileEntry *file) {
    static char access_str[512]; // Static buffer for the formatted string
    
    if (!file) return "unknown";
    
    // Start with owner who always has RW access
    int pos = snprintf(access_str, sizeof(access_str), "%s (RW)", file->owner);
    
    // Add each access entry
    AccessEntry *entry = file->access_list;
    while (entry && pos < (int)sizeof(access_str) - 20) { // Leave room for entry
        pos += snprintf(access_str + pos, sizeof(access_str) - pos, ", %s (%s)", 
                      entry->username, 
                      entry->write_access ? "RW" : "R");
        entry = entry->next;
    }
    
    return access_str;
}

// Handle ADDACCESS request
void handle_addaccess_request(int client_fd, const char *filename, const char *username, 
                             int read_access, int write_access, const char *requester) {
    printf("[NM] ADDACCESS request: %s wants to give %s %s access to %s\n", 
           requester, username, write_access ? "write" : "read", filename);
    
    FileEntry *file = find_file(filename);
    if (!file) {
        send_line(client_fd, "ERROR File not found");
        return;
    }
    
    // Only owner can manage access
    if (strcmp(file->owner, requester) != 0) {
        send_line(client_fd, "ERROR Only the file owner can manage access permissions");
        return;
    }
    
    // Check if target user exists in registry
    if (!is_user_in_registry(username)) {
        send_line(client_fd, "ERROR User not found in system");
        return;
    }
    
    // Don't allow owner to modify their own access
    if (strcmp(file->owner, username) == 0) {
        send_line(client_fd, "ERROR Owner always has full access");
        return;
    }
    
    // Add the access
    add_file_access(file, username, read_access, write_access);
    
    // Format the access list into a string
    char *access_str = format_access_list(file);
    
    // Send command to storage server to update the .bak file with new access info
    char cmd[BUF_SZ];
    char resp[BUF_SZ];
    snprintf(cmd, sizeof(cmd), "SS_UPDATE_ACCESS %s %s", filename, access_str);
    
    if (send_command_to_ss(file->ss, cmd, resp, sizeof(resp)) < 0) {
        log_message(LOGLVL_WARN, "Could not update access info on storage server for %s", filename);
        // Continue anyway since the nameserver's access list is updated
    }
    
    char response[256];
    snprintf(response, sizeof(response), "OK Access granted: %s now has %s access to %s", 
             username, write_access ? "write" : "read", filename);
    send_line(client_fd, response);
    
    // Save access registry after granting permission
    save_access_registry();
    
    printf("[NM] Access granted: %s -> %s (%s)\n", username, filename, 
           write_access ? "write" : "read");
}

// Handle REMACCESS request
void handle_remaccess_request(int client_fd, const char *filename, const char *username, const char *requester) {
    printf("[NM] REMACCESS request: %s wants to remove %s's access to %s\n", 
           requester, username, filename);
    
    FileEntry *file = find_file(filename);
    if (!file) {
        send_line(client_fd, "ERROR File not found");
        return;
    }
    
    // Only owner can manage access
    if (strcmp(file->owner, requester) != 0) {
        send_line(client_fd, "ERROR Only the file owner can manage access permissions");
        return;
    }
    
    // Don't allow owner to remove their own access
    if (strcmp(file->owner, username) == 0) {
        send_line(client_fd, "ERROR Cannot remove owner's access");
        return;
    }
    
    // Remove the access
    remove_file_access(file, username);
    
    // Format the access list into a string
    char *access_str = format_access_list(file);
    
    // Send command to storage server to update the .bak file with new access info
    char cmd[BUF_SZ];
    char resp[BUF_SZ];
    snprintf(cmd, sizeof(cmd), "SS_UPDATE_ACCESS %s %s", filename, access_str);
    
    if (send_command_to_ss(file->ss, cmd, resp, sizeof(resp)) < 0) {
        log_message(LOGLVL_WARN, "Could not update access info on storage server for %s", filename);
        // Continue anyway since the nameserver's access list is updated
    }
    
    char response[256];
    snprintf(response, sizeof(response), "OK Access removed: %s no longer has access to %s", 
             username, filename);
    send_line(client_fd, response);
    
    // Save access registry after removing permission
    save_access_registry();
    
    printf("[NM] Access removed: %s -> %s\n", username, filename);
}

// =====================================================
// File Registry Persistence Functions
// =====================================================

// Load file registry from disk
void load_file_registry(void) {
    FILE *fp = fopen("file_registry.dat", "r");
    if (!fp) {
        printf("[NM] No existing file registry found\n");
        return;
    }
    
    pthread_mutex_lock(&file_lock);
    
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        char filename[MAX_NAME], owner[64], ss_ip[64];
        int ss_port, size_bytes;
        time_t creation_time, last_access;
        
        // Try to read new format with creation_time first
        int fields = sscanf(line, "%255s %63s %63s %d %d %ld %ld", 
                   filename, owner, ss_ip, &ss_port, &size_bytes, &creation_time, &last_access);
        
        if (fields == 7) {
            // New format with creation_time
        } else if (sscanf(line, "%255s %63s %63s %d %d %ld", 
                   filename, owner, ss_ip, &ss_port, &size_bytes, &last_access) == 6) {
            // Old format - use last_access as creation_time for backward compatibility
            creation_time = last_access;
        } else {
            continue;
        }
        
        {
            
            // Find or create the storage server entry
            StorageServer *ss = find_ss_by_ipport(ss_ip, ss_port);
            if (!ss) {
                // Create a placeholder SS entry (it will be properly registered when SS connects)
                ss = malloc(sizeof(StorageServer));
                if (ss) {
                    strncpy(ss->ip, ss_ip, sizeof(ss->ip) - 1);
                    ss->ip[sizeof(ss->ip) - 1] = '\0';
                    ss->ss_port = ss_port;
                    ss->last_seen = 0; // Will be updated when SS actually connects
                    ss->next = ss_list;
                    ss_list = ss;
                }
            }
            
            if (ss) {
                // Create file entry
                unsigned idx = file_hash(filename);
                FileEntry *f = malloc(sizeof(FileEntry));
                if (f) {
                    strncpy(f->name, filename, sizeof(f->name) - 1);
                    f->name[sizeof(f->name) - 1] = '\0';
                    strncpy(f->owner, owner, sizeof(f->owner) - 1);
                    f->owner[sizeof(f->owner) - 1] = '\0';
                    f->ss = ss;
                    f->size_bytes = size_bytes;
                    f->creation_time = creation_time;
                    f->last_access = last_access;
                    f->access_list = NULL; // Access list not persisted yet (could be added later)
                    f->next = file_ht[idx];
                    file_ht[idx] = f;
                }
            }
        }
    }
    
    fclose(fp);
    pthread_mutex_unlock(&file_lock);
    printf("[NM] File registry loaded\n");
}

// Save file registry to disk
void save_file_registry(void) {
    FILE *fp = fopen("file_registry.dat", "w");
    if (!fp) {
        printf("[NM] Failed to save file registry\n");
        return;
    }
    
    pthread_mutex_lock(&file_lock);
    
    // Iterate through all hash buckets
    for (int i = 0; i < FILE_HT_SIZE; i++) {
        FileEntry *f = file_ht[i];
        while (f) {
            if (f->ss) {
                fprintf(fp, "%s %s %s %d %d %ld %ld\n", 
                        f->name, f->owner, f->ss->ip, f->ss->ss_port, 
                        f->size_bytes, f->creation_time, f->last_access);
            }
            f = f->next;
        }
    }
    
    fclose(fp);
    pthread_mutex_unlock(&file_lock);
}

// Save access control lists to disk
void save_access_registry(void) {
    FILE *fp = fopen("access_registry.dat", "w");
    if (!fp) {
        printf("[NM] Failed to save access registry\n");
        return;
    }
    
    pthread_mutex_lock(&file_lock);
    
    // Iterate through all files and their access lists
    for (int i = 0; i < FILE_HT_SIZE; i++) {
        FileEntry *f = file_ht[i];
        while (f) {
            AccessEntry *ace = f->access_list;
            while (ace) {
                // Format: filename username read_access write_access
                fprintf(fp, "%s %s %d %d\n", 
                        f->name, ace->username, ace->read_access, ace->write_access);
                ace = ace->next;
            }
            f = f->next;
        }
    }
    
    fclose(fp);
    pthread_mutex_unlock(&file_lock);
    printf("[NM] Access registry saved\n");
}

// Load access control lists from disk
void load_access_registry(void) {
    FILE *fp = fopen("access_registry.dat", "r");
    if (!fp) {
        printf("[NM] No existing access registry found\n");
        return;
    }
    
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        char filename[MAX_NAME], username[64];
        int read_access, write_access;
        
        if (sscanf(line, "%255s %63s %d %d", 
                   filename, username, &read_access, &write_access) == 4) {
            
            // Find the file
            FileEntry *f = find_file(filename);
            if (f) {
                // Add access entry to the file
                add_file_access(f, username, read_access, write_access);
            }
        }
    }
    
    fclose(fp);
    printf("[NM] Access registry loaded\n");
}
// ############## LLM Generated Code Ends ##############
