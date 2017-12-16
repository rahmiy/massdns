#define _GNU_SOURCE

#ifdef DEBUG
#include <sys/resource.h>
#endif

#include "massdns.h"
#include "string.h"
#include "random.h"
#include "net.h"
#include "cmd.h"
#include "dns.h"
#include "list.h"
#include "flow.h"
#include <unistd.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <stddef.h>
#include <sys/sysinfo.h>
#include <limits.h>

#ifdef PCAP_SUPPORT
#include <net/ethernet.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>
#include <netinet/udp.h>
#include <net/if.h>
#endif

void print_help()
{
    fprintf(stderr, ""
                    "Usage: %s [options] [domainlist]\n"
                    "  -b  --bindto           Bind to IP address and port. (Default: 0.0.0.0:0)\n"
                    "  -c  --resolve-count    Number of resolves for a name before giving up. (Default: 50)\n"
                    "      --drop-user        User to drop privileges to when running as root. (Default: nobody)\n"
                    "      --finalstats       Write final stats to STDERR when done.\n"
                    "      --flush            Flush the output file whenever a response was received.\n"
                    "  -h  --help             Show this help.\n"
                    "  -i  --interval         Interval in milliseconds to wait between multiple resolves of the same\n"
                    "                         domain. (Default: 500)\n"
                    "  -l  --error-log        Error log file path. (Default: /dev/stderr)\n"
                    "  -n  --norecurse        Use non-recursive queries. Useful for DNS cache snooping.\n"
                    "  -o  --output           Flags for output formatting.\n"
                    "      --predictable      Use resolvers incrementally. Useful for resolver tests.\n"
                    "      --processes        Number of processes to be used for resolving. (Default: 1)\n"
                    "  -q  --quiet            Quiet mode.\n"
                    "      --rcvbuf           Size of the receive buffer in bytes.\n"
                    "      --retry            Unacceptable DNS response codes. (Default: REFUSED)\n"
                    "  -r  --resolvers        Text file containing DNS resolvers.\n"
                    "      --root             Do not drop privileges when running as root. Not recommended.\n"
                    "  -s  --hashmap-size     Number of concurrent lookups. (Default: 100000)\n"
                    "      --sndbuf           Size of the send buffer in bytes.\n"
                    "      --sticky           Do not switch the resolver when retrying.\n"
                    "  -t  --type             Record type to be resolved. (Default: A)\n"
#ifdef PCAP_SUPPORT
                    "      --use-pcap         Enable pcap usage.\n"
#endif
                    "  -w  --outfile          Write to the specified output file instead of standard output.\n"
                    "  -x  --extreme          Value between 0 and 2 specifying transmission aggression. (Default: 0)\n"
                    "\n"
                    "Output flags:\n"
                    "  S - simple text output\n"
                    "  F - full text output\n"
                    "  B - binary output\n",
            context.cmd_args.argv[0] ? context.cmd_args.argv[0] : "massdns"
    );
}

void cleanup()
{
#ifdef PCAP_SUPPORT
    if(context.pcap != NULL)
    {
        pcap_close(context.pcap);
    }
#endif
    if(context.map)
    {
        hashmapFree(context.map);
    }
    timed_ring_destroy(&context.ring);

    free(context.resolvers.data);

    free(context.sockets.interfaces4.data);
    free(context.sockets.interfaces6.data);

    urandom_close();

    if(context.domainfile)
    {
        fclose(context.domainfile);
    }
    if(context.outfile)
    {
        fclose(context.outfile);
    }

    free(context.stat_messages);

    free(context.sockets.pipes);

    free(context.sockets.master_pipes_read);

    free(context.lookup_pool.data);
    free(context.lookup_space);

    for (size_t i = 0; i < context.cmd_args.num_processes * 2; i++)
    {
        if(context.sockets.pipes && context.sockets.pipes[i] >= 0)
        {
            close(context.sockets.pipes[i]);
        }
    }
}

void clean_exit(int status)
{
    cleanup();
    exit(status);
}


buffer_t massdns_resolvers_from_file(char *filename)
{
    char line[4096];
    FILE *f = fopen(filename, "r");
    if (f == NULL)
    {
        perror("Failed to open resolver file");
        clean_exit(EXIT_FAILURE);
    }
    single_list_t *list = single_list_new();
    while (!feof(f))
    {
        if (fgets(line, sizeof(line), f))
        {
            trim_end(line);
            resolver_t *resolver = safe_calloc(sizeof(*resolver));
            struct sockaddr_storage *addr = &resolver->address;
            if (str_to_addr(line, 53, addr))
            {
                if((addr->ss_family == AF_INET && context.sockets.interfaces4.len > 0)
                    || (addr->ss_family == AF_INET6 && context.sockets.interfaces6.len > 0))
                {
                    single_list_push_back(list, resolver);
                }
                else
                {
                    fprintf(stderr, "No query socket for resolver \"%s\" found.\n", line);
                }
            }
            else
            {
                fprintf(stderr, "\"%s\" is not a valid resolver. Skipped.\n", line);
            }
        }
    }
    fclose(f);
    buffer_t resolvers = single_list_to_array_copy(list, sizeof(resolver_t));
    if(single_list_count(list) == 0)
    {
        fprintf(stderr, "No usable resolvers were found. Terminating.\n");
        clean_exit(1);
    }
    single_list_free_with_elements(list);
    return resolvers;
}

void set_sndbuf(int fd)
{
    if(context.cmd_args.sndbuf
        && setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &context.cmd_args.sndbuf, sizeof(context.cmd_args.sndbuf)) == 0)
    {
        fprintf(stderr, "Failed to adjust send buffer size: %s\n", strerror(errno));
    }
}

void set_rcvbuf(int fd)
{
    if(context.cmd_args.rcvbuf
        && setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &context.cmd_args.rcvbuf, sizeof(context.cmd_args.rcvbuf)) == 0)
    {
        fprintf(stderr, "Failed to adjust receive buffer size: %s\n", strerror(errno));
    }
}

