/* buf.c - Buffer management */

/*
 * Copyright (c) 2015 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "adaptation.h"
#include "net/buf.h"
#include "adv.h"

#define LOG_TAG             "[MESH-buf]"
/* #define LOG_INFO_ENABLE */
#define LOG_DEBUG_ENABLE
#define LOG_WARN_ENABLE
#define LOG_ERROR_ENABLE
#define LOG_DUMP_ENABLE
#include "mesh_log.h"

#define NET_BUF_DBG(fmt, ...)
#define NET_BUF_ERR(fmt, ...)
#define NET_BUF_WARN(fmt, ...)
#define NET_BUF_INFO(fmt, ...)
#define NET_BUF_ASSERT(cond)

#if MESH_RAM_AND_CODE_MAP_DETAIL
#ifdef SUPPORT_MS_EXTENSIONS
#pragma bss_seg(".ble_mesh_buf_bss")
#pragma data_seg(".ble_mesh_buf_data")
#pragma const_seg(".ble_mesh_buf_const")
#pragma code_seg(".ble_mesh_buf_code")
#endif
#else /* MESH_RAM_AND_CODE_MAP_DETAIL */
#pragma bss_seg(".ble_mesh_bss")
#pragma data_seg(".ble_mesh_data")
#pragma const_seg(".ble_mesh_const")
#pragma code_seg(".ble_mesh_code")
#endif /* MESH_RAM_AND_CODE_MAP_DETAIL */

#if CONFIG_NET_BUF_WARN_ALLOC_INTERVAL > 0
#define WARN_ALLOC_INTERVAL K_SECONDS(CONFIG_NET_BUF_WARN_ALLOC_INTERVAL)
#else
#define WARN_ALLOC_INTERVAL K_FOREVER
#endif

/* Linker-defined symbol bound to the static pool structs */
extern struct net_buf_pool _net_buf_pool_list[];

struct net_buf_pool *net_buf_pool_get(int id)
{
    return &_net_buf_pool_list[id];
}

static int pool_id(struct net_buf_pool *pool)
{
    return pool - _net_buf_pool_list;
}

int net_buf_id(struct net_buf *buf)
{
    struct net_buf_pool *pool = net_buf_pool_get(buf->pool_id);

    return buf - pool->__bufs;
}

static inline struct net_buf *pool_get_uninit(struct net_buf_pool *pool,
        u16_t uninit_count)
{
    struct net_buf *buf;

    buf = &pool->__bufs[pool->buf_count - uninit_count];

    buf->pool_id = pool_id(pool);

    return buf;
}

void net_buf_reset(struct net_buf *buf)
{
    NET_BUF_ASSERT(buf->flags == 0);
    NET_BUF_ASSERT(buf->frags == NULL);

    net_buf_simple_reset(&buf->b);
}

static u8_t *fixed_data_alloc(struct net_buf *buf, size_t *size, s32_t timeout)
{
    struct net_buf_pool *pool = net_buf_pool_get(buf->pool_id);
    const struct net_buf_pool_fixed *fixed = pool->alloc->alloc_data;

    *size = min(fixed->data_size, *size);

#if NET_BUF_USE_MALLOC
    return (u8 *)(*((u32 *)fixed->data_pool)) + fixed->data_size * net_buf_id(buf);
#else
    return fixed->data_pool + fixed->data_size * net_buf_id(buf);
#endif /* NET_BUF_USE_MALLOC */
}

static void fixed_data_unref(struct net_buf *buf, u8_t *data)
{
    /* Nothing needed for fixed-size data pools */
}

const struct net_buf_data_cb net_buf_fixed_cb = {
    .alloc = fixed_data_alloc,
    .unref = fixed_data_unref,
};

static u8_t *data_alloc(struct net_buf *buf, size_t *size, s32_t timeout)
{
    struct net_buf_pool *pool = net_buf_pool_get(buf->pool_id);

    return pool->alloc->cb->alloc(buf, size, timeout);
}

static u8_t *data_ref(struct net_buf *buf, u8_t *data)
{
    struct net_buf_pool *pool = net_buf_pool_get(buf->pool_id);

    return pool->alloc->cb->ref(buf, data);
}

static void data_unref(struct net_buf *buf, u8_t *data)
{
    struct net_buf_pool *pool = net_buf_pool_get(buf->pool_id);

    if (buf->flags & NET_BUF_EXTERNAL_DATA) {
        return;
    }

    pool->alloc->cb->unref(buf, data);
}

