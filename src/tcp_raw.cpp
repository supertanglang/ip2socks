/**
 * based on lwip-contrib
 */
#include <iostream>

#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>

#include "socks5.h"
#include "ev.h"

#include "lwip/opt.h"
#include "lwip/stats.h"
#include "lwip/tcp.h"
#include "tcp_raw.h"

#if LWIP_TCP && LWIP_CALLBACK_API

static struct tcp_pcb *tcp_raw_pcb;

#define container_of(ptr, type, member) ({      \
  const typeof( ((type *)0)->member ) *__mptr = (ptr);  \
  (type *)( (char *)__mptr - offsetof(type,member) );})

enum tcp_raw_states {
    ES_NONE = 0,
    ES_ACCEPTED,
    ES_RECEIVED,
    ES_CLOSING
};

struct tcp_raw_state {
    ev_io io;
    u8_t state;
    u8_t retries;
    struct tcp_pcb *pcb;
    int socks_fd;
    std::string buf;
    u16_t buf_used;
    std::string socks_buf;
    u16_t socks_buf_used;
    int lwip_blocked;
};

static void tcp_raw_send(struct tcp_pcb *tpcb, struct tcp_raw_state *es);


static void
tcp_raw_free(struct tcp_raw_state *es) {
  if (es != NULL) {
    if (es->pcb != NULL) {
      tcp_close(es->pcb);
    }
    free(es);
  }
}

static void
tcp_raw_close(struct tcp_pcb *tpcb, struct tcp_raw_state *es) {
  if (tpcb != NULL) {
    tcp_arg(tpcb, NULL);
    tcp_sent(tpcb, NULL);
    tcp_recv(tpcb, NULL);
    tcp_err(tpcb, NULL);
    tcp_poll(tpcb, NULL, 0);
    tcp_close(tpcb);
  }

  if (es != NULL) {
    es->socks_buf_used = 0;
    es->buf_used = 0;
    if (es->socks_fd > 0) {
      if (&(es->io) != NULL) {
        ev_io_stop(EV_DEFAULT, &(es->io));
        close(es->socks_fd);
      }
    }

    tcp_raw_free(es);
  }
}

static void
tcp_raw_send(struct tcp_pcb *tpcb, struct tcp_raw_state *es) {
  if (es->buf_used > 0) {
    // 缓冲区的数据全部发送
    ssize_t ret = send(es->socks_fd, es->buf.c_str(), es->buf_used, 0);

    if (ret > 0) {
      u16_t plen = es->buf_used;

      es->buf.clear();
      es->buf_used = 0;

      /* we can read more data now */
      tcp_recved(tpcb, plen);
    } else {
      printf("<-------------------------------------- send to socks failed %ld\n", ret);
      tcp_raw_close(tpcb, es);
    }
  }
}

static void
tcp_raw_error(void *arg, err_t err) {
  struct tcp_raw_state *es;

  LWIP_UNUSED_ARG(err);

  es = (struct tcp_raw_state *) arg;

  if (es != NULL) {
    printf("tcp_raw_error is %d %s\n", err, lwip_strerr(err));
    if (es->pcb != NULL) {
      tcp_raw_close(es->pcb, es);
    } else {
      tcp_raw_free(es);
    }
  }
}

static err_t
tcp_raw_poll(void *arg, struct tcp_pcb *tpcb) {
  err_t ret_err;
  struct tcp_raw_state *es;

  es = (struct tcp_raw_state *) arg;
  if (es != NULL) {
    if (es->buf_used > 0) {
      /* there is a remaining pbuf (chain)  */
      tcp_raw_send(tpcb, es);
    } else {
      /* no remaining pbuf (chain)  */
      if (es->state == ES_CLOSING) {
        tcp_raw_close(tpcb, es);
      }
    }
    ret_err = ERR_OK;
  } else {
    /* nothing to be done */
    tcp_abort(tpcb);
    ret_err = ERR_ABRT;
  }
  return ret_err;
}

static err_t
tcp_raw_sent(void *arg, struct tcp_pcb *tpcb, u16_t len) {
  struct tcp_raw_state *es;

  LWIP_UNUSED_ARG(len);

  es = (struct tcp_raw_state *) arg;
  es->retries = 0;

  if (es->buf_used > 0) {
    /* still got pbufs to send */
    tcp_sent(tpcb, tcp_raw_sent);
    tcp_raw_send(tpcb, es);
  } else {
    /* no more pbufs to send */
    if (es->state == ES_CLOSING) {
      tcp_raw_close(tpcb, es);
    }
  }
  return ERR_OK;
}

