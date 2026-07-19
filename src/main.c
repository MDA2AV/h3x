/* CLI parsing, config validation, and the run lifecycle: spawn workers, join, summarize. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* sched_getaffinity, to honor the container's CPU set */
#endif
#include <getopt.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/resource.h>
#include <unistd.h>
#include <openssl/ssl.h>
#include "picotls/openssl.h"
#include "h3x.h"

struct config conf = {
    .method = "GET",
    .total_requests = 100,
    .concurrency = 10,
    /* .threads = 0 means auto: one per available CPU (see detect_ncpu) */
    .threads = 0,
    .io_timeout = DEFAULT_IO_TIMEOUT,
    .qpack_table = 4096,
};

const ptls_key_exchange_algorithm_t *h3_key_exchanges[8];

static const char *progname;

/* Number of CPUs actually available to this process, honoring Docker limits: the CPU set
 * (--cpuset-cpus, via sched_getaffinity) and a cgroup-v2 CPU quota (--cpus, via cpu.max). */
static unsigned detect_ncpu(void)
{
    unsigned n = 0;
    cpu_set_t set;
    if (sched_getaffinity(0, sizeof(set), &set) == 0)
        n = (unsigned)CPU_COUNT(&set);
    if (n == 0) {
        long s = sysconf(_SC_NPROCESSORS_ONLN);
        n = s > 0 ? (unsigned)s : 1;
    }
    FILE *f = fopen("/sys/fs/cgroup/cpu.max", "r"); /* "<quota> <period>" or "max <period>" */
    if (f != NULL) {
        char quota[32] = {0};
        unsigned long period = 0;
        if (fscanf(f, "%31s %lu", quota, &period) == 2 && period > 0 && strcmp(quota, "max") != 0) {
            unsigned long q = strtoul(quota, NULL, 10);
            unsigned cq = (unsigned)((q + period - 1) / period); /* ceil(quota/period) */
            if (cq >= 1 && cq < n)
                n = cq;
        }
        fclose(f);
    }
    return n >= 1 ? n : 1;
}

static void usage(void)
{
    fprintf(stderr,
            "Usage: %s [options] <url>\n"
            "  -n <count>   total requests to send (default: 100)\n"
            "  -d <seconds> run for this long instead of -n (overrides -n)\n"
            "  -m <num>     concurrent streams per connection (default: 10)\n"
            "  --connections <n> total connections across all threads (default: one per thread)\n"
            "  -t <num>     worker threads (default: all CPUs available to the process)\n"
            "  --method <method> request method (default: GET)\n"
            "  -H <name:value>   add a request header (repeatable)\n"
            "  -x <url>     connect to this host:port instead of the URL's (pins a backend)\n"
            "  -W <bytes>   HTTP/3 receive window (per stream)\n"
            "  -k           skip TLS certificate verification\n"
            "  --reconnect <N>   close each connection after N requests (exercises 0-RTT)\n"
            "  --send-batch <N>  accumulate N freed slots before refilling, so requests pack into\n"
            "                    fewer datagrams (1 = off; helps server-bound, hurts client-bound)\n"
            "  --socket-per-conn one UDP socket per connection (unique 4-tuple, the h2load model;\n"
            "                    avoids servers that kill connections sharing a source port)\n"
            "  --requests <file> load request templates (method/path/headers/body) from a .http file;\n"
            "                    multiple requests become a round-robin mix (overrides -m/-H)\n"
            "  --dump-requests   parse --requests and print the result, then exit\n"
            "  --no-resumption   force a full handshake on every connection\n"
            "  --max-udp-payload-size <bytes>\n"
            "  --initial-udp-payload-size <bytes>\n"
            "  --ack-frequency <0..1>\n"
            "  --disallow-delayed-ack\n"
            "  --no-ecn\n"
            "  --qpack-table <bytes>   QPACK encoder table capacity (default: 4096)\n"
            "  --key-exchange <name>   override the TLS key exchange (e.g. x25519)\n"
            "  -h           this help\n",
            progname);
}

static void add_header(const char *arg)
{
    const char *colon = strchr(arg, ':');
    if (colon == NULL) {
        fprintf(stderr, "no ':' in -H value\n");
        exit(EXIT_FAILURE);
    }
    const char *value = colon + 1;
    while (*value == ' ' || *value == '\t')
        ++value;
    if (conf.num_headers >= sizeof(conf.headers) / sizeof(conf.headers[0])) {
        fprintf(stderr, "too many headers\n");
        exit(EXIT_FAILURE);
    }
    h2o_iovec_t name = h2o_strdup(NULL, arg, colon - arg);
    h2o_strtolower(name.base, name.len);
    conf.headers[conf.num_headers].name = name;
    conf.headers[conf.num_headers].value = h2o_iovec_init(value, strlen(value));
    ++conf.num_headers;
}

