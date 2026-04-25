#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_CLIENT_COUNT 8
#define DEFAULT_PAYLOAD_SIZE 1024
#define DEFAULT_TIMEOUT_MS 15000

static volatile sig_atomic_t stop_requested;

struct echo_connection_arg {
	int sock;
};

struct client_thread_arg {
	const char *host;
	uint16_t port;
	int client_idx;
	int payload_size;
	int timeout_ms;
	int result;
	char error[128];
};

static long get_time_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);

	return (ts.tv_sec * 1000L) + (ts.tv_nsec / 1000000L);
}

static int send_all(int sock, const uint8_t *buf, size_t len)
{
	size_t sent_len = 0;

	while (sent_len < len) {
		ssize_t ret;

		ret = send(sock,
			   buf + sent_len,
			   len - sent_len,
			   MSG_NOSIGNAL);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}

		if (ret == 0) {
			errno = EPIPE;
			return -1;
		}

		sent_len += ret;
	}

	return 0;
}

static int recv_all(int sock, uint8_t *buf, size_t len, int timeout_ms)
{
	size_t recv_len = 0;
	long deadline_ms = get_time_ms() + timeout_ms;

	while (recv_len < len) {
		struct pollfd pfd = {
			.fd = sock,
			.events = POLLIN,
		};
		long remaining_ms = deadline_ms - get_time_ms();
		ssize_t ret;

		if (remaining_ms <= 0) {
			errno = ETIMEDOUT;
			return -1;
		}

		ret = poll(&pfd, 1, remaining_ms);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}

		if (ret == 0) {
			errno = ETIMEDOUT;
			return -1;
		}

		ret = recv(sock, buf + recv_len, len - recv_len, 0);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}

		if (ret == 0) {
			errno = ECONNRESET;
			return -1;
		}

		recv_len += ret;
	}

	return 0;
}

static void fill_payload(uint8_t *payload, size_t len, int client_idx)
{
	size_t i;
	char prefix[64];
	int prefix_len;

	prefix_len = snprintf(prefix,
			      sizeof(prefix),
			      "client-%02d:",
			      client_idx);
	if (prefix_len < 0)
		prefix_len = 0;

	for (i = 0; i < len; i++) {
		if ((size_t)prefix_len > i) {
			payload[i] = prefix[i];
			continue;
		}

		payload[i] = 'a' + ((client_idx + i) % 26);
	}
}

static int connect_with_retry(const char *host, uint16_t port, int timeout_ms)
{
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port = htons(port),
	};
	struct timespec retry_sleep = {.tv_nsec = 100000000};
	long deadline_ms = get_time_ms() + timeout_ms;

	if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
		errno = EINVAL;
		return -1;
	}

	for (;;) {
		int sock;
		int ret;

		sock = socket(AF_INET, SOCK_STREAM, 0);
		if (sock < 0)
			return -1;

		ret = connect(sock, (const struct sockaddr *)&addr, sizeof(addr));
		if (!ret)
			return sock;

		close(sock);
		if (get_time_ms() >= deadline_ms)
			return -1;

		(void)nanosleep(&retry_sleep, NULL);
	}
}

static void *echo_connection_thread_fn(void *arg)
{
	struct echo_connection_arg *conn = arg;
	uint8_t buf[4096];

	fprintf(stderr, "echo server accepted connection\n");

	for (;;) {
		ssize_t ret;

		ret = recv(conn->sock, buf, sizeof(buf), 0);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			break;
		}

		if (ret == 0)
			break;

		if (send_all(conn->sock, buf, ret))
			break;
	}

	close(conn->sock);
	fprintf(stderr, "echo server closed connection\n");
	free(conn);

	return NULL;
}

static void signal_handler(int signo)
{
	(void)signo;
	stop_requested = 1;
}

