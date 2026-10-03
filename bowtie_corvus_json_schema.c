/*
 * A Bowtie (https://github.com/bowtie-json-schema/bowtie) harness for the corvus-json-schema C library.
 *
 * It speaks IHOP (one JSON request per line on standard input, one response per line on standard output):
 * - `start` reports the implementation and its dialects;
 * - `dialect` sets the dialect for schemas without `$schema`;
 * - `run` compiles the case's schema with the case's `registry` as the document resolver and validates each instance
 *   (for `annotations` output, through a verbose collector, reporting each annotation with its instance location and
 *   `#...` keyword location);
 * - `stop` exits.
 *
 * Requests are read with a small JSON scanner that keeps each value's text as Bowtie wrote it, so schemas and
 * instances reach the library unchanged (numbers included).
 */
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>

#include "corvus_json_schema.h"

/* ---------------------------------------------------------------------------------------------------------------- */
/* Growable text */

typedef struct {
    char *data;
    size_t len, cap;
} buffer;

static void put(buffer *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        b->cap = (b->len + n + 1) * 2;
        b->data = realloc(b->data, b->cap);
        if (!b->data) abort();
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void puts_(buffer *b, const char *s) { put(b, s, strlen(s)); }

/* A JSON string literal for UTF-8 text. */
static void put_string(buffer *b, const char *s, size_t n) {
    puts_(b, "\"");
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        char esc[8];
        if (c == '"' || c == '\\') {
            esc[0] = '\\';
            esc[1] = (char)c;
            put(b, esc, 2);
        } else if (c < 0x20) {
            snprintf(esc, sizeof esc, "\\u%04x", c);
            puts_(b, esc);
        } else {
            put(b, (const char *)&c, 1);
        }
    }
    puts_(b, "\"");
}

/* ---------------------------------------------------------------------------------------------------------------- */
/* Scanning JSON text in place */

typedef struct {
    const char *p, *end;
} scanner;

static void ws(scanner *s) {
    while (s->p < s->end && (*s->p == ' ' || *s->p == '\t' || *s->p == '\n' || *s->p == '\r')) s->p++;
}

static bool expect(scanner *s, char c) {
    ws(s);
    if (s->p < s->end && *s->p == c) {
        s->p++;
        return true;
    }
    return false;
}

/* Skips one value, returning false for malformed text. */
static bool skip(scanner *s) {
    ws(s);
    if (s->p >= s->end) return false;
    char c = *s->p;
    if (c == '"') {
        for (s->p++; s->p < s->end && *s->p != '"'; s->p++)
            if (*s->p == '\\') s->p++;
        if (s->p >= s->end) return false;
        s->p++;
        return true;
    }
    if (c == '{' || c == '[') {
        char close = c == '{' ? '}' : ']';
        s->p++;
        if (expect(s, close)) return true;
        do {
            if (c == '{') {
                if (!skip(s) || !expect(s, ':')) return false;
            }
            if (!skip(s)) return false;
        } while (expect(s, ','));
        return expect(s, close);
    }
    while (s->p < s->end && !strchr(",]} \t\r\n", *s->p)) s->p++;
    return true;
}

typedef struct {
    const char *start;
    size_t len;
} span;

static span value(scanner *s, bool *ok) {
    ws(s);
    span v = {s->p, 0};
    *ok = skip(s);
    v.len = (size_t)(s->p - v.start);
    return v;
}

