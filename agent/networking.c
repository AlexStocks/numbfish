
/**
 * Tencent is pleased to support the open source community by making MSEC available.
 *
 * Copyright (C) 2016 THL A29 Limited, a Tencent company. All rights reserved.
 *
 * Licensed under the GNU General Public License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License. You may
 * obtain a copy of the License at
 *
 *     https://opensource.org/licenses/GPL-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed under the
 * License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
 * either express or implied. See the License for the specific language governing permissions
 * and limitations under the License.
 */


#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
// #include <sys/select.h>
#include <sys/epoll.h>
#include <sys/time.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include "log.h"
#include "config.h"
#include "nlbapi.h"
#include "networking.h"
#include "zkplugin.h"
#include "utils.h"
#include "nlbtime.h"
#include "routeprocess.h"

#define EVENT_NUM 64

/* 网络管理数据结构 */
struct netmng {
    uint64_t timeout;
    int32_t  listen_fd;

    int epfd;
    int ev_ready;
    int evlist_size;
    struct epoll_event *evlist;
};

static struct netmng net_mng = {
    .timeout = 10,        /* 默认10毫秒超时 */
    .listen_fd = -1,        /* agent监听fd */
    .epfd = 0,
    .ev_ready = 0,
    .evlist_size = 0,
    .evlist = NULL
};

/* 获取agent监听套接字 */
int32_t get_listen_fd(void)
{
    return net_mng.listen_fd;
}

/**
 * @brief  网络初始化
 * @return =0 成功 <0 失败
 */
int32_t network_init(void)
{
    int32_t  ret;
    int32_t  fd;
    int32_t type;
    struct epoll_event ev;

    fd = -1;

    /* 初始化zookeeper */
    ret = nlb_zk_init(get_zk_host(), get_zk_timeout());
    if (ret < 0) {
        NLOG_ERROR("Nlb zookeeper init failed, ret [%d]", ret);
        ret = -1;
        goto FAILED;
    }

    // 初始化epoll
    type = EPOLL_CLOEXEC;
    net_mng.epfd = epoll_create1(type);
    if (net_mng.epfd == -1)
    {
        NLOG_ERROR("Failed to create an epoll instance: %m");
        ret = -4;
        goto FAILED;
    }

    net_mng.evlist_size = EVENT_NUM;
    net_mng.evlist = safe_alloc(net_mng.evlist_size * sizeof(struct epoll_event *));
    if (!net_mng.evlist) {
        NLOG_ERROR("evlist = safe_alloc() is nil");
        ret = -6;
        goto FAILED;
    }

    /* 服务器模式不需要bind UDP端口 */
    if (get_worker_mode() == SERVER_MODE) {
        net_mng.listen_fd = -1;
        ret = 0;
        goto FAILED;
    }

    /* 创建UDP套接字，用于接收路由请求 */
    fd = create_udp_socket();
    if (fd < 0) {
        NLOG_ERROR("Create udp socket failed, ret [%d]", ret);
        ret = -2;
        goto FAILED;
    }

    ret = bind_port(fd, "127.0.0.1", get_listen_port());
    if (ret < 0) {
        NLOG_ERROR("Bind port failed, ret [%d]", ret);
        ret = -3;
        goto FAILED;
    }

    ev.data.fd = (int)fd;
    ev.events = EPOLLIN | EPOLLRDHUP;
    if (epoll_ctl(net_mng.epfd, EPOLL_CTL_MOD, fd, &ev) == -1) {
        if (errno != ENOENT) {
          NLOG_ERROR("epoll_ctl_mod failed with: %m");
        }

        /* New FD, lets add it */
        if (epoll_ctl(net_mng.epfd, EPOLL_CTL_ADD, fd, &ev) == -1) {
            NLOG_ERROR("epoll_ctl_add failed with: %m");
            ret = -5;
            goto FAILED;
        }
    }

    net_mng.listen_fd = fd;
    return 0;

FAILED:

    if (net_mng.evlist) {
        free(net_mng.evlist);
        net_mng.evlist = NULL;
        net_mng.evlist_size = 0;
    }

    if (fd > 0) {
        close(fd);
    }

    if (net_mng.epfd > 0) {
        close(net_mng.epfd);
        net_mng.epfd = 0;
    }

    return ret;
}

/**
 * @brief  关闭网络
 * @return void
 */
