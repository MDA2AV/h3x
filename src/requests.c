/* Parse a --requests file into conf.requests: request templates (method, path, headers, body) that
 * workers round-robin through to produce a request mix. Format is the ".http" / REST-client
 * convention:
 *
 *     METHOD /path            <- request line (no HTTP version; this is h3)
 *     header-name: value      <- zero or more header lines
 *                             <- blank line separates headers from body
 *     ...body until ### or EOF...
 *     ###                     <- separates requests
 *
 * '#' lines in the preamble are comments; the body is taken verbatim. The authority (:authority)
 * always comes from the command-line URL, never the file, so one file works against any target.
 * The whole file is slurped once and kept for the process lifetime; method/path/value/body iovecs
 * point into it (read-only after this runs), which is what lets workers share it without locking. */
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "h3x.h"

/* Headers that are illegal in HTTP/2 and HTTP/3 (connection-specific or h1-only). Rejected with a
 * clear error rather than sent as a malformed request. Names are compared after lowercasing. */
static const char *const FORBIDDEN[] = {"connection",  "keep-alive",       "transfer-encoding",
                                        "upgrade",      "proxy-connection", "host",
                                        NULL};

static void die(size_t reqno, const char *msg)
{
    fprintf(stderr, "--requests: request %zu: %s\n", reqno, msg);
    exit(EXIT_FAILURE);
}

static char *slurp(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "--requests: cannot open %s: %s\n", path, strerror(errno));
        exit(EXIT_FAILURE);
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fprintf(stderr, "--requests: cannot read %s\n", path);
        exit(EXIT_FAILURE);
    }
    long sz = ftell(f);
    rewind(f);
    if (sz < 0) {
        fprintf(stderr, "--requests: cannot size %s\n", path);
        exit(EXIT_FAILURE);
    }
    char *buf = h2o_mem_alloc((size_t)sz + 1);
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    *out_len = rd;
    return buf;
}

/* strip leading and trailing spaces, tabs, and a trailing CR; updates *len in place */
static char *trim(char *s, size_t *len)
{
    while (*len != 0 && (*s == ' ' || *s == '\t')) {
        ++s;
        --(*len);
    }
    while (*len != 0 && (s[*len - 1] == ' ' || s[*len - 1] == '\t' || s[*len - 1] == '\r'))
        --(*len);
    return s;
}

static int is_separator(const char *ts, size_t len)
{
    return len >= 3 && ts[0] == '#' && ts[1] == '#' && ts[2] == '#';
}

static void parse_request_line(char *ts, size_t llen, struct request *req, size_t reqno)
{
    size_t i = 0;
    while (i < llen && ts[i] != ' ' && ts[i] != '\t')
        ++i;
    if (i == 0 || i == llen)
        die(reqno, "bad request line (expected 'METHOD /path')");
    req->method = h2o_iovec_init(ts, i);
    while (i < llen && (ts[i] == ' ' || ts[i] == '\t'))
        ++i;
    char *ps = ts + i;
    size_t plen = llen - i;
    size_t j = 0;
    while (j < plen && ps[j] != ' ' && ps[j] != '\t')
        ++j;
    size_t pathlen = j;
    while (j < plen && (ps[j] == ' ' || ps[j] == '\t'))
        ++j;
    if (j < plen)
        die(reqno, "unexpected token after path (h3 request lines are 'METHOD /path', no HTTP version)");
    if (pathlen == 0 || ps[0] != '/')
        die(reqno, "path must start with '/'");
    req->path = h2o_iovec_init(ps, pathlen);
}

static void parse_header(char *ts, size_t llen, struct request *req, size_t reqno)
{
    char *colon = memchr(ts, ':', llen);
    if (colon == NULL)
        die(reqno, "header line has no ':'");
    size_t namelen = colon - ts;
    for (size_t i = 0; i < namelen; ++i)
        ts[i] = (char)tolower((unsigned char)ts[i]);
    if (namelen == 0)
        die(reqno, "empty header name");
    for (const char *const *f = FORBIDDEN; *f != NULL; ++f)
        if (namelen == strlen(*f) && memcmp(ts, *f, namelen) == 0)
            die(reqno, "header is illegal in HTTP/3 (connection-specific or h1-only)");
    size_t vlen = llen - namelen - 1;
    char *val = trim(colon + 1, &vlen);
    if (req->num_headers >= sizeof(req->headers) / sizeof(req->headers[0]))
        die(reqno, "too many headers (max 64)");
    req->headers[req->num_headers].name = h2o_iovec_init(ts, namelen);
    req->headers[req->num_headers].value = h2o_iovec_init(val, vlen);
    ++req->num_headers;
}

