#include "write_ops.h"
#include "ss.h"
#include "logger.h"
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>

// ############## LLM Generated Code Begins ##############

// Global variables for lock management
pthread_mutex_t write_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t file_write_lock = PTHREAD_MUTEX_INITIALIZER;  // Protects file lock list
SentenceLock *sentence_locks = NULL;
FileLock *file_locks = NULL;
WriteSession *active_sessions = NULL;

// Get or create FileLock entry for a filename
static FileLock *get_or_create_file_lock(const char *filename) {
    pthread_mutex_lock(&file_write_lock);

    FileLock *fl = file_locks;
    while (fl) {
        if (strcmp(fl->filename, filename) == 0) {
            pthread_mutex_unlock(&file_write_lock);
            return fl;
        }
        fl = fl->next;
    }

    // Create new file lock
    fl = malloc(sizeof(FileLock));
    if (fl) {
        strncpy(fl->filename, filename, sizeof(fl->filename) - 1);
        fl->filename[sizeof(fl->filename) - 1] = '\0';
        pthread_mutex_init(&fl->mutex, NULL);
        fl->version = 0;  // initial version
        fl->next = file_locks;
        file_locks = fl;
    }

    pthread_mutex_unlock(&file_write_lock);
    return fl;
}

// Read current file version (0 if no lock entry yet)
static int get_file_version(const char *filename) {
    int version = 0;

    pthread_mutex_lock(&file_write_lock);
    FileLock *fl = file_locks;
    while (fl) {
        if (strcmp(fl->filename, filename) == 0) {
            version = fl->version;
            break;
        }
        fl = fl->next;
    }
    pthread_mutex_unlock(&file_write_lock);

    return version;
}

static int sentences_equal(const Sentence *a, const Sentence *b) {
    if (!a || !b) return 0;
    if (a->word_count != b->word_count) return 0;
    for (int i = 0; i < a->word_count; i++) {
        if (strncmp(a->words[i].content, b->words[i].content, MAX_WORD_LENGTH) != 0) {
            return 0;
        }
    }
    return 1;
}

static int find_snapshot_sentence_index(ParsedFile *parsed, Sentence *snapshot) {
    if (!parsed || !snapshot) return -1;
    for (int i = 0; i < parsed->sentence_count; i++) {
        if (sentences_equal(&parsed->sentences[i], snapshot)) {
            return i;
        }
    }
    return -1;
}

static int determine_effective_sentence_index(ParsedFile *parsed,
                                              WriteSession *session,
                                              int current_version,
                                              char *error_msg,
                                              int error_msg_size,
                                              int *rebased) {
    if (rebased) *rebased = 0;
    if (!parsed || !session) return -1;

    if (session->snapshot_version == current_version) {
        return session->sentence_number;
    }

    if (session->snapshot_was_append) {
        if (rebased) *rebased = 1;
        return parsed->sentence_count; // append at new end
    }

    if (!session->snapshot_sentence_valid) {
        if (error_msg) {
            snprintf(error_msg, error_msg_size,
                     "ERROR Concurrent writes detected; please retry");
        }
        return -1;
    }

    int match_index = find_snapshot_sentence_index(parsed, &session->snapshot_sentence);
    if (match_index >= 0) {
        if (rebased) *rebased = 1;
        return match_index;
    }

    if (error_msg) {
        snprintf(error_msg, error_msg_size,
                 "ERROR Sentence modified by another write; please retry");
    }
    return -1;
}

// Check if character is a sentence delimiter (., !, ?)
int is_sentence_delimiter(char c) {
    return (c == '.' || c == '!' || c == '?');
}

