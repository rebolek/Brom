#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "save.h"
#include "transaction.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define PRESET_VERSION 1

#ifdef BROM_PRESET_TESTING
static brom_preset_save_test_stage test_failure_stage = BROM_PRESET_SAVE_FAIL_NONE;
void brom_preset_save_test_fail(brom_preset_save_test_stage stage) {
    test_failure_stage = stage;
}
#define FAIL_AT(stage) (test_failure_stage == (stage))
#else
#define FAIL_AT(stage) 0
#endif

static int valid_text(const char *text, size_t capacity) {
    return text && memchr(text, '\0', capacity) != NULL;
}

static int safe_name(const char *name) {
    if (!name || !*name) return 0;
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p)
        if (*p < 32u || *p == 127u || *p == '/' || *p == '\\') return 0;
    return 1;
}

static int append_literal(char *buffer, size_t capacity, size_t *used,
                          const char *literal) {
    const size_t length = strlen(literal);
    if (*used + length >= capacity) return 0;
    memcpy(buffer + *used, literal, length);
    *used += length;
    buffer[*used] = '\0';
    return 1;
}

static int append_quoted(char *buffer, size_t capacity, size_t *used,
                         const char *text) {
    for (const char *p = text; *p; ++p) {
        if ((*p == '"' || *p == '\\') && !append_literal(buffer, capacity, used, "\\")) return 0;
        if (*used + 2u >= capacity) return 0;
        buffer[(*used)++] = *p;
        buffer[*used] = '\0';
    }
    return 1;
}

static int render_preset(const brom_preset_schema *schema,
                         const brom_preset_snapshot *snapshot,
                         const char *name, const char *plugin_key, const char *key,
                         char *out, size_t capacity) {
    if (!schema || !snapshot || snapshot->count != (int32_t)schema->count ||
        schema->count == 0 || schema->count > BROM_PRESET_MAX_PARAMS ||
        !safe_name(name) || !safe_name(key) || !safe_name(plugin_key)) return 0;
    size_t used = 0;
    if (!append_literal(out, capacity, &used, "Recoil-Preset [\n    name: \"")) return 0;
    if (!append_quoted(out, capacity, &used, name) ||
        !append_literal(out, capacity, &used, "\"\n    version: 1\n    plugin: \"")) return 0;
    if (!append_quoted(out, capacity, &used, plugin_key) ||
        !append_literal(out, capacity, &used, "\"\n    key: \"")) return 0;
    if (!append_quoted(out, capacity, &used, key) ||
        !append_literal(out, capacity, &used, "\"\n    params: [\n")) return 0;

    for (uint32_t index = 0; index < schema->count; ++index) {
        const brom_preset_parameter *parameter = &schema->parameters[index];
        const double value = snapshot->values[index];
        if (!isfinite(value) || value < parameter->minimum || value > parameter->maximum ||
            !isfinite(parameter->minimum) || !isfinite(parameter->maximum) ||
            !isfinite(parameter->default_value) || parameter->minimum > parameter->maximum ||
            parameter->default_value < parameter->minimum || parameter->default_value > parameter->maximum ||
            !valid_text(parameter->key, sizeof(parameter->key)) || !parameter->key[0]) return 0;
        if (parameter->steps > 1u) {
            if (parameter->minimum == parameter->maximum) return 0;
            const double step = (parameter->maximum - parameter->minimum) /
                                (double)(parameter->steps - 1u);
            const double position = (value - parameter->minimum) / step;
            if (!isfinite(position) || fabs(position - nearbyint(position)) > 1e-9) return 0;
        }
        if (fabs(value - parameter->default_value) <=
            fmax(1.0, fabs(parameter->default_value)) * 1e-9) continue;
        for (const unsigned char *p = (const unsigned char *)parameter->key; *p; ++p)
            if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-')) return 0;
        int length = snprintf(out + used, capacity - used, "        %s: %.17g\n",
                              parameter->key, value);
        if (length < 0 || (size_t)length >= capacity - used) return 0;
        used += (size_t)length;
    }
    if (used + sizeof("    ]\n]\n") > capacity) return 0;
    memcpy(out + used, "    ]\n]\n", sizeof("    ]\n]\n"));
    return 1;
}

