#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <uv.h>

#define CHECK(expression, ...)			\
	if ((err = expression) != 0) {		\
		__VA_ARGS__;			\
		goto cleanup;			\
	}

enum log_level {
	LOG_NONE,
	LOG_ERROR,
	LOG_WARN,
	LOG_INFO,
	LOG_DEBUG,
};

enum pipe_state {
	PIPE_READY,
	PIPE_BUSY,
	PIPE_DONE
};

struct pipe {
	unsigned int connecting:1;
	unsigned int rdstate:2;
	unsigned int wrstate:2;

	uv_tcp_t   tcp;
	uv_timer_t timer;

	union {
		uv_req_t    req;
		uv_connect_t crq;
		uv_write_t   wrq;
	};

	struct tunnel *tunnel;

	uint8_t	rxbuf[65536];
	size_t  rxlen;
};

enum tunnel_state {
	TUNNEL_STATE_INIT,
	TUNNEL_STATE_SWITCH,
	TUNNEL_STATE_CONNECT,
	TUNNEL_STATE_PREPARE,
	TUNNEL_STATE_STREAM
};

struct tunnel {
	int refs;
	enum tunnel_state state;
	struct pipe incoming;
	struct pipe outgoing;

	uv_stream_t *listener;
};

static const char *cl_bind_addr = NULL;
static int         cl_bind_port = 0;
static const char *cl_http_addr = NULL;
static int         cl_http_port = 0;
static const char *cl_socks_addr = NULL;
static int         cl_socks_port = 0;
static int         cl_log_level = LOG_INFO;

static void pipe_close(struct pipe *pipe);
static int  pipe_connect(struct pipe *pipe, const char *host, int port);
static int  pipe_read_start(struct pipe *pipe);
static int  pipe_read_stop(struct pipe *pipe);
static int  pipe_write(struct pipe *pipe, const void *buffer, size_t length);
static int  pipe_timer_once(struct pipe *pipe, uint32_t timeout);
static int  pipe_timer_stop(struct pipe *pipe);

static int append_formatv(char **o_buf, size_t *o_len, const char *format, va_list args)
{
	int ret = 0;

	if (format[0] != '\0') {
		ret = vsnprintf(*o_buf, *o_len, format, args);
		if (ret < 0 || (size_t) ret >= *o_len) {
			va_end(args);
			return 1;
		}

		*o_buf += ret;
		*o_len -= ret;
	}

	return 0;
}

static int append_format(char **o_buf, size_t *o_len, const char *format, ...)
{
	va_list args;
	int res = 0;

	va_start(args, format);
	res = append_formatv(o_buf, o_len, format, args);
	va_end(args);

	return res;
}

static void log_msgv(void *context, enum log_level level,
		     const char *format, va_list args)
{
	char buffer[1024];
	char *p = buffer;
	const char *label = NULL;
	size_t n = sizeof buffer;

	if ((int) level > cl_log_level) {
		return;
	}

	switch (level) {
		case LOG_ERROR: label = "ERROR"; break;
		case LOG_WARN:  label = "WARN"; break;
		case LOG_INFO:  label = "INFO"; break;
		case LOG_DEBUG: label = "DEBUG"; break;
		default: label = "?????"; break;
	}

	if (context) {
		append_format(&p, &n, "%-6s: [%p] ", label, context);
	} else {
		append_format(&p, &n, "%-6s: ", label);
	}
	append_formatv(&p, &n, format, args);
	append_format(&p, &n, "\n");
	fputs(buffer, stderr);
}

void log_error(void *context, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	log_msgv(context, LOG_ERROR, format, args);
	va_end(args);
}

void log_warn(void *context, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	log_msgv(context, LOG_WARN, format, args);
	va_end(args);
}

void log_info(void *context, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	log_msgv(context, LOG_INFO, format, args);
	va_end(args);
}

void log_debug(void *context, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	log_msgv(context, LOG_DEBUG, format, args);
	va_end(args);
}

static struct tunnel *tunnel_alloc(void)
{
	struct tunnel *tun = NULL;

	tun = calloc(1, sizeof *tun);
	if (tun == NULL) {
		return NULL;
	}

	tun->refs = 1;
	tun->state = TUNNEL_STATE_INIT;

	return tun;
}

static void tunnel_ref(struct tunnel *tun)
{
	tun->refs++;
}

static void tunnel_unref(struct tunnel *tun)
{
	tun->refs--;
	log_debug(tun, "tunnel_unref -> %i", tun->refs);

	if (tun->refs == 0) {
		log_debug(tun, "deallocating tunnel...");
		free(tun);
	}
}

