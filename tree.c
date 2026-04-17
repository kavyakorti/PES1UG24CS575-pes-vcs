// tree.c — Tree object serialization and construction

#include "tree.h"
#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>

// Forward declaration from object.c
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// Mode constants
#define MODE_FILE 0100644
#define MODE_EXEC 0100755
#define MODE_DIR  0040000

uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    if (S_ISDIR(st.st_mode)) return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;

    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];

        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1;

        char mode_str[16] = {0};
        size_t mode_len = (size_t)(space - ptr);
        if (mode_len >= sizeof(mode_str)) return -1;

        memcpy(mode_str, ptr, mode_len);
        entry->mode = (uint32_t)strtol(mode_str, NULL, 8);
        ptr = space + 1;

        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1;

        size_t name_len = (size_t)(null_byte - ptr);
        if (name_len >= sizeof(entry->name)) return -1;

        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0';
        ptr = null_byte + 1;

        if (ptr + HASH_SIZE > end) return -1;
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }

    return 0;
}

static int compare_tree_entries(const void *a, const void *b) {
    const TreeEntry *ea = (const TreeEntry *)a;
    const TreeEntry *eb = (const TreeEntry *)b;
    return strcmp(ea->name, eb->name);
}

int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    size_t max_size = (size_t)tree->count * 296;
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;

    Tree sorted_tree = *tree;
    qsort(sorted_tree.entries, sorted_tree.count, sizeof(TreeEntry), compare_tree_entries);

    size_t offset = 0;
    for (int i = 0; i < sorted_tree.count; i++) {
        const TreeEntry *entry = &sorted_tree.entries[i];

        int written = sprintf((char *)buffer + offset, "%o %s", entry->mode, entry->name);
        offset += (size_t)written + 1;

        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out = offset;
    return 0;
}

static int write_tree_level(const IndexEntry *entries, int count,
                            const char *prefix, ObjectID *id_out) {
    Tree tree;
    tree.count = 0;

    for (int i = 0; i < count; i++) {
        const char *path = entries[i].path;
        const char *rel = path;

        if (prefix[0] != '\0') {
            size_t plen = strlen(prefix);
            if (strncmp(path, prefix, plen) != 0) continue;
            rel = path + plen;
        }

        const char *slash = strchr(rel, '/');

        if (slash == NULL) {
            if (tree.count >= MAX_TREE_ENTRIES) return -1;

            TreeEntry *te = &tree.entries[tree.count++];
            te->mode = entries[i].mode;
            te->hash = entries[i].hash;
            snprintf(te->name, sizeof(te->name), "%s", rel);
        } else {
            size_t dir_len = (size_t)(slash - rel);
            char dirname[256];

            if (dir_len >= sizeof(dirname)) return -1;

            memcpy(dirname, rel, dir_len);
            dirname[dir_len] = '\0';

            int already_added = 0;
            for (int j = 0; j < tree.count; j++) {
                if (tree.entries[j].mode == MODE_DIR &&
                    strcmp(tree.entries[j].name, dirname) == 0) {
                    already_added = 1;
                    break;
                }
            }
            if (already_added) continue;

            char child_prefix[1024];
            if (prefix[0] == '\0') {
                snprintf(child_prefix, sizeof(child_prefix), "%s/", dirname);
            } else {
                snprintf(child_prefix, sizeof(child_prefix), "%s%s/", prefix, dirname);
            }

            ObjectID child_id;
            if (write_tree_level(entries, count, child_prefix, &child_id) != 0) {
                return -1;
            }

            if (tree.count >= MAX_TREE_ENTRIES) return -1;

            TreeEntry *te = &tree.entries[tree.count++];
            te->mode = MODE_DIR;
            te->hash = child_id;
            snprintf(te->name, sizeof(te->name), "%s", dirname);
        }
    }

    void *data = NULL;
    size_t len = 0;

    if (tree_serialize(&tree, &data, &len) != 0) return -1;

    int rc = object_write(OBJ_TREE, data, len, id_out);
    free(data);
    return rc;
}

int tree_from_index(ObjectID *id_out) {
    Index index;
    if (index_load(&index) != 0) return -1;
    return write_tree_level(index.entries, index.count, "", id_out);
}
