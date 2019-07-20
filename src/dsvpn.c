#include "dsvpn.h"
#include "charm.h"
#include "os.h"

static const int POLLFD_TUN = 0, POLLFD_LISTENER = 1, POLLFD_CLIENT = 2, POLLFD_COUNT = 3;

typedef struct Context_ {
    const char *  wanted_name;
    const char *  local_tun_ip;
    const char *  remote_tun_ip;
    const char *  local_tun_ip6;
    const char *  remote_tun_ip6;
    const char *  server_ip;
    const char *  server_port;
    const char *  ext_if_name;
    const char *  wanted_ext_gw_ip;
    char          ext_gw_ip[64];
    char          if_name[IFNAMSIZ];
    int           is_server;
    int           tun_fd;
    int           client_fd;
    int           listen_fd;
    int           congestion;
    int           firewall_rules_set;
    struct pollfd fds[3];
    uint32_t      uc_kx_st[12];
    uint32_t      uc_st[2][12];
} Context;

volatile sig_atomic_t exit_signal_received;

static void signal_handler(int sig)
{
    signal(sig, SIG_DFL);
    exit_signal_received = 1;
}

static int firewall_rules(Context *context, int set)
{
    const char *       substs[][2] = { { "$LOCAL_TUN_IP6", context->local_tun_ip6 },
                                { "$REMOTE_TUN_IP6", context->remote_tun_ip6 },
                                { "$LOCAL_TUN_IP", context->local_tun_ip },
                                { "$REMOTE_TUN_IP", context->remote_tun_ip },
                                { "$EXT_IP", context->server_ip },
                                { "$EXT_PORT", context->server_port },
                                { "$EXT_IF_NAME", context->ext_if_name },
                                { "$EXT_GW_IP", context->ext_gw_ip },
                                { "$IF_NAME", context->if_name },
                                { NULL, NULL } };
    const char *const *cmds;
    size_t             i;

    if (context->firewall_rules_set == set) {
        return 0;
    }
    if ((cmds = (set ? firewall_rules_cmds(context->is_server).set
                     : firewall_rules_cmds(context->is_server).unset)) == NULL) {
        fprintf(stderr,
                "Routing commands for that operating system have not been "
                "added yet.\n");
        return 0;
    }
    for (i = 0; cmds[i] != NULL; i++) {
        if (shell_cmd(substs, cmds[i]) != 0) {
            fprintf(stderr, "Unable to run [%s]: [%s]\n", cmds[i], strerror(errno));
            return -1;
        }
    }
    context->firewall_rules_set = set;
    return 0;
}

static int tcp_client(const char *address, const char *port)
{
    struct addrinfo hints, *res;
    int             eai;
    int             client_fd;
    int             err;

    memset(&hints, 0, sizeof hints);
    hints.ai_flags    = 0;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_addr     = NULL;
#ifdef __OpenBSD__
    if (address == NULL) {
        hints.ai_family = AF_INET;
    }
#endif
    printf("Connecting to %s:%s...\n", address, port);
    if ((eai = getaddrinfo(address, port, &hints, &res)) != 0 ||
        (res->ai_family != AF_INET && res->ai_family != AF_INET6)) {
        fprintf(stderr, "Unable to create the client socket: [%s]\n", gai_strerror(eai));
        errno = EINVAL;
        return -1;
    }
    if ((client_fd = socket(res->ai_family, SOCK_STREAM, IPPROTO_TCP)) == -1) {
        freeaddrinfo(res);
        return -1;
    }
    if (connect(client_fd, (const struct sockaddr *) res->ai_addr, res->ai_addrlen) != 0) {
        freeaddrinfo(res);
        err = errno;
        (void) close(client_fd);
        errno = err;
        return -1;
    }
    freeaddrinfo(res);
    if (tcp_opts(client_fd) != 0) {
        err = errno;
        (void) close(client_fd);
        errno = err;
        return -1;
    }
    return client_fd;
}

