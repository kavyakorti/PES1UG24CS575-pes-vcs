// object.c — Content-addressable object store
//
// Every piece of data (file contents, directory listings, commits) is stored
// as an "object" named by its SHA-256 hash. Objects are stored under
// .pes/objects/XX/YYYYYY... where XX is the first two hex characters of the
// hash (directory sharding).

#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <openssl/evp.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

void hash_to_hex(const ObjectID *id, char *hex_out) {
    for (int i = 0; i < HASH_SIZE; i++) {
        sprintf(hex_out + i * 2, "%02x", id->hash[i]);
    }
    hex_out[HASH_HEX_SIZE] = '\0';
}

int hex_to_hash(const char *hex, ObjectID *id_out) {
    if (strlen(hex) < HASH_HEX_SIZE) return -1;
    for (int i = 0; i < HASH_SIZE; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
        id_out->hash[i] = (uint8_t)byte;
    }
    return 0;
}

void compute_hash(const void *data, size_t len, ObjectID *id_out) {
    unsigned int hash_len;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, id_out->hash, &hash_len);
    EVP_MD_CTX_free(ctx);
}

// Get the filesystem path where an object should be stored.
// Format: .pes/objects/XX/YYYYYYYY...
// The first 2 hex chars form the shard directory; the rest is the filename.
void object_path(const ObjectID *id, char *path_out, size_t path_size) {
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);
    snprintf(path_out, path_size, "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);
}

int object_exists(const ObjectID *id) {
    char path[512];
    object_path(id, path, sizeof(path));
    return access(path, F_OK) == 0;
}

// ─── TODO: Implement these ──────────────────────────────────────────────────

static const char *object_type_name(ObjectType type) {
    switch (type) {
        case OBJ_BLOB:   return "blob";
        case OBJ_TREE:   return "tree";
        case OBJ_COMMIT: return "commit";
        default:         return NULL;
    }
}

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    const char *type_str = object_type_name(type);
    if (!type_str || !data || !id_out) return -1;

    char header[64];
    int header_len = snprintf(header, sizeof(header), "%s %zu", type_str, len);
    if (header_len < 0 || (size_t)header_len >= sizeof(header)) return -1;

    size_t full_len = (size_t)header_len + 1 + len;
    unsigned char *full = malloc(full_len);
    if (!full) return -1;

    memcpy(full, header, (size_t)header_len);
    full[header_len] = '\0';
    memcpy(full + header_len + 1, data, len);

    ObjectID id;
    compute_hash(full, full_len, &id);
    *id_out = id;

    if (object_exists(&id)) {
        free(full);
        return 0;
    }

    if (mkdir(PES_DIR, 0755) == -1 && errno != EEXIST) {
        free(full);
        return -1;
    }
    if (mkdir(OBJECTS_DIR, 0755) == -1 && errno != EEXIST) {
        free(full);
        return -1;
    }

    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(&id, hex);

    char shard_dir[512];
    snprintf(shard_dir, sizeof(shard_dir), "%s/%.2s", OBJECTS_DIR, hex);

    if (mkdir(shard_dir, 0755) == -1 && errno != EEXIST) {
        free(full);
        return -1;
    }

    char final_path[512];
    object_path(&id, final_path, sizeof(final_path));

    char temp_path[512];
    snprintf(temp_path, sizeof(temp_path), "%s/.tmp-%ld-%d",
             shard_dir, (long)getpid(), rand());

    int fd = open(temp_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        free(full);
        return -1;
    }

    size_t written = 0;
    while (written < full_len) {
        ssize_t n = write(fd, full + written, full_len - written);
        if (n < 0) {
            close(fd);
            unlink(temp_path);
            free(full);
            return -1;
        }
        written += (size_t)n;
    }

    if (fsync(fd) == -1) {
        close(fd);
        unlink(temp_path);
        free(full);
        return -1;
    }

    if (close(fd) == -1) {
        unlink(temp_path);
        free(full);
        return -1;
    }

    if (rename(temp_path, final_path) == -1) {
        unlink(temp_path);
        free(full);
        return -1;
    }

    int dfd = open(shard_dir, O_RDONLY | O_DIRECTORY);
    if (dfd >= 0) {
        fsync(dfd);
        close(dfd);
    }

    free(full);
    return 0;
}

int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    if (!id || !type_out || !data_out || !len_out) return -1;

    char path[512];
    object_path(id, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }

    long file_size_long = ftell(f);
    if (file_size_long < 0) {
        fclose(f);
        return -1;
    }

    size_t file_size = (size_t)file_size_long;
    rewind(f);

    unsigned char *buf = malloc(file_size);
    if (!buf) {
        fclose(f);
        return -1;
    }

    if (fread(buf, 1, file_size, f) != file_size) {
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);

    ObjectID computed;
    compute_hash(buf, file_size, &computed);
    if (memcmp(computed.hash, id->hash, HASH_SIZE) != 0) {
        free(buf);
        return -1;
    }

    unsigned char *nul = memchr(buf, '\0', file_size);
    if (!nul) {
        free(buf);
        return -1;
    }

    size_t header_len = (size_t)(nul - buf);
    char header[64];
    if (header_len >= sizeof(header)) {
        free(buf);
        return -1;
    }

    memcpy(header, buf, header_len);
    header[header_len] = '\0';

    char type_str[16];
    size_t data_len = 0;
    if (sscanf(header, "%15s %zu", type_str, &data_len) != 2) {
        free(buf);
        return -1;
    }

    if (strcmp(type_str, "blob") == 0) {
        *type_out = OBJ_BLOB;
    } else if (strcmp(type_str, "tree") == 0) {
        *type_out = OBJ_TREE;
    } else if (strcmp(type_str, "commit") == 0) {
        *type_out = OBJ_COMMIT;
    } else {
        free(buf);
        return -1;
    }

    size_t actual_data_len = file_size - header_len - 1;
    if (actual_data_len != data_len) {
        free(buf);
        return -1;
    }

    void *out = malloc(data_len ? data_len : 1);
    if (!out) {
        free(buf);
        return -1;
    }

    memcpy(out, nul + 1, data_len);
    *data_out = out;
    *len_out = data_len;

    free(buf);
    return 0;
}