void set_default_socket(int version)
{
    socket_info_t info;

    info.descriptor = socket(version == 4 ? PF_INET : PF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    info.protocol = version == 4 ? PROTO_IPV4 : PROTO_IPV6;
    info.type = SOCKET_TYPE_QUERY;
    if(info.descriptor >= 0)
    {
        buffer_t *buffer = version == 4 ? &context.sockets.interfaces4 : &context.sockets.interfaces6;
        buffer->len = 1;
        buffer->data = flatcopy(&info, sizeof(info));
        set_rcvbuf(info.descriptor);
        set_sndbuf(info.descriptor);
    }
    else
    {
        fprintf(stderr, "Failed to create IPv%d socket: %s\n", version, strerror(errno));
    }
}

void set_user_sockets(single_list_t *bind_addrs, buffer_t *buffer)
{
    single_list_t sockets;
    single_list_init(&sockets);
    single_list_ref_foreach_free(bind_addrs, element)
    {
        struct sockaddr_storage* addr = element->data;
        socket_info_t info;
        info.descriptor = socket(addr->ss_family, SOCK_DGRAM, IPPROTO_UDP);
        info.protocol = addr->ss_family == AF_INET ? PROTO_IPV4 : PROTO_IPV6;
        info.type = SOCKET_TYPE_QUERY;
        if(info.descriptor >= 0)
        {
            if(bind(info.descriptor, (struct sockaddr*)addr, sizeof(*addr)) != 0)
            {
                fprintf(stderr, "Not adding socket due to bind failure: %s\n", strerror(errno));
            }
            else
            {
                set_rcvbuf(info.descriptor);
                set_sndbuf(info.descriptor);
                single_list_push_back(&sockets, flatcopy(&info, sizeof(info)));
            }
        }
        else
        {
            fprintf(stderr, "Failed to create IPv%d socket: %s\n", info.protocol, strerror(errno));
        }
        free(element->data);
    }
    single_list_init(bind_addrs);
    *buffer = single_list_to_array_copy(&sockets, sizeof(socket_info_t));
    single_list_clear(&sockets);
}

void query_sockets_setup()
{
    if(single_list_count(&context.cmd_args.bind_addrs4) == 0 && single_list_count(&context.cmd_args.bind_addrs6) == 0)
    {
        set_default_socket(4);
        set_default_socket(6);
    }
    else
    {
        set_user_sockets(&context.cmd_args.bind_addrs4, &context.sockets.interfaces4);
        set_user_sockets(&context.cmd_args.bind_addrs6, &context.sockets.interfaces6);
    }
}

bool next_query(char **qname)
{
    static char line[512];

    while (fgets(line, sizeof(line), context.domainfile))
    {
        trim_end(line);
        if (strcmp(line, "") == 0)
        {
            continue;
        }
        *qname = line;

        return true;
    }
    return false;
}


// This is the djb2 hashing method treating the DNS type as two extra characters
int hash_lookup_key(void *key)
{
    unsigned long hash = 5381;
    uint8_t *entry = ((lookup_key_t *)key)->name.name;
    int c;
    while ((c = *entry++) != 0)
    {
        hash = ((hash << 5) + hash) + tolower(c); /* hash * 33 + c */
    }
    hash = ((hash << 5) + hash) + ((((lookup_key_t *)key)->type & 0xFF00) >> 8);
    hash = ((hash << 5) + hash) + (((lookup_key_t *)key)->type & 0x00FF);
    hash = ((hash << 5) + hash) + ((lookup_key_t *)key)->name.length;
    return (int)hash;
}


// Converts a DNS name to the heap and makes sure it is a FQDN (appends a trailing dot)
// The result needs to be freed
char *canonicalized_name_copy(const char *qname)
{
    size_t len = strlen(qname);
    bool canonical = len > 0 && qname[len - 1] == '.';
    if(canonical)
    {
        return strmcpy(qname);
    }
    else
    {
        char *result = safe_malloc(len + 2);
        memcpy(result, qname, len);
        result[len] = '.';
        result[len + 1] = 0;
        return result;
    }
}

void end_warmup()
{
    context.state = STATE_QUERYING;
    if(context.cmd_args.extreme <= 1)
    {
        // Reduce our CPU load from epoll interrupts by removing the EPOLLOUT event
#ifdef PCAP_SUPPORT
        if(!context.pcap)
#endif
        {
            add_sockets(context.epollfd, EPOLLIN, EPOLL_CTL_MOD, &context.sockets.interfaces4);
            add_sockets(context.epollfd, EPOLLIN, EPOLL_CTL_MOD, &context.sockets.interfaces6);
        }
    }
}

lookup_t *new_lookup(const char *qname, dns_record_type type, bool *new)
{
    //lookup_key_t *key = safe_malloc(sizeof(*key));
    if(context.lookup_pool.len == 0)
    {
        fprintf(stderr, "Empty lookup pool.\n");
        clean_exit(EXIT_FAILURE);
    }
    lookup_entry_t *entry = ((lookup_entry_t**)context.lookup_pool.data)[--context.lookup_pool.len];
    lookup_key_t *key = &entry->key;
    lookup_t *value = &entry->value;
    bzero(value, sizeof(*value));


    key->name.length = (uint8_t)string_copy((char*)key->name.name, qname, sizeof(key->name.name));
    if(key->name.name[key->name.length - 1] != '.')
    {
        key->name.name[key->name.length] = '.';
        key->name.name[++key->name.length] = 0;
    }

    key->type = type;

    //lookup_t *value = safe_calloc(sizeof(*value));
    value->ring_entry = timed_ring_add(&context.ring, context.cmd_args.interval_ms * TIMED_RING_MS, value);
    urandom_get(&value->transaction, sizeof(value->transaction));
    value->key = key;

    errno = 0;
    *new = (hashmapPut(context.map, key, value) == NULL);
    if(errno != 0)
    {
        perror("Error");
        abort();
    }

    context.lookup_index++;
    context.stats.timeouts[0]++;
    if(context.lookup_index >= context.cmd_args.hashmap_size)
    {
        end_warmup();
    }

    return value;
}

void send_query(lookup_t *lookup)
{
    static uint8_t query_buffer[0x200];

    // Choose random resolver
    // Pool of resolvers cannot be empty due to check after parsing resolvers.
    if(!context.cmd_args.sticky || lookup->resolver == NULL)
    {
        if(context.cmd_args.predictable_resolver)
        {
            lookup->resolver = ((resolver_t *) context.resolvers.data) + context.lookup_index % context.resolvers.len;
        }
        else
        {
            lookup->resolver = ((resolver_t *) context.resolvers.data) + urandom_size_t() % context.resolvers.len;
        }
    }

    // We need to select the correct socket pool: IPv4 socket pool for IPv4 resolver/IPv6 socket pool for IPv6 resolver
    buffer_t *interfaces;
    if(lookup->resolver->address.ss_family == AF_INET)
    {
        interfaces = &context.sockets.interfaces4;
    }
    else
    {
        interfaces = &context.sockets.interfaces6;
    }

    // Pick a random socket from that pool
    // Pool of sockets cannot be empty due to check when parsing resolvers. Socket creation must have succeeded.
    size_t socket_index = urandom_size_t() % interfaces->len;
    int socket_descriptor = ((socket_info_t*)interfaces->data)[socket_index].descriptor;

    ssize_t result = dns_question_create(query_buffer, (char*)lookup->key->name.name, lookup->key->type,
                                                   lookup->transaction);
    if (result < DNS_PACKET_MINIMUM_SIZE)
    {
        fprintf(stderr, "Failed to create DNS question for query \"%s\".", lookup->key->name.name);
        return;
    }

    // Set or unset the QD bit based on user preference
    dns_buf_set_rd(query_buffer, !context.cmd_args.norecurse);

    ssize_t sent = sendto(socket_descriptor, query_buffer, (size_t) result, 0,
                          (struct sockaddr *) &lookup->resolver->address,
                          sizeof(lookup->resolver->address));
    if(sent != result)
    {
        fprintf(stderr, "Error sending: %s\n", strerror(errno));
    }
}

#define STAT_IDX_OK 0
#define STAT_IDX_NXDOMAIN 1
#define STAT_IDX_SERVFAIL 2
#define STAT_IDX_REFUSED 3
#define STAT_IDX_FORMERR 4

void my_stats_to_msg(stats_exchange_t *stats_msg)
{
    stats_msg->finished = context.stats.finished;
    stats_msg->finished_success = context.stats.finished_success;
    stats_msg->fork_index = context.fork_index;
    stats_msg->mismatch_domain = context.stats.mismatch_domain;
    stats_msg->mismatch_id = context.stats.mismatch_id;
    stats_msg->numdomains = context.stats.numdomains;
    stats_msg->numreplies = context.stats.numreplies;
    stats_msg->all_rcodes[STAT_IDX_OK] = context.stats.all_rcodes[DNS_RCODE_OK];
    stats_msg->all_rcodes[STAT_IDX_NXDOMAIN] = context.stats.all_rcodes[DNS_RCODE_NXDOMAIN];
    stats_msg->all_rcodes[STAT_IDX_SERVFAIL] = context.stats.all_rcodes[DNS_RCODE_SERVFAIL];
    stats_msg->all_rcodes[STAT_IDX_REFUSED] = context.stats.all_rcodes[DNS_RCODE_REFUSED];
    stats_msg->all_rcodes[STAT_IDX_FORMERR] = context.stats.all_rcodes[DNS_RCODE_FORMERR];
    stats_msg->final_rcodes[STAT_IDX_OK] = context.stats.final_rcodes[DNS_RCODE_OK];
    stats_msg->final_rcodes[STAT_IDX_NXDOMAIN] = context.stats.final_rcodes[DNS_RCODE_NXDOMAIN];
    stats_msg->final_rcodes[STAT_IDX_SERVFAIL] = context.stats.final_rcodes[DNS_RCODE_SERVFAIL];
    stats_msg->final_rcodes[STAT_IDX_REFUSED] = context.stats.final_rcodes[DNS_RCODE_REFUSED];
    stats_msg->final_rcodes[STAT_IDX_FORMERR] = context.stats.final_rcodes[DNS_RCODE_FORMERR];
    stats_msg->current_rate = context.stats.current_rate;
    stats_msg->numparsed = context.stats.numparsed;
    for(size_t i = 0; i <= context.cmd_args.resolve_count; i++)
    {
        stats_msg->timeouts[i] = context.stats.timeouts[i];
    }
}

void send_stats()
{
    static stats_exchange_t stats_msg;
    
    my_stats_to_msg(&stats_msg);

    if(write(context.sockets.write_pipe.descriptor, &stats_msg, sizeof(stats_msg)) != sizeof(stats_msg))
    {
        fprintf(stderr, "Could not send stats atomically.\n");
    }
}

void check_progress()
{
    static struct timespec last_time;
    static char timeouts[4096];
    static struct timespec now;
    static const char* stats_format = "\033[H\033[2J" // Clear screen (probably simplest and most portable solution)
            "Processed queries: %zu\n"
            "Received packets: %zu\n"
            "Progress: %.2f%% (%02lld h %02lld min %02lld sec / %02lld h %02lld min %02lld sec)\n"
            "Current incoming rate: %zu pps, average: %zu pps\n"
            "Finished total: %zu, success: %zu (%.2f%%)\n"
            "Mismatched domains: %zu (%.2f%%), IDs: %zu (%.2f%%)\n"
            "Failures: %s\n"
            "Response: | Success:               | Total:\n"
            "OK:       | %12zu (%6.2f%%) | %12zu (%6.2f%%)\n"
            "NXDOMAIN: | %12zu (%6.2f%%) | %12zu (%6.2f%%)\n"
            "SERVFAIL: | %12zu (%6.2f%%) | %12zu (%6.2f%%)\n"
            "REFUSED:  | %12zu (%6.2f%%) | %12zu (%6.2f%%)\n"
            "FORMERR:  | %12zu (%6.2f%%) | %12zu (%6.2f%%)\n";

    clock_gettime(CLOCK_MONOTONIC, &now);

    time_t elapsed_ns = (now.tv_sec - last_time.tv_sec) * 1000000000 + (now.tv_nsec - last_time.tv_nsec);
    size_t rate_pps = elapsed_ns == 0 ? 0 : context.stats.current_rate * TIMED_RING_S / elapsed_ns;
    last_time = now;

    // TODO: Hashmap size adaption logic will be handled here.

    // Send the stats of the child to the parent process
    if(context.cmd_args.num_processes > 1 && context.fork_index != 0)
    {
        send_stats();
        goto end_stats;
    }

    if(context.cmd_args.quiet)
    {
        return;
    }

    // Go on with printing stats.

    float progress = context.state == STATE_DONE ? 1 : 0;
    if(context.domainfile_size > 0) // If the domain file is not a real file, the progress cannot be estimated.
    {
        // Get a rough estimate of the progress, only roughly proportional to the number of domains.
        // Will be very inaccurate if the domain file is sorted per domain name length.
        long int domain_file_position = ftell(context.domainfile);
        if (domain_file_position >= 0)
        {
            progress = domain_file_position / (float)context.domainfile_size;
        }
    }

    time_t total_elapsed_ns = (now.tv_sec - context.stats.start_time.tv_sec) * 1000000000
        + (now.tv_nsec - context.stats.start_time.tv_nsec); // since last output
    long long elapsed = now.tv_sec - context.stats.start_time.tv_sec; // resolution of one second should be okay
    long long sec = elapsed % 60;
    long long min = (elapsed / 60) % 60;
    long long h = elapsed / 3600;

    long long estimated_time = progress == 0 ? 0 : (long long)(elapsed / progress);
    if(estimated_time < elapsed)
    {
        estimated_time = elapsed;
    }
    long long prog_sec = estimated_time % 60;
    long long prog_min = (estimated_time / 60) % 60;
    long long prog_h = (estimated_time / 3600);

#define stats_percent(a, b) ((b) == 0 ? 0 : (a) / (float) (b) * 100)
#define stat_abs_share(a, b) a, stats_percent(a, b)
#define rcode_stat(code) stat_abs_share(context.stats.final_rcodes[(code)], context.stats.finished_success),\
        stat_abs_share(context.stats.all_rcodes[(code)], context.stats.numparsed)
#define rcode_stat_multi(code) stat_abs_share(context.stat_messages[0].final_rcodes[(code)], \
    context.stat_messages[0].finished_success),\
        stat_abs_share(context.stat_messages[0].all_rcodes[(code)], context.stat_messages[0].numparsed)
    
    if(context.cmd_args.num_processes == 1)
    {
        size_t average_pps = elapsed == 0 ? rate_pps : context.stats.numreplies * TIMED_RING_S / total_elapsed_ns;

        // Print the detailed timeout stats (number of tries before timeout) to the timeouts buffer.
        int offset = 0;
        for (size_t i = 0; i <= context.cmd_args.resolve_count; i++)
        {
            float share = stats_percent(context.stats.timeouts[i], context.stats.finished);
            int result = snprintf(timeouts + offset, sizeof(timeouts) - offset, "%zu: %.2f%%, ", i, share);
            if (result <= 0 || result >= sizeof(timeouts) - offset)
            {
                break;
            }
            offset += result;
        }

        fprintf(stderr,
                stats_format,
                context.stats.numdomains,
                context.stats.numreplies,
                progress * 100, h, min, sec, prog_h, prog_min, prog_sec, rate_pps, average_pps,
                context.stats.finished,
                stat_abs_share(context.stats.finished_success, context.stats.finished),
                stat_abs_share(context.stats.mismatch_domain, context.stats.numparsed),
                stat_abs_share(context.stats.mismatch_id, context.stats.numparsed),
                timeouts,

                rcode_stat(DNS_RCODE_OK),
                rcode_stat(DNS_RCODE_NXDOMAIN),
                rcode_stat(DNS_RCODE_SERVFAIL),
                rcode_stat(DNS_RCODE_REFUSED),
                rcode_stat(DNS_RCODE_FORMERR)
        );
    }
    else
    {
        my_stats_to_msg(&context.stat_messages[0]);

        for(size_t j = 1; j < context.cmd_args.num_processes; j++)
        {
            for (size_t i = 0; i <= context.cmd_args.resolve_count; i++)
            {
                context.stat_messages[0].timeouts[i] += context.stat_messages[j].timeouts[i];
            }
            context.stat_messages[0].numreplies += context.stat_messages[j].numreplies;
            context.stat_messages[0].numparsed += context.stat_messages[j].numparsed;
            context.stat_messages[0].numdomains += context.stat_messages[j].numdomains;
            context.stat_messages[0].mismatch_id += context.stat_messages[j].mismatch_id;
            context.stat_messages[0].mismatch_domain += context.stat_messages[j].mismatch_domain;
            context.stat_messages[0].finished_success += context.stat_messages[j].finished_success;
            context.stat_messages[0].finished += context.stat_messages[j].finished;
            for(size_t i = 0; i < 5; i++)
            {
                context.stat_messages[0].all_rcodes[i] += context.stat_messages[j].all_rcodes[i];
            }
            for(size_t i = 0; i < 5; i++)
            {
                context.stat_messages[0].final_rcodes[i] += context.stat_messages[j].final_rcodes[i];
            }
            rate_pps += context.stat_messages[j].current_rate;
        }

        size_t average_pps = elapsed == 0 ? rate_pps :
                             context.stat_messages[0].numreplies * TIMED_RING_S / total_elapsed_ns;


        // Print the detailed timeout stats (number of tries before timeout) to the timeouts buffer.
        int offset = 0;
        for (size_t i = 0; i <= context.cmd_args.resolve_count; i++)
        {
            float share = stats_percent(context.stat_messages[0].timeouts[i], context.stat_messages[0].finished);
            int result = snprintf(timeouts + offset, sizeof(timeouts) - offset, "%zu: %.2f%%, ", i, share);
            if (result <= 0 || result >= sizeof(timeouts) - offset)
            {
                break;
            }
            offset += result;
        }

        fprintf(stderr,
                stats_format,
                context.stat_messages[0].numdomains,
                context.stat_messages[0].numreplies,
                progress * 100, h, min, sec, prog_h, prog_min, prog_sec, rate_pps, average_pps,
                context.stat_messages[0].finished,
                stat_abs_share(context.stat_messages[0].finished_success, context.stat_messages[0].finished),
                stat_abs_share(context.stat_messages[0].mismatch_domain, context.stat_messages[0].numparsed),
                stat_abs_share(context.stat_messages[0].mismatch_id, context.stat_messages[0].numparsed),
                timeouts,

                rcode_stat_multi(STAT_IDX_OK),
                rcode_stat_multi(STAT_IDX_NXDOMAIN),
                rcode_stat_multi(STAT_IDX_SERVFAIL),
                rcode_stat_multi(STAT_IDX_REFUSED),
                rcode_stat_multi(STAT_IDX_FORMERR)
        );
    }

end_stats:
    context.stats.current_rate = 0;
    // Call this function in about one second again
    timed_ring_add(&context.ring, TIMED_RING_S, check_progress);
}