static int tcp_listener(const char *address, const char *port)
{
    struct addrinfo hints, *res;
    int             on;
    int             listen_fd;
    int             backlog = 1;

    memset(&hints, 0, sizeof hints);
    hints.ai_flags    = AI_PASSIVE;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_addr     = NULL;
#ifdef __OpenBSD__
    if (address == NULL) {
        hints.ai_family = AF_INET;
    }
#endif
    if ((on = getaddrinfo(address, port, &hints, &res)) != 0 ||
        (res->ai_family != AF_INET && res->ai_family != AF_INET6)) {
        fprintf(stderr, "Unable to create the listening socket: [%s]\n", gai_strerror(on));
        errno = EINVAL;
        return -1;
    }
    on = 1;
    if ((listen_fd = socket(res->ai_family, SOCK_STREAM, IPPROTO_TCP)) == -1 ||
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, (char *) &on, sizeof on) != 0) {
        freeaddrinfo(res);
        return -1;
    }
#if defined(IPPROTO_IPV6) && defined(IPV6_V6ONLY)
    on = 0;
    (void) setsockopt(listen_fd, IPPROTO_IPV6, IPV6_V6ONLY, (char *) &on, sizeof on);
#endif
    printf("Listening to %s:%s\n", address == NULL ? "*" : address, port);
    if (bind(listen_fd, (struct sockaddr *) res->ai_addr, (socklen_t) res->ai_addrlen) != 0 ||
        listen(listen_fd, backlog) != 0) {
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);

    return listen_fd;
}

static void client_disconnect(Context *context)
{
    if (context->client_fd == -1) {
        return;
    }
    (void) close(context->client_fd);
    context->client_fd          = -1;
    context->fds[POLLFD_CLIENT] = (struct pollfd){ .fd = -1, .events = 0 };
    memset(context->uc_st, 0, sizeof context->uc_st);
}

static int server_key_exchange(Context *context, const int client_fd)
{
    uint32_t st[12];
    uint8_t  pkt1[32 + 8 + 32], pkt2[32 + 32];
    uint8_t  h[32];
    uint8_t  k[32];
    uint8_t  iv[16] = { 0 };
    uint64_t ts, now;

    memcpy(st, context->uc_kx_st, sizeof st);
    errno = EACCES;
    if (safe_read(client_fd, pkt1, sizeof pkt1, TIMEOUT) != sizeof pkt1) {
        return -1;
    }
    uc_hash(st, h, pkt1, 32 + 8);
    if (memcmp(h, pkt1 + 32 + 8, 32) != 0) {
        return -1;
    }
    memcpy(&ts, pkt1 + 32, 8);
    ts  = endian_swap64(ts);
    now = time(NULL);
    if ((ts > now && ts - now > TS_TOLERANCE) || (now > ts && now - ts > TS_TOLERANCE)) {
        fprintf(stderr,
                "Clock difference is too large: %" PRIu64 " (client) vs %" PRIu64 " (server)\n", ts,
                now);
        return -1;
    }
    uc_randombytes_buf(pkt2, 32);
    uc_hash(st, pkt2 + 32, pkt2, 32);
    if (safe_write(client_fd, pkt2, sizeof pkt2, TIMEOUT) != sizeof pkt2) {
        return -1;
    }
    uc_hash(st, k, NULL, 0);
    iv[0] = context->is_server;
    uc_state_init(context->uc_st[0], k, iv);
    iv[0] ^= 1;
    uc_state_init(context->uc_st[1], k, iv);

    return 0;
}

static int tcp_accept(Context *context, int listen_fd)
{
    struct sockaddr_storage client_sa;
    socklen_t               client_sa_len = sizeof client_sa;
    int                     client_fd;
    int                     err;

    if ((client_fd = accept(listen_fd, (struct sockaddr *) &client_sa, &client_sa_len)) < 0) {
        return -1;
    }
    if (client_sa_len <= (socklen_t) 0U) {
        (void) close(client_fd);
        errno = EINTR;
        return -1;
    }
    if (tcp_opts(client_fd) != 0) {
        err = errno;
        (void) close(client_fd);
        errno = err;
        return -1;
    }
    context->congestion = 0;
    if (server_key_exchange(context, client_fd) != 0) {
        fprintf(stderr, "Authentication failed\n");
        (void) close(client_fd);
        errno = EACCES;
        return -1;
    }
    return client_fd;
}