// Parse file content into sentences and words
int parse_file_to_sentences(const char *filename, ParsedFile *parsed) {
    printf("[SS] parse_file_to_sentences: filename=%s\n", filename ? filename : "NULL");
    
    if (!filename || !parsed) {
        printf("[SS] parse_file_to_sentences: invalid parameters\n");
        return -1;
    }
    
    printf("[SS] Opening file %s\n", filename);
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        printf("[SS] Failed to open file %s\n", filename);
        return -1;
    }
    
    printf("[SS] File opened successfully\n");
    
    // Initialize parsed structure
    memset(parsed, 0, sizeof(ParsedFile));
    strncpy(parsed->filename, filename, MAX_FILENAME_LENGTH - 1);
    
    char buffer[BUF_SIZE];
    size_t total_read = 0;
    size_t n;
    
    printf("[SS] Reading file content...\n");
    
    // Read entire file into buffer
    while ((n = fread(buffer + total_read, 1, sizeof(buffer) - total_read - 1, fp)) > 0) {
        total_read += n;
        if (total_read >= sizeof(buffer) - 1) break;
    }
    fclose(fp);
    
    printf("[SS] Read %zu bytes from file\n", total_read);
    
    if (total_read == 0) {
        printf("[SS] Empty file - creating one empty sentence\n");
        // Empty file - create one empty sentence
        parsed->sentence_count = 1;
        parsed->sentences[0].word_count = 0;
        return 0;
    }
    
    buffer[total_read] = '\0';
    printf("[SS] File content: '%s'\n", buffer);
    
    // Parse into sentences and words
    int current_sentence = 0;
    int current_word = 0;
    char current_word_buffer[MAX_WORD_LENGTH];
    int word_pos = 0;
    
    for (size_t i = 0; i <= total_read; i++) {
        char c = (i < total_read) ? buffer[i] : '\0';
        
        if (c == ' ' || c == '\t' || c == '\n' || c == '\0' || is_sentence_delimiter(c)) {
            // End of current word
            if (word_pos > 0) {
                current_word_buffer[word_pos] = '\0';
                if (current_sentence < MAX_SENTENCES && current_word < MAX_WORDS_PER_SENTENCE) {
                    strncpy(parsed->sentences[current_sentence].words[current_word].content,
                           current_word_buffer, MAX_WORD_LENGTH - 1);
                    parsed->sentences[current_sentence].words[current_word].content[MAX_WORD_LENGTH - 1] = '\0';
                    current_word++;
                    parsed->sentences[current_sentence].word_count = current_word;
                }
                word_pos = 0;
            }
            
            // Check for sentence delimiter
            if (is_sentence_delimiter(c)) {
                // Add delimiter as last character of current word if we have words
                if (current_word > 0 && current_sentence < MAX_SENTENCES) {
                    // Append delimiter to the last word
                    int last_word_idx = current_word - 1;
                    size_t len = strlen(parsed->sentences[current_sentence].words[last_word_idx].content);
                    if (len < MAX_WORD_LENGTH - 1) {
                        parsed->sentences[current_sentence].words[last_word_idx].content[len] = c;
                        parsed->sentences[current_sentence].words[last_word_idx].content[len + 1] = '\0';
                    }
                }
                
                // Start new sentence
                current_sentence++;
                current_word = 0;
                if (current_sentence >= MAX_SENTENCES) break;
            }
        } else {
            // Add character to current word
            if (word_pos < MAX_WORD_LENGTH - 1) {
                current_word_buffer[word_pos++] = c;
            }
        }
    }
    
    parsed->sentence_count = current_sentence + (current_word > 0 ? 1 : 0);
    if (parsed->sentence_count == 0) {
        parsed->sentence_count = 1; // Always have at least one sentence
    }
    
    return 0;
}

// Reconstruct file from parsed sentences
int reconstruct_file_from_sentences(const char *filename, ParsedFile *parsed) {
    if (!filename || !parsed) return -1;
    
    // Create temporary file
    char temp_filename[MAX_FILENAME_LENGTH + 16];
    snprintf(temp_filename, sizeof(temp_filename), "%s.tmp.%d", filename, (int)getpid());
    
    FILE *fp = fopen(temp_filename, "w");
    if (!fp) return -1;
    
    // Write sentences back to file
    for (int s = 0; s < parsed->sentence_count; s++) {
        Sentence *sentence = &parsed->sentences[s];
        
        // If sentence is empty but not the last, write a placeholder period
        if (sentence->word_count == 0) {
            if (s < parsed->sentence_count - 1) {
                // Write empty sentence marker (just a period)
                fputs(".", fp);
                fputc(' ', fp);
            }
            // If it's the last sentence and empty, skip it
            continue;
        }
        
        // Write non-empty sentence
        for (int w = 0; w < sentence->word_count; w++) {
            if (w > 0) {
                fputc(' ', fp); // Space between words
            }
            fputs(sentence->words[w].content, fp);
        }
        
        // Don't auto-add delimiters - sentences end only if user adds them
        
        // Add space after sentence if not the last one
        if (s < parsed->sentence_count - 1) {
            fputc(' ', fp);
        }
    }
    
    fclose(fp);
    
    // Atomic rename
    if (rename(temp_filename, filename) != 0) {
        unlink(temp_filename);
        return -1;
    }
    
    return 0;
}

