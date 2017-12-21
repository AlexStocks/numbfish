
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

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <linux/sockios.h>
#include <unistd.h>

#include "commtype.h"
#include "log.h"
#include "sysinfo.h"

#define MAX_LINE_SIZE     256
#define DEAULT_ETH_SPEED  1000
#define MAX_ETH_NUM       8
#define PROC_CPU_STAT     "/proc/stat"
#define PROC_MEM_STAT     "/proc/meminfo"
#define PROC_NET_DEV      "/proc/net/dev"
#define CALC_ETH_SPEED(s) (((s) * 1024 * 1024) / 8)

typedef unsigned long long   llu64_t;

/* CPU 实时快照结构定义 */
typedef struct _tag_cpu_stat
{
    uint64_t    total;          /* 总的计数值 */
    uint64_t    user;           /* 累计的 用户态 */
    uint64_t    nice;           /* 累计的 nice为负 */
    uint64_t    system;         /* 系统态运行时间 */
    uint64_t    idle;           /* IO等待以外的其他等待时间 */
    uint64_t    iowait;         /* IO等待时间 */
    uint64_t    irq;            /* 硬中断 */
    uint64_t    softirq;        /* 软中断 */
} cpu_stat_t;

/* 内存数据结构定义 */
typedef struct _tag_mem_stat
{
    uint64_t    mem_total;      /* 总的内存 KB  */
    uint64_t    mem_free;       /* 可用物理内存 */
    uint64_t    buffers;        /* buffer内存   */
    uint64_t    cached;         /* 文件cache等  */
    uint64_t    mapped;         /* 已映射内存   */
    uint64_t    inactive_file;  /* 不活跃的文件cache */
} mem_stat_t;

typedef struct _tag_eth_info
{
    uint32_t    ip;
    char        eth_name[32];
} eth_info_t;

/* 网卡数据结构定义 */
typedef struct _tag_network_stat
{
    uint64_t    rcv_bytes;
    uint64_t    snd_bytes;
    uint32_t    speed;
    eth_info_t  eth_info;
} network_stat_t, *network_stat_p;

/* 进程cpu数据定义 */
typedef struct _tag_proc_cpu
{
    uint64_t    utime;          /* 用户态时间 */
    uint64_t    stime;          /* 系统态时间 */
    uint64_t    cutime;         /* 子线程用户态时间 */
    uint64_t    cstime;         /* 子线程系统态时间 */
    uint64_t    proc_total;     /* 进程汇总时间 */
    uint64_t    sys_total;      /* 系统切片时间 */
} proc_cpu_t;

/* 全局的系统资源信息 */
struct sysinfo
{
    uint64_t        cpu_idle;        /* cpu 空闲时间 */
    uint64_t        cpu_total;       /* cpu 总数 */
    uint64_t        mem_total;       /* 内存总数 KB  */
    uint64_t        mem_free;        /* 内存可用 KB  */
    cpu_stat_t      records[2];      /* cpu统计值, 2次取差值 */
    network_stat_t  network;         /* 网络数据链表 */
};

/* 进程资源信息 */
struct procinfo
{
    uint32_t    pid;              /* 进程pid */
    uint64_t    mem_used;         /* 物理内存kb */
    uint32_t    cpu_used;         /* 比例相对单CPU比例 */
    proc_cpu_t  records[2];       /* 快照信息 */
};

static eth_info_t eth[MAX_ETH_NUM];
static struct sysinfo sys_stat;

/**
 * @brief 提取CPU统计信息
 */
static void extract_cpu_stat(cpu_stat_t *stat)
{
    char name[64];
    char line[512];
    FILE* fp = fopen(PROC_CPU_STAT, "r");
    if (!fp) {
        NLOG_ERROR("open /proc/stat failed, [%m]");
        return;
    }
    memset(stat, 0, sizeof(*stat));

    while (fgets(line, sizeof(line)-1, fp)) {
        /* cpu  155328925 640355 14677305 9174748668 331430975 0 457328 7242581 */
        if (sscanf(line, "%s%llu%llu%llu%llu%llu%llu%llu", name,
                   (llu64_t*)&stat->user, (llu64_t*)&stat->nice,
                   (llu64_t*)&stat->system, (llu64_t*)&stat->idle,
                   (llu64_t*)&stat->iowait, (llu64_t*)&stat->irq,
                   (llu64_t*)&stat->softirq) != 8)
        {
            continue;
        }

        /* 只获取CPU总统计数据 */
        if (!strncmp(name, "cpu", 4)) {
            stat->total = stat->user + stat->nice + stat->system + stat->idle
                          + stat->iowait + stat->irq + stat->softirq;
            break;
        }
    }

    fclose(fp);

    return;
}

/**
 * @brief 提取内存统计信息
 */