void done()
{
    context.state = STATE_DONE;
    check_progress();
}

void can_send()
{
    char *qname;
    bool new;

    while (hashmapSize(context.map) < context.cmd_args.hashmap_size && context.state <= STATE_QUERYING)
    {
        if(!next_query(&qname))
        {
            context.state = STATE_COOLDOWN; // We will not create any new queries
            if(hashmapSize(context.map) <= 0)
            {
                done();
            }
            break;
            continue;
        }
        context.stats.numdomains++;
        lookup_t *lookup = new_lookup(qname, context.cmd_args.record_type, &new);
        if(!new)
        {
            break;
        }
        send_query(lookup);
    }
}

bool is_unacceptable(dns_pkt_t *packet)
{
    return context.cmd_args.retry_codes[packet->head.header.rcode];
}

void lookup_done(lookup_t *lookup)
{
    context.stats.finished++;

    hashmapRemove(context.map, lookup->key);

    // Return lookup to pool.
    ((lookup_key_t**)context.lookup_pool.data)[context.lookup_pool.len++] = lookup->key;


    // When transmission is not aggressive, we only start a new lookup after another one has finished.
    // When our transmission is very aggressive, we also start a new lookup, although we listen for EPOLLOUT
    // events as well.
    if(context.cmd_args.extreme == 0 || context.cmd_args.extreme == 2)
    {
        can_send();
    }

    if(context.state == STATE_COOLDOWN && hashmapSize(context.map) <= 0)
    {
        done();
    }
}

