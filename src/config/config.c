/* YAML-to-gw_config translation, defaulting, expansion, and policy validation. */
#include "gateway/config.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <yaml.h>

static void set_error(gw_error *error, gw_status code, const char *format, ...)
{
    va_list arguments;

    if (error == NULL) {
        return;
    }
    error->code = code;
    va_start(arguments, format);
    vsnprintf(error->message, sizeof(error->message), format, arguments);
    va_end(arguments);
}

static gw_status copy_text(char *destination, size_t capacity, const char *source)
{
    size_t length;

    if (destination == NULL || source == NULL || capacity == 0U) {
        return GW_ERR_ARGUMENT;
    }
    length = strlen(source);
    if (length >= capacity) {
        return GW_ERR_OVERFLOW;
    }
    memcpy(destination, source, length + 1U);
    return GW_OK;
}

static yaml_node_t *mapping_value(yaml_document_t *document, yaml_node_t *mapping,
                                  const char *key)
{
    yaml_node_pair_t *pair;

    if (mapping == NULL || mapping->type != YAML_MAPPING_NODE) {
        return NULL;
    }
    /* libyaml stores pair members as document-local node IDs, not pointers. */
    for (pair = mapping->data.mapping.pairs.start;
         pair < mapping->data.mapping.pairs.top; ++pair) {
        yaml_node_t *key_node = yaml_document_get_node(document, pair->key);
        if (key_node != NULL && key_node->type == YAML_SCALAR_NODE &&
            strcmp((const char *)key_node->data.scalar.value, key) == 0) {
            return yaml_document_get_node(document, pair->value);
        }
    }
    return NULL;
}

static const char *scalar_text(yaml_node_t *node)
{
    if (node == NULL || node->type != YAML_SCALAR_NODE) {
        return NULL;
    }
    return (const char *)node->data.scalar.value;
}

static gw_status read_string(yaml_document_t *document, yaml_node_t *mapping,
                             const char *key, char *destination, size_t capacity,
                             bool required, gw_error *error)
{
    yaml_node_t *node = mapping_value(document, mapping, key);
    const char *value;
    gw_status status;

    if (node == NULL) {
        if (required) {
            set_error(error, GW_ERR_PARSE, "missing required field %s", key);
            return GW_ERR_PARSE;
        }
        return GW_OK;
    }
    value = scalar_text(node);
    if (value == NULL) {
        set_error(error, GW_ERR_PARSE, "%s must be a scalar string", key);
        return GW_ERR_PARSE;
    }
    status = copy_text(destination, capacity, value);
    if (status == GW_ERR_ARGUMENT) {
        set_error(error, status, "invalid destination for %s", key);
    } else if (status == GW_ERR_OVERFLOW) {
        set_error(error, status, "%s exceeds %zu bytes", key, capacity - 1U);
    }
    return status;
}

static gw_status read_int(yaml_document_t *document, yaml_node_t *mapping,
                          const char *key, int *destination, bool required,
                          gw_error *error)
{
    yaml_node_t *node = mapping_value(document, mapping, key);
    const char *value;
    char *end = NULL;
    long parsed;

    if (node == NULL) {
        if (required) {
            set_error(error, GW_ERR_PARSE, "missing required field %s", key);
            return GW_ERR_PARSE;
        }
        return GW_OK;
    }
    value = scalar_text(node);
    if (value == NULL) {
        set_error(error, GW_ERR_PARSE, "%s must be an integer", key);
        return GW_ERR_PARSE;
    }
    errno = 0;
    parsed = strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed < INT_MIN ||
        parsed > INT_MAX) {
        set_error(error, GW_ERR_PARSE, "%s is not a valid integer", key);
        return GW_ERR_PARSE;
    }
    *destination = (int)parsed;
    return GW_OK;
}

static gw_status read_bool(yaml_document_t *document, yaml_node_t *mapping,
                           const char *key, bool *destination, gw_error *error)
{
    yaml_node_t *node = mapping_value(document, mapping, key);
    const char *value;

    if (node == NULL) {
        return GW_OK;
    }
    value = scalar_text(node);
    if (value == NULL) {
        set_error(error, GW_ERR_PARSE, "%s must be true or false", key);
        return GW_ERR_PARSE;
    }
    if (strcmp(value, "true") == 0 || strcmp(value, "True") == 0 ||
        strcmp(value, "TRUE") == 0) {
        *destination = true;
        return GW_OK;
    }
    if (strcmp(value, "false") == 0 || strcmp(value, "False") == 0 ||
        strcmp(value, "FALSE") == 0) {
        *destination = false;
        return GW_OK;
    }
    set_error(error, GW_ERR_PARSE, "%s must be true or false", key);
    return GW_ERR_PARSE;
}

