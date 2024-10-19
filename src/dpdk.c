/*
 * Copyright (c) 2021-2022 Baidu.com, Inc. All Rights Reserved.
 * Copyright (c) 2022-2024 Jianzhang Peng. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Author: Jianzhang Peng (pengjianzhang@baidu.com)
 *         Jianzhang Peng (pengjianzhang@gmail.com)
 */

#include "dpdk.h"

#include <sys/types.h>
#include <stdio.h>
#include <unistd.h>
#include <rte_eal.h>
#include <rte_launch.h>
#include <rte_atomic.h>
#include <rte_prefetch.h>
#include <rte_mbuf.h>
#include <rte_compat.h>
#include <rte_pdump.h>
#if RTE_VERSION >= RTE_VERSION_NUM(20, 0, 0, 0)
#include <rte_vect.h>
#endif

#include "mbuf.h"
#include "flow.h"
#include "tick.h"
#include "kni.h"
#include "rss.h"

static void dpdk_set_lcores(struct config *cfg, char *lcores)
{
    int i = 0;
    char lcore_buf[64];

    for (i = 0; i < cfg->cpu_num; i++) {
        if (i == 0) {
            sprintf(lcore_buf, "%d@(%d)", i, cfg->cpu[i]);
        } else {
            sprintf(lcore_buf, ",%d@(%d)", i, cfg->cpu[i]);
        }
        strcat(lcores, lcore_buf);
    }
}

static int dpdk_append_pci(struct config *cfg, int argc, char *argv[], char *flag_pci)
{
    int i = 0;
    int num = 0;
    struct netif_port *port = NULL;

    config_for_each_port(cfg, port) {
        if(port->is_vdev) {
            argv[argc++] = port->vdev_param;
            num++;
            continue;
        }
        for (i = 0; i < port->pci_num; i++) {
            argv[argc] = flag_pci;
            argv[argc+1] = port->pci_list[i];
            argc += 2;
            num += 2;
        }
    }

    return num;
}

static int dpdk_set_socket_mem(struct config *cfg, char *socket_mem, char *file_prefix)
{
    int size = 0;

    if (strlen(cfg->socket_mem) <= 0) {
        return 0;
    }

    size = snprintf(socket_mem, RTE_ARG_LEN, "--socket-mem=%s", cfg->socket_mem);
    if (size >= RTE_ARG_LEN) {
        return -1;
    }
    
    if(cfg->file_prefix[0] == '\0')
        size = snprintf(file_prefix, RTE_ARG_LEN, "--file-prefix=dperf-%d", getpid());
    else
        size = snprintf(file_prefix, RTE_ARG_LEN, "--file-prefix=%s", cfg->file_prefix);
    if (size >= RTE_ARG_LEN) {
        return -1;
    }

    return 0;
}

#if RTE_VERSION >= RTE_VERSION_NUM(20, 0, 0, 0)
static void dpdk_set_simd_bitwidth(struct config *cfg)
{
    if (cfg->simd512) {
        rte_vect_set_max_simd_bitwidth(RTE_VECT_SIMD_512);
    }
}
#else
static void dpdk_set_simd_bitwidth(__rte_unused struct config *cfg)
{
}
#endif

static int dpdk_eal_init(struct config *cfg, char *argv0)
{
    int argc = 5;
    char log_level[64];
    char lcores[2048] = "--lcores=";
    char no_pci[] = "--no-pci";
#if RTE_VERSION >= RTE_VERSION_NUM(20, 0, 0, 0)
    char telementry[64] = "--no-telemetry";
    char flag_pci[] = "-a";
#else
    char flag_pci[] = "-w";
#endif
    char socket_mem[64] = "";
    char file_prefix[64] = "";
    char *argv[6 + (NETIF_PORT_MAX * PCI_NUM_MAX* 2)] = {argv0, lcores, socket_mem,
            file_prefix, log_level, NULL, NULL};

#if RTE_VERSION >= RTE_VERSION_NUM(20, 0, 0, 0)
    argv[argc] = telementry;
    argc++;
#endif

    if(cfg->no_cpi)
        argv[argc++] = no_pci;

    sprintf(log_level, "--log-level=%d", cfg->log_level);
    if (dpdk_set_socket_mem(cfg, socket_mem, file_prefix) < 0) {
        printf("dpdk_set_socket_mem fail\n");
        return -1;
    }

    dpdk_set_lcores(cfg, lcores);
    argc += dpdk_append_pci(cfg, argc, argv, flag_pci);

    dpdk_set_simd_bitwidth(cfg);
    if (rte_eal_init(argc, argv) < 0) {
        printf("rte_eal_init fail\n");
        return -1;
    }

    return 0;
}

int dpdk_init(struct config *cfg, char *argv0)
{
    if (dpdk_eal_init(cfg, argv0) < 0) {
        printf("dpdk_eal_init fail\n");
        return -1;
    }

    rte_pdump_init();

    if (port_init_all(cfg) < 0) {
        printf("port init fail\n");
        return -1;
    }

    if (port_start_all(cfg) < 0) {
        printf("start port fail\n");
        return -1;
    }

    if (kni_start(cfg) < 0) {
        printf("kni start fail\n");
        return -1;
    }

    /* One-way traffic does not require RSS and FDIR */
    if (cfg->flow == FLOW_FDIR) {
        if (flow_init(cfg) < 0) {
            printf("flow init fail\n");
            return -1;
        }
    }

    tick_init(cfg->ticks_per_sec);
    config_set_tsc(cfg, g_tsc_per_second);

    return 0;
}

void dpdk_close(struct config *cfg)
{
    rte_pdump_uninit();
    flow_flush(cfg);
    port_stop_all(cfg);
    kni_stop(cfg);
    rte_eal_cleanup();
}

void dpdk_run(int (*lcore_main)(void*), void* data)
{
    int lcore_id = 0;

    RTE_LCORE_FOREACH(lcore_id) {
        if (lcore_id == 0) {
            continue;
        }
        rte_eal_remote_launch(lcore_main, data, lcore_id);
    }

    lcore_main(data);
    rte_eal_mp_wait_lcore();
}