bool retry(lookup_t *lookup)
{
    context.stats.timeouts[lookup->tries]--;
    context.stats.timeouts[++lookup->tries]++;
    if(lookup->tries < context.cmd_args.resolve_count)
    {
        lookup->ring_entry = timed_ring_add(&context.ring, context.cmd_args.interval_ms * TIMED_RING_MS, lookup);
        send_query(lookup);
        return true;
    }
    return false;
}

void ring_timeout(void *param)
{
    if(param == check_progress)
    {
        check_progress();
        return;
    }

    lookup_t *lookup = param;
    if(!retry(lookup))
    {
        lookup_done(lookup);
    }
}

char *sockaddr2str(struct sockaddr_storage *addr)
{
    static char str[INET6_ADDRSTRLEN + sizeof(":65535") - 1 + 2]; // + 2 for [ and ]
    static uint16_t port;
    size_t len;

    if(addr->ss_family == AF_INET)
    {
        port = ntohs(((struct sockaddr_in*)addr)->sin_port);
        inet_ntop(addr->ss_family, &((struct sockaddr_in*)addr)->sin_addr, str, sizeof(str));
        len = strlen(str);
        // inet_ntop does not allow us to determine, how long the printed string was.
        // Thus, we have to use strlen.
    }
    else
    {
        str[0] = '[';
        port = ntohs(((struct sockaddr_in6*)addr)->sin6_port);
        inet_ntop(addr->ss_family, &((struct sockaddr_in6*)addr)->sin6_addr, str + 1, sizeof(str) - 1);
        len = strlen(str);
        str[len++] = ']';
        str[len] = 0;
    }

    snprintf(str + len, sizeof(str) - len, ":%" PRIu16, port);

    return str;
}