// Lock management functions
int lock_sentence(const char *filename, int sentence_number, const char *owner) {
    pthread_mutex_lock(&write_lock);
    
    // Check if already locked
    SentenceLock *lock = sentence_locks;
    while (lock) {
        if (strcmp(lock->filename, filename) == 0 && lock->sentence_number == sentence_number) {
            pthread_mutex_unlock(&write_lock);
            return -1; // Already locked
        }
        lock = lock->next;
    }
    
    // Create new lock
    lock = malloc(sizeof(SentenceLock));
    if (!lock) {
        pthread_mutex_unlock(&write_lock);
        return -1;
    }
    
    strncpy(lock->filename, filename, MAX_FILENAME_LENGTH - 1);
    lock->filename[MAX_FILENAME_LENGTH - 1] = '\0';
    lock->sentence_number = sentence_number;
    strncpy(lock->owner, owner, sizeof(lock->owner) - 1);
    lock->owner[sizeof(lock->owner) - 1] = '\0';
    lock->lock_time = time(NULL);
    lock->next = sentence_locks;
    sentence_locks = lock;
    
    pthread_mutex_unlock(&write_lock);
    return 0;
}

int unlock_sentence(const char *filename, int sentence_number, const char *owner) {
    pthread_mutex_lock(&write_lock);
    
    SentenceLock *prev = NULL;
    SentenceLock *lock = sentence_locks;
    
    while (lock) {
        if (strcmp(lock->filename, filename) == 0 && 
            lock->sentence_number == sentence_number &&
            strcmp(lock->owner, owner) == 0) {
            
            if (prev) {
                prev->next = lock->next;
            } else {
                sentence_locks = lock->next;
            }
            
            free(lock);
            pthread_mutex_unlock(&write_lock);
            return 0;
        }
        prev = lock;
        lock = lock->next;
    }
    
    pthread_mutex_unlock(&write_lock);
    return -1; // Lock not found
}

int is_sentence_locked(const char *filename, int sentence_number, const char *owner) {
    pthread_mutex_lock(&write_lock);
    
    SentenceLock *lock = sentence_locks;
    while (lock) {
        if (strcmp(lock->filename, filename) == 0 && lock->sentence_number == sentence_number) {
            int locked = (strcmp(lock->owner, owner) != 0); // Locked if different owner
            pthread_mutex_unlock(&write_lock);
            return locked;
        }
        lock = lock->next;
    }
    
    pthread_mutex_unlock(&write_lock);
    return 0; // Not locked
}

// WRITE session management
WriteSession *start_write_session(const char *filename, int sentence_number, const char *owner) {
    // First try to lock the sentence
    if (lock_sentence(filename, sentence_number, owner) != 0) {
        return NULL; // Could not acquire lock
    }
    
    pthread_mutex_lock(&write_lock);
    
    WriteSession *session = malloc(sizeof(WriteSession));
    if (!session) {
        pthread_mutex_unlock(&write_lock);
        unlock_sentence(filename, sentence_number, owner);
        return NULL;
    }
    
    strncpy(session->filename, filename, MAX_FILENAME_LENGTH - 1);
    session->filename[MAX_FILENAME_LENGTH - 1] = '\0';
    session->sentence_number = sentence_number;
    strncpy(session->owner, owner, sizeof(session->owner) - 1);
    session->owner[sizeof(session->owner) - 1] = '\0';
    session->updates = NULL;
    // Capture file version at the time this WRITE session starts
    session->snapshot_version = get_file_version(filename);
    session->snapshot_sentence_valid = 0;
    session->snapshot_was_append = 0;
    memset(&session->snapshot_sentence, 0, sizeof(Sentence));

    ParsedFile snapshot_parsed;
    if (parse_file_to_sentences(filename, &snapshot_parsed) == 0) {
        if (sentence_number >= snapshot_parsed.sentence_count) {
            session->snapshot_was_append = 1;
        } else if (sentence_number >= 0) {
            memcpy(&session->snapshot_sentence,
                   &snapshot_parsed.sentences[sentence_number],
                   sizeof(Sentence));
            session->snapshot_sentence_valid = 1;
        }
    }

    session->next = active_sessions;
    active_sessions = session;
    
    pthread_mutex_unlock(&write_lock);
    return session;
}

int add_word_update(WriteSession *session, int word_index, const char *content) {
    if (!session || !content) return -1;
    
    WordUpdate *update = malloc(sizeof(WordUpdate));
    if (!update) return -1;
    
    update->word_index = word_index;
    strncpy(update->content, content, MAX_WORD_LENGTH - 1);
    update->content[MAX_WORD_LENGTH - 1] = '\0';
    update->next = session->updates;
    session->updates = update;
    
    return 0;
}