static void tunnel_close(struct tunnel *tun)
{
	pipe_close(&tun->incoming);
	pipe_close(&tun->outgoing);
}

static int tunnel_stream(struct pipe *from, struct pipe *to)
{
	int err = 0;

	if (from->rdstate == PIPE_READY) {
		err = pipe_read_start(from);
	} else if (from->rdstate == PIPE_BUSY) {
		;
	} else if (from->rdstate == PIPE_DONE) {
		from->rdstate = PIPE_READY;
		log_debug(from->tunnel, "writing from %p to %p: %zu bytes",
			  from, to, from->rxlen);
		err = pipe_write(to, from->rxbuf, from->rxlen);
	}

	return err;
}

static void tunnel_step(struct tunnel *tun)
{
	int err = 0;
	struct pipe *in = &tun->incoming;
	struct pipe *out = &tun->outgoing;

	log_debug(tun, "in tunnel_step: state=%i", tun->state);

	switch (tun->state) {
	case TUNNEL_STATE_INIT:
		CHECK(pipe_read_start(&tun->incoming));
		tun->state = TUNNEL_STATE_SWITCH;
		break;
	case TUNNEL_STATE_SWITCH:
		if (in->rxbuf[0] == 4 || in->rxbuf[0] == 5) {
			err = pipe_connect(out, cl_socks_addr, cl_socks_port);
		} else {
			err = pipe_connect(out, cl_http_addr, cl_http_port);
		}

		if (err == 0) {
			tun->state = TUNNEL_STATE_CONNECT;
		}
		break;
	case TUNNEL_STATE_CONNECT:
		CHECK(pipe_write(out, in->rxbuf, in->rxlen));
		tun->state = TUNNEL_STATE_PREPARE;
		break;
	case TUNNEL_STATE_PREPARE:
		CHECK(pipe_read_start(in));
		CHECK(pipe_read_start(out));
		tun->state = TUNNEL_STATE_STREAM;
		break;
	case TUNNEL_STATE_STREAM:
		CHECK(tunnel_stream(in, out));
		CHECK(tunnel_stream(out, in));
		break;
	default:
		err = UV_EINVAL;
		break;
	}

cleanup:
	if (err) {
		log_error(tun, "in %s: error %s", __func__, uv_strerror(err));
		tunnel_close(tun);
	}
}

static void on_pipe_close(uv_handle_t *handle)
{
	struct pipe *pipe = handle->data;

	log_debug(pipe, "in on_pipe_close");
	tunnel_unref(pipe->tunnel);
}

static void pipe_close(struct pipe *pipe)
{
	if (pipe->connecting) {
		uv_cancel(&pipe->req);
	}
	if (pipe->tcp.data) {
		uv_close((uv_handle_t *) &pipe->tcp, on_pipe_close);
	}
	if (pipe->timer.data) {
		uv_close((uv_handle_t *) &pipe->timer, on_pipe_close);
	}
}

static int pipe_init(struct pipe *pipe, struct tunnel *tun)
{
	int err = 0;

	pipe->tunnel = tun;

	CHECK(uv_tcp_init(tun->listener->loop, &pipe->tcp));
	pipe->tcp.data = pipe;
	tunnel_ref(tun);

	CHECK(uv_timer_init(tun->listener->loop, &pipe->timer));
	pipe->timer.data = pipe;
	tunnel_ref(tun);

	pipe->req.data = pipe;

cleanup:
	if (err) {
		log_error(tun, "in %s: error %s", __func__, uv_strerror(err));
		pipe_close(pipe);
	}
	return err;
}

static void pipe_on_alloc(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf)
{
	struct pipe *pipe = handle->data;

        (void) suggested_size;

	buf->base = (char *) pipe->rxbuf;
	buf->len  = sizeof pipe->rxbuf;
}

static void pipe_on_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
	struct pipe *pipe = stream->data;
	struct tunnel *tun = pipe->tunnel;

        (void) buf;

	log_debug(tun, "in pipe_on_read: %i", (int) nread);

	pipe_read_stop(pipe);
	pipe->rdstate = PIPE_DONE;

	if (nread <= 0) {
		if (nread != UV_EOF) {
			log_warn(tun, "pipe: read error: %s", uv_strerror(nread));
		}
		tunnel_close(tun);
		return;
	}

	pipe->rxlen = (size_t) nread;
	tunnel_step(tun);
}

static void pipe_on_write(uv_write_t *write, int status)
{
	struct pipe *pipe = write->data;
	struct tunnel *tun = pipe->tunnel;

	log_debug(tun, "in pipe_on_write: %i", status);

	assert(pipe->wrstate == PIPE_BUSY);
	pipe->wrstate = PIPE_DONE;

	if (status != 0) {
		log_warn(tun, "pipe: write error: %s", uv_strerror(status));
		tunnel_close(tun);
	} else {
		tunnel_step(tun);
	}
}