void gw_config_init(gw_config *config)
{
    if (config == NULL) {
        return;
    }
    memset(config, 0, sizeof(*config));
    snprintf(config->server.listen, sizeof(config->server.listen), "%s",
                   "127.0.0.1");
    config->server.port = 9080U;
    snprintf(config->mediamtx.publish_base_url,
                   sizeof(config->mediamtx.publish_base_url), "%s",
                   "rtsp://127.0.0.1:8554");
    config->defaults.probe_timeout_sec = 10;
    config->defaults.startup_timeout_sec = 15;
    config->defaults.progress_timeout_sec = 10;
    config->defaults.stable_run_sec = 60;
    config->defaults.stop_timeout_sec = 5;
    config->defaults.max_retries = 10;
    config->defaults.max_backoff_sec = 30;
}

static void channel_init(gw_channel_config *channel)
{
    memset(channel, 0, sizeof(*channel));
    channel->enabled = true;
    snprintf(channel->input.type, sizeof(channel->input.type), "%s", "rtsp");
    snprintf(channel->input.transport, sizeof(channel->input.transport), "%s",
                   "tcp");
    snprintf(channel->video.decoder, sizeof(channel->video.decoder), "%s",
                   "h264_rkmpp");
    channel->video.width = 1280;
    channel->video.height = 720;
    snprintf(channel->video.encoder, sizeof(channel->video.encoder), "%s",
                   "h264_rkmpp");
    channel->video.bitrate_kbps = 4000;
    channel->video.fps = 25;
}

static gw_status parse_channel(yaml_document_t *document, yaml_node_t *node,
                               gw_channel_config *channel, size_t index,
                               gw_error *error)
{
    yaml_node_t *input;
    yaml_node_t *video;
    yaml_node_t *output;
    gw_status status;

    if (node == NULL || node->type != YAML_MAPPING_NODE) {
        set_error(error, GW_ERR_PARSE, "channels[%zu] must be a mapping", index);
        return GW_ERR_PARSE;
    }
    channel_init(channel);

#define READ_CHANNEL_STRING(map, key, member, required_value)                     \
    do {                                                                           \
        status = read_string(document, map, key, member, sizeof(member),            \
                             required_value, error);                                \
        if (status != GW_OK) {                                                      \
            return status;                                                         \
        }                                                                          \
    } while (0)

    READ_CHANNEL_STRING(node, "id", channel->id, true);
    status = read_bool(document, node, "enabled", &channel->enabled, error);
    if (status != GW_OK) {
        return status;
    }

    input = mapping_value(document, node, "input");
    video = mapping_value(document, node, "video");
    output = mapping_value(document, node, "output");
    if (input == NULL || input->type != YAML_MAPPING_NODE || video == NULL ||
        video->type != YAML_MAPPING_NODE || output == NULL ||
        output->type != YAML_MAPPING_NODE) {
        set_error(error, GW_ERR_PARSE,
                  "channels[%zu] requires input, video, and output mappings", index);
        return GW_ERR_PARSE;
    }

    READ_CHANNEL_STRING(input, "type", channel->input.type, false);
    READ_CHANNEL_STRING(input, "url", channel->input.url, true);
    READ_CHANNEL_STRING(input, "transport", channel->input.transport, false);
    READ_CHANNEL_STRING(video, "decoder", channel->video.decoder, false);
    READ_CHANNEL_STRING(video, "encoder", channel->video.encoder, false);
    READ_CHANNEL_STRING(output, "path", channel->output.path, true);

#undef READ_CHANNEL_STRING

#define READ_CHANNEL_INT(key, member)                                               \
    do {                                                                            \
        status = read_int(document, video, key, &member, false, error);              \
        if (status != GW_OK) {                                                       \
            return status;                                                          \
        }                                                                           \
    } while (0)

    READ_CHANNEL_INT("width", channel->video.width);
    READ_CHANNEL_INT("height", channel->video.height);
    READ_CHANNEL_INT("bitrate_kbps", channel->video.bitrate_kbps);
    READ_CHANNEL_INT("fps", channel->video.fps);
#undef READ_CHANNEL_INT

    {
        /* Expansion uses a separate buffer because in-place replacement is unsafe. */
        char expanded[GW_URL_CAP];
        status = gw_expand_environment(channel->input.url, expanded, sizeof(expanded),
                                       error);
        if (status != GW_OK) {
            return status;
        }
        status = copy_text(channel->input.url, sizeof(channel->input.url), expanded);
        if (status != GW_OK) {
            set_error(error, status, "cannot store expanded input URL");
        }
        return status;
    }
}