void do_read(uint8_t *offset, size_t len, struct sockaddr_storage *recvaddr)
{
    static dns_pkt_t packet;
    static uint8_t *parse_offset;
    static lookup_t *lookup;

    context.stats.current_rate++;
    context.stats.numreplies++;

    if(!dns_parse_question(offset, len, &packet.head, &parse_offset))
    {
        return;
    }

    context.stats.numparsed++;
    context.stats.all_rcodes[packet.head.header.rcode]++;

    // TODO: Remove unnecessary copy.
    //search_key.domain = (char*)packet.head.question.name.name;
    lookup = hashmapGet(context.map, &packet.head.question);
    if(!lookup) // Most likely reason: delayed response after duplicate query
    {
        context.stats.mismatch_domain++;
        return;
    }

    if(lookup->transaction != packet.head.header.id)
    {
        context.stats.mismatch_id++;
        return;
    }

    timed_ring_remove(&context.ring, lookup->ring_entry); // Clear timeout trigger

    // Check whether we want to retry resending the packet
    if(is_unacceptable(&packet))
    {
        // We may have tried to many times already.
        if(!retry(lookup))
        {
            // If this is the case, we will not try again.
            lookup_done(lookup);
        }
    }
    else
    {
        // We are done with the lookup because we received an acceptable reply.
        lookup_done(lookup);
        context.stats.finished_success++;
        context.stats.final_rcodes[packet.head.header.rcode]++;

        // Print packet
        time_t now = time(NULL);
        uint16_t short_len = (uint16_t) len;
        uint8_t *next = parse_offset;
        dns_record_t rec;
        size_t rec_index = 0;

        switch(context.cmd_args.output)
        {
            case OUTPUT_BINARY:
                // The output file is platform dependent for performance reasons.
                fwrite(&now, sizeof(now), 1, context.outfile);
                fwrite(recvaddr, sizeof(*recvaddr), 1, context.outfile);
                fwrite(&short_len, sizeof(short_len), 1, context.outfile);
                fwrite(offset, short_len, 1, context.outfile);
                break;

            case OUTPUT_TEXT_FULL: // Print packet similar to dig style
                // Resolver and timestamp are not part of the packet, we therefore have to print it manually
                fprintf(context.outfile, ";; Server: %s\n;; Size: %" PRIu16 "\n;; Unix time: %lu\n",
                        sockaddr2str(recvaddr), short_len, now);
                dns_print_packet(context.outfile, &packet, offset, len, next);
                break;

            case OUTPUT_TEXT_SIMPLE: // Only print records from answer section that match the query name
                while(dns_parse_record_raw(offset, next, offset + len, &next, &rec)
                    && rec_index++ < packet.head.header.ans_count)
                {
                    if(!dns_names_eq(&rec.name, &packet.head.question.name))
                    {
                        continue;
                    }
                    fprintf(context.outfile,
                            "%s %s %s\n",
                            dns_name2str(&rec.name),
                            dns_record_type2str((dns_record_type) rec.type),
                            dns_raw_record_data2str(&rec, offset, offset + short_len));
                }
                break;
        }

        // Sometimes, users may want to obtain results immediately.
        if(context.cmd_args.flush)
        {
            fflush(context.outfile);
        }
    }
}

#ifdef PCAP_SUPPORT
void pcap_callback(u_char *arg, const struct pcap_pkthdr *header, const u_char *packet)
{
    static struct sockaddr_storage addr;
    static size_t len;
    static const uint8_t *frame;
    static ssize_t remaining;

    // We expect at least an Ethernet header + IPv4/IPv6 header (>= 20) + UDP header
    if(header->len < 42)
    {
        return;
    }
    frame = ((uint8_t*)packet) + 14;
    remaining = header->len - 14;

    if(((struct ether_header*)packet)->ether_type == context.ether_type_ip)
    {
        unsigned int ip_hdr_len = ((struct iphdr *) frame)->ihl * 4;
        remaining -= ip_hdr_len;

        // Check whether the packet is long enough to still contain a UDP frame
        if(((struct iphdr *) frame)->protocol != 17
            || remaining < 0)
        {
            return;
        }
        frame += ip_hdr_len;
        len = (size_t)remaining;
        remaining -= ntohs(((struct udphdr *) frame)->len);
        if(remaining != 0)
        {
            return;
        }
        frame += 8;
        addr.ss_family = AF_INET;
    }
    else
    {
        return;
    }
    do_read((uint8_t*)frame, len, &addr);
}

void pcap_can_read()
{
    pcap_dispatch(context.pcap, 1, pcap_callback, NULL);
}
#endif

void can_read(socket_info_t *info)
{
    static uint8_t readbuf[0xFFFF];
    static struct sockaddr_storage recvaddr;
    static socklen_t fromlen;
    static ssize_t num_received;



    fromlen = sizeof(recvaddr);
    num_received = recvfrom(info->descriptor, readbuf, sizeof(readbuf), 0, (struct sockaddr *) &recvaddr, &fromlen);
    if(num_received <= 0)
    {
        return;
    }

    do_read(readbuf, (size_t)num_received, &recvaddr);
}

bool cmp_lookup(void *lookup1, void *lookup2)
{
    return dns_names_eq(&((lookup_key_t *) lookup1)->name, &((lookup_key_t *) lookup1)->name);
    //return strcasecmp(((lookup_key_t *) lookup1)->domain,((lookup_key_t *) lookup2)->domain) == 0;
}

void binfile_write_head()
{
    // Write file type signature including null character
    char signature[] = "massdns";
    fwrite(signature, sizeof(signature), 1, context.outfile);

    // Write a uint32_t integer in native byte order to allow detection of endianness
    uint32_t endianness = 0x12345678;
    fwrite(&endianness, sizeof(endianness), 1, context.outfile);

    // Write uint32_t file version number
    // Number is to be incremented if file format is changed
    fwrite(&OUTPUT_BINARY_VERSION, sizeof(OUTPUT_BINARY_VERSION), 1, context.outfile);

    // Write byte length of native size_t type
    uint8_t size_t_len = sizeof(size_t);
    fwrite(&size_t_len, sizeof(size_t_len), 1, context.outfile);


    // Write byte length of sockaddr_storage size
    size_t sockaddr_storage_len = sizeof(struct sockaddr_storage);
    fwrite(&sockaddr_storage_len, sizeof(sockaddr_storage_len), 1, context.outfile);

    // Write offset of ss_family within sockaddr_storage
    size_t ss_family_offset = offsetof(struct sockaddr_storage, ss_family);
    fwrite(&ss_family_offset, sizeof(ss_family_offset), 1, context.outfile);

    // Write size of sa_family_size within sockaddr_storage
    size_t sa_family_size = sizeof(sa_family_t);
    fwrite(&sa_family_size, sizeof(sa_family_size), 1, context.outfile);


    // Write IPv4 family constant
    sa_family_t family_inet = AF_INET;
    fwrite(&family_inet, sizeof(family_inet), 1, context.outfile);

    // Write offset of sin_addr within sockaddr_in
    size_t sin_addr_offset = offsetof(struct sockaddr_in, sin_addr);
    fwrite(&sin_addr_offset, sizeof(sin_addr_offset), 1, context.outfile);

    // Write offset of sin_port within sockaddr_in
    size_t sin_port_offset = offsetof(struct sockaddr_in, sin_port);
    fwrite(&sin_port_offset, sizeof(sin_port_offset), 1, context.outfile);


    // Write IPv6 family constant
    sa_family_t family_inet6 = AF_INET6;
    fwrite(&family_inet6, sizeof(family_inet6), 1, context.outfile);

    // Write offset of sin6_addr within sockaddr_in6
    size_t sin6_addr_offset = offsetof(struct sockaddr_in6, sin6_addr);
    fwrite(&sin6_addr_offset, sizeof(sin6_addr_offset), 1, context.outfile);

    // Write offset of sin6_port within sockaddr_in6
    size_t sin6_port_offset = offsetof(struct sockaddr_in6, sin6_port);
    fwrite(&sin6_port_offset, sizeof(sin6_port_offset), 1, context.outfile);
}