static void pipe_on_timeout(uv_timer_t *timer)
{
	struct pipe *pipe = timer->data;
	struct tunnel *tun = pipe->tunnel;

	log_debug(tun, "pipe timeout");
}

static int pipe_read_start(struct pipe *pipe)
{
	int err = 0;

	assert(pipe->rdstate == PIPE_READY || pipe->rdstate == PIPE_DONE);

	CHECK(uv_read_start((uv_stream_t *) &pipe->tcp,
			    pipe_on_alloc, pipe_on_read));
	pipe->rdstate = PIPE_BUSY;

cleanup:
	if (err) {
		log_error(pipe->tunnel, "in %s: error %s", __func__, uv_strerror(err));
	}

	return err;
}

static int pipe_read_stop(struct pipe *pipe)
{
	int err = 0;

	assert(pipe->rdstate == PIPE_BUSY);
	CHECK(uv_read_stop((uv_stream_t *) &pipe->tcp));
	pipe->rdstate = PIPE_DONE;

cleanup:
	if (err) {
		log_error(pipe->tunnel, "in %s: error %s", __func__, uv_strerror(err));
	}

	return err;
}

static int pipe_write(struct pipe *pipe, const void *buffer, size_t length)
{
	int err = 0;
	uv_buf_t buf = {
		.base = (void *) buffer,
		.len = length
	};

	assert(pipe->wrstate == PIPE_READY || pipe->wrstate == PIPE_DONE);
	CHECK(uv_write(&pipe->wrq, (uv_stream_t *) &pipe->tcp, &buf, 1, pipe_on_write));
	pipe->wrstate = PIPE_BUSY;

cleanup:
	if (err) {
		log_error(pipe->tunnel, "in %s: error %s", __func__, uv_strerror(err));
	}

	return err;
}

static int pipe_timer_once(struct pipe *pipe, uint32_t timeout)
{
	return uv_timer_start(&pipe->timer, pipe_on_timeout, timeout, 0);
}

static int pipe_timer_stop(struct pipe *pipe)
{
	return uv_timer_stop(&pipe->timer);
}

static void pipe_on_connect(uv_connect_t *connect, int status)
{
	struct pipe *pipe = connect->data;
	struct tunnel *tun = pipe->tunnel;

	log_debug(tun, "in pipe_on_connect: %i", status);

	assert(pipe->connecting);
	pipe->connecting = false;
	tunnel_unref(tun);

	pipe_timer_stop(pipe);

	if (status != 0) {
		log_warn(tun, "pipe: connect error: %s", uv_strerror(status));
		tunnel_close(tun);
	} else {
		tunnel_step(tun);
	}
}

static int pipe_connect(struct pipe *pipe, const char *host, int port)
{
	int err = 0;
	struct sockaddr_in addr;

	pipe->crq.data = pipe;
	CHECK(uv_ip4_addr(host, port, &addr));
	CHECK(uv_tcp_connect(&pipe->crq, &pipe->tcp,
			     (struct sockaddr *) &addr, pipe_on_connect));

	pipe_timer_once(pipe, 20 * 1000);

	pipe->connecting = true;
	tunnel_ref(pipe->tunnel);

cleanup:
	if (err) {
		log_error(pipe->tunnel, "in %s: error %s", __func__, uv_strerror(err));
	}

	return err;
}

static int tunnel_init(struct tunnel *tun, uv_stream_t *server)
{
	int err = 0;

	tun->listener = server;
	CHECK(pipe_init(&tun->incoming, tun));
	CHECK(pipe_init(&tun->outgoing, tun));

	CHECK(uv_accept(server, (uv_stream_t *) &tun->incoming.tcp), {
			log_error("uv_accept failed: %s", uv_strerror(err));
		 });

cleanup:
	if (err) {
		log_error(tun, "in %s: error %s", __func__, uv_strerror(err));
	}

	return err;
}

static void on_accept(uv_stream_t *server, int status)
{
	int err = 0;
	struct tunnel *tun = NULL;

	log_debug(server, "in on_accept: %s", status == 0 ? "ok" : uv_strerror(status));

	if (status != 0) {
		log_warn(server, "failed to accept: %s", uv_strerror(status));
		return;
	}

	if ((tun = tunnel_alloc()) == NULL) {
		log_error(server, "accept: failed to allocate memory");
		abort();
		return;
	}

	CHECK(tunnel_init(tun, server));
	tunnel_step(tun);

cleanup:
	tunnel_unref(tun);

	if (err) {
		log_error(tun, "in %s: error %s", __func__, uv_strerror(err));
		tunnel_close(tun);
	}
}