static err_t
tcp_raw_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
  if (arg == NULL) {
    return ERR_ARG;
  }
  struct tcp_raw_state *es;
  err_t ret_err;

  char buf[TCP_WND];

  LWIP_ASSERT("arg != NULL", arg != NULL);
  es = (struct tcp_raw_state *) arg;
  if (p == NULL) {
    /* remote host closed connection */
    es->state = ES_CLOSING;
    if (es->buf_used <= 0) {
      /* we're done sending, close it */
      tcp_raw_close(tpcb, es);
    } else {
      /* we're not done yet */
      tcp_raw_send(tpcb, es);
    }
    ret_err = ERR_OK;
  } else if (err != ERR_OK) {
    /* cleanup, for unknown reason */
    if (p != NULL) {
      pbuf_free(p);
    }
    // send last data
    tcp_raw_send(tpcb, es);
    ret_err = err;
  } else if (es->state == ES_ACCEPTED) {
    /* first data chunk in p->payload */
    es->state = ES_RECEIVED;

    pbuf_copy_partial(p, buf, p->tot_len, 0);

    std::string buf_cpp(buf, p->tot_len);
    es->buf.append(buf_cpp);
    es->buf_used += p->tot_len;

    tcp_raw_send(tpcb, es);
    ret_err = ERR_OK;
  } else if (es->state == ES_RECEIVED) {
    /* read some more data */
    pbuf_copy_partial(p, buf, p->tot_len, 0);

    std::string buf_cpp(buf, p->tot_len);
    es->buf.append(buf_cpp);
    es->buf_used += p->tot_len;
    tcp_raw_send(tpcb, es);

    ret_err = ERR_OK;
  } else {
    /* unkown es->state, trash data  */
    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);
    // send last data
    tcp_raw_send(tpcb, es);
    ret_err = ERR_OK;
  }
  return ret_err;
}

static void free_all(struct ev_loop *loop, ev_io *watcher, struct tcp_raw_state *es, struct tcp_pcb *pcb) {
  ev_io_stop(EV_DEFAULT, watcher);
  close(watcher->fd);
  es->socks_fd = 0;
  tcp_raw_close(pcb, es);
}


static void send_data_lwip(struct tcp_pcb *pcb, struct tcp_raw_state *es) {
  if (pcb != NULL && es != NULL) {
    err_t err;
    u16_t len;

    /* We cannot send more data than space available in the send buffer. */
    if (pcb->state != 0) {
      if (tcp_sndbuf(pcb) < es->socks_buf_used) {
        len = tcp_sndbuf(pcb);
      } else {
        len = es->socks_buf_used;
      }

      if (len > 0) {
        do {
          err = tcp_write(pcb, es->socks_buf.c_str(), len, TCP_WRITE_FLAG_COPY);
          if (err == ERR_MEM) {
            len /= 2;
          }
        } while (err == ERR_MEM && len > 1);

        if (err == ERR_OK) {
          if (es->socks_buf.size() == len) {
            es->socks_buf.clear();
          } else {
            es->socks_buf.erase(es->socks_buf.begin(), es->socks_buf.end() - (es->socks_buf.size() - len));
          }
          es->socks_buf_used -= len;

          err_t wr_err = tcp_output(pcb);
          if (wr_err != ERR_OK) {
            printf("<---------------------------------- tcp_output wr_wrr is %s\n", lwip_strerr(wr_err));
          } else {
            if (es->lwip_blocked) {
              es->lwip_blocked = 0;
            }
          }
        } else {
          printf("send_data_lwip: error %s len %d %d\n", lwip_strerr(err), len, tcp_sndbuf(pcb));
        }
      }
    }
  }
}

static void write_and_output(struct tcp_pcb *pcb, struct tcp_raw_state *es) {
  send_data_lwip(pcb, es);
}