void privilege_drop()
{
    if (geteuid() != 0)
    {
        return;
    }
    char *username = context.cmd_args.drop_user ? context.cmd_args.drop_user : COMMON_UNPRIVILEGED_USER;
    if(!context.cmd_args.root)
    {
        struct passwd *drop_user = getpwnam(username);
        if (drop_user && setuid(drop_user->pw_uid) == 0)
        {
            if (!context.cmd_args.quiet)
            {
                fprintf(stderr, "Privileges have been dropped to \"%s\" for security reasons.\n\n", username);
            }
        }
        else
        {
            fprintf(stderr, "Privileges could not be dropped to \"%s\".\n"
                "For security reasons, this program will only run as root user when supplied with --root"
                "which is not recommended.\n"
                "It is better practice to run this program as a different user.\n", username);
            clean_exit(1);
        }
    }
    else
    {
        if (!context.cmd_args.quiet)
        {
            fprintf(stderr, "[WARNING] Privileges were not dropped. This is not recommended.\n\n");
        }
    }
}

#ifdef PCAP_SUPPORT
void pcap_setup()
{
    context.pcap_dev = pcap_lookupdev(context.pcap_error);
    if(context.pcap_dev == NULL)
    {
        goto pcap_error;
    }
    fprintf(stderr, "Default pcap device: %s", context.pcap_dev);


    char mac_filter[sizeof("ether dst ") - 1 + MAC_READABLE_BUFLEN];
    char *mac_readable = mac_filter + sizeof("ether dst ") - 1;
    strcpy(mac_filter, "ether dst ");

    if(get_iface_hw_addr_readable(context.pcap_dev, mac_readable) != 0)
    {
        fprintf(stderr, "\nFailed to determine the hardware address of the device.\n");
        goto pcap_error_noprint;
    }
    fprintf(stderr, ", address: %s\n", mac_readable);


    context.pcap = pcap_create(context.pcap_dev, context.pcap_error);
    if(context.pcap == NULL)
    {
        goto pcap_error;
    }

    if(pcap_set_snaplen(context.pcap, 0xFFFF) != 0)
    {
        goto pcap_error;
    }

    if(pcap_setnonblock(context.pcap, 1, context.pcap_error) == -1)
    {
        goto pcap_error;
    }

    if(pcap_set_buffer_size(context.pcap, 1024 * 1024) != 0)
    {
        goto pcap_error;
    }

    int activation_status = pcap_activate(context.pcap);
    if(activation_status != 0)
    {
        fprintf(stderr, "Error during pcap activation: %s\n", pcap_statustostr(activation_status));
        goto pcap_error_noprint;
    }

    if(pcap_compile(context.pcap, &context.pcap_filter, mac_filter, 0, PCAP_NETMASK_UNKNOWN) != 0)
    {
        fprintf(stderr, "Error during pcap filter compilation: %s\n", pcap_geterr(context.pcap));
        goto pcap_error_noprint;
    }

    if(pcap_setfilter(context.pcap, &context.pcap_filter) != 0)
    {
        fprintf(stderr, "Error setting pcap filter: %s\n", pcap_geterr(context.pcap));
        goto pcap_error_noprint;
    }

    context.pcap_info.descriptor = pcap_get_selectable_fd(context.pcap);
    if(context.pcap_info.descriptor < 0)
    {
        goto pcap_error;
    }

    struct epoll_event ev;
    bzero(&ev, sizeof(ev));
    ev.data.ptr = &context.pcap_info;
    ev.events = EPOLLIN;
    if (epoll_ctl(context.epollfd, EPOLL_CTL_ADD, context.pcap_info.descriptor, &ev) != 0)
    {
        perror("Failed to add epoll event");
        clean_exit(EXIT_FAILURE);
    }
    return;

pcap_error:
    fprintf(stderr, "Error during pcap setup: %s\n", context.pcap_error);
pcap_error_noprint:
    cleanup();
    clean_exit(1);
}
#endif

void init_pipes()
{
    // We don't need any pipes if the process is not forked
    if(context.cmd_args.num_processes <= 1)
    {
        return;
    }

    // Otherwise create a unidirectional pipe for reading and writing from every fork
    context.sockets.pipes = safe_malloc(sizeof(*context.sockets.pipes) * 2 * context.cmd_args.num_processes);
    for(size_t i = 0; i < context.cmd_args.num_processes; i++)
    {
        if(pipe(context.sockets.pipes + i * 2) != 0)
        {
            perror("Pipe failed");
            clean_exit(EXIT_FAILURE);
        }
    }

}

void setup_pipes()
{
    if(context.fork_index == 0) // We are in the main process
    {
        context.sockets.master_pipes_read = safe_calloc(sizeof(socket_info_t) * context.cmd_args.num_processes);

        // Close all pipes that the children use to write
        for (size_t i = 0; i < context.cmd_args.num_processes; i++)
        {
            close(context.sockets.pipes[2 * i + 1]);
            context.sockets.pipes[2 * i + 1] = -1;

            context.sockets.master_pipes_read[i].descriptor = context.sockets.pipes[2 * i];
            context.sockets.master_pipes_read[i].type = SOCKET_TYPE_CONTROL;
            context.sockets.master_pipes_read[i].data = (void*)i;

            // Add all pipes the main process can read from to the epoll descriptor
            struct epoll_event ev;
            bzero(&ev, sizeof(ev));
            ev.data.ptr = &context.sockets.master_pipes_read[i];
            ev.events = EPOLLIN;
            if (epoll_ctl(context.epollfd, EPOLL_CTL_ADD, context.sockets.master_pipes_read[i].descriptor, &ev) != 0)
            {
                perror("Failed to add epoll event");
                clean_exit(EXIT_FAILURE);
            }
        }
    }
    else // It's a child process
    {
        // Close all pipes except the two belonging to the current process
        for (size_t i = 0; i < context.cmd_args.num_processes; i++)
        {
            if (i == context.fork_index)
            {
                continue;
            }
            close(context.sockets.pipes[2 * i]);
            close(context.sockets.pipes[2 * i + 1]);
            context.sockets.pipes[2 * i] = -1;
            context.sockets.pipes[2 * i + 1] = -1;
        }
        context.sockets.write_pipe.descriptor = context.sockets.pipes[2 * context.fork_index + 1];
        context.sockets.write_pipe.type = SOCKET_TYPE_CONTROL;
        close(context.sockets.pipes[2 * context.fork_index]);
        context.sockets.pipes[2 * context.fork_index] = -1;
    }
}

void read_control_message(socket_info_t *socket_info)
{
    size_t process = (size_t)socket_info->data;
    ssize_t read_result = read(socket_info->descriptor, context.stat_messages + process, sizeof(stats_exchange_t));
    if(read_result < sizeof(stats_exchange_t))
    {
        fprintf(stderr, "Atomic read failed %ld.\n", read_result);
    }

}

