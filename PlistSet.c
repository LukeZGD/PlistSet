#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <plist/plist.h>

static char *str_dup(const char *s)
{
    size_t len = strlen(s) + 1;
    char *p = malloc(len);
    if (p) memcpy(p, s, len);
    return p;
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;

    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        *--end = '\0';

    return s;
}

static int get_index(const char *s, uint32_t *index)
{
    char *end;
    unsigned long n;

    if (!*s || *s == '-') return -1;

    n = strtoul(s, &end, 10);

    if (*end || n > UINT32_MAX)
        return -1;

    *index = (uint32_t)n;
    return 0;
}

/* Traverse a colon-separated plist path and replace its final item. */
static int plist_set(plist_t root, const char *path, const char *value)
{
    char *copy = str_dup(path);
    char *parts[128];
    char *saveptr = NULL;
    char *token;
    int count = 0;
    plist_t node = root;

    if (!copy) return -1;

    token = strtok_r(copy, ":", &saveptr);

    while (token && count < 128) {
        parts[count++] = token;
        token = strtok_r(NULL, ":", &saveptr);
    }

    if (token || count == 0) {
        free(copy);
        return -1;
    }

    /* Traverse all components except the final one. */
    for (int i = 0; i < count - 1; i++) {
        uint32_t index;

        if (plist_get_node_type(node) == PLIST_DICT) {
            node = plist_dict_get_item(node, parts[i]);
        }
        else if (plist_get_node_type(node) == PLIST_ARRAY) {
            if (get_index(parts[i], &index) != 0 ||
                index >= plist_array_get_size(node)) {
                node = NULL;
            } else {
                node = plist_array_get_item(node, index);
            }
        }
        else {
            node = NULL;
        }

        if (!node) {
            free(copy);
            return -1;
        }
    }

    /* Replace the final dictionary key or array element. */
    if (plist_get_node_type(node) == PLIST_DICT) {
        plist_t old = plist_dict_get_item(node, parts[count - 1]);

        if (!old) {
            free(copy);
            return -1;
        }

        plist_dict_set_item(node, parts[count - 1], plist_new_string(value));
    }
    else if (plist_get_node_type(node) == PLIST_ARRAY) {
        uint32_t index;

        if (get_index(parts[count - 1], &index) != 0 ||
            index >= plist_array_get_size(node)) {
            free(copy);
            return -1;
        }

        plist_array_set_item(node, plist_new_string(value), index);
    }
    else {
        free(copy);
        return -1;
    }

    free(copy);
    return 0;
}

static int execute_command(plist_t plist, const char *command)
{
    char *copy = str_dup(command);
    char *p, *path, *value;
    int ret = -1;

    if (!copy) return -1;

    p = trim(copy);

    /* Parse command name. */
    if (strncmp(p, "Set", 3) != 0 || !isspace((unsigned char)p[3])) {
        fprintf(stderr, "Unsupported command: %s\n", p);
        goto done;
    }

    p = trim(p + 3);

    /* Parse path. */
    path = p;

    while (*p && !isspace((unsigned char)*p))
        p++;

    if (!*p) {
        fprintf(stderr, "Missing value\n");
        goto done;
    }

    *p++ = '\0';
    value = trim(p);

    if (!*path || !*value) {
        fprintf(stderr, "Invalid Set command\n");
        goto done;
    }

    /* Remove optional surrounding quotes. */
    size_t len = strlen(value);

    if (len >= 2 &&
        ((value[0] == '"' && value[len - 1] == '"') ||
         (value[0] == '\'' && value[len - 1] == '\''))) {
        value[len - 1] = '\0';
        value++;
    }

    if (plist_set(plist, path, value) != 0) {
        fprintf(stderr, "Failed to set plist path: %s\n", path);
        goto done;
    }

    ret = 0;

done:
    free(copy);
    return ret;
}

int main(int argc, char **argv)
{
    const char *command = NULL;
    const char *filename = NULL;
    char *data = NULL;
    char *output = NULL;
    uint32_t len = 0;
    uint32_t output_len = 0;
    plist_t plist = NULL;
    plist_format_t format;
    FILE *f;
    int ret = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            command = argv[++i];
        }
        else {
            filename = argv[i];
        }
    }

    if (!command || !filename) {
        fprintf(stderr, "Usage: PlistSet -c \"Set path value\" file.plist\n");
        return 1;
    }

    f = fopen(filename, "rb");
    if (!f) {
        perror(filename);
        return 1;
    }

    if (fseek(f, 0, SEEK_END) != 0) goto cleanup;
    long size = ftell(f);
    if (size <= 0 || size > UINT32_MAX) goto cleanup;
    rewind(f);

    len = (uint32_t)size;
    data = malloc(len);

    if (!data || fread(data, 1, len, f) != len)
        goto cleanup;

    fclose(f);
    f = NULL;

    if (plist_from_memory(data, len, &plist, &format) != PLIST_ERR_SUCCESS) {
        fprintf(stderr, "Failed to parse plist\n");
        goto cleanup;
    }

    if (execute_command(plist, command) != 0)
        goto cleanup;

    /* Serialize using the original plist format. */
    if (format == PLIST_FORMAT_BINARY)
        plist_to_bin(plist, &output, &output_len);
    else
        plist_to_xml(plist, &output, &output_len);

    if (!output || !output_len)
        goto cleanup;

    f = fopen(filename, "wb");
    if (!f) {
        perror(filename);
        goto cleanup;
    }

    if (fwrite(output, 1, output_len, f) != output_len) {
        fprintf(stderr, "Failed to write plist\n");
        goto cleanup;
    }

    ret = 0;

cleanup:
    if (f) fclose(f);
    free(data);
    free(output);
    if (plist) plist_free(plist);

    return ret;
}