static int slug_name(const char *name, char *out, size_t capacity) {
    size_t used = 0;
    int pending_hyphen = 0;
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p) {
        unsigned char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            if (pending_hyphen && used && used + 1u < capacity) out[used++] = '-';
            pending_hyphen = 0;
            if (used + 1u >= capacity) return 0;
            out[used++] = (char)((c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c);
        } else pending_hyphen = 1;
    }
    if (!used) return 0;
    out[used] = '\0';
    return 1;
}

static int make_destination(const char *directory, const brom_preset_identity *target,
                            const char *display_name, char *out, size_t capacity,
                            char *saved_key, size_t key_capacity) {
    char filename[BROM_PRESET_NAME_BYTES];
    if (target->kind == BROM_PRESET_IDENTITY_USER) {
        static const char prefix[] = "user:";
        if (strncmp(target->key, prefix, sizeof(prefix) - 1u) != 0) return 0;
        const char *base = target->key + sizeof(prefix) - 1u;
        if (!*base || strchr(base, '/') || strchr(base, '\\') ||
            strlen(base) >= sizeof(filename) ||
            strlen(base) < 8u || strcmp(base + strlen(base) - 7u, ".preset") != 0) return 0;
        memcpy(filename, base, strlen(base) + 1u);
        if (snprintf(saved_key, key_capacity, "%s", target->key) >= (int)key_capacity) return 0;
    } else if (target->kind == BROM_PRESET_IDENTITY_FACTORY) {
        char slug[BROM_PRESET_NAME_BYTES];
        if (!slug_name(display_name, slug, sizeof(slug))) return 0;
        if (snprintf(filename, sizeof(filename), "%s.preset", slug) >= (int)sizeof(filename) ||
            snprintf(saved_key, key_capacity, "user:%s", filename) >= (int)key_capacity) return 0;
    } else return 0;
    int length = snprintf(out, capacity, "%s/%s", directory, filename);
    return length >= 0 && (size_t)length < capacity;
}

static void observe_destination(brom_preset_save_ticket *ticket) {
    struct stat info;
    if (lstat(ticket->destination, &info) == 0) {
        ticket->destination_existed = true;
        ticket->observed_device = (uint64_t)info.st_dev;
        ticket->observed_inode = (uint64_t)info.st_ino;
        ticket->observed_size = (uint64_t)info.st_size;
        ticket->observed_mtime = (int64_t)info.st_mtime;
    }
}

brom_preset_result brom_preset_save_capture(
    const brom_preset_schema *schema, const brom_preset_view *view,
    uint64_t expected_generation, const char *directory,
    const char *plugin_key, const brom_preset_identity *target, const char *display_name,
    brom_preset_save_ticket *out_ticket) {
    if (!schema || !view || !directory || !*directory || !target || !out_ticket ||
        !valid_text(target->key, sizeof(target->key)) || !target->key[0] ||
        !safe_name(plugin_key) || strlen(plugin_key) >= BROM_PRESET_KEY_BYTES ||
        !safe_name(display_name) || view->working.count != (int32_t)schema->count)
        return BROM_PRESET_INVALID;
    if (expected_generation != view->generation) return BROM_PRESET_STALE;
    brom_preset_save_ticket ticket = {0};
    ticket.generation = view->generation;
    ticket.working_revision = view->working_revision;
    ticket.snapshot = view->working;
    ticket.target = *target;
    memcpy(ticket.plugin_key, plugin_key, strlen(plugin_key) + 1u);
    memcpy(ticket.display_name, display_name, strlen(display_name) + 1u);
    char saved_key[BROM_PRESET_KEY_BYTES] = {0};
    if (!make_destination(directory, target, display_name, ticket.destination,
                         sizeof(ticket.destination), saved_key, sizeof(saved_key)))
        return BROM_PRESET_INVALID;
    ticket.saved_identity.kind = BROM_PRESET_IDENTITY_USER;
    memcpy(ticket.saved_identity.key, saved_key, strlen(saved_key) + 1u);
    memcpy(ticket.saved_identity.display_name, display_name, strlen(display_name) + 1u);
    if (!render_preset(schema, &ticket.snapshot, display_name, plugin_key, saved_key,
                       ticket.text, sizeof(ticket.text))) return BROM_PRESET_INVALID;
    observe_destination(&ticket);
    *out_ticket = ticket;
    return BROM_PRESET_OK;
}

