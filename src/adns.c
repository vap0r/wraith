#include "common.h"
#include "adns.h"
#include "egg_timer.h"
#include "main.h"
#include "net.h"
#include "socket.h"
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>

typedef struct {
	char **list;
	int len;
} dns_answer_t;

typedef struct dns_query {
	struct dns_query *next;
	dns_answer_t answer;
	dns_callback_t callback;
	time_t time;
	int id;
	int remaining;
	char *query;
	void *client_data;
} dns_query_t;

typedef struct {
	unsigned short id;
	unsigned short flags;
	unsigned short question_count;
	unsigned short answer_count;
	unsigned short ns_count;
	unsigned short ar_count;
} dns_header_t;

typedef struct {
	/* char name[]; */
	unsigned short type;
	unsigned short dclass;
	int ttl;
	unsigned short rdlength;
	/* char rdata[]; */
} dns_rr_t;

/* Entries from resolv.conf */
typedef struct dns_server {
	char *ip;
	int idx;
} dns_server_t;

/* Entries from hosts */
typedef struct {
	time_t expiretime;
	char *host;
        char *ip;
} dns_host_t;

static int query_id = 1;
static dns_header_t _dns_header = {0, 0, 0, 0, 0, 0};
static dns_query_t *query_head = NULL;
static dns_host_t *hosts = NULL;
static int nhosts = 0;
static dns_server_t *servers = NULL;
static int nservers = 0;
static int cur_server = -1;

static char separators[] = " ,\t\r\n";

int dns_idx = -1;
int dns_sock = -1;
const char *dns_ip = NULL;

static int make_header(char *buf, int id);
static int cut_host(const char *host, char *query);
static int reverse_ip(const char *host, char *reverse);
static void read_resolv(char *fname);
static void read_hosts(char *fname);
static int get_dns_idx();
//static int dns_on_read(void *client_data, int idx, char *buf, int len);
//static int dns_on_eof(void *client_data, int idx, int err, const char *errmsg);
static void dns_on_read(int idx, char *buf, int atr);
static void dns_on_eof(int idx);
static const char *dns_next_server();
static void parse_reply(char *response, int nbytes);

time_t async_resolve_timeout = 30;

static void 
dns_display(int idx, char *buf)
{
  sprintf(buf, "adns   waited %lis", now - dcc[idx].timeval);
}

static void
dns_reinit(int idx)
{
        sdprintf("Re-opening dns socket...");
        killsock(dcc[idx].sock);
        lostdcc(idx);
	dns_idx = -1;
        dns_sock = -1;
	dns_ip = NULL;

        if (!get_dns_idx())
          sdprintf("Successfully reopened dns socket");
        else
          sdprintf("Failed to reopen dns socket");
}

static void
dns_timeout(int idx)
{
  sdprintf("DNS socket timed out");
  /*egg_dns_cancel(dcc[idx].u.dns_id, 1);*/
  dns_reinit(idx);  
}

static struct dcc_table dns_handler = {
  "adns",
  DCT_VALIDIDX,
  dns_on_eof,
  dns_on_read,
  &async_resolve_timeout,
  dns_timeout,
  dns_display,
  NULL,
  NULL,
  NULL,
};

static void answer_init(dns_answer_t *answer)
{
	memset(answer, 0, sizeof(*answer));
}

static void answer_add(dns_answer_t *answer, const char *what)
{
	answer->list = (char **) realloc(answer->list, sizeof(*answer->list) * (answer->len+2));
	answer->list[answer->len] = strdup(what);
	answer->len++;
	answer->list[answer->len] = NULL;
}

static void answer_free(dns_answer_t *answer)
{
	int i;
	for (i = 0; i < answer->len; i++) free(answer->list[i]);
	if (answer->list) free(answer->list);
}

static dns_query_t *alloc_query(void *client_data, dns_callback_t callback, const char *query)
{
	dns_query_t *q = (dns_query_t *) calloc(1, sizeof(*q));

	q->id = query_id;
	query_id++;
	q->query = strdup(query);
	q->callback = callback;
	q->client_data = client_data;
	q->time = now;
	q->next = query_head;
	query_head = q;

	return q;
}