int commit_write_session(WriteSession *session, char *error_msg, int error_msg_size) {
    if (!session) {
        printf("[SS] commit_write_session: session is NULL\n");
        if (error_msg) snprintf(error_msg, error_msg_size, "ERROR Internal error: session is NULL");
        return -1;
    }
    
    printf("[SS] Committing write session for %s sentence %d\n", session->filename, session->sentence_number);

    // Get per-file lock/metadata for atomic read-modify-write
    FileLock *fl = get_or_create_file_lock(session->filename);
    if (!fl) {
        printf("[SS] Failed to get file lock entry\n");
        return -1;
    }

    pthread_mutex_lock(&fl->mutex);
    printf("[SS] Acquired file write lock for %s (version=%d, snapshot=%d)\n",
           session->filename, fl->version, session->snapshot_version);

    int rebased = 0;
    
    // Parse current file - allocate on heap to avoid stack overflow
    ParsedFile *parsed = malloc(sizeof(ParsedFile));
    if (!parsed) {
        printf("[SS] Failed to allocate memory for ParsedFile\n");
        pthread_mutex_unlock(&fl->mutex);
        return -1;
    }
    
    if (parse_file_to_sentences(session->filename, parsed) != 0) {
        printf("[SS] Failed to parse file %s\n", session->filename);
        free(parsed);
        pthread_mutex_unlock(&fl->mutex);
        return -1;
    }

    // Determine effective sentence index, rebasing if needed
    int effective_sentence = determine_effective_sentence_index(parsed,
                                                                session,
                                                                fl->version,
                                                                error_msg,
                                                                error_msg_size,
                                                                &rebased);
    if (effective_sentence < 0) {
        free(parsed);
        pthread_mutex_unlock(&fl->mutex);
        return -1;
    }
    
    if (rebased) {
        printf("[SS] Session sentence %d rebased to %d (version %d -> %d)\n",
               session->sentence_number, effective_sentence,
               session->snapshot_version, fl->version);
    }
    session->sentence_number = effective_sentence;
    session->snapshot_version = fl->version;

    // Create backup before modifying the file
    log_message(LOGLVL_INFO, "Creating backup before write for file: %s", session->filename);
    if (create_backup(session->filename) != 0) {
        log_message(LOGLVL_WARN, "Failed to create backup for %s, continuing anyway", session->filename);
    }
    
    printf("[SS] Parsed file has %d sentences\n", parsed->sentence_count);
    
    // Validate sentence number - allow appending new sentence
    if (session->sentence_number < 0 || session->sentence_number > parsed->sentence_count) {
        printf("[SS] Invalid sentence number %d (file has %d sentences, max allowed: %d)\n", 
               session->sentence_number, parsed->sentence_count, parsed->sentence_count);
        if (error_msg) {
            snprintf(error_msg, error_msg_size, "ERROR Sentence index out of range");
        }
        free(parsed);
        pthread_mutex_unlock(&fl->mutex);
        return -1;
    }
    
    // If writing to a new sentence (append), check if previous sentence has delimiter
    if (session->sentence_number == parsed->sentence_count) {
        if (parsed->sentence_count >= MAX_SENTENCES) {
            printf("[SS] Cannot add sentence: file already has maximum %d sentences\n", MAX_SENTENCES);
            free(parsed);
            pthread_mutex_unlock(&fl->mutex);
            return -1;
        }
        
        // Check if previous sentence ends with a delimiter
        if (parsed->sentence_count > 0) {
            Sentence *prev = &parsed->sentences[parsed->sentence_count - 1];
            if (prev->word_count > 0) {
                char *last_word = prev->words[prev->word_count - 1].content;
                int len = strlen(last_word);
                if (len > 0) {
                    char last_char = last_word[len - 1];
                    if (!is_sentence_delimiter(last_char)) {
                        printf("[SS] Cannot add sentence %d: previous sentence doesn't end with delimiter\n", 
                               session->sentence_number);
                        if (error_msg) {
                            snprintf(error_msg, error_msg_size, "ERROR Sentence index out of range");
                        }
                        free(parsed);
                        pthread_mutex_unlock(&fl->mutex);
                        return -1;
                    }
                }
            }
        }
        
        printf("[SS] Appending new sentence %d to file\n", session->sentence_number);
        parsed->sentence_count++;
        // Initialize the new sentence
        parsed->sentences[session->sentence_number].word_count = 0;
    }
    
    // Count updates
    int update_count = 0;
    WordUpdate *update = session->updates;
    while (update) {
        update_count++;
        update = update->next;
    }
    printf("[SS] Applying %d word updates\n", update_count);
    
    Sentence *target = &parsed->sentences[session->sentence_number];
    printf("[SS] Target sentence has %d words initially\n", target->word_count);

    // Detect and strip trailing sentence delimiter from original sentence (if any)
    int had_trailing_delim = 0;
    char trailing_delim = '\0';
    if (target->word_count > 0) {
        int last_idx = target->word_count - 1;
        char *last_word = target->words[last_idx].content;
        size_t last_len = strlen(last_word);
        if (last_len > 0 && is_sentence_delimiter(last_word[last_len - 1])) {
            had_trailing_delim = 1;
            trailing_delim = last_word[last_len - 1];
            last_word[last_len - 1] = '\0';  // Work on content without delimiter
        }
    } else if (target->word_count == 0 && parsed->sentence_count > 1) {
        // Empty sentences in a multi-sentence file come from '.' placeholders.
        // When we write real content into such a sentence, treat it as if it had
        // an implicit trailing '.' so the delimiter is preserved after the update.
        had_trailing_delim = 1;
        trailing_delim = '.';
    }
    
    // First, collect all updates into an array (reverse order since linked list is LIFO)
    WordUpdate *temp_updates[MAX_WORDS_PER_SENTENCE];
    int temp_count = 0;
    update = session->updates;
    while (update && temp_count < MAX_WORDS_PER_SENTENCE) {
        temp_updates[temp_count++] = update;
        update = update->next;
    }
    
    // Reverse to get chronological order (oldest first)
    WordUpdate *sorted_updates[MAX_WORDS_PER_SENTENCE];
    for (int i = 0; i < temp_count; i++) {
        sorted_updates[i] = temp_updates[temp_count - 1 - i];
    }
    int sorted_count = temp_count;
    
    // NO SORTING! Updates must be processed in chronological order.
    // This allows later updates to modify the results of earlier updates.
    
    // Start with original words, then apply insertions one by one
    char result_words[MAX_WORDS_PER_SENTENCE][MAX_WORD_LENGTH];
    int result_count = target->word_count;
    
    // Copy original words
    for (int i = 0; i < target->word_count && i < MAX_WORDS_PER_SENTENCE; i++) {
        strncpy(result_words[i], target->words[i].content, MAX_WORD_LENGTH - 1);
        result_words[i][MAX_WORD_LENGTH - 1] = '\0';
    }
    
    // Process each update in order: insert at index, shifting words right
    for (int u = 0; u < sorted_count; u++) {
        int insert_idx = sorted_updates[u]->word_index;
        
        printf("[SS] Inserting '%s' at index %d (current count: %d)\n", 
               sorted_updates[u]->content, insert_idx, result_count);
        
        if (insert_idx > result_count) {
            // Can't insert beyond current count + 1
            printf("[SS] Warning: index %d beyond valid range, skipping\n", insert_idx);
            continue;
        }
        
        // Shift words from insert_idx onwards to the right
        for (int i = result_count - 1; i >= insert_idx && i >= 0; i--) {
            if (i + 1 < MAX_WORDS_PER_SENTENCE) {
                strncpy(result_words[i + 1], result_words[i], MAX_WORD_LENGTH - 1);
                result_words[i + 1][MAX_WORD_LENGTH - 1] = '\0';
            }
        }
        
        // Insert new word at the specified index
        if (insert_idx < MAX_WORDS_PER_SENTENCE) {
            strncpy(result_words[insert_idx], sorted_updates[u]->content, MAX_WORD_LENGTH - 1);
            result_words[insert_idx][MAX_WORD_LENGTH - 1] = '\0';
            result_count++;
            
            if (result_count > MAX_WORDS_PER_SENTENCE) {
                result_count = MAX_WORDS_PER_SENTENCE;
            }
        }
    }

    // If original sentence ended with a delimiter, reattach it to the new last word
    if (had_trailing_delim && result_count > 0) {
        int last_idx = result_count - 1;
        size_t len = strlen(result_words[last_idx]);
        if (len < MAX_WORD_LENGTH - 1) {
            result_words[last_idx][len] = trailing_delim;
            result_words[last_idx][len + 1] = '\0';
        }
    }
    
    // Build sentence text from result words
    char sentence_text[MAX_WORDS_PER_SENTENCE * MAX_WORD_LENGTH];
    sentence_text[0] = '\0';
    for (int i = 0; i < result_count; i++) {
        if (i > 0) strcat(sentence_text, " ");
        strcat(sentence_text, result_words[i]);
    }
    
    printf("[SS] Final word count: %d\n", result_count);
    
    // Update target sentence with result words (don't split into multiple sentences)
    target->word_count = result_count;
    for (int i = 0; i < result_count && i < MAX_WORDS_PER_SENTENCE; i++) {
        strncpy(target->words[i].content, result_words[i], MAX_WORD_LENGTH - 1);
        target->words[i].content[MAX_WORD_LENGTH - 1] = '\0';
    }
    
    printf("[SS] Reconstructing file from sentences\n");
    
    // Write back to file
    if (reconstruct_file_from_sentences(session->filename, parsed) != 0) {
        printf("[SS] Failed to reconstruct file %s\n", session->filename);
        free(parsed);
        pthread_mutex_unlock(&fl->mutex);
        return -1;
    }
    
    printf("[SS] Successfully committed write session\n");
    free(parsed);
    
    // Bump file version after successful commit so later snapshots see change
    pthread_mutex_lock(&file_write_lock);
    fl->version++;
    pthread_mutex_unlock(&file_write_lock);

    // Release file write lock after successful commit
    pthread_mutex_unlock(&fl->mutex);
    printf("[SS] Released file write lock for %s (new version=%d)\n", session->filename, fl->version);
    
    return 0;
}

