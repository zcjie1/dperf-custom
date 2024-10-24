/*
 * Copyright (c) 2021 Baidu.com, Inc. All Rights Reserved.
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
 */

#ifndef __MBUF_CACHE_H
#define __MBUF_CACHE_H

#include <stdint.h>
#include <stdbool.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>

#include "mbuf.h"

struct mbuf_data {
    uint8_t data[MBUF_DATA_SIZE];
    bool ipv6;
    bool vxlan;
    uint16_t l2_len;
    uint16_t l3_len;
    uint16_t l4_len;
    uint16_t data_len;
    uint16_t total_len;
};

struct mbuf_cache {
    struct rte_mempool *mbuf_pool;
    struct mbuf_data data;
};

static inline void mbuf_set_userdata(struct rte_mbuf *m, void *data)
{
#if RTE_VERSION >= RTE_VERSION_NUM(20, 0, 0, 0)
    uint64_t *p = (uint64_t *)(&(m->dynfield1[1]));
    *p = (uint64_t)data;
#else
        m->userdata = data;
#endif
}

static inline void *mbuf_get_userdata(struct rte_mbuf *m)
{
#if RTE_VERSION >= RTE_VERSION_NUM(20, 0, 0, 0)
    uint64_t *p = (uint64_t *)(&(m->dynfield1[1]));
    return (void *)(*p);
#else
    return m->userdata;
#endif
}

struct rte_mbuf *mbuf_cache_alloc(struct work_space *ws, struct mbuf_cache *p);

int mbuf_cache_init_tcp(struct mbuf_cache *cache, struct work_space *ws, const char *name, uint16_t mss,
    const char *data);
int mbuf_cache_init_udp(struct mbuf_cache *cache, struct work_space *ws, const char *name, const char *data);
void mbuf_cache_set_dmac(struct mbuf_cache *cache, struct eth_addr *ea);

#endif
