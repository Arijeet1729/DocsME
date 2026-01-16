#ifndef NM_H
#define NM_H

// ############## LLM Generated Code Begins ##############

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <time.h>
#include <sys/types.h>

#define BACKLOG 10
#define BUF_SZ 4096
#define MAX_NAME 256
#define SS_TIMEOUT 20
#define CLIENT_TIMEOUT 60

// Forward declarations of core structures

typedef struct StorageServer {
    char ip[64];
    int ss_port;        // port NM connects to for SS commands
    int nm_fd_port;     // unused
    time_t last_seen;
    struct StorageServer *next;
} StorageServer;

// Access control entry for a file
typedef struct AccessEntry {
    char username[64];
    int read_access;    // 1 if user has read access, 0 otherwise
    int write_access;   // 1 if user has write access, 0 otherwise
    struct AccessEntry *next;
} AccessEntry;

typedef struct FileEntry {
    char name[MAX_NAME];
    char owner[64];
    StorageServer *ss;  // which storage server stores this file
    int size_bytes;
    time_t creation_time;  // when the file was created
    time_t last_access;    // when the file was last accessed (read/write)
    AccessEntry *access_list;  // linked list of access permissions
    struct FileEntry *next;   // next in hash bucket chain
} FileEntry;

typedef struct ClientInfo {
    int fd;
    char username[64];
    char ip[64];
    int client_port;
    time_t last_seen;
    struct ClientInfo *next;
} ClientInfo;

// Persistent user registry entry
typedef struct UserEntry {
    char username[64];
    char last_ip[64];
    time_t first_registered;
    time_t last_seen;
    struct UserEntry *next;
} UserEntry;

// File hash table size
#define FILE_HT_SIZE 1024

// LRU cache configuration for efficient search
#define CACHE_SIZE 256

// LRU Cache node for O(1) lookups
typedef struct CacheNode {
    char key[MAX_NAME];
    FileEntry *value;
    struct CacheNode *prev;
    struct CacheNode *next;
} CacheNode;

// LRU Cache structure
typedef struct {
    CacheNode *head;
    CacheNode *tail;
    CacheNode *cache_map[CACHE_SIZE];  // Hash table for O(1) lookup
    int size;
    unsigned long hits;
    unsigned long misses;
} LRUCache;

// Extern declarations for global tables defined in implementation files
extern StorageServer *ss_list;
extern FileEntry *file_ht[FILE_HT_SIZE];
extern UserEntry *user_registry;

// Common utility (implemented in nameserver.c)
ssize_t send_line(int fd, const char *s);

// Storage server management (implemented in nm_storage.c)
StorageServer *find_ss_by_ipport(const char *ip, int port);
StorageServer *register_storage_server(const char *ip, int ss_port);
StorageServer *choose_ss_for_new_file(void);
int send_command_to_ss(StorageServer *ss, const char *cmd, char *resp, size_t resp_sz);
void *ss_heartbeat_monitor(void *arg);

// File table management (implemented in nm_files.c)
FileEntry *find_file(const char *fname);
int create_file(const char *fname, const char *owner, StorageServer *ss);
int delete_file(const char *fname);
void handle_view(int client_fd, int list_all, int detailed);
void handle_read_request(int client_fd, const char *filename, const char *original_cmd);
void handle_stream_request(int client_fd, const char *filename);
void handle_write_request(int client_fd, const char *filename, int sentence_number);
void handle_info_request(int client_fd, const char *filename);
void handle_exec_request(int client_fd, const char *filename);
void handle_list_users(int client_fd);
void handle_addaccess_request(int client_fd, const char *filename, const char *username, int read_access, int write_access, const char *requester);
void handle_remaccess_request(int client_fd, const char *filename, const char *username, const char *requester);

// Cache management functions
void cache_init(void);
void cache_put(const char *key, FileEntry *value);
FileEntry *cache_get(const char *key);
void cache_invalidate(const char *key);
void cache_print_stats(void);

// Client info helpers (implemented in nameserver.c)
void register_client_info(int fd, const char *ip, const char *username, int client_port);
ClientInfo *find_client_by_fd(int fd);
void remove_client_info(int fd);

// Persistent user registry (implemented in nm_files.c)
void load_user_registry(void);
void save_user_registry(void);
void add_user_to_registry(const char *username, const char *ip);
int is_user_in_registry(const char *username);

// Access control helpers (implemented in nm_files.c)
int check_file_access(const char *filename, const char *username, int need_write);
void add_file_access(FileEntry *file, const char *username, int read_access, int write_access);
void remove_file_access(FileEntry *file, const char *username);
void cleanup_file_access(FileEntry *file);

// File registry persistence (implemented in nm_files.c)
void load_file_registry(void);
void save_file_registry(void);
void load_access_registry(void);
void save_access_registry(void);

#endif // NM_H
// ############## LLM Generated Code Ends ##############
