#ifndef WRITE_OPS_H
#define WRITE_OPS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

// ############## LLM Generated Code Begins ##############

// Maximum limits for parsing (reduced to reasonable sizes)
#define MAX_SENTENCES 1000
#define MAX_WORDS_PER_SENTENCE 100
#define MAX_WORD_LENGTH 64
#define MAX_FILENAME_LENGTH 256

// Structure to represent a word in a sentence
typedef struct {
    char content[MAX_WORD_LENGTH];
} Word;

// Structure to represent a sentence
typedef struct {
    Word words[MAX_WORDS_PER_SENTENCE];
    int word_count;
} Sentence;

// Structure to represent a file's content parsed into sentences
typedef struct {
    Sentence sentences[MAX_SENTENCES];
    int sentence_count;
    char filename[MAX_FILENAME_LENGTH];
} ParsedFile;

// Structure to track sentence locks
typedef struct SentenceLock {
    char filename[MAX_FILENAME_LENGTH];
    int sentence_number;
    char owner[64];
    time_t lock_time;
    struct SentenceLock *next;
} SentenceLock;

// Structure to track file-level locks for commit phase
typedef struct FileLock {
    char filename[MAX_FILENAME_LENGTH];
    pthread_mutex_t mutex;
    int version;              // Monotonic version for snapshot/concurrency control
    struct FileLock *next;
} FileLock;

// Structure to track pending word updates during a WRITE session
typedef struct WordUpdate {
    int word_index;
    char content[MAX_WORD_LENGTH];
    struct WordUpdate *next;
} WordUpdate;

// Structure to track active WRITE sessions
typedef struct WriteSession {
    char filename[MAX_FILENAME_LENGTH];
    int sentence_number;
    char owner[64];
    WordUpdate *updates;
    int snapshot_version;     // File version when this session started
    Sentence snapshot_sentence; // Copy of sentence content when session started
    int snapshot_sentence_valid; // 1 if snapshot_sentence contains valid data
    int snapshot_was_append;   // 1 if this session started as an append
    struct WriteSession *next;
} WriteSession;

// Global lock management
extern pthread_mutex_t write_lock;
extern pthread_mutex_t file_write_lock;  // Protects file reconstruction
extern SentenceLock *sentence_locks;
extern FileLock *file_locks;  // Per-file locks for commit phase
extern WriteSession *active_sessions;

// Core functions
int parse_file_to_sentences(const char *filename, ParsedFile *parsed);
int reconstruct_file_from_sentences(const char *filename, ParsedFile *parsed);
int is_sentence_delimiter(char c);

// Lock management
int lock_sentence(const char *filename, int sentence_number, const char *owner);
int unlock_sentence(const char *filename, int sentence_number, const char *owner);
int is_sentence_locked(const char *filename, int sentence_number, const char *owner);

// WRITE session management
WriteSession *start_write_session(const char *filename, int sentence_number, const char *owner);
int add_word_update(WriteSession *session, int word_index, const char *content);
int commit_write_session(WriteSession *session, char *error_msg, int error_msg_size);
void cleanup_write_session(WriteSession *session);

// Main SS_WRITE handler
int handle_ss_write(const char *filename, int sentence_number, const char *client_ip, 
                   int client_port, const char *owner);

// Backup and UNDO functions
int create_backup(const char *filename);
int restore_from_backup(const char *filename);
int handle_ss_undo(const char *filename);

#endif // WRITE_OPS_H
// ############## LLM Generated Code Ends ##############