static int get_dns_idx()
{
	int i, sock;
       
	sock = -1;
	for (i = 0; i < nservers; i++) {
		if (!dns_ip) dns_ip = dns_next_server();
		sock = socket_create(dns_ip, DNS_PORT, NULL, 0, SOCKET_CLIENT | SOCKET_NONBLOCK | SOCKET_UDP);
		if (sock < 0) {
			/* Try the next server. */
			dns_ip = NULL;
		}
		else break;
	}
	if (i == nservers) return 1;
//	dns_idx = sockbuf_new();
//	sockbuf_set_handler(dns_idx, &dns_handler, NULL);
//	sockbuf_set_sock(dns_idx, sock, 0);
//        allocsock(sock, SOCK_CONNECT);

        dns_idx = new_dcc(&dns_handler, 0);
        sdprintf("dns_idx: %d", dns_idx);
        dcc[dns_idx].sock = sock;
        dns_sock = sock;
        sdprintf("dns_sock: %d", dcc[dns_idx].sock);
        strcpy(dcc[dns_idx].nick, "(new_dns)");
        sdprintf("dns_ip: %s", dns_ip);
        dcc[dns_idx].timeval = now;
        dns_handler.timeout_val = 0;
        return 0;
}

void egg_dns_send(char *query, int len)
{
	if (dns_idx < 0) {
		if (get_dns_idx()) {
                  sdprintf("get_dns_idx() failed in egg_dns_send");
                  return;
                }
	}
        dns_handler.timeout_val = &async_resolve_timeout;
        dcc[dns_idx].timeval = now;
        write(dcc[dns_idx].sock, query, len);
//	sockbuf_write(dns_idx, query, len);
}

/* Perform an async dns lookup. This is host -> ip. For ip -> host, use
 * egg_dns_reverse(). We return a dns id that you can use to cancel the
 * lookup. */
int egg_dns_lookup(const char *host, int timeout, dns_callback_t callback, void *client_data)
{
	char buf[512] = "";
	dns_query_t *q = NULL;
	int i, len;

	if (socket_valid_ip(host)) {
		/* If it's already an ip, we're done. */
		dns_answer_t answer;

		answer_init(&answer);
		answer_add(&answer, host);
		callback(client_data, host, answer.list);
		answer_free(&answer);
		return(-1);
	}

	/* Ok, now see if it's in our host cache. */
	for (i = 0; i < nhosts; i++) {
		if (!strcasecmp(host, hosts[i].host)) {
			dns_answer_t answer;
sdprintf("ANSWER FROM CACHE!");
			answer_init(&answer);
			answer_add(&answer, hosts[i].ip);
			callback(client_data, host, answer.list);
			answer_free(&answer);
			return(-1);
		}
	}

	/* Allocate our query struct. */
        q = alloc_query(client_data, callback, host);

	/* Send the ipv4 query. */
	q->remaining = 1;
	len = make_header(buf, q->id);
	len += cut_host(host, buf + len);
	buf[len] = 0; len++; buf[len] = 1; len++;
	buf[len] = 0; len++; buf[len] = 1; len++;

	egg_dns_send(buf, len);

#ifdef USE_IPV6
	/* Now send the ipv6 query. */
	q->remaining++;
	len = make_header(buf, q->id);
	len += cut_host(host, buf + len);
	buf[len] = 0; len++; buf[len] = 28; len++;
	buf[len] = 0; len++; buf[len] = 1; len++;

	egg_dns_send(buf, len);
#endif

	return(q->id);
}

/* Perform an async dns reverse lookup. This does ip -> host. For host -> ip
 * use egg_dns_lookup(). We return a dns id that you can use to cancel the
 * lookup. */