void cleanup_write_session(WriteSession *session) {
    if (!session) return;
    
    // Free all updates
    WordUpdate *update = session->updates;
    while (update) {
        WordUpdate *next = update->next;
        free(update);
        update = next;
    }
    
    // Remove from active sessions list
    pthread_mutex_lock(&write_lock);
    WriteSession *prev = NULL;
    WriteSession *current = active_sessions;
    
    while (current) {
        if (current == session) {
            if (prev) {
                prev->next = current->next;
            } else {
                active_sessions = current->next;
            }
            break;
        }
        prev = current;
        current = current->next;
    }
    pthread_mutex_unlock(&write_lock);
    
    // Unlock the sentence
    unlock_sentence(session->filename, session->sentence_number, session->owner);
    
    free(session);
}

// Main SS_WRITE handler - connects to client and manages the WRITE session
int handle_ss_write(const char *filename, int sentence_number, const char *client_ip, 
                   int client_port, const char *owner) {
    
    printf("[SS] Starting WRITE session for %s sentence %d, client %s:%d, owner %s\n", 
           filename, sentence_number, client_ip, client_port, owner);
    
    // Check if file exists, create empty file if it doesn't
    FILE *test_file = fopen(filename, "r");
    if (!test_file) {
        printf("[SS] File %s doesn't exist, creating empty file\n", filename);
        test_file = fopen(filename, "w");
        if (test_file) {
            fclose(test_file);
        } else {
            printf("[SS] Failed to create file %s\n", filename);
            return -1;
        }
    } else {
        fclose(test_file);
    }
    
    // Start write session (this locks the sentence)
    WriteSession *session = start_write_session(filename, sentence_number, owner);
    if (!session) {
        printf("[SS] Could not start write session (sentence may be locked)\n");
        
        // Connect to client to send error message
        int client_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (client_sock >= 0) {
            struct sockaddr_in client_addr;
            memset(&client_addr, 0, sizeof(client_addr));
            client_addr.sin_family = AF_INET;
            client_addr.sin_port = htons(client_port);
            inet_pton(AF_INET, client_ip, &client_addr.sin_addr);
            
            if (connect(client_sock, (struct sockaddr *)&client_addr, sizeof(client_addr)) == 0) {
                const char *error_msg = "ERROR Sentence is locked by another user\n";
                write(client_sock, error_msg, strlen(error_msg));
            }
            close(client_sock);
        }
        
        return -1; // Could not lock sentence
    }
    
    printf("[SS] Write session started, connecting to client...\n");
    
    // Connect to client
    int client_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (client_sock < 0) {
        printf("[SS] Failed to create socket for client connection\n");
        cleanup_write_session(session);
        return -1;
    }
    
    struct sockaddr_in client_addr;
    memset(&client_addr, 0, sizeof(client_addr));
    client_addr.sin_family = AF_INET;
    client_addr.sin_port = htons(client_port);
    if (inet_pton(AF_INET, client_ip, &client_addr.sin_addr) <= 0) {
        printf("[SS] Invalid client IP address: %s\n", client_ip);
        close(client_sock);
        cleanup_write_session(session);
        return -1;
    }
    
    if (connect(client_sock, (struct sockaddr *)&client_addr, sizeof(client_addr)) < 0) {
        printf("[SS] Failed to connect to client %s:%d\n", client_ip, client_port);
        close(client_sock);
        cleanup_write_session(session);
        return -1;
    }
    
    printf("[SS] Connected to client, sending ready signal\n");
    
    // Send ready signal to client
    const char *ready_msg = "READY_FOR_WRITE\n";
    write(client_sock, ready_msg, strlen(ready_msg));
    
    // Read word updates from client
    char buffer[BUF_SIZE];
    while (1) {
        int n = read(client_sock, buffer, sizeof(buffer) - 1);
        if (n <= 0) {
            printf("[SS] Client connection closed or error\n");
            break;
        }
        
        buffer[n] = '\0';
        printf("[SS] Received from client: %s\n", buffer);
        
        char *line = strtok(buffer, "\n");
        
        while (line) {
            // Check for ETIRW (end of write session)
            if (strcmp(line, "ETIRW") == 0) {
                printf("[SS] Received ETIRW, committing changes\n");
                
                // Safety check before calling commit
                if (!session) {
                    printf("[SS] ERROR: session is NULL before commit!\n");
                    const char *error_msg = "WRITE_ERROR\n";
                    write(client_sock, error_msg, strlen(error_msg));
                    goto cleanup;
                }
                
                printf("[SS] Session valid, filename=%s, sentence=%d\n", session->filename, session->sentence_number);
                printf("[SS] About to call commit_write_session...\n");
                
                // Commit all changes
                char commit_error[256] = {0};
                if (commit_write_session(session, commit_error, sizeof(commit_error)) == 0) {
                    update_bak_access(filename, owner);
                    const char *success_msg = "WRITE_COMPLETE\n";
                    write(client_sock, success_msg, strlen(success_msg));
                    printf("[SS] Write session completed successfully\n");
                } else {
                    // Send descriptive error message if available
                    if (commit_error[0] != '\0') {
                        char error_with_newline[300];
                        snprintf(error_with_newline, sizeof(error_with_newline), "%s\n", commit_error);
                        write(client_sock, error_with_newline, strlen(error_with_newline));
                    } else {
                        const char *error_msg = "WRITE_ERROR\n";
                        write(client_sock, error_msg, strlen(error_msg));
                    }
                    printf("[SS] Write session failed: %s\n", commit_error[0] ? commit_error : "unknown error");
                }
                goto cleanup;
            }
            
            // Parse word update: "<word_index> <word1> [word2] [word3] ..."
            int word_index;
            char *ptr = line;
            
            // Extract word index
            if (sscanf(ptr, "%d", &word_index) == 1) {
                // Skip past the word index
                while (*ptr && (*ptr == ' ' || *ptr == '\t')) ptr++;
                while (*ptr && *ptr != ' ' && *ptr != '\t') ptr++;
                while (*ptr && (*ptr == ' ' || *ptr == '\t')) ptr++;
                
                // Now ptr points to the content (potentially multiple words)
                if (*ptr != '\0') {
                    // Split content by spaces and add each word
                    char content_copy[1024];
                    strncpy(content_copy, ptr, sizeof(content_copy) - 1);
                    content_copy[sizeof(content_copy) - 1] = '\0';
                    
                    char *word = strtok(content_copy, " \t");
                    int current_index = word_index;
                    int words_added = 0;
                    
                    while (word != NULL) {
                        printf("[SS] Adding word update: index %d, content '%s'\n", current_index, word);
                        add_word_update(session, current_index, word);
                        current_index++;
                        words_added++;
                        word = strtok(NULL, " \t");
                    }
                    
                    if (words_added > 0) {
                        const char *ack_msg = "ACK\n";
                        write(client_sock, ack_msg, strlen(ack_msg));
                    } else {
                        const char *error_msg = "ERROR_INVALID_FORMAT\n";
                        write(client_sock, error_msg, strlen(error_msg));
                    }
                } else {
                    printf("[SS] No content provided\n");
                    const char *error_msg = "ERROR_INVALID_FORMAT\n";
                    write(client_sock, error_msg, strlen(error_msg));
                }
            } else {
                printf("[SS] Invalid format: %s\n", line);
                const char *error_msg = "ERROR_INVALID_FORMAT\n";
                write(client_sock, error_msg, strlen(error_msg));
            }
            
            line = strtok(NULL, "\n");
        }
    }
    
cleanup:
    printf("[SS] Cleaning up write session\n");
    close(client_sock);
    cleanup_write_session(session);
    return 0;
}