int main(int argc, char **argv)
{
    progname = argv[0];
    { /* --socket-per-conn opens up to 2 sockets per connection; lift the fd ceiling to the max */
        struct rlimit rl;
        if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
            rl.rlim_cur = rl.rlim_max;
            setrlimit(RLIMIT_NOFILE, &rl);
        }
    }
    SSL_load_error_strings();
    SSL_library_init();
    OpenSSL_add_all_algorithms();

    enum {
        OPT_MAX_UDP = 0x100,
        OPT_INIT_UDP,
        OPT_ACK_FREQ,
        OPT_NO_DELAYED_ACK,
        OPT_NO_ECN,
        OPT_QPACK_TABLE,
        OPT_KEY_EXCHANGE,
        OPT_RECONNECT,
        OPT_NO_RESUMPTION,
        OPT_SEND_BATCH,
        OPT_CONNECTIONS,
        OPT_SOCKET_PER_CONN,
        OPT_REQUESTS,
        OPT_DUMP_REQUESTS,
        OPT_METHOD,
    };
    static struct option longopts[] = {{"max-udp-payload-size", required_argument, NULL, OPT_MAX_UDP},
                                        {"initial-udp-payload-size", required_argument, NULL, OPT_INIT_UDP},
                                        {"ack-frequency", required_argument, NULL, OPT_ACK_FREQ},
                                        {"disallow-delayed-ack", no_argument, NULL, OPT_NO_DELAYED_ACK},
                                        {"no-ecn", no_argument, NULL, OPT_NO_ECN},
                                        {"qpack-table", required_argument, NULL, OPT_QPACK_TABLE},
                                        {"key-exchange", required_argument, NULL, OPT_KEY_EXCHANGE},
                                        {"reconnect", required_argument, NULL, OPT_RECONNECT},
                                        {"no-resumption", no_argument, NULL, OPT_NO_RESUMPTION},
                                        {"send-batch", required_argument, NULL, OPT_SEND_BATCH},
                                        {"connections", required_argument, NULL, OPT_CONNECTIONS},
                                        {"socket-per-conn", no_argument, NULL, OPT_SOCKET_PER_CONN},
                                        {"requests", required_argument, NULL, OPT_REQUESTS},
                                        {"dump-requests", no_argument, NULL, OPT_DUMP_REQUESTS},
                                        {"method", required_argument, NULL, OPT_METHOD},
                                        {"help", no_argument, NULL, 'h'},
                                        {NULL}};
    int dump_requests = 0;
    int opt;
    while ((opt = getopt_long(argc, argv, "n:m:t:d:H:x:W:kh", longopts, NULL)) != -1) {
        switch (opt) {
        case 'n':
            conf.total_requests = (unsigned)strtoul(optarg, NULL, 10);
            break;
        case 'd':
            conf.duration = atof(optarg);
            break;
        case 'm': /* concurrent streams per connection (h2load calls this -m too) */
            conf.concurrency = (unsigned)strtoul(optarg, NULL, 10);
            break;
        case 't':
            conf.threads = (unsigned)strtoul(optarg, NULL, 10);
            break;
        case OPT_METHOD:
            conf.method = optarg;
            break;
        case 'H':
            add_header(optarg);
            break;
        case 'x':
            conf.connect_to = optarg;
            break;
        case 'W':
            conf.recv_window = strtoull(optarg, NULL, 10);
            break;
        case 'k':
            conf.verify_none = 1;
            break;
        case OPT_MAX_UDP:
            conf.max_udp_payload_size = strtoull(optarg, NULL, 10);
            break;
        case OPT_INIT_UDP:
            conf.initial_udp_payload_size = (uint16_t)strtoul(optarg, NULL, 10);
            break;
        case OPT_ACK_FREQ:
            conf.ack_frequency = atof(optarg);
            conf.have_ack_frequency = 1;
            break;
        case OPT_NO_DELAYED_ACK:
            conf.disallow_delayed_ack = 1;
            break;
        case OPT_NO_ECN:
            conf.no_ecn = 1;
            break;
        case OPT_QPACK_TABLE:
            conf.qpack_table = (uint32_t)strtoul(optarg, NULL, 10);
            break;
        case OPT_KEY_EXCHANGE:
            conf.key_exchange = optarg;
            break;
        case OPT_RECONNECT:
            conf.reconnect = (unsigned)strtoul(optarg, NULL, 10);
            break;
        case OPT_NO_RESUMPTION:
            conf.no_resumption = 1;
            break;
        case OPT_SEND_BATCH:
            conf.send_batch = (unsigned)strtoul(optarg, NULL, 10);
            break;
        case OPT_CONNECTIONS:
            conf.connections = (unsigned)strtoul(optarg, NULL, 10);
            break;
        case OPT_SOCKET_PER_CONN:
            conf.socket_per_conn = 1;
            break;
        case OPT_REQUESTS:
            conf.requests_file = optarg;
            break;
        case OPT_DUMP_REQUESTS:
            dump_requests = 1;
            break;
        case 'h':
            usage();
            return 0;
        default:
            usage();
            return EXIT_FAILURE;
        }
    }
    argc -= optind;
    argv += optind;

    if (conf.requests_file != NULL)
        load_requests(conf.requests_file);
    if (dump_requests) { /* inspect what was parsed, then exit (no URL needed) */
        if (conf.requests == NULL) {
            fprintf(stderr, "--dump-requests needs --requests <file>\n");
            return EXIT_FAILURE;
        }
        for (size_t i = 0; i < conf.num_requests; ++i) {
            struct request *r = &conf.requests[i];
            printf("[%zu] %.*s %.*s\n", i, (int)r->method.len, r->method.base, (int)r->path.len, r->path.base);
            for (size_t j = 0; j < r->num_headers; ++j)
                printf("      %.*s: %.*s\n", (int)r->headers[j].name.len, r->headers[j].name.base,
                       (int)r->headers[j].value.len, r->headers[j].value.base);
            if (r->body.base != NULL)
                printf("      body: %zu bytes\n", r->body.len);
        }
        return 0;
    }

    if (argc < 1) {
        usage();
        return EXIT_FAILURE;
    }
    conf.url = argv[0];

    if (conf.concurrency < 1)
        conf.concurrency = 1;
    { /* --send-batch <N> (or H3X_SEND_BATCH) holds freed slots until N accumulate, then starts them
         together so quicly packs the requests into fewer datagrams (send-side coalescing); 1 = off */
        const char *e;
        if (conf.send_batch == 0 && (e = getenv("H3X_SEND_BATCH")) != NULL)
            conf.send_batch = (unsigned)strtoul(e, NULL, 10);
        if (conf.send_batch < 1)
            conf.send_batch = 1;
        if (conf.send_batch > conf.concurrency)
            conf.send_batch = conf.concurrency;
    }
    if (conf.threads < 1)
        conf.threads = detect_ncpu(); /* default: use every CPU the container gives us */
    if (conf.total_requests < 1)
        conf.total_requests = 1;
    if (conf.duration == 0 && conf.threads > conf.total_requests)
        conf.threads = conf.total_requests;
    /* connections: total across all threads, default one per thread. Fewer connections than threads
     * would leave threads idle, so trim the thread count to match. */
    if (conf.connections == 0)
        conf.connections = conf.threads;
    if (conf.connections < conf.threads)
        conf.threads = conf.connections;
    /* reconnect (0-RTT churn) tracks a single connection per worker, so it needs 1 conn per thread */
    if (conf.reconnect != 0 && conf.connections > conf.threads) {
        fprintf(stderr, "--reconnect needs one connection per thread; use --connections %u or fewer\n", conf.threads);
        return EXIT_FAILURE;
    }
    if (conf.key_exchange != NULL) {
        ptls_key_exchange_algorithm_t **named;
        for (named = ptls_openssl_key_exchanges_all; *named != NULL; ++named)
            if (strcasecmp((*named)->name, conf.key_exchange) == 0)
                break;
        if (*named == NULL) {
            fprintf(stderr, "unknown key exchange: %s\n", conf.key_exchange);
            return EXIT_FAILURE;
        }
        h3_key_exchanges[0] = *named;
        h3_key_exchanges[1] = NULL;
    } else {
        size_t i = 0;
#if PTLS_OPENSSL_HAVE_X25519
        h3_key_exchanges[i++] = &ptls_openssl_x25519;
#endif
        h3_key_exchanges[i++] = &ptls_openssl_secp256r1;
    }

    struct worker *workers = h2o_mem_alloc(sizeof(*workers) * conf.threads);
    memset(workers, 0, sizeof(*workers) * conf.threads);
    unsigned base = conf.total_requests / conf.threads, rem = conf.total_requests % conf.threads;
    unsigned conns_base = conf.connections / conf.threads, conns_rem = conf.connections % conf.threads;

    char budget[32];
    if (conf.duration != 0)
        snprintf(budget, sizeof budget, "%gs", conf.duration);
    else
        snprintf(budget, sizeof budget, "%u requests", conf.total_requests);
    fprintf(stderr, "h3x -> %s  (%u threads, %u conns x %u streams, %s%s)\n", conf.url, conf.threads,
            conf.connections, conf.concurrency, budget, conf.reconnect != 0 ? ", reconnect mode" : "");

    struct timeval t0, t1;
    gettimeofday(&t0, NULL);
    if (conf.duration != 0) {
        uint64_t deadline_us = (uint64_t)t0.tv_sec * 1000000 + t0.tv_usec + (uint64_t)(conf.duration * 1e6);
        g_deadline.tv_sec = deadline_us / 1000000;
        g_deadline.tv_usec = deadline_us % 1000000;
    }
    for (unsigned i = 0; i < conf.threads; ++i) {
        workers[i].idx = i;
        workers[i].req_target = base + (i < rem ? 1 : 0);
        workers[i].n_conns = conns_base + (i < conns_rem ? 1 : 0);
        if (pthread_create(&workers[i].tid, NULL, worker_main, &workers[i]) != 0) {
            perror("pthread_create");
            return EXIT_FAILURE;
        }
    }
    for (unsigned i = 0; i < conf.threads; ++i)
        pthread_join(workers[i].tid, NULL);
    gettimeofday(&t1, NULL);

    double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_usec - t0.tv_usec) / 1e6;
    unsigned n_fail = print_summary(workers, elapsed);
    free(workers);
    return n_fail != 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