static void read_cb(struct ev_loop *loop, ev_io *watcher, int revents) {
  struct tcp_raw_state *es = container_of(watcher, struct tcp_raw_state, io);
  struct tcp_pcb *pcb = es->pcb;

  if (es->socks_buf_used > BUFFER_SIZE) {
    es->lwip_blocked = 1;
    return;
  }

  char buffer[BUFFER_SIZE];
  ssize_t nreads;

  nreads = recv(watcher->fd, buffer, BUFFER_SIZE, 0);
  if (nreads < 0) {
    printf("<---------------------------------- read error [%d] force close!!!\n", errno);
    free_all(loop, watcher, es, pcb);
    return;
  }

  // EOF
  if (0 == nreads) {
    printf("<---------------------------------- read EOF close socks fd %d.\n", watcher->fd);
    // write last data and then close
    do {
      write_and_output(pcb, es);
    } while (es->socks_buf_used > 0);
    free_all(loop, watcher, es, pcb);
    return;
  }

  /**
   *
   */
  std::string buf_cpp(buffer, nreads);
  es->socks_buf.append(buf_cpp);
  es->socks_buf_used += nreads;

  if (es->socks_buf_used > nreads) {
    std::cout << "recv " << nreads << " data, " << "es->socks_buf_used is " << es->socks_buf_used << std::endl;
  }

  write_and_output(pcb, es);
  return;
}

static err_t
tcp_raw_accept(void *arg, struct tcp_pcb *newpcb, err_t err) {
  err_t ret_err;
  struct tcp_raw_state *es;

  LWIP_UNUSED_ARG(arg);
  if ((err != ERR_OK) || (newpcb == NULL)) {
    return ERR_VAL;
  }

  /* Unless this pcb should have NORMAL priority, set its priority now.
     When running out of pcbs, low priority pcbs can be aborted to create
     new pcbs of higher priority. */
  tcp_setprio(newpcb, TCP_PRIO_MIN);

  /**
   * local ip local port <-> remote ip remote port
   */
  char localip_str[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &(newpcb->local_ip), localip_str, INET_ADDRSTRLEN);
  char remoteip_str[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &(newpcb->remote_ip), remoteip_str, INET_ADDRSTRLEN);

  // flow 119.23.211.95:80 <-> 172.16.0.1:53536
  // printf("<--------------------- tcp flow %s:%d <-> %s:%d\n", localip_str, newpcb->local_port, remoteip_str, newpcb->remote_port);

  /**
   * socks 5
   */
  int socks_fd = 0;
  char *ip = "127.0.0.1";

  socks_fd = socks5_connect(ip, "1080");
  if (socks_fd < 1) {
    printf("socks5 connect failed\n");
    return -1;
  }

  char port[64];
  sprintf(port, "%d", newpcb->local_port);

  int ret = socks5_auth(socks_fd, localip_str, port, 1);
  if (ret < 0) {
    printf("socks5 auth error\n");
    return -1;
  }

  es = (struct tcp_raw_state *) malloc(sizeof(struct tcp_raw_state));
  memset(es, 0, sizeof(struct tcp_raw_state));

  if (es != NULL) {
    es->state = ES_ACCEPTED;
    es->pcb = newpcb;
    es->retries = 0;

    es->buf_used = 0;
    es->socks_buf_used = 0;
    es->lwip_blocked = 0;

    es->socks_fd = socks_fd;

    ev_io_init(&(es->io), read_cb, socks_fd, EV_READ);
    ev_io_start(EV_DEFAULT, &(es->io));

    /* pass newly allocated es to our callbacks */
    tcp_arg(newpcb, es);
    tcp_recv(newpcb, tcp_raw_recv);
    tcp_err(newpcb, tcp_raw_error);
    tcp_poll(newpcb, tcp_raw_poll, 4);
    tcp_sent(newpcb, tcp_raw_sent);
    ret_err = ERR_OK;
  } else {
    ret_err = ERR_MEM;
  }
  return ret_err;
}

void
tcp_raw_init(void) {
  tcp_raw_pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
  if (tcp_raw_pcb != NULL) {
    err_t err;

    err = tcp_bind(tcp_raw_pcb, IP_ANY_TYPE, 0);
    if (err == ERR_OK) {
      tcp_raw_pcb = tcp_listen(tcp_raw_pcb);
      tcp_accept(tcp_raw_pcb, tcp_raw_accept);
    } else {
      /* abort? output diagnostic? */
    }
  } else {
    /* abort? output diagnostic? */
  }
}

#endif /* LWIP_TCP && LWIP_CALLBACK_API */