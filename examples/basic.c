/* Minimal tinykv walkthrough: open, put, get, iterate, compact. */
#include "tinykv/db.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check(const char *what, tinykv_status status)
{
    if (status != TINYKV_OK) {
        fprintf(stderr, "%s failed: %s\n", what, tinykv_status_string(status));
        exit(1);
    }
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "demo.tkv";
    tinykv_db *db = NULL;
    tinykv_iter *iter = NULL;
    void *value = NULL;
    size_t value_length = 0;
    const void *key = NULL, *iter_value = NULL;
    size_t key_length = 0, iter_value_length = 0;

    check("open", tinykv_open(path, NULL, &db));

    check("put greeting", tinykv_put(db, "greeting", 8, "hello, world", 12));
    check("put user", tinykv_put(db, "user", 4, "naveen", 6));
    check("put empty", tinykv_put(db, "note", 4, NULL, 0));

    check("get greeting",
          tinykv_get(db, "greeting", 8, &value, &value_length));
    printf("greeting = %.*s\n", (int)value_length, (char *)value);
    free(value);

    /* Overwrite, then delete: the log keeps every version until compact. */
    check("put user again", tinykv_put(db, "user", 4, "tinykv-fan", 10));
    check("delete note", tinykv_delete(db, "note", 4));
    check("compact", tinykv_compact(db));

    check("iterate", tinykv_iter_new(db, &iter));
    while (tinykv_iter_next(iter, &key, &key_length, &iter_value,
                            &iter_value_length) == TINYKV_OK && key != NULL)
        printf("%.*s = %.*s\n", (int)key_length, (const char *)key,
               (int)iter_value_length, (const char *)iter_value);
    tinykv_iter_free(iter);

    tinykv_close(db);
    return 0;
}