static int run_echo_server(uint16_t port)
{
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port = htons(port),
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};
	struct sigaction sa = {0};
	int listen_sock;
	int enable = 1;
	int ret;

	sa.sa_handler = signal_handler;
	sigemptyset(&sa.sa_mask);
	(void)sigaction(SIGINT, &sa, NULL);
	(void)sigaction(SIGTERM, &sa, NULL);

	listen_sock = socket(AF_INET, SOCK_STREAM, 0);
	if (listen_sock < 0) {
		perror("socket");
		return EXIT_FAILURE;
	}

	ret = setsockopt(listen_sock,
			 SOL_SOCKET,
			 SO_REUSEADDR,
			 &enable,
			 sizeof(enable));
	if (ret) {
		perror("setsockopt");
		close(listen_sock);
		return EXIT_FAILURE;
	}

	ret = bind(listen_sock, (const struct sockaddr *)&addr, sizeof(addr));
	if (ret) {
		perror("bind");
		close(listen_sock);
		return EXIT_FAILURE;
	}

	ret = listen(listen_sock, 64);
	if (ret) {
		perror("listen");
		close(listen_sock);
		return EXIT_FAILURE;
	}

	fprintf(stderr, "echo server listening on 127.0.0.1:%u\n", port);

	while (!stop_requested) {
		struct pollfd pfd = {
			.fd = listen_sock,
			.events = POLLIN,
		};

		ret = poll(&pfd, 1, 200);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			perror("poll");
			close(listen_sock);
			return EXIT_FAILURE;
		}

		if (ret == 0)
			continue;

		if (pfd.revents & POLLIN) {
			struct echo_connection_arg *conn;
			pthread_t thread;
			int client_sock;

			client_sock = accept(listen_sock, NULL, NULL);
			if (client_sock < 0) {
				if (errno == EINTR)
					continue;
				perror("accept");
				close(listen_sock);
				return EXIT_FAILURE;
			}

			conn = calloc(1, sizeof(*conn));
			if (!conn) {
				close(client_sock);
				close(listen_sock);
				return EXIT_FAILURE;
			}

			conn->sock = client_sock;
			ret = pthread_create(&thread,
					     NULL,
					     echo_connection_thread_fn,
					     conn);
			if (ret) {
				fprintf(stderr, "pthread_create failed: %d\n", ret);
				close(client_sock);
				free(conn);
				close(listen_sock);
				return EXIT_FAILURE;
			}

			(void)pthread_detach(thread);
		}
	}

	close(listen_sock);
	return EXIT_SUCCESS;
}

static void *client_thread_fn(void *arg)
{
	struct client_thread_arg *client = arg;
	uint8_t *payload;
	uint8_t *response;
	int sock;

	payload = calloc(client->payload_size, 1);
	response = calloc(client->payload_size, 1);
	if (!payload || !response) {
		snprintf(client->error,
			 sizeof(client->error),
			 "allocation failed");
		free(payload);
		free(response);
		client->result = -1;
		return NULL;
	}

	fill_payload(payload, client->payload_size, client->client_idx);

	sock = connect_with_retry(client->host, client->port, client->timeout_ms);
	if (sock < 0) {
		snprintf(client->error,
			 sizeof(client->error),
			 "connect failed: errno %d",
			 errno);
		free(payload);
		free(response);
		client->result = -1;
		return NULL;
	}

	if (send_all(sock, payload, client->payload_size)) {
		snprintf(client->error,
			 sizeof(client->error),
			 "send failed: errno %d",
			 errno);
		close(sock);
		free(payload);
		free(response);
		client->result = -1;
		return NULL;
	}

	if (recv_all(sock, response, client->payload_size, client->timeout_ms)) {
		snprintf(client->error,
			 sizeof(client->error),
			 "recv failed: errno %d",
			 errno);
		close(sock);
		free(payload);
		free(response);
		client->result = -1;
		return NULL;
	}

	if (memcmp(payload, response, client->payload_size) != 0) {
		snprintf(client->error,
			 sizeof(client->error),
			 "echo mismatch");
		close(sock);
		free(payload);
		free(response);
		client->result = -1;
		return NULL;
	}

	close(sock);
	free(payload);
	free(response);
	client->result = 0;

	return NULL;
}