static void on_signal(uv_signal_t *signal, int signo)
{
	if (cl_log_level > LOG_NONE) {
		fprintf(stderr, "\n");
	}

	log_debug(signal, "on_signal: %i", signo);
	uv_stop(signal->loop);
}

static void on_walk_cb(uv_handle_t *handle, void *arg)
{
	log_debug(arg, " - handle %p (%s)", handle, uv_handle_type_name(handle->type));

	switch (uv_handle_get_type(handle)) {
	case UV_SIGNAL:
		uv_close(handle, NULL);
		break;
	case UV_TIMER:
		uv_close(handle, on_pipe_close);
		break;
	case UV_TCP:
		if (handle->data == NULL) {
			/* Server socket. */
			uv_close(handle, NULL);
		} else {
			uv_close(handle, on_pipe_close);
		}
		break;
	default:
		break;
	}
}

static void usage(const char **argv)
{
	fprintf(stderr, "USAGE: %s  [-d LEVEL] -I ADDR -i PORT -H HOST -h PORT -S HOST -s PORT\n", argv[0]);
	fprintf(stderr, "OPTIONS:\n");
	fprintf(stderr, "   -I ADDR -i PORT   Listen on ADDR and PORT\n");
	fprintf(stderr, "   -H ADDR -h PORT   Proxy HTTP connections to ADDR:PORT\n");
	fprintf(stderr, "   -S ADDR -s PORT   Proxy SOCKS connections to ADDR:PORT\n");
	fprintf(stderr, "   -d LEVEL          Debug level: 0-NONE, 1-ERROR, 2-WARN, 3-INFO, 4-DEBUG\n");
}

static int parse_options(int argc, const char **argv)
{
	int opt = 0;

        if (argc <= 1) {
            usage(argv);
            return EXIT_SUCCESS;
        }

	while ((opt = getopt(argc, (char **) argv, "d:I:i:H:h:S:s:")) != -1) {
		switch (opt) {
		case 'd': cl_log_level = strtoul(optarg, NULL, 10); break;
		case 'I': cl_bind_addr = optarg; break;
		case 'i': cl_bind_port = strtoul(optarg, NULL, 10); break;
		case 'H': cl_http_addr = optarg; break;
		case 'h': cl_http_port = strtoul(optarg, NULL, 10); break;
		case 'S': cl_socks_addr = optarg; break;
		case 's': cl_socks_port = strtoul(optarg, NULL, 10); break;
		default:
			usage(argv);
			return EXIT_FAILURE;
		}
	}

	if (cl_bind_addr == NULL || cl_bind_port == 0 ||
	    cl_http_addr == NULL || cl_http_port == 0 ||
	    cl_socks_addr == NULL || cl_socks_port == 0) {
		fprintf(stderr, "ERROR: insufficient arguments\n");
		usage(argv);
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}

int main(int argc, const char **argv)
{
	int err = 0;
	int ret = 0;
	struct sockaddr_in addr;
	uv_loop_t loop;
	uv_tcp_t  server;
	uv_signal_t sigint;
	uv_signal_t sigterm;

	if ((err = parse_options(argc, argv)) != EXIT_SUCCESS) {
		return err;
	}

	uv_loop_init(&loop);

	uv_signal_init(&loop, &sigint);
	uv_signal_start(&sigint, on_signal, SIGINT);
	uv_signal_init(&loop, &sigterm);
	uv_signal_start(&sigterm, on_signal, SIGTERM);

	uv_tcp_init(&loop, &server);
	server.data = NULL;
	CHECK(uv_ip4_addr(cl_bind_addr, cl_bind_port, &addr));
	CHECK(uv_tcp_bind(&server, (struct sockaddr *) &addr, 0));
	uv_listen((uv_stream_t *) &server, 128, on_accept);

	log_info(NULL, "starting...");
	ret = uv_run(&loop, UV_RUN_DEFAULT);
	log_debug(NULL, "uv_run[%i] = %i", 1, ret);

	log_info(NULL, "terminating...");

	/* Close all handles. */
	uv_walk(&loop, on_walk_cb, NULL);

	ret = uv_run(&loop, UV_RUN_DEFAULT);
	log_debug(NULL, "uv_run[%i] = %i", 2, ret);

	uv_loop_close(&loop);
	log_info(NULL, "terminated");

cleanup:
	return err ? EXIT_FAILURE : EXIT_SUCCESS;
}