static int client_key_exchange(Context *context)
{
    uint32_t st[12];
    uint8_t  pkt1[32 + 8 + 32], pkt2[32 + 32];
    uint8_t  h[32];
    uint8_t  k[32];
    uint8_t  iv[16] = { 0 };
    uint64_t now;

    memcpy(st, context->uc_kx_st, sizeof st);
    uc_randombytes_buf(pkt1, 32);
    now = endian_swap64(time(NULL));
    memcpy(pkt1 + 32, &now, 8);
    uc_hash(st, pkt1 + 32 + 8, pkt1, 32 + 8);
    if (safe_write(context->client_fd, pkt1, sizeof pkt1, TIMEOUT) != sizeof pkt1) {
        return -1;
    }
    errno = EACCES;
    if (safe_read(context->client_fd, pkt2, sizeof pkt2, TIMEOUT) != sizeof pkt2) {
        return -1;
    }
    uc_hash(st, h, pkt2, 32);
    if (memcmp(h, pkt2 + 32, 32) != 0) {
        return -1;
    }
    uc_hash(st, k, NULL, 0);
    iv[0] = context->is_server;
    uc_state_init(context->uc_st[0], k, iv);
    iv[0] ^= 1;
    uc_state_init(context->uc_st[1], k, iv);

    return 0;
}

static int client_connect(Context *context)
{
    const char *ext_gw_ip;

    if (context->wanted_ext_gw_ip == NULL && (ext_gw_ip = get_default_gw_ip()) != NULL &&
        strcmp(ext_gw_ip, context->ext_gw_ip) != 0) {
        printf("Gateway changed from [%s] to [%s]\n", context->ext_gw_ip, ext_gw_ip);
        firewall_rules(context, 0);
        snprintf(context->ext_gw_ip, sizeof context->ext_gw_ip, "%s", ext_gw_ip);
        firewall_rules(context, 1);
    }
    memset(context->uc_st, 0, sizeof context->uc_st);
    context->uc_st[context->is_server][0] ^= 1;
    context->client_fd = tcp_client(context->server_ip, context->server_port);
    if (context->client_fd == -1) {
        perror("TCP client");
        return -1;
    }
#if BUFFERBLOAT_CONTROL
    fcntl(context->client_fd, F_SETFL, fcntl(context->client_fd, F_GETFL, 0) | O_NONBLOCK);
#endif
    context->congestion = 0;
    if (client_key_exchange(context) != 0) {
        fprintf(stderr, "Authentication failed\n");
        client_disconnect(context);
        return -1;
    }
    firewall_rules(context, 1);
    context->fds[POLLFD_CLIENT] =
        (struct pollfd){ .fd = context->client_fd, .events = POLLIN, .revents = 0 };
    puts("Connected");

    return 0;
}

static int client_reconnect(Context *context)
{
    unsigned int i;

    client_disconnect(context);
    if (context->is_server) {
        return 0;
    }
    for (i = 0; exit_signal_received == 0 && i < RECONNECT_ATTEMPTS; i++) {
        puts("Trying to reconnect");
        sleep(i > 3 ? 3 : i);
        if (client_connect(context) == 0) {
            return 0;
        }
    }
    return -1;
}