void run()
{
    static char multiproc_outfile_name[8192];

    if(!urandom_init())
    {
        fprintf(stderr, "Failed to open /dev/urandom: %s\n", strerror(errno));
        clean_exit(1);
    }

    if(context.cmd_args.output == OUTPUT_BINARY)
    {
        binfile_write_head();
    }

    context.map = hashmapCreate(context.cmd_args.hashmap_size, hash_lookup_key, cmp_lookup);
    if(context.map == NULL)
    {
        fprintf(stderr, "Failed to create hashmap.\n");
        clean_exit(EXIT_FAILURE);
    }

    context.lookup_pool.len = context.cmd_args.hashmap_size * 2;
    context.lookup_pool.data = safe_calloc(context.lookup_pool.len * sizeof(void*));
    context.lookup_space = safe_calloc(context.lookup_pool.len * sizeof(*context.lookup_space));
    for(size_t i = 0; i < context.lookup_pool.len; i++)
    {
        ((lookup_entry_t**)context.lookup_pool.data)[i] = context.lookup_space + i;
    }

    timed_ring_init(&context.ring, max(context.cmd_args.interval_ms, 1000), 2 * TIMED_RING_MS, context.cmd_args.timed_ring_buckets);

    uint32_t socket_events = EPOLLOUT;

    struct epoll_event pevents[100000];
    bzero(pevents, sizeof(pevents));

    init_pipes();
    context.fork_index = split_process(context.cmd_args.num_processes);
    context.epollfd = epoll_create(1);
#ifdef PCAP_SUPPORT
    if(context.cmd_args.use_pcap)
    {
        pcap_setup();
    }
    else
#endif
    {
        socket_events |= EPOLLIN;
    }
    if(context.cmd_args.num_processes > 1)
    {
        setup_pipes();
        if(context.fork_index == 0)
        {
            context.stat_messages = safe_calloc(context.cmd_args.num_processes * sizeof(stats_exchange_t));
        }
    }

    if(strcmp(context.cmd_args.outfile_name, "-") != 0)
    {
        if(context.cmd_args.num_processes > 1)
        {
            snprintf(multiproc_outfile_name, sizeof(multiproc_outfile_name), "%s%zd", context.cmd_args.outfile_name,
            context.fork_index);
            context.outfile = fopen(multiproc_outfile_name, "w");
        }
        else
        {
            context.outfile = fopen(context.cmd_args.outfile_name, "w");
        }
        if(!context.outfile)
        {
            perror("Failed to open output file");
            clean_exit(EXIT_FAILURE);
        }
    }
    else
    {
        if(context.cmd_args.num_processes > 1)
        {
            fprintf(stderr, "Multiprocessing is currently only supported through the -w parameter.\n");
            clean_exit(EXIT_FAILURE);
        }
    }


    // It is important to call default interface sockets setup before reading the resolver list
    // because that way we can warn if the socket creation for a certain IP protocol failed although a resolver
    // requires the protocol.
    query_sockets_setup();
    context.resolvers = massdns_resolvers_from_file(context.cmd_args.resolvers);

    privilege_drop();

    add_sockets(context.epollfd, socket_events, EPOLL_CTL_ADD, &context.sockets.interfaces4);
    add_sockets(context.epollfd, socket_events, EPOLL_CTL_ADD, &context.sockets.interfaces6);


    clock_gettime(CLOCK_MONOTONIC, &context.stats.start_time);
    check_progress();

    while(context.state < STATE_DONE)
    {
        int ready = epoll_wait(context.epollfd, pevents, sizeof(pevents) / sizeof(pevents[0]), 1);
        if (ready < 0)
        {
            perror("Epoll failure");
        }
        else if(ready == 0) // Epoll timeout
        {
            timed_ring_handle(&context.ring, ring_timeout);
        }
        else if (ready > 0)
        {
            for (int i = 0; i < ready; i++)
            {
                socket_info_t *socket_info = pevents[i].data.ptr;
                if ((pevents[i].events & EPOLLOUT) && socket_info->type == SOCKET_TYPE_QUERY)
                {
                    can_send();
                    timed_ring_handle(&context.ring, ring_timeout);
                }
                if ((pevents[i].events & EPOLLIN) && socket_info->type == SOCKET_TYPE_QUERY)
                {
                    can_read(socket_info);
                }
#ifdef PCAP_SUPPORT
                else if((pevents[i].events & EPOLLIN) && socket_info == &context.pcap_info)
                {
                    pcap_can_read();
                }
#endif
                else if((pevents[i].events & EPOLLIN) && socket_info->type == SOCKET_TYPE_CONTROL)
                {
                    read_control_message(socket_info);
                }
            }
            timed_ring_handle(&context.ring, ring_timeout);
        }
    }
}

void use_stdin()
{
    if (!context.cmd_args.quiet)
    {
        fprintf(stderr, "Reading domain list from stdin.\n");
    }
    context.domainfile = stdin;
}