// Create a backup of the file before modifying it
// The backup is stored with a _prev extension (previous version for UNDO)
int create_backup(const char *filename) {
    if (!filename) {
        log_message(LOGLVL_ERROR, "create_backup: NULL filename");
        return -1;
    }
    
    char backup_name[MAX_FILENAME_LENGTH + 10];
    snprintf(backup_name, sizeof(backup_name), "%s_prev", filename);
    
    log_message(LOGLVL_DEBUG, "Creating backup: %s -> %s", filename, backup_name);
    
    // Open source file
    int src_fd = open(filename, O_RDONLY);
    if (src_fd < 0) {
        log_message(LOGLVL_ERROR, "Failed to open source file %s for backup: %s", filename, strerror(errno));
        return -1;
    }
    
    // Open/create backup file (overwrite if exists)
    int dst_fd = open(backup_name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dst_fd < 0) {
        log_message(LOGLVL_ERROR, "Failed to create backup file %s: %s", backup_name, strerror(errno));
        close(src_fd);
        return -1;
    }
    
    // Copy content
    char buffer[4096];
    ssize_t bytes_read, bytes_written;
    while ((bytes_read = read(src_fd, buffer, sizeof(buffer))) > 0) {
        bytes_written = write(dst_fd, buffer, bytes_read);
        if (bytes_written != bytes_read) {
            log_message(LOGLVL_ERROR, "Failed to write to backup file %s", backup_name);
            close(src_fd);
            close(dst_fd);
            return -1;
        }
    }
    
    close(src_fd);
    close(dst_fd);
    
    if (bytes_read < 0) {
        log_message(LOGLVL_ERROR, "Failed to read from source file %s: %s", filename, strerror(errno));
        return -1;
    }
    
    log_message(LOGLVL_INFO, "Backup created successfully: %s", backup_name);
    return 0;
}

