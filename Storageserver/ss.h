#ifndef SS_H
#define SS_H

#include "../posix.h"

#define BUF_SIZE 4096
#define HEARTBEAT_INTERVAL 10

// ############## LLM Generated Code Begins ##############

typedef struct {
    char nm_ip[64];
    int nm_port;          // Name Server port
    int ss_cmd_port;      // port where NM connects to send commands
    char ss_ip[64];       // IP address of this Storage Server
} SSConfig;

extern SSConfig g_cfg;

int connect_to_nm(void);
void send_registration_once(void);
void *heartbeat_thread(void *arg);
int connect_to_client(const char *ip, int port);
int serve_read_to_client(const char *filename, const char *client_ip, int client_port);
int serve_stream_to_client(const char *filename, const char *client_ip, int client_port);
int handle_ss_create(const char *filename, const char *owner);
int handle_ss_delete(const char *filename);
void *nm_command_handler(void *arg);
int create_listen_socket(int port);
int handle_ss_info(const char *filename, char *out, size_t out_sz);

// WRITE operations (implemented in write_ops.c)
int handle_ss_write(const char *filename, int sentence_number, const char *client_ip, 
                   int client_port, const char *owner);

// Metadata helper functions
void update_bak_access(const char *filename, const char *username);

#endif // SS_H
// ############## LLM Generated Code Ends ##############