static int has_header(const struct request *req, const char *name)
{
    size_t n = strlen(name);
    for (size_t i = 0; i < req->num_headers; ++i)
        if (req->headers[i].name.len == n && memcmp(req->headers[i].name.base, name, n) == 0)
            return 1;
    return 0;
}

/* HTTP/3 delimits the body with the stream FIN, so content-length is optional - but the reference
 * client sends it and some application servers require it, so add it when the user did not. */
static void add_content_length(struct request *req, size_t reqno)
{
    char *buf = h2o_mem_alloc(24);
    int n = snprintf(buf, 24, "%zu", req->body.len);
    if (req->num_headers >= sizeof(req->headers) / sizeof(req->headers[0]))
        die(reqno, "too many headers (max 64)");
    req->headers[req->num_headers].name = h2o_iovec_init("content-length", 14);
    req->headers[req->num_headers].value = h2o_iovec_init(buf, (size_t)n);
    ++req->num_headers;
}

/* Parse one block (between separators) into *req. Returns 1 if a request was parsed, 0 if the block
 * held only comments/blank lines. reqno is 1-based, for error messages. */
static int parse_block(char *bstart, char *bend, struct request *req, size_t reqno)
{
    char *p = bstart;
    int request_seen = 0;
    while (p < bend) {
        char *nl = memchr(p, '\n', bend - p);
        char *lend = nl != NULL ? nl : bend;
        char *nextp = nl != NULL ? nl + 1 : bend;
        size_t llen = lend - p;
        char *ts = trim(p, &llen);
        if (!request_seen) {
            if (llen == 0 || ts[0] == '#') { /* skip leading blanks and comments */
                p = nextp;
                continue;
            }
            parse_request_line(ts, llen, req, reqno);
            request_seen = 1;
            p = nextp;
            continue;
        }
        if (llen == 0) { /* blank line: the body is everything after it, verbatim */
            size_t blen = bend - nextp;
            if (blen != 0 && nextp[blen - 1] == '\n') { /* drop the newline before the separator */
                --blen;
                if (blen != 0 && nextp[blen - 1] == '\r')
                    --blen;
            }
            if (blen != 0) {
                req->body = h2o_iovec_init(nextp, blen);
                if (!has_header(req, "content-length"))
                    add_content_length(req, reqno);
            }
            return 1;
        }
        if (ts[0] == '#') { /* comment among headers */
            p = nextp;
            continue;
        }
        parse_header(ts, llen, req, reqno);
        p = nextp;
    }
    return request_seen; /* reached block end with no blank line => no body */
}

void load_requests(const char *path)
{
    size_t n;
    char *buf = slurp(path, &n); /* kept for the process lifetime; iovecs point into it */
    char *end = buf + n;

    /* upper bound on request count = separator lines + 1 */
    size_t cap = 1;
    for (char *p = buf; p < end;) {
        char *nl = memchr(p, '\n', end - p);
        char *lend = nl != NULL ? nl : end;
        size_t llen = lend - p;
        char *ts = trim(p, &llen);
        if (is_separator(ts, llen))
            ++cap;
        p = nl != NULL ? nl + 1 : end;
    }
    conf.requests = h2o_mem_alloc(sizeof(*conf.requests) * cap);
    memset(conf.requests, 0, sizeof(*conf.requests) * cap);

    /* walk blocks separated by "###" lines */
    conf.num_requests = 0;
    char *p = buf;
    while (p < end) {
        char *bstart = p;
        char *bend = end;
        while (p < end) { /* find the next separator line, or EOF */
            char *nl = memchr(p, '\n', end - p);
            char *lend = nl != NULL ? nl : end;
            size_t llen = lend - p;
            char *ts = trim(p, &llen);
            if (is_separator(ts, llen)) {
                bend = p;
                p = nl != NULL ? nl + 1 : end;
                break;
            }
            p = nl != NULL ? nl + 1 : end;
            if (nl == NULL)
                break;
        }
        if (parse_block(bstart, bend, &conf.requests[conf.num_requests], conf.num_requests + 1))
            ++conf.num_requests;
    }

    if (conf.num_requests == 0) {
        fprintf(stderr, "--requests: %s contains no requests\n", path);
        exit(EXIT_FAILURE);
    }
}