int egg_dns_reverse(const char *ip, int timeout, dns_callback_t callback, void *client_data)
{
	dns_query_t *q;
	char buf[512], *reversed_ip;
	int i, len;

	if (!socket_valid_ip(ip)) {
		/* If it's not a valid ip, don't even make the request. */
		callback(client_data, ip, NULL);
		return(-1);
	}

	/* Ok, see if we have it in our host cache. */
	for (i = 0; i < nhosts; i++) {
		if (!strcasecmp(hosts[i].ip, ip)) {
			dns_answer_t answer;

			answer_init(&answer);
			answer_add(&answer, hosts[i].host);
			callback(client_data, ip, answer.list);
			answer_free(&answer);
			return(-1);
		}
	}

	/* We need to transform the ip address into the proper form
	 * for reverse lookup. */
	if (strchr(ip, ':')) {
		char temp[65];

		socket_ipv6_to_dots(ip, temp);
		reversed_ip = (char *) malloc(strlen(temp) + 10);
		reverse_ip(temp, reversed_ip);
		strcat(reversed_ip, ".in6.arpa");
	}
	else {
		reversed_ip = (char *) malloc(strlen(ip) + 14);
		reverse_ip(ip, reversed_ip);
		strcat(reversed_ip, ".in-addr.arpa");
	}

	len = make_header(buf, query_id);
	len += cut_host(reversed_ip, buf + len);
	buf[len] = 0; len++; buf[len] = 12; len++;
	buf[len] = 0; len++; buf[len] = 1; len++;

	free(reversed_ip);

	q = alloc_query(client_data, callback, ip);

	egg_dns_send(buf, len);

	return(q->id);
}

//static int dns_on_read(void *client_data, int idx, char *buf, int len)
static void dns_on_read(int idx, char *buf, int atr)
{
        dcc[idx].timeval = now;
        dns_handler.timeout_val = 0;
        atr = read(dcc[idx].sock, buf, 512);

	parse_reply(buf, atr);
	return;
}

//static int dns_on_eof(void *client_data, int idx, int err, const char *errmsg)
static void dns_on_eof(int idx)
{
//	sockbuf_delete(idx);
        sdprintf("eof on dns idx: %d sock: %d", idx, dcc[idx].sock);
        dns_reinit(idx);

	return;
}

/* for .restart
int egg_dns_shutdown(void)
{
	int i;

	if (nservers > 0) {
		for (i = 0; i < nservers; i++) {
			if (servers[i].ip) free(servers[i].ip);
		}
		free(servers); servers = NULL;
		nservers = 0;
	}
	
	if (nhosts > 0) {
		for (i = 0; i < nhosts; i++) {
			if (hosts[i].host) free(hosts[i].host);
			if (hosts[i].ip) free(hosts[i].ip);
		}
		free(hosts); hosts = NULL;
		nhosts = 0;
	}

	return (0);
}
*/
static const char *dns_next_server()
{
	if (!servers || nservers < 1) return("127.0.0.1");
	cur_server++;
	if (cur_server >= nservers) cur_server = 0;
	return(servers[cur_server].ip);
}

static void add_server(char *ip)
{
	servers = (dns_server_t *) realloc(servers, (nservers+1)*sizeof(*servers));
	servers[nservers].ip = strdup(ip);
	nservers++;
        sdprintf("Added NS: %s", ip);
}

static void add_host(char *host, char *ip, time_t ttl)
{
	hosts = (dns_host_t *) realloc(hosts, (nhosts+1)*sizeof(*hosts));
	hosts[nhosts].host = strdup(host);
	hosts[nhosts].ip = strdup(ip);
	hosts[nhosts].expiretime = now + ttl;
	nhosts++;
}

static void del_host(int id)
{
  free(hosts[id].host);
  free(hosts[id].ip);
  hosts[id].expiretime = 0;

  nhosts--;

  if (id < nhosts)
    egg_memcpy(&hosts[id], &hosts[nhosts], sizeof(dns_host_t));
  else
    egg_bzero(&hosts[id], sizeof(dns_host_t)); /* drummer */

  hosts = (dns_host_t *) realloc(hosts, (nhosts+1)*sizeof(*hosts));
}

static int read_thing(char *buf, char *ip)
{
	int skip, len;

	skip = strspn(buf, separators);
	buf += skip;
	len = strcspn(buf, separators);
	memcpy(ip, buf, len);
	ip[len] = 0;
	return(skip + len);
}