static int event_loop(Context *context)
{
    struct __attribute__((aligned(16))) {
#if TAG_LEN < 16 - 2
        unsigned char _pad[16 - TAG_LEN - 2];
#endif
        unsigned char len[2];
        unsigned char tag[TAG_LEN];
        unsigned char data[MAX_PACKET_LEN];
    } buf;
    struct pollfd *const fds = context->fds;
    ssize_t              len;
    int                  found_fds;
    int                  new_client_fd;

    if (exit_signal_received != 0) {
        return -2;
    }
    if ((found_fds = poll(fds, POLLFD_COUNT, 1500)) == -1) {
        return -1;
    }
    if (fds[POLLFD_LISTENER].revents & POLLIN) {
        puts("Accepting new client");
        new_client_fd = tcp_accept(context, context->listen_fd);
        if (new_client_fd == -1) {
            perror("tcp_accept");
            return 0;
        }
        if (context->client_fd != -1) {
            (void) close(context->client_fd);
        }
        context->client_fd = new_client_fd;
        puts("Accepted");
        fds[POLLFD_CLIENT] = (struct pollfd){ .fd = context->client_fd, .events = POLLIN };
    }
    if ((fds[POLLFD_TUN].revents & POLLERR) || (fds[POLLFD_TUN].revents & POLLHUP)) {
        puts("HUP (tun)");
        return -1;
    }
    if (fds[POLLFD_TUN].revents & POLLIN) {
        len = tun_read(context->tun_fd, buf.data, sizeof buf.data);
        if (len <= 0) {
            perror("tun_read");
            return -1;
        }
        if (context->congestion) {
            context->congestion = 0;
            return 0;
        }
        if (context->client_fd != -1) {
            unsigned char tag_full[16];
            ssize_t       writenb;
            uint16_t      binlen = endian_swap16((uint16_t) len);

            memcpy(buf.len, &binlen, 2);
            uc_encrypt(context->uc_st[0], buf.data, len, tag_full);
            memcpy(buf.tag, tag_full, TAG_LEN);
            writenb = safe_write_partial(context->client_fd, buf.len, 2U + TAG_LEN + len);
            if (writenb != (ssize_t)(2U + TAG_LEN + len)) {
                if (errno == EAGAIN) {
                    context->congestion = 1;
                    writenb = safe_write(context->client_fd, buf.len, 2U + TAG_LEN + len, TIMEOUT);
                }
            }
            if (writenb != (ssize_t)(2U + TAG_LEN + len)) {
                perror("safe_write (client)");
                return client_reconnect(context);
            }
        }
    }
    if ((fds[POLLFD_CLIENT].revents & POLLERR) || (fds[POLLFD_CLIENT].revents & POLLHUP)) {
        puts("HUP (client)");
        return client_reconnect(context);
    }
    if (fds[POLLFD_CLIENT].revents & POLLIN) {
        uint16_t binlen;

        if (safe_read(context->client_fd, &binlen, sizeof binlen, TIMEOUT) != sizeof binlen) {
            len = -1;
        } else {
            len = (ssize_t) endian_swap16(binlen);
            if ((size_t) len > sizeof buf.data) {
                len = -1;
            } else {
                len = safe_read(context->client_fd, buf.tag, TAG_LEN + (size_t) binlen, TIMEOUT);
            }
        }
        if (len < TAG_LEN) {
            puts("Client disconnected");
            return client_reconnect(context);
        } else {
            len -= TAG_LEN;
            if (uc_decrypt(context->uc_st[1], buf.data, len, buf.tag, TAG_LEN) != 0) {
                fprintf(stderr, "Corrupted stream\n");
                return client_reconnect(context);
            }
            if (tun_write(context->tun_fd, buf.data, len) != len) {
                perror("tun_write");
            }
        }
    }
    return 0;
}

static int doit(Context *context)
{
    context->client_fd = context->listen_fd = -1;
    memset(context->fds, 0, sizeof *context->fds);
    context->fds[POLLFD_TUN] =
        (struct pollfd){ .fd = context->tun_fd, .events = POLLIN, .revents = 0 };
    if (context->is_server) {
        if ((context->listen_fd = tcp_listener(context->server_ip, context->server_port)) == -1) {
            perror("tcp_listener");
            return -1;
        }
        context->fds[POLLFD_LISTENER] = (struct pollfd){
            .fd     = context->listen_fd,
            .events = POLLIN,
        };
    }
    if (!context->is_server && client_reconnect(context) != 0) {
        fprintf(stderr, "Unable to connect to server: [%s]\n", strerror(errno));
        return -1;
    }
    while (event_loop(context) == 0)
        ;
    return 0;
}

static int load_key_file(Context *context, const char *file)
{
    unsigned char key[32];
    int           fd;

    if ((fd = open(file, O_RDONLY)) == -1) {
        return -1;
    }
    if (safe_read(fd, key, sizeof key, TIMEOUT) != sizeof key) {
        return -1;
        memset(key, 0, sizeof key);
    }
    uc_state_init(context->uc_kx_st, key, (const unsigned char *) "VPN Key Exchange");
    uc_memzero(key, sizeof key);

    return close(fd);
}