static gw_status parse_document(yaml_document_t *document, gw_config *config,
                                gw_error *error)
{
    yaml_node_t *root = yaml_document_get_root_node(document);
    yaml_node_t *server;
    yaml_node_t *mediamtx;
    yaml_node_t *defaults;
    yaml_node_t *channels;
    yaml_node_item_t *item;
    int port;
    gw_status status;

    if (root == NULL || root->type != YAML_MAPPING_NODE) {
        set_error(error, GW_ERR_PARSE, "configuration root must be a mapping");
        return GW_ERR_PARSE;
    }
    server = mapping_value(document, root, "server");
    mediamtx = mapping_value(document, root, "mediamtx");
    defaults = mapping_value(document, root, "defaults");
    channels = mapping_value(document, root, "channels");

    if (server != NULL) {
        status = read_string(document, server, "listen", config->server.listen,
                             sizeof(config->server.listen), false, error);
        if (status != GW_OK) {
            return status;
        }
        port = config->server.port;
        status = read_int(document, server, "port", &port, false, error);
        if (status != GW_OK) {
            return status;
        }
        if (port <= 0 || port > UINT16_MAX) {
            set_error(error, GW_ERR_VALIDATION, "server.port must be 1..65535");
            return GW_ERR_VALIDATION;
        }
        config->server.port = (uint16_t)port;
    }
    if (mediamtx != NULL) {
        status = read_string(document, mediamtx, "publish_base_url",
                             config->mediamtx.publish_base_url,
                             sizeof(config->mediamtx.publish_base_url), false, error);
        if (status != GW_OK) {
            return status;
        }
    }
    if (defaults != NULL) {
#define READ_DEFAULT(key, member)                                                   \
        do {                                                                        \
            status = read_int(document, defaults, key, &config->defaults.member,    \
                              false, error);                                        \
            if (status != GW_OK) {                                                  \
                return status;                                                      \
            }                                                                       \
        } while (0)
        READ_DEFAULT("probe_timeout_sec", probe_timeout_sec);
        READ_DEFAULT("startup_timeout_sec", startup_timeout_sec);
        READ_DEFAULT("progress_timeout_sec", progress_timeout_sec);
        READ_DEFAULT("stable_run_sec", stable_run_sec);
        READ_DEFAULT("stop_timeout_sec", stop_timeout_sec);
        READ_DEFAULT("max_retries", max_retries);
        READ_DEFAULT("max_backoff_sec", max_backoff_sec);
#undef READ_DEFAULT
    }
    if (channels == NULL || channels->type != YAML_SEQUENCE_NODE) {
        set_error(error, GW_ERR_PARSE, "channels must be a sequence");
        return GW_ERR_PARSE;
    }
    for (item = channels->data.sequence.items.start;
         item < channels->data.sequence.items.top; ++item) {
        yaml_node_t *channel_node;
        if (config->channel_count >= GW_MAX_CHANNELS) {
            set_error(error, GW_ERR_VALIDATION, "channels exceeds limit of %u",
                      (unsigned int)GW_MAX_CHANNELS);
            return GW_ERR_VALIDATION;
        }
        channel_node = yaml_document_get_node(document, *item);
        status = parse_channel(document, channel_node,
                               &config->channels[config->channel_count],
                               config->channel_count, error);
        if (status != GW_OK) {
            return status;
        }
        ++config->channel_count;
    }
    return GW_OK;
}