void network_close(void)
{
    if (net_mng.listen_fd > 0) {
        close(net_mng.listen_fd);
        net_mng.listen_fd = -1;
    }

    if (net_mng.epfd > 0) {
        close(net_mng.epfd);
        net_mng.epfd = -1;
    }

    if (net_mng.evlist != NULL) {
        free(net_mng.evlist);
        net_mng.evlist = NULL;
        net_mng.evlist_size = 0;
        net_mng.ev_ready = 0;
    }
}

static int check_interests(uint64_t *zk_timeout)
{
    int32_t zkfd;
    int32_t zk_events;
    int32_t ret;
    struct epoll_event ev;

    /* 获取zookeeper关注事件 */
    zkfd = -1;
    ret = nlb_zk_poll_events(&zkfd, &zk_events, zk_timeout);
    if (ret != 0) {
        if (zkfd != -1 && (ret == ZINVALIDSTATE || ret == ZCONNECTIONLOSS)) {
            /* Note that ev must be !NULL for kernels < 2.6.9 */
            epoll_ctl(net_mng.epfd, EPOLL_CTL_DEL, zkfd, &ev);
        }
        NLOG_ERROR("Nlb zookeeper poll events failed, ret [%d]", ret);
        return -1;
    }

    ev.data.fd = (int)zkfd;
    ev.events = 0;
    if (zkfd != -1) {
        if (zk_events & NLB_POLLIN) {
            ev.events |= EPOLLIN;
        }

        if (zk_events & NLB_POLLOUT) {
            ev.events |= EPOLLOUT;
        }
    }
    ev.events |= EPOLLRDHUP;

    if (epoll_ctl(net_mng.epfd, EPOLL_CTL_MOD, zkfd, &ev) == -1) {
        if (errno != ENOENT)
          NLOG_ERROR("epoll_ctl_mod failed with: %m");

        /* New FD, lets add it */
        if (epoll_ctl(net_mng.epfd, EPOLL_CTL_ADD, zkfd, &ev) == -1) {
            NLOG_ERROR("epoll_ctl_add failed with: %m");
        }
    }

    return 0;
}

/**
 * @brief  监听网络事件
 * @info   监听zookeeper和路由请求
 */
int32_t network_poll(void)
{
    int32_t ret;
    int32_t ready;
    uint64_t zk_timeout;
    uint64_t timeout;
    struct epoll_event* list;

    ret = check_interests(&zk_timeout);
    if (ret != 0) {
        NLOG_ERROR("Check zookeeper interest events failed, ret [%d]", ret);
        return -1;
    }

    ready = 0;
    timeout = min(zk_timeout, net_mng.timeout);
    ready = epoll_wait(net_mng.epfd, net_mng.evlist, net_mng.evlist_size, timeout);
    if (ready == -1) {
      if (errno == EINTR) {
        return 0;
      }

      NLOG_ERROR("epoll_wait failed with: %m");
      return -2;
    }
    if (ready == net_mng.evlist_size) {
        net_mng.evlist_size <<= 1;
        list = (struct epoll_event*)safe_realloc(net_mng.evlist, (size_t)ready, (size_t)net_mng.evlist_size);
        if (list) {
            net_mng.evlist = list;
        } else {
            net_mng.evlist_size = ready;
        }
    }
    net_mng.ev_ready = ready;

    return 0;
}

/**
 * @brief 网络事件处理主函数
 */
int32_t network_process(void)
{
    uint32_t nlb_events = 0;
    int32_t listen_fd = net_mng.listen_fd;
    struct epoll_event *evlist = net_mng.evlist;

    /* Go over file descriptors that are ready */
    for (int32_t i = 0; i < net_mng.ev_ready; i++) {
        nlb_events = 0;
        if (evlist[i].events & (EPOLLIN | EPOLLOUT)) {
            if (evlist[i].events & EPOLLIN) {
                nlb_events |= NLB_POLLIN;
            }
            if (evlist[i].events & EPOLLOUT) {
                nlb_events |= NLB_POLLOUT;
            }

            if (evlist[i].data.fd != listen_fd) {
                nlb_zk_process(nlb_events);
            } else {
                process_route_request(evlist[i].data.fd);
            }
        } else if (evlist[i].events & (EPOLLHUP | EPOLLERR | EPOLLRDHUP)) {
            /* Invalid FDs will be removed when zookeeper_interest() indicates
             * they are not valid anymore */
        } else {
            NLOG_ERROR("Unknown events: %d\n", evlist[i].events);
        }
    }
    net_mng.ev_ready = 0;

    return 0;
}