static void usage(void)
{
    puts(
        "Usage:\n"
        "\n"
        "dsvpn\t\"server\"\n\t<key file>\n\t<vpn server ip>|\"auto\"\n\t<vpn "
        "server port>|\"auto\"\n\t<tun interface>|\"auto\"\n\t<local tun "
        "ip>|\"auto\"\n\t<remote tun ip>\"auto\"\n\t<external ip>|\"auto\""
        "\n\n"
        "dsvpn\t\"client\"\n\t<key file>\n\t<vpn server ip>\n\t<vpn server "
        "port>|\"auto\"\n\t<tun interface>|\"auto\"\n\t<local tun "
        "ip>|\"auto\"\n\t<remote tun ip>|\"auto\"\n\t<gateway ip>\"auto\"\n");
    exit(254);
}

static void get_tun6_addresses(Context *context)
{
    static char local_tun_ip6[40], remote_tun_ip6[40];

    snprintf(local_tun_ip6, sizeof local_tun_ip6, "64:ff9b::%s", context->local_tun_ip);
    snprintf(remote_tun_ip6, sizeof remote_tun_ip6, "64:ff9b::%s", context->remote_tun_ip);
    context->local_tun_ip6  = local_tun_ip6;
    context->remote_tun_ip6 = remote_tun_ip6;
}

int main(int argc, char *argv[])
{
    Context     context;
    const char *ext_gw_ip;

    (void) safe_read_partial;

    if (argc < 3) {
        usage();
    }
    memset(&context, 0, sizeof context);
    context.is_server = strcmp(argv[1], "server") == 0;
    if (load_key_file(&context, argv[2]) != 0) {
        fprintf(stderr, "Unable to load the key file [%s]\n", argv[2]);
        return 1;
    }
    context.server_ip = (argc <= 3 || strcmp(argv[3], "auto") == 0) ? NULL : argv[3];
    if (context.server_ip == NULL && !context.is_server) {
        usage();
    }
    context.server_port  = (argc <= 4 || strcmp(argv[4], "auto") == 0) ? DEFAULT_PORT : argv[4];
    context.wanted_name  = (argc <= 5 || strcmp(argv[5], "auto") == 0) ? NULL : argv[5];
    context.local_tun_ip = (argc <= 6 || strcmp(argv[6], "auto") == 0)
                               ? (context.is_server ? DEFAULT_SERVER_IP : DEFAULT_CLIENT_IP)
                               : argv[6];
    context.remote_tun_ip = (argc <= 7 || strcmp(argv[7], "auto") == 0)
                                ? (context.is_server ? DEFAULT_CLIENT_IP : DEFAULT_SERVER_IP)
                                : argv[7];
    context.wanted_ext_gw_ip = (argc <= 8 || strcmp(argv[8], "auto") == 0) ? NULL : argv[8];
    ext_gw_ip = context.wanted_ext_gw_ip ? context.wanted_ext_gw_ip : get_default_gw_ip();
    snprintf(context.ext_gw_ip, sizeof context.ext_gw_ip, "%s", ext_gw_ip == NULL ? "" : ext_gw_ip);
    if (ext_gw_ip == NULL && !context.is_server) {
        fprintf(stderr, "Unable to automatically determine the gateway IP\n");
        return 1;
    }
    if ((context.ext_if_name = get_default_ext_if_name()) == NULL && context.is_server) {
        fprintf(stderr, "Unable to automatically determine the external interface\n");
        return 1;
    }
    get_tun6_addresses(&context);
    context.tun_fd = tun_create(context.if_name, context.wanted_name);
    if (context.tun_fd == -1) {
        perror("tun_create");
        return 1;
    }
    printf("Interface: [%s]\n", context.if_name);
    if (tun_set_mtu(context.if_name, DEFAULT_MTU) != 0) {
        perror("mtu");
    }
    if (context.is_server && firewall_rules(&context, 1) != 0) {
        return -1;
    }
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    if (doit(&context) != 0) {
        return -1;
    }
    firewall_rules(&context, 0);

    return 0;
}
