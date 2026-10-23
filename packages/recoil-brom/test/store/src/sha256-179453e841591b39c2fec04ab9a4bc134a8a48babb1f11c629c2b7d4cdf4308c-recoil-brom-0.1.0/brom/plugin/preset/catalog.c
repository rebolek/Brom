#define _POSIX_C_SOURCE 200809L
#include "catalog.h"

#include <dirent.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define CATALOG_MAX_ENTRIES 65536u
#define CATALOG_TEXT_LIMIT 16384u

struct brom_preset_catalog {
    atomic_uint references;
    uint32_t count;
    brom_preset_catalog_entry *entries;
};

struct brom_preset_host_mapping {
    brom_preset_catalog *generation;
};

static int entry_compare(const void *left, const void *right) {
    const brom_preset_catalog_entry *a = left;
    const brom_preset_catalog_entry *b = right;
    return strcmp(a->identity.key, b->identity.key);
}

static int is_preset_file(const char *name) {
    const size_t length = strlen(name);
    return length > 7u && strcmp(name + length - 7u, ".preset") == 0;
}

static int file_has_read_permission(const struct stat *info) {
    return (info->st_mode & (S_IRUSR | S_IRGRP | S_IROTH)) != 0;
}

static int read_display_name(const char *path, char *name, size_t capacity) {
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    char line[CATALOG_TEXT_LIMIT];
    int found = 0;
    while (fgets(line, sizeof(line), file)) {
        char *field = strstr(line, "name:");
        if (!field) continue;
        field += 5;
        while (*field == ' ' || *field == '\t') ++field;
        if (*field++ != '"') break;
        size_t used = 0;
        int closed = 0;
        while (*field && *field != '\n' && *field != '\r') {
            if (*field == '"') { closed = 1; break; }
            if (*field == '\\' && field[1]) ++field;
            if (used + 1 >= capacity) break;
            name[used++] = *field++;
        }
        if (closed && used > 0) {
            name[used] = '\0';
            found = 1;
        }
        break;
    }
    const int read_error = ferror(file);
    const int close_error = fclose(file);
    return found && !read_error && close_error == 0;
}

static int identity_is_terminated(const brom_preset_identity *identity) {
    return memchr(identity->key, '\0', sizeof(identity->key)) != NULL &&
           memchr(identity->display_name, '\0', sizeof(identity->display_name)) != NULL;
}

static int append_entry(brom_preset_catalog_entry **entries, uint32_t *count,
                        uint32_t *capacity, const brom_preset_catalog_entry *entry) {
    if (*count == CATALOG_MAX_ENTRIES) return 0;
    if (*count == *capacity) {
        uint32_t next = *capacity ? *capacity * 2u : 16u;
        if (next > CATALOG_MAX_ENTRIES) next = CATALOG_MAX_ENTRIES;
        void *grown = realloc(*entries, (size_t)next * sizeof(**entries));
        if (!grown) return 0;
        *entries = grown;
        *capacity = next;
    }
    (*entries)[(*count)++] = *entry;
    return 1;
}

static int has_identity_key(const brom_preset_catalog_entry *entries,
                            uint32_t count, const char *key) {
    for (uint32_t index = 0; index < count; ++index)
        if (strcmp(entries[index].identity.key, key) == 0) return 1;
    return 0;
}