static void read_resolv(char *fname)
{
	FILE *fp;
	char buf[512], ip[512];

	fp = fopen(fname, "r");
	if (!fp) return;
	while (fgets(buf, sizeof(buf), fp)) {
		if (!strncasecmp(buf, "nameserver", 10)) {
			read_thing(buf+10, ip);
			if (strlen(ip)) add_server(ip);
		}
	}
	fclose(fp);
}

static void read_hosts(char *fname)
{
	FILE *fp;
	char buf[512], ip[512], host[512];
	int skip, n;

	fp = fopen(fname, "r");
	if (!fp) return;
	while (fgets(buf, sizeof(buf), fp)) {
		if (strchr(buf, '#')) continue;
		skip = read_thing(buf, ip);
		if (!strlen(ip)) continue;
		while ((n = read_thing(buf+skip, host))) {
			skip += n;
			if (strlen(host)) add_host(host, ip, 0);
		}
	}
	fclose(fp);
}


static int make_header(char *buf, int id)
{
	_dns_header.question_count = htons(1);
	_dns_header.id = htons(id);
	memcpy(buf, &_dns_header, 12);
	return(12);
}

static int cut_host(const char *host, char *query)
{
	char *period, *orig;
	int len;

	orig = query;
	while ((period = strchr(host, '.'))) {
		len = period - host;
		if (len > 63) return(-1);
		*query++ = len;
		memcpy(query, host, len);
		query += len;
		host = period+1;
	}
	len = strlen(host);
	if (len) {
		*query++ = len;
		memcpy(query, host, len);
		query += len;
	}
	*query++ = 0;
	return(query-orig);
}

static int reverse_ip(const char *host, char *reverse)
{
	char *period;
	int offset, len;

	period = strchr(host, '.');
	if (!period) {
		len = strlen(host);
		memcpy(reverse, host, len);
		return(len);
	}
	else {
		len = period - host;
		offset = reverse_ip(host+len+1, reverse);
		reverse[offset++] = '.';
		memcpy(reverse+offset, host, len);
		reverse[offset+len] = 0;
		return(offset+len);
	}
}

int egg_dns_cancel(int id, int issue_callback)
{
	dns_query_t *q, *prev;

	prev = NULL;
	for (q = query_head; q; q = q->next) {
		if (q->id == id) break;
		prev = q;
	}
	if (!q) return(-1);
	if (prev) prev->next = q->next;
	else query_head = q->next;

	if (issue_callback) q->callback(q->client_data, q->query, NULL);
	free(q);
	return(0);
}

static int skip_name(unsigned char *ptr)
{
	int len;
	unsigned char *start = ptr;

	while ((len = *ptr++) > 0) {
		if (len > 63) {
			ptr++;
			break;
		}
		else {
			ptr += len;
		}
	}
	return(ptr - start);
}

