#include "../posix.h"

int main(){
    const char *path = "example.dpp";
    const char *owner = "alice";
    const char *raw = "Hello world. This is second sentence? Exclaim!";
    size_t n;
    char **sents = tokenize_sentences_from_text(raw, &n);

    write_file(path, owner, raw, n , sents);
}