gw_status gw_config_load_file(const char *path, gw_config *config, gw_error *error)
{
    FILE *file;
    yaml_parser_t parser;
    yaml_document_t document;
    gw_status status;

    if (path == NULL || config == NULL) {
        set_error(error, GW_ERR_ARGUMENT, "configuration path and output are required");
        return GW_ERR_ARGUMENT;
    }

    if (error != NULL) {
        error->code = GW_OK;
        error->message[0] = '\0';
    }

    file = fopen(path, "rb");
    if (file == NULL) {
        set_error(error, GW_ERR_IO, "cannot open configuration '%s': %s", path,
                  strerror(errno));
        return GW_ERR_IO;
    }
    if (yaml_parser_initialize(&parser) == 0) {
        fclose(file);
        set_error(error, GW_ERR_NO_MEMORY, "cannot initialize YAML parser");
        return GW_ERR_NO_MEMORY;
    }
    yaml_parser_set_input_file(&parser, file);
    if (yaml_parser_load(&parser, &document) == 0) {
        set_error(error, GW_ERR_PARSE, "YAML parse error at line %zu: %s",
                  parser.problem_mark.line + 1U,
                  parser.problem != NULL ? parser.problem : "unknown error");
        yaml_parser_delete(&parser);
        fclose(file);
        return GW_ERR_PARSE;
    }

    gw_config_init(config);

    status = parse_document(&document, config, error);
    if (status == GW_OK) {
        status = gw_config_validate(config, error);
    }
    yaml_document_delete(&document);
    yaml_parser_delete(&parser);
    fclose(file);
    return status;
}

static bool valid_identifier(const char *value)
{
    const unsigned char *cursor = (const unsigned char *)value;
    if (*cursor == '\0') {
        return false;
    }
    while (*cursor != '\0') {
        if (!isalnum(*cursor) && *cursor != '_' && *cursor != '-') {
            return false;
        }
        ++cursor;
    }
    return true;
}

static bool supported_codec(const char *codec)
{
    return strcmp(codec, "h264_rkmpp") == 0 || strcmp(codec, "hevc_rkmpp") == 0;
}

gw_status gw_config_validate(const gw_config *config, gw_error *error)
{
    size_t index;
    size_t previous;

    if (config == NULL) {
        set_error(error, GW_ERR_ARGUMENT, "configuration is required");
        return GW_ERR_ARGUMENT;
    }
    if (config->server.port == 0U) {
        set_error(error, GW_ERR_VALIDATION, "server.port must be 1..65535");
        return GW_ERR_VALIDATION;
    }
    if (strncmp(config->mediamtx.publish_base_url, "rtsp://", 7U) != 0) {
        set_error(error, GW_ERR_VALIDATION,
                  "mediamtx.publish_base_url must use rtsp://");
        return GW_ERR_VALIDATION;
    }
    if (config->defaults.probe_timeout_sec <= 0 ||
        config->defaults.startup_timeout_sec <= 0 ||
        config->defaults.progress_timeout_sec <= 0 ||
        config->defaults.stable_run_sec <= 0 ||
        config->defaults.stop_timeout_sec <= 0 || config->defaults.max_retries < 0 ||
        config->defaults.max_backoff_sec <= 0) {
        set_error(error, GW_ERR_VALIDATION,
                  "timeouts/backoff must be positive and max_retries non-negative");
        return GW_ERR_VALIDATION;
    }
    /* Parsing checks representation; this pass enforces gateway-specific policy. */
    for (index = 0U; index < config->channel_count; ++index) {
        const gw_channel_config *channel = &config->channels[index];
        if (!valid_identifier(channel->id)) {
            set_error(error, GW_ERR_VALIDATION,
                      "channels[%zu].id may contain only letters, digits, '_' and '-'",
                      index);
            return GW_ERR_VALIDATION;
        }
        for (previous = 0U; previous < index; ++previous) {
            if (strcmp(config->channels[previous].id, channel->id) == 0) {
                set_error(error, GW_ERR_VALIDATION, "duplicate channel id '%s'",
                          channel->id);
                return GW_ERR_VALIDATION;
            }
        }
        if (strcmp(channel->input.type, "rtsp") != 0 ||
            strncmp(channel->input.url, "rtsp://", 7U) != 0) {
            set_error(error, GW_ERR_VALIDATION,
                      "channel '%s' phase-1 input must be an rtsp:// URL", channel->id);
            return GW_ERR_VALIDATION;
        }
        if (strcmp(channel->input.transport, "tcp") != 0 &&
            strcmp(channel->input.transport, "udp") != 0) {
            set_error(error, GW_ERR_VALIDATION,
                      "channel '%s' transport must be tcp or udp", channel->id);
            return GW_ERR_VALIDATION;
        }
        if (!supported_codec(channel->video.decoder) ||
            !supported_codec(channel->video.encoder)) {
            set_error(error, GW_ERR_VALIDATION,
                      "channel '%s' requires h264_rkmpp or hevc_rkmpp codecs",
                      channel->id);
            return GW_ERR_VALIDATION;
        }
        if (channel->video.width < 64 || channel->video.width > 7680 ||
            channel->video.height < 64 || channel->video.height > 4320 ||
            channel->video.bitrate_kbps < 64 ||
            channel->video.bitrate_kbps > 100000 || channel->video.fps < 1 ||
            channel->video.fps > 240) {
            set_error(error, GW_ERR_VALIDATION,
                      "channel '%s' video dimensions, bitrate, or fps are out of range",
                      channel->id);
            return GW_ERR_VALIDATION;
        }
        if (!valid_identifier(channel->output.path)) {
            set_error(error, GW_ERR_VALIDATION,
                      "channel '%s' output.path must be a safe path segment", channel->id);
            return GW_ERR_VALIDATION;
        }
    }
    if (error != NULL) {
        error->code = GW_OK;
        error->message[0] = '\0';
    }
    return GW_OK;
}