struct net_buf *net_buf_alloc_len(struct net_buf_pool *pool, size_t size,
                                  s32_t timeout)
{
    BT_INFO("--func=%s", __FUNCTION__);
    struct net_buf *buf;
    unsigned int key;
    u16_t uninit_count;

    NET_BUF_ASSERT(pool);

    NET_BUF_DBG("%s():%d: pool 0x%x size %u timeout %d", func, line, pool,
                size, timeout);

    /* We need to lock interrupts temporarily to prevent race conditions
     * when accessing pool->uninit_count.
     */
    key = irq_lock();

    /* ASSERT(pool->free_count, "---> pool->free_count 0"); */
#if NET_BUF_FREE_EN
    if (pool->free_count) {

        pool->free_count--;
        BT_INFO("free_count=%d", pool->free_count);
        do {
            uninit_count = pool->uninit_count--;

            if (0 == pool->uninit_count) {
                pool->uninit_count = pool->buf_count;
            }

            buf = pool_get_uninit(pool, uninit_count);

        } while (buf->flags);
#else
    /* If there are uninitialized buffers we're guaranteed to succeed
     * with the allocation one way or another.
     */
    if (pool->uninit_count) {
        uninit_count = pool->uninit_count--;

        if (0 == pool->uninit_count) {
            pool->uninit_count = pool->buf_count;
        }

        buf = pool_get_uninit(pool, uninit_count);

#endif /* NET_BUF_FREE_EN */
        irq_unlock(key);

        goto success;
    }

    irq_unlock(key);

    NET_BUF_ERR("%s():%d: Failed to get free buffer", func, line);
    BT_ERR("%s():%d: Failed to get free buffer", __FUNCTION__, __LINE__);

    return NULL;

success:
    NET_BUF_DBG("allocated buf 0x%x", buf);

    if (size) {
        buf->__buf = data_alloc(buf, &size, timeout);
        if (!buf->__buf) {
            NET_BUF_ERR("%s():%d: Failed to allocate data",
                        func, line);
            return NULL;
        }
    } else {
        buf->__buf = NULL;
    }

    buf->ref   = 1;
    buf->flags = 0;
    buf->frags = NULL;
    buf->size  = size;
    net_buf_reset(buf);

    return buf;
}

struct net_buf *net_buf_alloc_fixed(struct net_buf_pool *pool, s32_t timeout)
{
    const struct net_buf_pool_fixed *fixed = pool->alloc->alloc_data;

    return net_buf_alloc_len(pool, fixed->data_size, timeout);
}

#if NET_BUF_FREE_EN

int net_buf_free(struct net_buf *buf)
{
    unsigned int key;
    struct net_buf_pool *pool;

    BT_INFO("--func=%s", __FUNCTION__);

    key = irq_lock();

    buf->flags = 0;

    pool = net_buf_pool_get(buf->pool_id);

    if (pool->free_count < pool->buf_count) {
        pool->free_count++;
    }

    irq_unlock(key);

    BT_INFO("free_count=%d, addr=0x%x", pool->free_count, buf);

    return 0;
}

struct net_buf *net_buf_get_next(struct net_buf *buf)
{
    unsigned int key;
    int next_id;

    key = irq_lock();

    struct net_buf_pool *pool = net_buf_pool_get(buf->pool_id);

    next_id = net_buf_id(buf) + 1;

    next_id = (next_id < pool->buf_count) ? next_id : 0;

    buf = &pool->__bufs[next_id];

    irq_unlock(key);

    return buf;
}

#endif /* NET_BUF_FREE_EN */

#if NET_BUF_TEST_EN

#define TEST_BUF_COUNT          20
#define TEST_DATA_SIZE          29
#define TEST_USER_DATA_SIZE     4

NET_BUF_POOL_DEFINE(test_net_buf_pool, TEST_BUF_COUNT,
                    TEST_DATA_SIZE, TEST_USER_DATA_SIZE, NULL);

void net_buf_test(void)
{
    struct net_buf_pool *pool = &test_net_buf_pool;
    struct net_buf *buf[TEST_BUF_COUNT + 1];
    int i, j;

    //< test 1
    for (i = 0; ; i++) {
        BT_INFO("allloc i=%d, buf_count=%d, uninit_count=%d, free_count=%d",
                i, pool->buf_count, pool->uninit_count, pool->free_count);
        buf[i] = net_buf_alloc_fixed(pool, 0);
        BT_INFO("buf addr=0x%x", buf[i]);
        if (NULL == buf[i]) {
            break;
        }
    }
    if ((i != pool->buf_count) && (pool->free_count != 0)) {
        BT_ERR("net_buf_test process 1 alloc error");
        return;
    }
    for (j = 0; j < i; j++) {
        BT_INFO("free j=%d, free_count=%d", j, pool->free_count);
        BT_INFO("buf addr=0x%x", buf[j]);
        net_buf_free(buf[j]);
        BT_INFO("next buf addr=0x%x", net_buf_get_next(buf[j]));
    }
    if (pool->free_count != pool->buf_count) {
        BT_ERR("net_buf_test process 1 free error");
        return;
    }

    //< test 2
    struct net_buf *buf_temp;
    buf_temp = net_buf_alloc_fixed(pool, 0);
    if (buf[0] != buf_temp) {
        BT_ERR("net_buf_test process 2 alloc error", buf[0], buf_temp);
        return;
    }
    net_buf_free(buf[0]);
    buf_temp = net_buf_alloc_fixed(pool, 0);
    if (buf[1] != buf_temp) {
        BT_ERR("net_buf_test process 2 free error", buf[1], buf_temp);
        return;
    }

    BT_INFO("--- net_buf_test succ !!!");

    while (1);
}

#endif /* NET_BUF_TEST_EN */

void net_buf_simple_reserve(struct net_buf_simple *buf, size_t reserve)
{
    NET_BUF_ASSERT(buf);
    NET_BUF_ASSERT(buf->len == 0);
    NET_BUF_DBG("buf 0x%x reserve %u", buf, reserve);

    buf->data = buf->__buf + reserve;
}

void net_buf_unref(struct net_buf *buf)
{
    BT_INFO("--func=%s", __FUNCTION__);
    NET_BUF_ASSERT(buf);

#if NET_BUF_FREE_EN
    BT_INFO("buf=0x%x", buf);
    BT_INFO("BT_MESH_ADV(buf)=0x%x", BT_MESH_ADV(buf));
    BT_INFO("BT_MESH_ADV(buf)->busy=0x%x", BT_MESH_ADV(buf)->busy);
    if (buf && BT_MESH_ADV(buf) && BT_MESH_ADV(buf)->busy) {
        return;
    }
    if (buf && (buf->flags & NET_BUF_FRIEND_POLL_CACHE)) {
        return;
    }
    if (buf && (buf->flags & NET_BUF_FRIEND_QUEUE_CACHE)) {
        return;
    }
    if (buf && (buf->flags & NET_BUF_PBADV_CACHE)) {
        return;
    }
#endif /* NET_BUF_FREE_EN */

    while (buf) {
        struct net_buf *frags = buf->frags;
        struct net_buf_pool *pool;

        NET_BUF_DBG("buf 0x%x ref %u pool_id %u frags 0x%x", buf, buf->ref,
                    buf->pool_id, buf->frags);

#if !NET_BUF_FREE_EN
        if (--buf->ref > 0) {
            return;
        }

        if (buf->__buf) {
            data_unref(buf, buf->__buf);
            buf->__buf = NULL;
        }

        buf->data = NULL;
#endif /* NET_BUF_FREE_EN */

        buf->frags = NULL;

        pool = net_buf_pool_get(buf->pool_id);

#if defined(CONFIG_NET_BUF_POOL_USAGE)
        pool->avail_count++;
        NET_BUF_ASSERT(pool->avail_count <= pool->buf_count);
#endif

        if (pool->destroy) {
            pool->destroy(buf);
        }

#if NET_BUF_FREE_EN
        net_buf_free(buf);
#endif /* NET_BUF_FREE_EN */

        buf = frags;
    }
}

struct net_buf *net_buf_ref(struct net_buf *buf)
{
    NET_BUF_ASSERT(buf);

    NET_BUF_DBG("buf 0x%x (old) ref %u pool_id %u",
                buf, buf->ref, buf->pool_id);
    buf->ref++;
    return buf;
}

#if defined(CONFIG_NET_BUF_SIMPLE_LOG)
#define NET_BUF_SIMPLE_DBG(fmt, ...) NET_BUF_DBG(fmt, ##__VA_ARGS__)
#define NET_BUF_SIMPLE_ERR(fmt, ...) NET_BUF_ERR(fmt, ##__VA_ARGS__)
#define NET_BUF_SIMPLE_WARN(fmt, ...) NET_BUF_WARN(fmt, ##__VA_ARGS__)
#define NET_BUF_SIMPLE_INFO(fmt, ...) NET_BUF_INFO(fmt, ##__VA_ARGS__)
#define NET_BUF_SIMPLE_ASSERT(cond) NET_BUF_ASSERT(cond)
#else
#define NET_BUF_SIMPLE_DBG(fmt, ...)
#define NET_BUF_SIMPLE_ERR(fmt, ...)
#define NET_BUF_SIMPLE_WARN(fmt, ...)
#define NET_BUF_SIMPLE_INFO(fmt, ...)
#define NET_BUF_SIMPLE_ASSERT(cond)
#endif /* CONFIG_NET_BUF_SIMPLE_LOG */

void *net_buf_simple_add(struct net_buf_simple *buf, size_t len)
{
    u8_t *tail = net_buf_simple_tail(buf);

    NET_BUF_SIMPLE_DBG("buf 0x%x len %u", buf, len);

    NET_BUF_SIMPLE_ASSERT(net_buf_simple_tailroom(buf) >= len);

    buf->len += len;
    return tail;
}

void *net_buf_simple_add_mem(struct net_buf_simple *buf, const void *mem,
                             size_t len)
{
    NET_BUF_SIMPLE_DBG("buf 0x%x len %u", buf, len);

    return memcpy(net_buf_simple_add(buf, len), mem, len);
}

u8_t *net_buf_simple_add_u8(struct net_buf_simple *buf, u8_t val)
{
    u8_t *u8;

    NET_BUF_SIMPLE_DBG("buf 0x%x val 0x%02x", buf, val);

    u8 = net_buf_simple_add(buf, 1);
    *u8 = val;

    return u8;
}

void net_buf_simple_add_le16(struct net_buf_simple *buf, u16_t val)
{
    NET_BUF_SIMPLE_DBG("buf 0x%x val %u", buf, val);

    val = sys_cpu_to_le16(val);
    memcpy(net_buf_simple_add(buf, sizeof(val)), &val, sizeof(val));
}

void net_buf_simple_add_be16(struct net_buf_simple *buf, u16_t val)
{
    NET_BUF_SIMPLE_DBG("buf 0x%x val %u", buf, val);

    val = sys_cpu_to_be16(val);
    memcpy(net_buf_simple_add(buf, sizeof(val)), &val, sizeof(val));
}

void net_buf_simple_add_le32(struct net_buf_simple *buf, u32_t val)
{
    NET_BUF_SIMPLE_DBG("buf 0x%x val %u", buf, val);

    val = sys_cpu_to_le32(val);
    memcpy(net_buf_simple_add(buf, sizeof(val)), &val, sizeof(val));
}

void net_buf_simple_add_be32(struct net_buf_simple *buf, u32_t val)
{
    NET_BUF_SIMPLE_DBG("buf 0x%x val %u", buf, val);

    val = sys_cpu_to_be32(val);
    memcpy(net_buf_simple_add(buf, sizeof(val)), &val, sizeof(val));
}

void *net_buf_simple_push(struct net_buf_simple *buf, size_t len)
{
    NET_BUF_SIMPLE_DBG("buf 0x%x len %u", buf, len);

    NET_BUF_SIMPLE_ASSERT(net_buf_simple_headroom(buf) >= len);

    buf->data -= len;
    buf->len += len;
    return buf->data;
}

void net_buf_simple_push_le16(struct net_buf_simple *buf, u16_t val)
{
    NET_BUF_SIMPLE_DBG("buf 0x%x val %u", buf, val);

    val = sys_cpu_to_le16(val);
    memcpy(net_buf_simple_push(buf, sizeof(val)), &val, sizeof(val));
}

void net_buf_simple_push_be16(struct net_buf_simple *buf, u16_t val)
{
    NET_BUF_SIMPLE_DBG("buf 0x%x val %u", buf, val);

    val = sys_cpu_to_be16(val);
    memcpy(net_buf_simple_push(buf, sizeof(val)), &val, sizeof(val));
}

void net_buf_simple_push_u8(struct net_buf_simple *buf, u8_t val)
{
    u8_t *data = net_buf_simple_push(buf, 1);

    *data = val;
}

void *net_buf_simple_pull(struct net_buf_simple *buf, size_t len)
{
    NET_BUF_SIMPLE_DBG("buf 0x%x len %u", buf, len);

    NET_BUF_SIMPLE_ASSERT(buf->len >= len);

    buf->len -= len;
    return buf->data += len;
}

u8_t net_buf_simple_pull_u8(struct net_buf_simple *buf)
{
    u8_t val;

    val = buf->data[0];
    net_buf_simple_pull(buf, 1);

    return val;
}

u16_t net_buf_simple_pull_le16(struct net_buf_simple *buf)
{
    u16_t val;

    val = UNALIGNED_GET((u16_t *)buf->data);
    net_buf_simple_pull(buf, sizeof(val));

    return sys_le16_to_cpu(val);
}

u16_t net_buf_simple_pull_be16(struct net_buf_simple *buf)
{
    u16_t val;

    val = UNALIGNED_GET((u16_t *)buf->data);
    net_buf_simple_pull(buf, sizeof(val));

    return sys_be16_to_cpu(val);
}

u32_t net_buf_simple_pull_le32(struct net_buf_simple *buf)
{
    u32_t val;

    val = UNALIGNED_GET((u32_t *)buf->data);
    net_buf_simple_pull(buf, sizeof(val));

    return sys_le32_to_cpu(val);
}

u32_t net_buf_simple_pull_be32(struct net_buf_simple *buf)
{
    u32_t val;

    val = UNALIGNED_GET((u32_t *)buf->data);
    net_buf_simple_pull(buf, sizeof(val));

    return sys_be32_to_cpu(val);
}

size_t net_buf_simple_headroom(struct net_buf_simple *buf)
{
    return buf->data - buf->__buf;
}

size_t net_buf_simple_tailroom(struct net_buf_simple *buf)
{
    return buf->size - net_buf_simple_headroom(buf) - buf->len;
}

static void log_dump_slist(sys_slist_t *list)
{
    BT_INFO("list->head=0x%x", list->head);

    sys_snode_t *node = list->head;
    while (node) {
        BT_INFO("node next=0x%x", node);
        node = node->next;
    }

    BT_INFO("list->tail=0x%x", list->tail);
}

void net_buf_slist_put(sys_slist_t *list, struct net_buf *buf)
{
    struct net_buf *tail;
    unsigned int key;

    BT_INFO("--func=%s", __FUNCTION__);

    log_dump_slist(list);

    NET_BUF_ASSERT(list);
    NET_BUF_ASSERT(buf);

    for (tail = buf; tail->frags; tail = tail->frags) {
        tail->flags |= NET_BUF_FRAGS;
        BT_INFO("net_buf_slist_put NET_BUF_FRAGS");
    }

    key = irq_lock();
    sys_slist_append_list(list, &buf->node, &tail->node);
    irq_unlock(key);

    log_dump_slist(list);
}

void net_buf_slist_simple_put(sys_slist_t *head_list, sys_snode_t *dst_node)
{
    sys_snode_t *tail_node;
    unsigned int key;

    tail_node = dst_node;

    key = irq_lock();
    sys_slist_append_list(head_list, dst_node, tail_node);
    irq_unlock(key);
}

#define GET_STRUCT_MEMBER_OFFSET(type, member) \
    (u32)&(((struct type*)0)->member)

struct net_buf *net_buf_slist_simple_get(sys_slist_t *list)
{
    u8 *buf;
    unsigned int key;

    key = irq_lock();
    buf = (void *)sys_slist_get(list);
    if (buf) {
        buf -= GET_STRUCT_MEMBER_OFFSET(net_buf, entry_node);
    }
    irq_unlock(key);

    return (struct net_buf *)buf;
}

struct net_buf *net_buf_slist_get(sys_slist_t *list)
{
    struct net_buf *buf, *frag;
    unsigned int key;

    BT_INFO("--func=%s", __FUNCTION__);

    log_dump_slist(list);

    NET_BUF_ASSERT(list);

    key = irq_lock();
    buf = (void *)sys_slist_get(list);
    irq_unlock(key);

    if (!buf) {
        log_dump_slist(list);
        return NULL;
    }

    /* Get any fragments belonging to this buffer */
    for (frag = buf; (frag->flags & NET_BUF_FRAGS); frag = frag->frags) {
        BT_INFO("NET_BUF_FRAGS");
        key = irq_lock();
        frag->frags = (void *)sys_slist_get(list);
        irq_unlock(key);

        NET_BUF_ASSERT(frag->frags);

        /* The fragments flag is only for list-internal usage */
        frag->flags &= ~NET_BUF_FRAGS;
    }

    /* Mark the end of the fragment list */
    frag->frags = NULL;

    log_dump_slist(list);

    return buf;
}