static int run_multi_client_test(const char *host,
				 uint16_t port,
				 int num_clients,
				 int payload_size,
				 int timeout_ms)
{
	struct client_thread_arg *clients;
	pthread_t *threads;
	int i;
	int ret = EXIT_SUCCESS;

	clients = calloc(num_clients, sizeof(*clients));
	threads = calloc(num_clients, sizeof(*threads));
	if (!clients || !threads) {
		free(clients);
		free(threads);
		fprintf(stderr, "unable to allocate client threads\n");
		return EXIT_FAILURE;
	}

	for (i = 0; i < num_clients; i++) {
		clients[i].host = host;
		clients[i].port = port;
		clients[i].client_idx = i;
		clients[i].payload_size = payload_size;
		clients[i].timeout_ms = timeout_ms;
		clients[i].result = -1;

		if (pthread_create(&threads[i], NULL, client_thread_fn, &clients[i])) {
			fprintf(stderr, "unable to start client thread %d\n", i);
			clients[i].result = -1;
			ret = EXIT_FAILURE;
			num_clients = i;
			break;
		}
	}

	for (i = 0; i < num_clients; i++)
		(void)pthread_join(threads[i], NULL);

	for (i = 0; i < num_clients; i++) {
		if (clients[i].result == 0)
			continue;

		fprintf(stderr,
			"client %d failed: %s\n",
			i,
			clients[i].error[0] ? clients[i].error : "unknown error");
		ret = EXIT_FAILURE;
	}

	if (ret == EXIT_SUCCESS) {
		fprintf(stderr,
			"verified %d simultaneous clients via %s:%u\n",
			num_clients,
			host,
			port);
	}

	free(clients);
	free(threads);

	return ret;
}

static int parse_int_arg(const char *name, const char *value, int min_value)
{
	char *endptr = NULL;
	long parsed;

	parsed = strtol(value, &endptr, 10);
	if (!value[0] || (endptr && *endptr) || parsed < min_value || parsed > 65535) {
		fprintf(stderr, "invalid %s: %s\n", name, value);
		return -1;
	}

	return (int)parsed;
}

static void print_usage(const char *prog)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s echo-server --port <port>\n"
		"  %s multi-client --host <ipv4> --port <port> [--clients <n>] "
		"[--payload-size <bytes>] [--timeout-ms <ms>]\n",
		prog,
		prog);
}

int main(int argc, char **argv)
{
	const char *mode;

	if (argc < 2) {
		print_usage(argv[0]);
		return EXIT_FAILURE;
	}

	mode = argv[1];

	if (strcmp(mode, "echo-server") == 0) {
		uint16_t port = 0;
		int i;

		for (i = 2; i < argc; i++) {
			if ((strcmp(argv[i], "--port") == 0) && (i + 1 < argc)) {
				int parsed = parse_int_arg("port", argv[++i], 1);

				if (parsed < 0)
					return EXIT_FAILURE;
				port = parsed;
				continue;
			}

			print_usage(argv[0]);
			return EXIT_FAILURE;
		}

		if (!port) {
			print_usage(argv[0]);
			return EXIT_FAILURE;
		}

		return run_echo_server(port);
	}

	if (strcmp(mode, "multi-client") == 0) {
		const char *host = "127.0.0.1";
		uint16_t port = 0;
		int num_clients = DEFAULT_CLIENT_COUNT;
		int payload_size = DEFAULT_PAYLOAD_SIZE;
		int timeout_ms = DEFAULT_TIMEOUT_MS;
		int i;

		for (i = 2; i < argc; i++) {
			if ((strcmp(argv[i], "--host") == 0) && (i + 1 < argc)) {
				host = argv[++i];
				continue;
			}

			if ((strcmp(argv[i], "--port") == 0) && (i + 1 < argc)) {
				int parsed = parse_int_arg("port", argv[++i], 1);

				if (parsed < 0)
					return EXIT_FAILURE;
				port = parsed;
				continue;
			}

			if ((strcmp(argv[i], "--clients") == 0) && (i + 1 < argc)) {
				num_clients = parse_int_arg("clients", argv[++i], 1);
				if (num_clients < 0)
					return EXIT_FAILURE;
				continue;
			}

			if ((strcmp(argv[i], "--payload-size") == 0) && (i + 1 < argc)) {
				payload_size = parse_int_arg("payload-size", argv[++i], 1);
				if (payload_size < 0)
					return EXIT_FAILURE;
				continue;
			}

			if ((strcmp(argv[i], "--timeout-ms") == 0) && (i + 1 < argc)) {
				timeout_ms = parse_int_arg("timeout-ms", argv[++i], 1);
				if (timeout_ms < 0)
					return EXIT_FAILURE;
				continue;
			}

			print_usage(argv[0]);
			return EXIT_FAILURE;
		}

		if (!port) {
			print_usage(argv[0]);
			return EXIT_FAILURE;
		}

		return run_multi_client_test(host,
					     port,
					     num_clients,
					     payload_size,
					     timeout_ms);
	}

	print_usage(argv[0]);
	return EXIT_FAILURE;
}