// Restore file from its previous version (_prev file)
int restore_from_backup(const char *filename) {
    if (!filename) {
        log_message(LOGLVL_ERROR, "restore_from_backup: NULL filename");
        return -1;
    }
    
    char backup_name[MAX_FILENAME_LENGTH + 10];
    snprintf(backup_name, sizeof(backup_name), "%s_prev", filename);
    
    log_message(LOGLVL_DEBUG, "Restoring from backup: %s <- %s", filename, backup_name);
    
    // Check if backup exists
    if (access(backup_name, F_OK) != 0) {
        log_message(LOGLVL_ERROR, "Backup file %s does not exist", backup_name);
        return -1;
    }
    
    // Open backup file
    int src_fd = open(backup_name, O_RDONLY);
    if (src_fd < 0) {
        log_message(LOGLVL_ERROR, "Failed to open backup file %s: %s", backup_name, strerror(errno));
        return -1;
    }
    
    // Open/create target file (overwrite)
    int dst_fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dst_fd < 0) {
        log_message(LOGLVL_ERROR, "Failed to open target file %s: %s", filename, strerror(errno));
        close(src_fd);
        return -1;
    }
    
    // Copy content
    char buffer[4096];
    ssize_t bytes_read, bytes_written;
    while ((bytes_read = read(src_fd, buffer, sizeof(buffer))) > 0) {
        bytes_written = write(dst_fd, buffer, bytes_read);
        if (bytes_written != bytes_read) {
            log_message(LOGLVL_ERROR, "Failed to write to target file %s", filename);
            close(src_fd);
            close(dst_fd);
            return -1;
        }
    }
    
    close(src_fd);
    close(dst_fd);
    
    if (bytes_read < 0) {
        log_message(LOGLVL_ERROR, "Failed to read from backup file %s: %s", backup_name, strerror(errno));
        return -1;
    }
    
    log_message(LOGLVL_INFO, "File restored successfully from backup: %s", filename);
    return 0;
}

// Handle UNDO command - restore file from backup
int handle_ss_undo(const char *filename) {
    if (!filename) {
        log_message(LOGLVL_ERROR, "handle_ss_undo: NULL filename");
        return -1;
    }
    
    log_message(LOGLVL_INFO, "UNDO request for file: %s", filename);
    
    // Check if file exists
    if (access(filename, F_OK) != 0) {
        log_message(LOGLVL_WARN, "File %s does not exist", filename);
        return -1;
    }
    
    // Restore from backup
    if (restore_from_backup(filename) != 0) {
        log_message(LOGLVL_ERROR, "Failed to undo changes for file %s", filename);
        return -1;
    }
    
    log_message(LOGLVL_INFO, "Successfully undid last change to file: %s", filename);
    return 0;
}
// ############## LLM Generated Code Ends ##############