gw_status gw_expand_environment(const char *input, char *output, size_t output_size,
                                gw_error *error)
{
    size_t source = 0U;
    size_t target = 0U;

    if (input == NULL || output == NULL || output_size == 0U) {
        set_error(error, GW_ERR_ARGUMENT, "invalid environment expansion arguments");
        return GW_ERR_ARGUMENT;
    }
    while (input[source] != '\0') {
        if (input[source] == '$' && input[source + 1U] == '{') {
            size_t name_start = source + 2U;
            size_t name_end = name_start;
            char name[128];
            const char *value;
            size_t value_length;

            while (input[name_end] != '\0' && input[name_end] != '}') {
                ++name_end;
            }
            if (input[name_end] != '}' || name_end == name_start ||
                name_end - name_start >= sizeof(name)) {
                set_error(error, GW_ERR_ENV, "invalid environment reference in '%s'",
                          input);
                return GW_ERR_ENV;
            }
            memcpy(name, input + name_start, name_end - name_start);
            name[name_end - name_start] = '\0';
            value = getenv(name);
            if (value == NULL) {
                set_error(error, GW_ERR_ENV,
                          "required environment variable '%s' is not set", name);
                return GW_ERR_ENV;
            }
            value_length = strlen(value);
            if (target + value_length >= output_size) {
                set_error(error, GW_ERR_OVERFLOW, "expanded value exceeds %zu bytes",
                          output_size - 1U);
                return GW_ERR_OVERFLOW;
            }
            memcpy(output + target, value, value_length);
            target += value_length;
            source = name_end + 1U;
        } else {
            if (target + 1U >= output_size) {
                set_error(error, GW_ERR_OVERFLOW, "expanded value exceeds %zu bytes",
                          output_size - 1U);
                return GW_ERR_OVERFLOW;
            }
            output[target++] = input[source++];
        }
    }
    output[target] = '\0';
    return GW_OK;
}

gw_status gw_redact_url(const char *url, char *output, size_t output_size)
{
    const char *scheme;
    const char *authority;
    const char *authority_end;
    const char *at;
    const char *colon = NULL;
    const char *cursor;
    int written;

    if (url == NULL || output == NULL || output_size == 0U) {
        return GW_ERR_ARGUMENT;
    }
    scheme = strstr(url, "://");
    if (scheme == NULL) {
        return copy_text(output, output_size, url);
    }
    authority = scheme + 3;
    authority_end = strpbrk(authority, "/?#");
    if (authority_end == NULL) {
        authority_end = url + strlen(url);
    }
    at = memchr(authority, '@', (size_t)(authority_end - authority));
    if (at == NULL) {
        return copy_text(output, output_size, url);
    }
    for (cursor = authority; cursor < at; ++cursor) {
        if (*cursor == ':') {
            colon = cursor;
        }
    }
    if (colon != NULL) {
        written = snprintf(output, output_size, "%.*s:***%s",
                           (int)(colon - url), url, at);
    } else {
        written = snprintf(output, output_size, "%.*s***%s",
                           (int)(authority - url), url, at);
    }
    if (written < 0 || (size_t)written >= output_size) {
        if (output_size > 0U) {
            output[0] = '\0';
        }
        return GW_ERR_OVERFLOW;
    }
    return GW_OK;
}

const char *gw_status_string(gw_status status)
{
    switch (status) {
    case GW_OK:
        return "ok";
    case GW_ERR_ARGUMENT:
        return "invalid argument";
    case GW_ERR_IO:
        return "I/O error";
    case GW_ERR_PARSE:
        return "parse error";
    case GW_ERR_VALIDATION:
        return "validation error";
    case GW_ERR_ENV:
        return "environment error";
    case GW_ERR_NO_MEMORY:
        return "out of memory";
    case GW_ERR_OVERFLOW:
        return "size limit exceeded";
    }
    return "unknown error";
}