brom_preset_result brom_preset_catalog_refresh(
    const char *root, const brom_preset_catalog_factory_bank *factory,
    brom_preset_catalog **out_generation) {
    if (!root || !*root || !factory || !out_generation ||
        (factory->count && !factory->entries)) return BROM_PRESET_INVALID;
    if (factory->count > CATALOG_MAX_ENTRIES) return BROM_PRESET_CAPACITY;
    *out_generation = NULL;

    brom_preset_catalog_entry *entries = NULL;
    uint32_t count = 0, capacity = 0;
    for (uint32_t index = 0; index < factory->count; ++index) {
        brom_preset_catalog_entry item = {0};
        item.identity = factory->entries[index].identity;
        if (!identity_is_terminated(&item.identity) ||
            item.identity.kind != BROM_PRESET_IDENTITY_FACTORY ||
            !item.identity.key[0] || !item.identity.display_name[0] ||
            has_identity_key(entries, count, item.identity.key)) {
            free(entries);
            return BROM_PRESET_INVALID;
        }
        if (!append_entry(&entries, &count, &capacity, &item)) {
            free(entries);
            return BROM_PRESET_CAPACITY;
        }
    }

    DIR *directory = opendir(root);
    if (!directory) { free(entries); return BROM_PRESET_IO_ERROR; }
    struct dirent *item;
    while ((item = readdir(directory)) != NULL) {
        if (!is_preset_file(item->d_name)) continue;
        const size_t root_length = strlen(root), name_length = strlen(item->d_name);
        if (root_length + name_length + 2u >= 4096u) continue;
        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", root, item->d_name);
        struct stat info;
        if (stat(path, &info) != 0 || !S_ISREG(info.st_mode) ||
            !file_has_read_permission(&info)) continue;
        char display_name[BROM_PRESET_NAME_BYTES] = {0};
        if (!read_display_name(path, display_name, sizeof(display_name))) continue;
        brom_preset_catalog_entry user = {0};
        user.identity.kind = BROM_PRESET_IDENTITY_USER;
        const int key_length = snprintf(user.identity.key, sizeof(user.identity.key),
                                        "user:%s", item->d_name);
        if (key_length < 0 || (size_t)key_length >= sizeof(user.identity.key)) continue;
        if (has_identity_key(entries, count, user.identity.key)) continue;
        memcpy(user.identity.display_name, display_name, strlen(display_name) + 1u);
        if (!append_entry(&entries, &count, &capacity, &user)) {
            closedir(directory); free(entries); return BROM_PRESET_CAPACITY;
        }
    }
    const int directory_error = closedir(directory);
    if (directory_error != 0) { free(entries); return BROM_PRESET_IO_ERROR; }

    if (count > factory->count) {
        qsort(entries + factory->count, count - factory->count,
              sizeof(*entries), entry_compare);
    }
    brom_preset_catalog *generation = calloc(1, sizeof(*generation));
    if (!generation) { free(entries); return BROM_PRESET_CAPACITY; }
    atomic_init(&generation->references, 1u);
    generation->count = count;
    generation->entries = entries;
    *out_generation = generation;
    return BROM_PRESET_OK;
}

void brom_preset_catalog_retain(brom_preset_catalog *generation) {
    if (generation) atomic_fetch_add_explicit(&generation->references, 1u, memory_order_relaxed);
}

void brom_preset_catalog_release(brom_preset_catalog *generation) {
    if (!generation) return;
    if (atomic_fetch_sub_explicit(&generation->references, 1u, memory_order_acq_rel) == 1u) {
        free(generation->entries);
        free(generation);
    }
}

uint32_t brom_preset_catalog_count(const brom_preset_catalog *generation) {
    return generation ? generation->count : 0u;
}

brom_preset_result brom_preset_catalog_read(
    const brom_preset_catalog *generation, uint32_t index,
    brom_preset_catalog_entry *out_entry) {
    if (!generation || !out_entry) return BROM_PRESET_INVALID;
    if (index >= generation->count) return BROM_PRESET_NOT_FOUND;
    *out_entry = generation->entries[index];
    return BROM_PRESET_OK;
}

brom_preset_host_mapping *brom_preset_host_mapping_create(brom_preset_catalog *generation) {
    if (!generation) return NULL;
    brom_preset_host_mapping *mapping = malloc(sizeof(*mapping));
    if (!mapping) return NULL;
    mapping->generation = generation;
    brom_preset_catalog_retain(generation);
    return mapping;
}

void brom_preset_host_mapping_release(brom_preset_host_mapping *mapping) {
    if (!mapping) return;
    brom_preset_catalog_release(mapping->generation);
    free(mapping);
}

uint32_t brom_preset_host_mapping_count(const brom_preset_host_mapping *mapping) {
    return mapping ? brom_preset_catalog_count(mapping->generation) : 0u;
}

brom_preset_result brom_preset_host_mapping_read(
    const brom_preset_host_mapping *mapping, uint32_t slot,
    brom_preset_catalog_entry *out_entry) {
    if (!mapping) return BROM_PRESET_INVALID;
    return brom_preset_catalog_read(mapping->generation, slot, out_entry);
}