int parse_cmd(int argc, char **argv)
{
    context.cmd_args.argc = argc;
    context.cmd_args.argv = argv;
    context.cmd_args.help_function = print_help;

    if (argc <= 1)
    {
        print_help();
        return 1;
    }

#ifdef PCAP_SUPPORT
    // Precompute values so we do not have to call htons for each incoming packet
    context.ether_type_ip = htons(ETHERTYPE_IP);
    context.ether_type_ip6 = htons(ETHERTYPE_IPV6);
#endif

    context.cmd_args.record_type = DNS_REC_INVALID;
    context.domainfile_size = -1;
    context.state = STATE_WARMUP;
    context.logfile = stderr;
    context.outfile = stdout;
    context.cmd_args.outfile_name = "-";

    context.cmd_args.resolve_count = 50;
    context.cmd_args.hashmap_size = 100000;
    context.cmd_args.interval_ms = 500;
    context.cmd_args.timed_ring_buckets = 10000;
    context.cmd_args.output = OUTPUT_TEXT_FULL;
    context.cmd_args.retry_codes[DNS_RCODE_REFUSED] = true;
    context.cmd_args.num_processes = 1;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
        {
            print_help();
            return 1;
        }
        else if (strcmp(argv[i], "--resolvers") == 0 || strcmp(argv[i], "-r") == 0)
        {
            if (context.cmd_args.resolvers == NULL)
            {
                expect_arg(i);
                context.cmd_args.resolvers = argv[++i];
            }
            else
            {
                fprintf(stderr, "Resolvers may only be supplied once.\n\n");
                print_help();
                return 1;
            }
        }
        else if(strcmp(argv[i], "--retry") == 0)
        {
            expect_arg(i);
            dns_rcode rcode;
            if(dns_str2rcode(argv[++i], &rcode))
            {
                if(!context.cmd_args.retry_codes_set)
                {
                    context.cmd_args.retry_codes[DNS_RCODE_REFUSED] = false;
                    context.cmd_args.retry_codes_set = true;
                }
                context.cmd_args.retry_codes[rcode] = true;
            }
            else if(strcasecmp(argv[i], "never") == 0)
            {
                context.cmd_args.retry_codes[DNS_RCODE_REFUSED] = false;
                context.cmd_args.retry_codes_set = true;
            }
            else
            {
                fprintf(stderr, "Invalid retry code: %s.\n", argv[i]);
            }
        }
        else if (strcmp(argv[i], "--bindto") == 0 || strcmp(argv[i], "-b") == 0)
        {
            expect_arg(i);
            struct sockaddr_storage *addr = safe_malloc(sizeof(addr));
            if (!str_to_addr(argv[++i], 0, addr))
            {
                free(addr);
                fprintf(stderr, "Invalid address for socket binding.\n\n");
                print_help();
                return 1;

            }
            single_list_push_back(addr->ss_family == AF_INET ? &context.cmd_args.bind_addrs4 :
                                  &context.cmd_args.bind_addrs6, addr);
        }
        else if (strcmp(argv[i], "--outfile") == 0 || strcmp(argv[i], "-w") == 0)
        {
            expect_arg(i);
            context.cmd_args.outfile_name = argv[++i];

        }
        else if (strcmp(argv[i], "--error-log") == 0 || strcmp(argv[i], "-l") == 0)
        {
            expect_arg(i);
            char *filename = argv[++i];
            if(strcmp(filename, "-") != 0)
            {
                context.logfile = fopen(filename, "w");
                if(!context.logfile)
                {
                    perror("Failed to open log file");
                    return 1;
                }
            }
        }
        else if (strcmp(argv[i], "--types") == 0 || strcmp(argv[i], "-t") == 0)
        {
            expect_arg(i);
            if (context.cmd_args.record_type != DNS_REC_INVALID)
            {
                fprintf(stderr, "Currently, only one record type is supported.\n\n");
                return 1;
            }
            dns_record_type rtype = dns_str_to_record_type(argv[++i]);
            if (rtype == DNS_REC_INVALID)
            {
                fprintf(stderr, "Unsupported record type: %s\n\n", argv[i]);
                print_help();
                return 1;
            }
            context.cmd_args.record_type = rtype;
        }
        else if (strcmp(argv[i], "--drop-user") == 0)
        {
            expect_arg(i);
            context.cmd_args.drop_user = argv[++i];
        }
        else if (strcmp(argv[i], "--root") == 0)
        {
            context.cmd_args.root = true;
        }
        else if (strcmp(argv[i], "--norecurse") == 0 || strcmp(argv[i], "-n") == 0)
        {
            context.cmd_args.norecurse = true;
        }
        else if (strcmp(argv[i], "--output") == 0 || strcmp(argv[i], "-o") == 0)
        {
            expect_arg(i++);
            if(strchr(argv[i], 'B'))
            {
                context.cmd_args.output = OUTPUT_BINARY;
            }
            else if(strchr(argv[i], 'S'))
            {
                context.cmd_args.output = OUTPUT_TEXT_SIMPLE;
            }
            else if(strchr(argv[i], 'F'))
            {
                context.cmd_args.output = OUTPUT_TEXT_FULL;
            }
        }
#ifdef PCAP_SUPPORT
        else if (strcmp(argv[i], "--use-pcap") == 0)
        {
            context.cmd_args.use_pcap = true;
        }
#endif
        else if (strcmp(argv[i], "--predictable") == 0)
        {
            context.cmd_args.predictable_resolver = true;
        }
        else if (strcmp(argv[i], "--sticky") == 0)
        {
            context.cmd_args.sticky = true;
        }
        else if (strcmp(argv[i], "--finalstats") == 0)
        {
            context.cmd_args.finalstats = true;
        }
        else if (strcmp(argv[i], "--quiet") == 0 || strcmp(argv[i], "-q") == 0)
        {
            context.cmd_args.quiet = true;
        }
        else if (strcmp(argv[i], "--extreme") == 0 || strcmp(argv[i], "-x") == 0)
        {
            context.cmd_args.extreme = (int)expect_arg_nonneg(i++, 0, 2);
        }
        else if (strcmp(argv[i], "--resolve-count") == 0 || strcmp(argv[i], "-c") == 0)
        {
            context.cmd_args.resolve_count = (uint8_t)expect_arg_nonneg(i++, 1, UINT8_MAX);
        }
        else if (strcmp(argv[i], "--hashmap-size") == 0 || strcmp(argv[i], "-s") == 0)
        {
            context.cmd_args.hashmap_size = (size_t) expect_arg_nonneg(i++, 1, SIZE_MAX);
        }
        else if (strcmp(argv[i], "--processes") == 0)
        {
            context.cmd_args.num_processes = (size_t) expect_arg_nonneg(i++, 0, SIZE_MAX);
            if(context.cmd_args.num_processes == 0)
            {
                int cores = get_nprocs_conf();
                if(cores <= 0)
                {
                    fprintf(stderr, "Failed to determine number of processor cores.\n");
                    clean_exit(1);
                }
                context.cmd_args.num_processes = (size_t)cores;
            }
        }
        else if (strcmp(argv[i], "--interval") == 0 || strcmp(argv[i], "-i") == 0)
        {
            context.cmd_args.interval_ms = (unsigned int) expect_arg_nonneg(i++, 1, UINT_MAX);
        }
        else if (strcmp(argv[i], "--sndbuf") == 0)
        {
            context.cmd_args.sndbuf = (int)expect_arg_nonneg(i++, 0, INT_MAX);
        }
        else if (strcmp(argv[i], "--rcvbuf") == 0)
        {
            context.cmd_args.rcvbuf = (int)expect_arg_nonneg(i++, 0, INT_MAX);
        }
        else if (strcmp(argv[i], "--flush") == 0)
        {
            context.cmd_args.flush = true;
        }
        else
        {
            if (context.cmd_args.domains == NULL)
            {
                context.cmd_args.domains = argv[i];
                if (strcmp(argv[i], "-") == 0)
                {
                    use_stdin();
                }
                else
                {
                    // If we can seek through the domain file, we seek to the end and store the file size
                    // in order to be able to report an estimate progress of resolving.
                    context.domainfile = fopen(argv[i], "r");
                    if (context.domainfile == NULL)
                    {
                        fprintf(stderr, "Failed to open domain file \"%s\".\n", argv[i]);
                        clean_exit(1);
                    }
                    if(fseek(context.domainfile, 0, SEEK_END) != 0)
                    {
                        // Not a seekable stream.
                        context.domainfile_size = -1;
                    }
                    else
                    {
                        context.domainfile_size = ftell(context.domainfile);
                        if(fseek(context.domainfile, 0, SEEK_SET) != 0)
                        {
                            // Should never happen because seeking was possible before but we can still recover.
                            context.domainfile_size = -1;
                        }
                    }
                }
            }
            else
            {
                fprintf(stderr, "The domain list may only be supplied once.\n\n");
                print_help();
                return 1;
            }
        }
    }
    if (context.cmd_args.record_type == DNS_REC_INVALID)
    {
        context.cmd_args.record_type = DNS_REC_A;
    }
    if (context.cmd_args.record_type == DNS_REC_ANY)
    {
        // Some operators will not reply to ANY requests:
        // https://blog.cloudflare.com/deprecating-dns-any-meta-query-type/
        // https://lists.dns-oarc.net/pipermail/dns-operations/2013-January/009501.html
        fprintf(stderr, "Note that DNS ANY scans might be unreliable.\n");
    }
    if (context.cmd_args.resolvers == NULL)
    {
        fprintf(stderr, "Resolvers are required to be supplied.\n\n");
        print_help();
        return 1;
    }
    if (context.domainfile == NULL)
    {
        if(!isatty(STDIN_FILENO))
        {
            use_stdin();
        }
        else
        {
            fprintf(stderr, "The domain list is required to be supplied.\n\n");
            print_help();
            return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
#ifdef DEBUG
    // Create core dump on crash in debug mode
    struct rlimit core_limits;
    core_limits.rlim_cur = core_limits.rlim_max = RLIM_INFINITY;
    setrlimit(RLIMIT_CORE, &core_limits);
#endif

    int rcode = parse_cmd(argc, argv);
    if(rcode != 0)
    {
        return rcode;
    }

    run();
    cleanup();

    return 0;
}