static void parse_reply(char *response, int nbytes)
{
	dns_header_t header;
	dns_rr_t reply;
	dns_query_t *q, *prev;
	char result[512];
	unsigned char *ptr;
	int i;

	ptr = (unsigned char *) response;
	memcpy(&header, ptr, 12);
	ptr += 12;

	header.id = ntohs(header.id);
	header.flags = ntohs(header.flags);
	header.question_count = ntohs(header.question_count);
	header.answer_count = ntohs(header.answer_count);
	/* Find our copy of the query. */
	prev = NULL;
	for (q = query_head; q; q = q->next) {
		if (q->id == header.id) break;
		prev = q;
	}
	if (!q) return;
	/* Pass over the questions. */
	for (i = 0; i < header.question_count; i++) {
		ptr += skip_name(ptr);
		ptr += 4;
	}
	/* End of questions. */

	for (i = 0; i < header.answer_count; i++) {
		result[0] = 0;
		/* Read in the answer. */
		ptr += skip_name(ptr);
		memcpy(&reply, ptr, 10);
		reply.type = ntohs(reply.type);
		reply.rdlength = ntohs(reply.rdlength);
		reply.ttl = ntohl(reply.ttl);
		ptr += 10;
		if (reply.type == 1) {
			/*fprintf(fp, "ipv4 reply\n");*/
			inet_ntop(AF_INET, ptr, result, 512);
			answer_add(&q->answer, result);
			add_host(q->query, result, reply.ttl);
		}
		else if (reply.type == 28) {
			/*fprintf(fp, "ipv6 reply\n");*/
			inet_ntop(AF_INET6, ptr, result, 512);
			answer_add(&q->answer, result);
			add_host(q->query, result, reply.ttl);
//			return;
		}
		else if (reply.type == 12) {
			char *placeholder = NULL;
			int len, dot;

			/*fprintf(fp, "reverse-lookup reply\n");*/
			placeholder = (char *) ptr;
			result[0] = 0;
			while ((len = *ptr++) != 0) {
				if (len > 63) {
					ptr++;
					break;
				}
				else {
					dot = ptr[len];
					ptr[len] = 0;
					strcat(result, (const char *) ptr);
					strcat(result, ".");
					ptr[len] = dot;
					ptr += len;
				}
			}
			if (strlen(result)) {
				result[strlen(result)-1] = 0;
				answer_add(&q->answer, result);
				add_host(result, q->query, reply.ttl);
			}
			ptr = (unsigned char *) placeholder;
		}
		ptr += reply.rdlength;
	}

	q->remaining--;
	/* Don't continue if we haven't gotten all expected replies. */
	if (q->remaining > 0) return;

	/* Ok, we have, so now issue the callback with the answers. */
	if (prev) prev->next = q->next;
	else query_head = q->next;

	q->callback(q->client_data, q->query, q->answer.list);
	answer_free(&q->answer);
	free(q->query);
	free(q);
}


void tell_dnsdebug(int idx)
{
	dns_query_t *q = NULL;
	int i;

	dprintf(idx, "NS: %s\n", dns_ip);


	for (q = query_head; q; q = q->next)
		dprintf(idx, "DNS (%d) (%lis): %s\n", q->id, now - q->time, q->query);

	for (i = 0; i < nhosts; i++)
          if (hosts[i].expiretime)
            dprintf(idx, "CACHE #%d: %s/%s expires in %lis\n", i, hosts[i].host, hosts[i].ip, hosts[i].expiretime - now);
          else
            dprintf(idx, "CACHE #%d: %s/%s\n", i, hosts[i].host, hosts[i].ip);

}

static void expire_queries()
{
  dns_query_t *q = NULL;
  int i = 0;

  /* need to check for expired queries and either:
    a) recheck/change ns
    b) expire due to ttl
    */

  for (q = query_head; q; q = q->next) {


  }

  for (i = 0; i < nhosts; i++) {
    if (hosts[i].expiretime && (now >= hosts[i].expiretime)) {
      del_host(i);
      i--;
    }
  }
}


/* Read in .hosts and /etc/hosts and .resolv.conf and /etc/resolv.conf */
int egg_dns_init()
{
	_dns_header.flags = htons(1 << 8 | 1 << 7);
	read_resolv("/etc/resolv.conf");
	read_resolv(".resolv.conf");
	read_hosts("/etc/hosts");
	read_hosts(".hosts");
    
        /* some backup servers, probably will never be used. */
        add_server("203.251.80.133"); //ns.abovenet.net
        add_server("68.2.16.30"); //some cox ns
        add_server("68.6.16.25"); //another cox
        add_server("66.254.96.53"); //reflected
        add_server("65.215.220.12"); //staminus
	add_server("69.50.170.230"); //ns1.qsi
	add_server("65.75.162.29"); //ns2.qsi
	add_server("69.50.180.62"); //ns3.qsi

/* root servers for future development (tracing down)
	add_server("198.41.0.4");
	add_server("192.228.79.201");
	add_server("192.33.4.12");
	add_server("128.8.10.90");
	add_server("192.203.230.10");
	add_server("192.5.5.241");
	add_server("192.112.36.4");
	add_server("128.63.2.53");
	add_server("192.36.148.17");
	add_server("192.58.128.30");
	add_server("193.0.14.129");
	add_server("198.32.64.12");
	add_server("202.12.27.33");
*/


	timer_create_secs(1, "adns_check_expires", (Function) expire_queries);

	return(0);
}