static int32_t extract_mem_info(mem_stat_t *stat)
{
    char name[MAX_LINE_SIZE];
    char line[MAX_LINE_SIZE];
    int32_t finish_num = 0;
    llu64_t value;
    FILE* fp = fopen(PROC_MEM_STAT, "r");
    if (!fp) {
        NLOG_ERROR("open /proc/meminfo failed, [%m]");
        return -1;
    }

    while (fgets(line, sizeof(line)-1, fp)) {
        if (sscanf(line, "%s%llu", name, &value) != 2) {
            continue;
        }

        if (!strcmp(name, "MemTotal:")) {
            finish_num++;
            stat->mem_total = (uint64_t)value;
        } else if (!strcmp(name, "MemFree:")) {
            finish_num++;
            stat->mem_free = (uint64_t)value;
        } else if (!strcmp(name, "Buffers:")) {
            finish_num++;
            stat->buffers = (uint64_t)value;
        } else if (!strcmp(name, "Cached:")) {
            finish_num++;
            stat->cached = (uint64_t)value;
        } /* else if (! strcmp(name, "Inactive:")) {
            finish_num++;
            stat->inactive_file = (uint64_t)value;
        }*/  else if (!strcmp(name, "Mapped:")) {
            finish_num++;
            stat->mapped = (uint64_t)value;
        }
    }

    fclose(fp);

    /* 确认所有字段都提取到了 */
    if (finish_num != 5) {
        NLOG_ERROR("extract /proc/meminfo failed!");
        return -2;
    }

    return 0;
}

/**
 * @brief 获取内存空闲大小
 */
static void calc_mem_free(struct sysinfo *info)
{
    mem_stat_t stat;

    extract_mem_info(&stat);
    info->mem_total = stat.mem_total;
    info->mem_free  = stat.mem_free + stat.buffers + stat.cached - stat.mapped;
}

/*  struct for ethtool driver   */
struct ethtool_cmd
{
    unsigned int    cmd;
    unsigned int    supported;  /* Features this interface supports */
    unsigned int    advertising;    /* Features this interface advertises */
    unsigned short  speed;  /* The forced speed, 10Mb, 100Mb, gigabit */
    unsigned char   duplex; /* Duplex, half or full */
    unsigned char   port;   /* Which connector port */
    unsigned char   phy_address;
    unsigned char   transceiver;    /* Which tranceiver to use */
    unsigned char   autoneg;    /* Enable or disable autonegotiation */
    unsigned int    maxtxpkt;   /* Tx pkts before generating tx int */
    unsigned int    maxrxpkt;   /* Rx pkts before generating rx int */
    unsigned int    reserved[4];
};

static int get_eth_ip(char *name, uint32_t *ip)
{
    int ret;
    int fd;
    struct ifreq ifr;
    struct ethtool_cmd ecmd;

    /*  this entire function is almost copied from ethtool source code */
    memset(&ifr, 0, sizeof(ifr));
    strcpy(ifr.ifr_name, name);  //eth0

    /* Open control socket. */
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }
    /*  Pass the "get info" command to eth tool driver  */
    ecmd.cmd = 0x00000001;
    ifr.ifr_data = (caddr_t)&ecmd;
    ret = ioctl(fd, SIOCGIFADDR, &ifr);
    close(fd);

    memcpy(ip, &((struct sockaddr_in*)&ifr.ifr_addr)->sin_addr, sizeof(*ip));

    return ret;
}

static int get_eth_speed(char *name, uint32_t *speed)
{
    int ret;
    int fd;
    struct ifreq ifr;
    struct ethtool_cmd ecmd;

    /*  this entire function is almost copied from ethtool source code */
    memset(&ifr, 0, sizeof(ifr));
    strcpy(ifr.ifr_name, name);  //eth0

    /* Open control socket. */
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }
    /*  Pass the "get info" command to eth tool driver  */
    ecmd.cmd = 0x00000001;
    ifr.ifr_data = (caddr_t)&ecmd;
    ret = ioctl(fd, SIOCETHTOOL, &ifr);
    *speed = ecmd.speed;

    close(fd);

    return ret;
}

static void parse_netstat_line(char* line, network_stat_p stat)
{
    // alloc a new network_stat_t struct, and populate the rcv_bytes, snd_bytes, and eth_name parts of it
    uint64_t dummy, rcv_bytes, snd_bytes;
    sscanf(line, "%lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu",
            &rcv_bytes, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy,
            &snd_bytes, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy);

    stat->rcv_bytes = rcv_bytes;
    stat->snd_bytes = snd_bytes;
    stat->speed = 1000; // default: 1000Mb/s
}

/**
 * @brief 提取netowrk adapter统计信息
 */
