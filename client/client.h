#ifndef CLIENT_H
#define CLIENT_H
// ############## LLM Generated Code Begins ##############

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>

#define MAX_CMD 2048
#define BUF_SIZE 4096

extern int client_listen_fd;
extern int client_listen_port;

int connect_to_server(char *ip, int port);
void send_line(int sock, const char *msg);
int recv_line(int sock, char *buffer);
int create_client_listen_socket(int *out_port);
void *client_data_listener(void *arg);
void handle_storage_server(char *ss_ip, int ss_port, char *nm_response);

#endif // CLIENT_H
// ############## LLM Generated Code Ends ##############