/* Appends a string literal's UTF-8 text (unescaped) to b. */
static bool unescape(span literal, buffer *b) {
    const char *p = literal.start + 1, *end = literal.start + literal.len - 1;
    if (literal.len < 2 || literal.start[0] != '"') return false;
    while (p < end) {
        if (*p != '\\') {
            put(b, p++, 1);
            continue;
        }
        if (++p >= end) return false;
        char c = *p++;
        const char *simple = strchr("\"\\/bfnrt", c);
        if (simple) {
            static const char to[] = "\"\\/\b\f\n\r\t";
            put(b, &to[simple - "\"\\/bfnrt"], 1);
            continue;
        }
        if (c != 'u' || end - p < 4) return false;
        unsigned long cp = strtoul((char[5]){p[0], p[1], p[2], p[3], 0}, NULL, 16);
        p += 4;
        if (cp >= 0xD800 && cp < 0xDC00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
            unsigned long lo = strtoul((char[5]){p[2], p[3], p[4], p[5], 0}, NULL, 16);
            if (lo >= 0xDC00 && lo < 0xE000) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                p += 6;
            }
        }
        char u[4];
        size_t n;
        if (cp < 0x80) {
            u[0] = (char)cp;
            n = 1;
        } else if (cp < 0x800) {
            u[0] = (char)(0xC0 | (cp >> 6));
            u[1] = (char)(0x80 | (cp & 0x3F));
            n = 2;
        } else if (cp < 0x10000) {
            u[0] = (char)(0xE0 | (cp >> 12));
            u[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
            u[2] = (char)(0x80 | (cp & 0x3F));
            n = 3;
        } else {
            u[0] = (char)(0xF0 | (cp >> 18));
            u[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
            u[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
            u[3] = (char)(0x80 | (cp & 0x3F));
            n = 4;
        }
        put(b, u, n);
    }
    return true;
}

/* An object's member whose (unescaped) name is `name`, as raw text; `found` says whether there was one. */
static span member(span object, const char *name, bool *found) {
    scanner s = {object.start, object.start + object.len};
    span none = {NULL, 0};
    *found = false;
    if (!expect(&s, '{') || expect(&s, '}')) return none;
    do {
        bool ok;
        span key = value(&s, &ok);
        buffer k = {0};
        if (!ok || !unescape(key, &k) || !expect(&s, ':')) {
            free(k.data);
            return none;
        }
        span v = value(&s, &ok);
        bool match = k.data && strcmp(k.data, name) == 0;
        if (!k.data && name[0] == '\0') match = true;
        free(k.data);
        if (!ok) return none;
        if (match) {
            *found = true;
            return v;
        }
    } while (expect(&s, ','));
    return none;
}

/* A member's string value (unescaped, caller frees), or NULL. */
static char *member_string(span object, const char *name) {
    bool found;
    span v = member(object, name, &found);
    buffer b = {0};
    if (!found || v.len == 0 || v.start[0] != '"' || !unescape(v, &b)) {
        free(b.data);
        return NULL;
    }
    return b.data ? b.data : calloc(1, 1);
}

/* Visits each member (name unescaped) or element (name NULL) of an object or array. */
typedef void (*visit_fn)(void *ctx, const char *name, span v);

static bool each(span container, visit_fn visit, void *ctx) {
    scanner s = {container.start, container.start + container.len};
    ws(&s);
    bool object = s.p < s.end && *s.p == '{';
    if (!expect(&s, object ? '{' : '[')) return false;
    if (expect(&s, object ? '}' : ']')) return true;
    do {
        bool ok;
        buffer k = {0};
        if (object) {
            span key = value(&s, &ok);
            if (!ok || !unescape(key, &k) || !expect(&s, ':')) {
                free(k.data);
                return false;
            }
        }
        span v = value(&s, &ok);
        if (!ok) {
            free(k.data);
            return false;
        }
        visit(ctx, object ? (k.data ? k.data : "") : NULL, v);
        free(k.data);
    } while (expect(&s, ','));
    return true;
}

/* ---------------------------------------------------------------------------------------------------------------- */
/* The case's registry, as the document resolver */

typedef struct {
    char **uris;
    span *schemas;
    size_t count;
} registry;

static size_t without_fragment(const char *uri, size_t len) {
    const char *hash = memchr(uri, '#', len);
    return hash ? (size_t)(hash - uri) : len;
}

static void add_to_registry(void *ctx, const char *name, span v) {
    registry *r = ctx;
    r->uris = realloc(r->uris, (r->count + 1) * sizeof *r->uris);
    r->schemas = realloc(r->schemas, (r->count + 1) * sizeof *r->schemas);
    r->uris[r->count] = strndup(name, without_fragment(name, strlen(name)));
    r->schemas[r->count] = v;
    r->count++;
}

static cjs_status resolve(void *user_data, const char *uri, size_t len, cjs_resolved *out) {
    registry *r = user_data;
    size_t n = without_fragment(uri, len);
    for (size_t i = 0; i < r->count; i++)
        if (strlen(r->uris[i]) == n && memcmp(r->uris[i], uri, n) == 0)
            return cjs_resolved_set_json(out, r->schemas[i].start, r->schemas[i].len);
    return CJS_OK; /* unknown */
}

/* ---------------------------------------------------------------------------------------------------------------- */
/* Annotations */

/* Percent-encodes text as a URI fragment does (upper-case hex, UTF-8), keeping the characters a fragment allows. */
static void put_percent_encoded(buffer *b, const char *text) {
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (*p < 128 && (isalnum(*p) || strchr("-._~!$&'()*+,;=:@/?", *p))) {
            put(b, (const char *)p, 1);
        } else {
            char hex[4];
            snprintf(hex, sizeof hex, "%%%02X", *p);
            puts_(b, hex);
        }
    }
}

static void put_unescaped_token(buffer *b, const char *token) {
    for (const char *p = token; *p; p++) {
        if (p[0] == '~' && p[1] == '1') {
            puts_(b, "/");
            p++;
        } else if (p[0] == '~' && p[1] == '0') {
            puts_(b, "~");
            p++;
        } else {
            put(b, p, 1);
        }
    }
}

typedef struct {
    buffer *out;
    bool first;
    const char *instance_location, *token;
} annotation_ctx;

static void each_schema_location(void *ctx, const char *schema_location, span value) {
    annotation_ctx *a = ctx;
    buffer keyword = {0}, location = {0};
    put_unescaped_token(&keyword, a->token);
    puts_(&location, schema_location);
    puts_(&location, "/");
    put_percent_encoded(&location, a->token);
    if (!a->first) puts_(a->out, ",");
    a->first = false;
    puts_(a->out, "{\"keyword\":");
    put_string(a->out, keyword.data ? keyword.data : "", keyword.len);
    puts_(a->out, ",\"instanceLocation\":");
    put_string(a->out, a->instance_location, strlen(a->instance_location));
    puts_(a->out, ",\"keywordLocation\":");
    put_string(a->out, location.data, location.len);
    puts_(a->out, ",\"annotation\":");
    put(a->out, value.start, value.len);
    puts_(a->out, "}");
    free(keyword.data);
    free(location.data);
}

static void each_keyword(void *ctx, const char *token, span locations) {
    annotation_ctx *a = ctx;
    a->token = token;
    each(locations, each_schema_location, a);
}

static void each_instance_location(void *ctx, const char *instance_location, span keywords) {
    annotation_ctx *a = ctx;
    a->instance_location = instance_location;
    each(keywords, each_keyword, a);
}

/*
 * The annotations a verbose collector grouped (instance location, then keyword as a JSON-pointer token, then schema
 * location as a `#...` fragment; the same in every Corvus implementation) as Bowtie lists them: each with its keyword
 * unescaped, and its keyword location the schema location's fragment followed by `/` and the keyword token,
 * percent-encoded as the fragment is.
 */
static void put_annotations(buffer *out, cjs_collector *collector) {
    cjs_str grouped;
    puts_(out, "[");
    if (cjs_collector_annotations_json(collector, &grouped) == CJS_OK) {
        annotation_ctx a = {out, true, "", ""};
        span all = {grouped.ptr, grouped.len};
        each(all, each_instance_location, &a);
    }
    puts_(out, "]");
}

/* ---------------------------------------------------------------------------------------------------------------- */
/* Commands */

static const struct {
    const char *uri;
    cjs_dialect dialect;
} dialects[] = {
    {"https://json-schema.org/draft/2020-12/schema", CJS_DRAFT202012},
    {"https://json-schema.org/draft/2019-09/schema", CJS_DRAFT201909},
    {"http://json-schema.org/draft-07/schema#", CJS_DRAFT7},
    {"http://json-schema.org/draft-06/schema#", CJS_DRAFT6},
    {"http://json-schema.org/draft-04/schema#", CJS_DRAFT4},
};
#define DIALECT_COUNT (sizeof dialects / sizeof dialects[0])

static void put_error(buffer *out, const char *message) {
    puts_(out, "\"errored\":true,\"context\":{\"message\":");
    put_string(out, message, strlen(message));
    puts_(out, "}");
}

static void put_last_error(buffer *out) {
    cjs_str m = cjs_last_error_message();
    puts_(out, "\"errored\":true,\"context\":{\"message\":");
    put_string(out, m.ptr ? m.ptr : "", m.len);
    puts_(out, "}");
}

typedef struct {
    buffer *out;
    const cjs_validator *validator;
    bool annotations, first;
} run_ctx;

static void run_test(void *ctx, const char *name, span test) {
    (void)name;
    run_ctx *r = ctx;
    bool found, valid = false;
    span instance = member(test, "instance", &found);
    if (!r->first) puts_(r->out, ",");
    r->first = false;
    puts_(r->out, "{");
    if (!found) {
        put_error(r->out, "the test has no instance");
    } else if (!r->annotations) {
        if (cjs_validator_validate_json(r->validator, instance.start, instance.len, &valid) == CJS_OK)
            puts_(r->out, valid ? "\"valid\":true" : "\"valid\":false");
        else
            put_last_error(r->out);
    } else {
        cjs_collector *collector = cjs_collector_new(CJS_VERBOSE);
        if (cjs_validator_evaluate_json(r->validator, instance.start, instance.len, collector, &valid) == CJS_OK) {
            puts_(r->out, valid ? "\"valid\":true,\"annotations\":" : "\"valid\":false,\"annotations\":");
            put_annotations(r->out, collector);
        } else {
            put_last_error(r->out);
        }
        cjs_collector_free(collector);
    }
    puts_(r->out, "}");
}

static void run(span request, cjs_dialect dialect, buffer *out) {
    bool found;
    span seq = member(request, "seq", &found);
    span test_case = member(request, "case", &found);
    span schema = member(test_case, "schema", &found);
    span registry_text = member(test_case, "registry", &found);
    registry reg = {0};
    if (found) each(registry_text, add_to_registry, &reg);
    char *output = member_string(request, "output");

    puts_(out, "{\"seq\":");
    put(out, seq.start, seq.len);
    puts_(out, ",");
    cjs_options *options = cjs_options_new();
    cjs_options_set_default_dialect(options, dialect);
    cjs_options_set_resolver(options, resolve, &reg, NULL);
    cjs_validator *validator = NULL;
    if (cjs_compile(schema.start, schema.len, options, &validator) != CJS_OK) {
        put_last_error(out);
    } else {
        run_ctx r = {out, validator, output && strcmp(output, "annotations") == 0, true};
        puts_(out, "\"results\":[");
        each(member(test_case, "tests", &found), run_test, &r);
        puts_(out, "]");
    }
    puts_(out, "}");
    cjs_validator_free(validator);
    cjs_options_free(options);
    for (size_t i = 0; i < reg.count; i++) free(reg.uris[i]);
    free(reg.uris);
    free(reg.schemas);
    free(output);
}

static void start(buffer *out) {
    struct utsname os;
    uname(&os);
    cjs_str version = cjs_version_string();
    puts_(out, "{\"version\":1,\"implementation\":{\"language\":\"c\",\"name\":\"corvus-json-schema\",\"version\":");
    put_string(out, version.ptr, version.len);
    puts_(out, ",\"homepage\":\"https://github.com/corvus-dotnet/Corvus.JsonSchema\","
               "\"documentation\":\"https://github.com/corvus-dotnet/Corvus.JsonSchema/tree/main/src-rs/"
               "corvus-json-schema-capi\","
               "\"issues\":\"https://github.com/corvus-dotnet/Corvus.JsonSchema/issues\","
               "\"source\":\"https://github.com/corvus-dotnet/Corvus.JsonSchema\",\"dialects\":[");
    for (size_t i = 0; i < DIALECT_COUNT; i++) {
        if (i) puts_(out, ",");
        put_string(out, dialects[i].uri, strlen(dialects[i].uri));
    }
    puts_(out, "],\"os\":");
    put_string(out, os.sysname, strlen(os.sysname));
    puts_(out, ",\"os_version\":");
    put_string(out, os.release, strlen(os.release));
    puts_(out, ",\"language_version\":");
#ifdef __VERSION__
    const char *compiler = "C11 (gcc " __VERSION__ ")";
#else
    const char *compiler = "C11";
#endif
    put_string(out, compiler, strlen(compiler));
    puts_(out, "}}");
}

static void fail(const char *message) {
    fprintf(stderr, "%s\n", message);
    exit(EXIT_FAILURE);
}

int main(void) {
    bool started = false;
    cjs_dialect dialect = CJS_DRAFT202012;
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    while ((n = getline(&line, &cap, stdin)) > 0) {
        span request = {line, (size_t)n};
        scanner check = {line, line + n};
        ws(&check);
        if (check.p == check.end) continue;
        char *cmd = member_string(request, "cmd");
        buffer out = {0};
        if (!cmd) fail("A request has no command");
        if (strcmp(cmd, "start") == 0) {
            bool found;
            span version = member(request, "version", &found);
            if (!found || version.len != 1 || version.start[0] != '1') fail("Unsupported IHOP version");
            started = true;
            start(&out);
        } else if (strcmp(cmd, "dialect") == 0) {
            if (!started) fail("Not started");
            char *uri = member_string(request, "dialect");
            bool known = false;
            for (size_t i = 0; uri && i < DIALECT_COUNT; i++)
                if (strcmp(uri, dialects[i].uri) == 0) {
                    dialect = dialects[i].dialect;
                    known = true;
                }
            puts_(&out, known ? "{\"ok\":true}" : "{\"ok\":false}");
            free(uri);
        } else if (strcmp(cmd, "run") == 0) {
            if (!started) fail("Not started");
            run(request, dialect, &out);
        } else if (strcmp(cmd, "stop") == 0) {
            if (!started) fail("Not started");
            free(cmd);
            free(line);
            return EXIT_SUCCESS;
        } else {
            fail("Unknown command");
        }
        free(cmd);
        fwrite(out.data, 1, out.len, stdout);
        fputc('\n', stdout);
        fflush(stdout);
        free(out.data);
    }
    free(line);
    return EXIT_SUCCESS;
}