static int destination_matches(const brom_preset_save_ticket *ticket,
                               const struct stat *info, int exists) {
    if (exists != ticket->destination_existed) return 0;
    if (!exists) return 1;
    return (uint64_t)info->st_dev == ticket->observed_device &&
           (uint64_t)info->st_ino == ticket->observed_inode &&
           (uint64_t)info->st_size == ticket->observed_size &&
           (int64_t)info->st_mtime == ticket->observed_mtime;
}

brom_preset_result brom_preset_save_write(
    const brom_preset_save_ticket *ticket,
    brom_preset_save_confirmation confirmation,
    brom_preset_save_completion *out_completion) {
    if (!ticket || !out_completion ||
        !valid_text(ticket->destination, sizeof(ticket->destination)) ||
        !valid_text(ticket->text, sizeof(ticket->text))) return BROM_PRESET_INVALID;
    if (FAIL_AT(BROM_PRESET_SAVE_FAIL_OPEN)) return BROM_PRESET_IO_ERROR;
    char directory[BROM_PRESET_SAVE_PATH_BYTES];
    memcpy(directory, ticket->destination, strlen(ticket->destination) + 1u);
    char *separator = strrchr(directory, '/');
    if (!separator) return BROM_PRESET_INVALID;
    *separator = '\0';
    int directory_fd = open(directory, O_RDONLY);
    if (directory_fd < 0) return BROM_PRESET_IO_ERROR;
    if (flock(directory_fd, LOCK_EX) != 0) { close(directory_fd); return BROM_PRESET_IO_ERROR; }
    struct stat current;
    const int exists = lstat(ticket->destination, &current) == 0;
    if (!destination_matches(ticket, &current, exists) ||
        (exists && confirmation != BROM_PRESET_SAVE_CONFIRM_OVERWRITE)) {
        flock(directory_fd, LOCK_UN); close(directory_fd);
        return BROM_PRESET_CONFIRMATION_REQUIRED;
    }
    char temporary[BROM_PRESET_SAVE_PATH_BYTES];
    int length = snprintf(temporary, sizeof(temporary), "%s.tmp-XXXXXX", ticket->destination);
    if (length < 0 || (size_t)length >= sizeof(temporary)) {
        flock(directory_fd, LOCK_UN); close(directory_fd); return BROM_PRESET_CAPACITY;
    }
    int fd = mkstemp(temporary);
    if (fd < 0) { flock(directory_fd, LOCK_UN); close(directory_fd); return BROM_PRESET_IO_ERROR; }
    const size_t text_length = strlen(ticket->text);
    size_t written = 0;
    while (written < text_length && !FAIL_AT(BROM_PRESET_SAVE_FAIL_WRITE)) {
        ssize_t amount = write(fd, ticket->text + written, text_length - written);
        if (amount <= 0) break;
        written += (size_t)amount;
    }
    const int flush_error = FAIL_AT(BROM_PRESET_SAVE_FAIL_FLUSH) ? EIO : fsync(fd);
    const int close_error = close(fd);
    if (written != text_length || flush_error != 0 || close_error != 0 ||
        FAIL_AT(BROM_PRESET_SAVE_FAIL_CLOSE)) {
        unlink(temporary); flock(directory_fd, LOCK_UN); close(directory_fd);
        return BROM_PRESET_IO_ERROR;
    }
    int install_error;
    if (FAIL_AT(BROM_PRESET_SAVE_FAIL_INSTALL)) {
        errno = EIO;
        install_error = -1;
    } else if (ticket->destination_existed) install_error = rename(temporary, ticket->destination);
    else {
        install_error = link(temporary, ticket->destination);
        if (install_error == 0) unlink(temporary);
    }
    if (install_error != 0) {
        const int result = errno == EEXIST ? BROM_PRESET_CONFIRMATION_REQUIRED : BROM_PRESET_IO_ERROR;
        unlink(temporary); flock(directory_fd, LOCK_UN); close(directory_fd);
        return (brom_preset_result)result;
    }
    fsync(directory_fd);
    flock(directory_fd, LOCK_UN);
    close(directory_fd);
    brom_preset_save_completion completion = {0};
    completion.generation = ticket->generation;
    completion.working_revision = ticket->working_revision;
    completion.snapshot = ticket->snapshot;
    completion.identity = ticket->saved_identity;
    *out_completion = completion;
    return BROM_PRESET_OK;
}