static int extract_network_stat(network_stat_p stat)
{
    char* colon;
    char line[MAX_LINE_SIZE];
    char eth_name[32];
    uint32_t speed;
    size_t   len;
    int      ret;

    FILE* fp = fopen(PROC_NET_DEV, "r");
    if (!fp) {
        NLOG_ERROR("open %s failed, [%m]", PROC_NET_DEV);
        return -1;
    }

    ret = -2;
    while (fgets(line, MAX_LINE_SIZE, fp) != NULL) {
        if ((colon = strchr(line, ':')) != NULL) {
            len = colon-line;
            if (32 < len) {
                len = 32;
            }
            strncpy(eth_name, line, len);
            if (0 ==strncmp(eth_name, stat->eth_info.eth_name, len)) {
                parse_netstat_line(colon + 1, stat);
                if (get_eth_speed(eth_name, &speed) == 0) {
                    stat->speed = speed;
                }

                break;
            }
        }
    }

    fclose(fp);

    return ret;
}

/**
 * @brief 初始化网卡绑定的ip
 */
static void extract_eth_info(void)
{
    char* colon;
    char line[MAX_LINE_SIZE];
    uint32_t idx;
    uint32_t ip;
    int      ret;

    FILE* fp = fopen(PROC_NET_DEV, "r");
    if (!fp) {
        NLOG_ERROR("open %s failed, [%m]", PROC_NET_DEV);
        return;
    }

    idx = 0;
    while (fgets(line, MAX_LINE_SIZE, fp) != NULL) {
        if ((colon = strchr(line, ':')) != NULL) {
            if (idx >= MAX_ETH_NUM) {
                NLOG_ERROR("ethernet adapters number is not less than %u", idx);
                break;
            }

            strncpy(eth[idx].eth_name, line, colon-line);
            ret = get_eth_ip(eth[idx].eth_name, &ip);
            if (0 == ret) {
                eth[idx].ip = ip;
            }
            idx ++;
        }
    }

    fclose(fp);

    return;
}

/**
 * @brief 初始化系统信息
 */
void init_sysinfo(void)
{
    /* 获取CPU占用快照 */
    extract_cpu_stat(&sys_stat.records[0]);
    extract_cpu_stat(&sys_stat.records[1]);
    sys_stat.cpu_idle  = sys_stat.records[0].total;

    /* 获取内存信息 */
    calc_mem_free(&sys_stat);

    // 获取网卡绑定的ip地址
    memset(eth, 0, sizeof(eth));
    extract_eth_info();
}

/**
 * @brief 更新系统信息
 */
static void update_sysinfo(uint32_t ip)
{
    /* 获取CPU占用快照 */
    cpu_stat_t *last_stat = &sys_stat.records[0];
    cpu_stat_t *cur_stat  = &sys_stat.records[1];

    memcpy(last_stat, cur_stat, sizeof(cpu_stat_t));
    extract_cpu_stat(cur_stat);
    sys_stat.cpu_idle   = cur_stat->idle - last_stat->idle;
    sys_stat.cpu_total  = cur_stat->total - last_stat->total;

    /* 获取内存信息 */
    calc_mem_free(&sys_stat);

    // 获取网卡信息
    int ret = -1;
    network_stat_t network_stat;
    for (int32_t idx = 0; idx < MAX_ETH_NUM; idx++) {
        if (eth[idx].ip == ip) {
            memcpy(&(network_stat.eth_info), eth + idx, sizeof(network_stat.eth_info));
            ret = extract_network_stat(&network_stat);
            if (ret == 0) {
                memcpy(&(sys_stat.network), &network_stat, sizeof(sys_stat.network));
            }
            break;
        }
    }

    if (ret != 0) {
        NLOG_ERROR("can't get ethernet adapter info for ip %s", inet_ntoa(*(struct in_addr *)(void*)(&ip)));
    }
}

/**
 * @brief 获取系统信息
 * @info  CPU占用百分比，MEM总量，MEM空闲
 */
void get_sysinfo(uint32_t ip, sys_load_info_p sys_load)
{
    memset(&(sys_stat.network), 0, sizeof(sys_stat.network));
    /* 更新当前系统信息 */
    update_sysinfo(ip);

    /* CPU百分比 */
    sys_load->cpu_percent = 100 - (uint32_t)((100.0*sys_stat.cpu_idle)/sys_stat.cpu_total);

    /* 内存使用情况 */
    sys_load->mem_total = sys_stat.mem_total;
    sys_load->mem_free = sys_stat.mem_free;

    // 网卡使用情况
    if (sys_stat.network.eth_info.ip == ip) {
        sys_load->net_total = sys_stat.network.speed;
        if (sys_load->net_total != 0) {
            sys_load->net_snd_ratio = (uint64_t)(((float)(sys_stat.network.snd_bytes) / sys_load->net_total) * 1000.f);
            sys_load->net_rcv_ratio = (uint64_t)(((float)(sys_stat.network.rcv_bytes) / sys_load->net_total) * 1000.f);
        }
    }
}
