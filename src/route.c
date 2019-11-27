/*
 * DPVS is a software load balancer (Virtual Server) based on DPDK.
 *
 * Copyright (C) 2017 iQIYI (www.iqiyi.com).
 * All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <string.h>
#include <assert.h>
#include "route.h"
#include "conf/route.h"
#include "ctrl.h"


#define RTE_LOGTYPE_ROUTE       RTE_LOGTYPE_USER1
#define LOCAL_ROUTE_TAB_SIZE    (1 << 8)
#define LOCAL_ROUTE_TAB_MASK    (LOCAL_ROUTE_TAB_SIZE - 1)
#define NET_ROUTE_TAB_SIZE      8
#define NET_ROUTE_TAB_MASK      (NET_ROUTE_TAB_SIZE - 1)

#define this_route_lcore        (RTE_PER_LCORE(route_lcore))

#define this_local_route_table  (this_route_lcore.local_route_table)
#define this_net_route_table    (this_route_lcore.net_route_table)

#define this_num_routes         (RTE_PER_LCORE(num_routes))

/**
 * use per-lcore structure for lockless
 * to improve performance.
 */
struct route_lcore {
    struct list_head local_route_table[LOCAL_ROUTE_TAB_SIZE];
    struct list_head net_route_table;
};

static RTE_DEFINE_PER_LCORE(struct route_lcore, route_lcore);
static RTE_DEFINE_PER_LCORE(rte_atomic32_t, num_routes);

static inline bool net_cmp(const struct netif_port *port, uint32_t dest,
                           uint8_t mask, const struct route_entry *route_node)
{
    if ((port->id == route_node->port->id)&&
        (ip_addr_netcmp(dest, mask, route_node)))
        return 1;
    return 0;
}

static inline unsigned int
route_local_hashkey(uint32_t ip_addr, const struct netif_port *port)
{
    return rte_be_to_cpu_32(ip_addr)&LOCAL_ROUTE_TAB_MASK;
}

static int route_local_hash(struct route_entry *route)
{
    unsigned hashkey;
    hashkey = route_local_hashkey(route->dest.s_addr, route->port);
    list_add(&route->list, &this_local_route_table[hashkey]);
    rte_atomic32_inc(&route->refcnt);
    return EDPVS_OK;
}

static int route_local_unhash(struct route_entry *route)
{
    list_del(&route->list);
    rte_atomic32_dec(&route->refcnt);
    return EDPVS_OK;
}

static struct route_entry *route_new_entry(struct in_addr* dest, 
                                           uint8_t netmask, uint32_t flag, 
                                           struct in_addr* gw, struct netif_port *port,
                                           struct in_addr* src, unsigned long mtu,
                                           short metric)
{
    struct route_entry *new_route=NULL;
    if(!dest)
        return NULL;
    new_route = rte_zmalloc("new_route_entry", sizeof(struct route_entry), 0);
    if (new_route == NULL){
        return NULL;
    }
    new_route->dest = *dest;
    new_route->netmask = netmask;
    new_route->flag = flag;
    if(!gw)
        new_route->gw.s_addr = 0;
    else
        new_route->gw = *gw;
    new_route->port = port;
    if(!src)
        new_route->src.s_addr = 0;
    else
        new_route->src = *src;
    if(mtu != 0)
        new_route->mtu = mtu;
    else
        new_route->mtu = port->mtu;
    new_route->metric = metric;
    rte_atomic32_set(&new_route->refcnt, 0);
    return new_route;
    
}

static int route_net_add(struct in_addr *dest, uint8_t netmask, uint32_t flag, 
                         struct in_addr *gw, struct netif_port *port, 
                         struct in_addr *src, unsigned long mtu,short metric)
{
    struct route_entry *route_node, *route;
    list_for_each_entry(route_node, &this_net_route_table, list){
        if (net_cmp(port, dest->s_addr, netmask, route_node) 
                && (netmask == route_node->netmask)){
            return EDPVS_EXIST;
        }
        if (route_node->netmask < netmask){
            route = route_new_entry(dest,netmask, flag,
                                    gw, port, src, mtu, metric);
            if (!route){
                return EDPVS_NOMEM;
            }
            __list_add(&route->list, (&route_node->list)->prev, 
                       &route_node->list);
            rte_atomic32_inc(&this_num_routes);
            rte_atomic32_inc(&route->refcnt);
            return EDPVS_OK;
        } 
    }
    route = route_new_entry(dest,netmask, flag,
                      gw, port, src, mtu, metric);
    if (!route){
        return EDPVS_NOMEM;
    }
    list_add_tail(&route->list,&this_net_route_table);
    rte_atomic32_inc(&this_num_routes);
    rte_atomic32_inc(&route->refcnt);
    return EDPVS_OK;
}

static int route_net_del(struct route_entry *route)
{
    if (route){
        DPVS_WAIT_WHILE(rte_atomic32_read(&route->refcnt) > 2);
        list_del(&route->list);
        rte_free(route);
        rte_atomic32_dec(&this_num_routes);
        return EDPVS_OK;
    }
    return EDPVS_NOTEXIST;
}

static struct route_entry *route_local_lookup(uint32_t dest, const struct netif_port *port)
{
    unsigned hashkey;
    struct route_entry *route_node;
    hashkey = route_local_hashkey(dest, port);
    list_for_each_entry(route_node, &this_local_route_table[hashkey], list){
        if (dest == route_node->dest.s_addr) {
            rte_atomic32_inc(&route_node->refcnt);
            return route_node;
        }
    }
    return NULL;
}

struct route_entry *route_out_local_lookup(uint32_t dest)
{
    unsigned hashkey;
    struct route_entry *route_node;
    hashkey = route_local_hashkey(dest, NULL);
    list_for_each_entry(route_node, &this_local_route_table[hashkey], list){
        if (dest == route_node->dest.s_addr){
            rte_atomic32_inc(&route_node->refcnt);
            return route_node;
        }
    }
    return NULL;
}

static struct route_entry *route_net_lookup(struct netif_port *port, 
                                            struct in_addr *dest, uint8_t netmask)
{
    struct route_entry *route_node;
    list_for_each_entry(route_node, &this_net_route_table, list){
        if (net_cmp(port, dest->s_addr, netmask, route_node)){
            rte_atomic32_inc(&route_node->refcnt);
            return route_node;
        }
    }
    return NULL;
}

static struct route_entry *route_in_net_lookup(const struct netif_port *port,
                                               const struct in_addr *dest)
{
    struct route_entry *route_node;
    list_for_each_entry(route_node, &this_net_route_table, list){
        if (net_cmp(route_node->port, dest->s_addr, route_node->netmask, route_node)){
            rte_atomic32_inc(&route_node->refcnt);
            return route_node;
        }
    }
    return NULL;
}

static struct route_entry *route_out_net_lookup(const struct in_addr *dest)
{
    struct route_entry *route_node;
    list_for_each_entry(route_node, &this_net_route_table, list){
        if (net_cmp(route_node->port, dest->s_addr, route_node->netmask, route_node)){
            rte_atomic32_inc(&route_node->refcnt);
            return route_node;
        }
    }
    return NULL;
}

static int route_local_add(struct in_addr* dest, uint8_t netmask, uint32_t flag,
                           struct in_addr* gw, struct netif_port *port,
                           struct in_addr* src, unsigned long mtu,short metric)
{
    unsigned hashkey;
    struct route_entry *route_node, *route;

    hashkey = route_local_hashkey(*(uint32_t *)(dest),NULL);
    list_for_each_entry(route_node, &this_local_route_table[hashkey], list){
        if (net_cmp(port, dest->s_addr, netmask, route_node) 
             && (dest->s_addr == route_node->dest.s_addr) ){
            return EDPVS_EXIST;
        }
    }

    route = route_new_entry(dest,netmask, flag,
                      gw, port, src, mtu,metric);
    if (!route){
        return EDPVS_NOMEM;
    }
    route_local_hash(route);
    rte_atomic32_inc(&this_num_routes);
    return EDPVS_OK;
}

static int route_local_del(struct route_entry *route)
{
    if(route){
        DPVS_WAIT_WHILE(rte_atomic32_read(&route->refcnt) > 2);
        route_local_unhash(route);
        rte_free(route);
        rte_atomic32_dec(&this_num_routes);
        return EDPVS_OK;
    }
    return EDPVS_NOTEXIST;
}

static int route_add_lcore(struct in_addr* dest,uint8_t netmask, uint32_t flag,
              struct in_addr* gw, struct netif_port *port,
              struct in_addr* src, unsigned long mtu,short metric)
{
    if((flag & RTF_LOCALIN) || (flag & RTF_KNI))
        return route_local_add(dest, netmask, flag, gw,
                               port, src, mtu, metric);
    if((flag & RTF_FORWARD) || (flag & RTF_DEFAULT))
        return route_net_add(dest, netmask, flag, gw,
                             port, src, mtu, metric);

    return EDPVS_INVAL;
}

static int route_del_lcore(struct in_addr* dest,uint8_t netmask, uint32_t flag,
              struct in_addr* gw, struct netif_port *port,
              struct in_addr* src, unsigned long mtu,short metric)
{
    struct route_entry *route = NULL;
    int error;
    if(flag & RTF_LOCALIN || (flag & RTF_KNI)){
        route = route_local_lookup(dest->s_addr, port);
        error = route_local_del(route);
        return error;
    }
    if(flag & RTF_FORWARD || (flag & RTF_DEFAULT)){
        route = route_net_lookup(port, dest, netmask);
        error = route_net_del(route);
        return error;
    }
    return EDPVS_INVAL;
}

static int route_add_del(bool add, struct in_addr* dest,
                         uint8_t netmask, uint32_t flag,
                         struct in_addr* gw, struct netif_port *port,
                         struct in_addr* src, unsigned long mtu,
                         short metric)
{
    lcoreid_t cid = rte_lcore_id();
    int err;
    struct dpvs_msg *msg;
    struct dp_vs_route_conf cf;

    if (cid != rte_get_master_lcore()) {
        RTE_LOG(INFO, ROUTE, "[%s] must set from master lcore\n", __func__);
        return EDPVS_NOTSUPP;
    }

    /* set route on master lcore first */
    if (add)
        err = route_add_lcore(dest, netmask, flag, gw, port, src, mtu, metric);
    else
        err = route_del_lcore(dest, netmask, flag, gw, port, src, mtu, metric);

    if (err != EDPVS_OK) {
        RTE_LOG(INFO, ROUTE, "[%s] fail to set route\n", __func__);
        return err;
    }

    /* set route on all slave lcores */
    memset(&cf, 0, sizeof(struct dp_vs_route_conf));
    if (dest)
        cf.dst.in = *dest;
    cf.plen = netmask;
    cf.flags = flag;
    if (gw)
        cf.via.in = *gw;
    snprintf(cf.ifname, sizeof(cf.ifname), "%s", port->name);
    if (src)
        cf.src.in = *src; 
    cf.mtu = mtu;
    cf.metric = metric;

    if (add)
        msg = msg_make(MSG_TYPE_ROUTE_ADD, 0, DPVS_MSG_MULTICAST, 
                       cid, sizeof(struct dp_vs_route_conf), &cf);
    else
        msg = msg_make(MSG_TYPE_ROUTE_DEL, 0, DPVS_MSG_MULTICAST, 
                       cid, sizeof(struct dp_vs_route_conf), &cf);

    err = multicast_msg_send(msg, 0/*DPVS_MSG_F_ASYNC*/, NULL);
    if (err != EDPVS_OK) {
        msg_destroy(&msg);
        RTE_LOG(INFO, ROUTE, "[%s] fail to send multicast message\n", __func__);
        return err;
    }
    msg_destroy(&msg);

    return EDPVS_OK;
}

int route_add(struct in_addr* dest,uint8_t netmask, uint32_t flag,
              struct in_addr* gw, struct netif_port *port,
              struct in_addr* src, unsigned long mtu,short metric)
{
    return route_add_del(true, dest, netmask, flag, gw, port, src, mtu, metric);
}

int route_del(struct in_addr* dest,uint8_t netmask, uint32_t flag,
              struct in_addr* gw, struct netif_port *port,
              struct in_addr* src, unsigned long mtu,short metric)
{
    return route_add_del(false, dest, netmask, flag, gw, port, src, mtu, metric);
}

struct route_entry *route4_input(const struct rte_mbuf *mbuf,
                                const struct in_addr *daddr,
                                const struct in_addr *saddr,
                                uint8_t tos,//service type
                                const struct netif_port *port
                                )
{
    struct route_entry *route;
    route = route_local_lookup(daddr->s_addr, port);
    if (route){
        return route;
    }
         
    route = route_in_net_lookup(port, daddr);
    if (route){
        return route;
    }

    return NULL;
}

struct route_entry *route4_local(uint32_t src, struct netif_port *port)
{
    struct route_entry *route;
    route = route_local_lookup(src, port);
    if (route && (route->flag & RTF_LOCALIN)){
        return route;
    } 
    if (route)
	route4_put(route);
    return NULL;
}

uint32_t route_select_addr(struct netif_port *port)
{
    struct route_entry *route_node;
    unsigned hashkey;
    for(hashkey = 0; hashkey < LOCAL_ROUTE_TAB_SIZE; hashkey++){
        list_for_each_entry(route_node, &this_local_route_table[hashkey], list){
            if((port->id == route_node->port->id) && (route_node->flag & RTF_LOCALIN)){
                return *(uint32_t *)(&route_node->dest);
            }
        }
    }
    return 0;    
}

struct route_entry *route4_output(const struct flow4 *fl4)
{
    struct route_entry *route;
    

    route = route_out_local_lookup(fl4->daddr.s_addr);
    if(route){
        return route;
    }
    
    route = route_out_net_lookup(&fl4->daddr);
    if(route){
        return route;
    }
    
    return NULL;
}

int route_flush(void)
{
    int i = 0;
    struct route_entry *route_node;

    for (i = 0; i < LOCAL_ROUTE_TAB_SIZE; i++){
        list_for_each_entry(route_node, &this_local_route_table[i], list){
            route_local_del(route_node);
        }
    }

    list_for_each_entry(route_node, &this_net_route_table, list){
        route_net_del(route_node);
    }
   
    return EDPVS_OK;
}

/**
 * control plane
 */
#define NIPQUAD(addr) \
	((unsigned char *)&addr)[0], \
	((unsigned char *)&addr)[1], \
	((unsigned char *)&addr)[2], \
	((unsigned char *)&addr)[3]
#define NIPQUAD_FMT "%u.%u.%u.%u"
static int route_sockopt_set(sockoptid_t opt, const void *conf, size_t size)
{
    struct dp_vs_route_conf *cf = (void *)conf;
    struct netif_port *dev;
    uint32_t flags = 0;

    if (!conf || size < sizeof(*cf))
        return EDPVS_INVAL;

    if (cf->af != AF_INET && cf->af != AF_UNSPEC)
        return EDPVS_NOTSUPP;

    if (cf->scope == ROUTE_CF_SCOPE_HOST) {
        flags |= RTF_LOCALIN;

        if (inet_is_addr_any(cf->af, &cf->dst) || cf->plen != 32)
            return EDPVS_INVAL;
    } 
    else if (cf->scope == ROUTE_CF_SCOPE_KNI) {
        flags |= RTF_KNI;
        if (inet_is_addr_any(cf->af, &cf->dst) || cf->plen != 32)
            return EDPVS_INVAL;
    }
    else {
        if (inet_is_addr_any(cf->af, &cf->dst)) {
            flags |= RTF_DEFAULT;
        } else {
            flags |= RTF_FORWARD;
        }
    }

    dev = netif_port_get_by_name(cf->ifname);
    if (!dev) /* no dev is OK ? */
        return EDPVS_INVAL;

    switch (opt) {
    case SOCKOPT_SET_ROUTE_ADD:
	return route_add(&cf->dst.in, cf->plen, flags,
                         &cf->via.in, dev, &cf->src.in, cf->mtu, cf->metric);
    case SOCKOPT_SET_ROUTE_DEL:
        return route_del(&cf->dst.in, cf->plen, flags,
                         &cf->via.in, dev, &cf->src.in, cf->mtu, cf->metric);
    case SOCKOPT_SET_ROUTE_SET:
        return EDPVS_NOTSUPP;
    case SOCKOPT_SET_ROUTE_FLUSH:
        return EDPVS_NOTSUPP;
    default:
        return EDPVS_NOTSUPP;
    }
}

static void route_fill_conf(int af, struct dp_vs_route_conf *cf, 
                           const struct route_entry *entry)
{
    memset(cf, 0, sizeof(*cf));

    /* 
     * FIXME: 
     * some config fields are not implemented in route_entry.
     */
    cf->af      = af;
    cf->dst.in  = entry->dest;
    cf->plen    = entry->netmask;
    cf->via.in  = entry->gw;
    cf->src.in  = entry->src;
    cf->mtu     = entry->mtu;
    cf->metric  = entry->metric;

    if (entry->flag & RTF_LOCALIN){
        cf->scope = ROUTE_CF_SCOPE_HOST;
    } else if (entry->flag & RTF_KNI) {
        cf->scope = ROUTE_CF_SCOPE_KNI;
    } else if (entry->gw.s_addr == htonl(INADDR_ANY)) {
        cf->scope = ROUTE_CF_SCOPE_LINK;
        cf->flags |= ROUTE_CF_FLAG_ONLINK;
    } else {
        cf->scope = ROUTE_CF_SCOPE_GLOBAL;
    }

    if (entry->port)
        snprintf(cf->ifname, sizeof(cf->ifname), "%s", entry->port->name);

    return;
}

static int route_sockopt_get(sockoptid_t opt, const void *conf, size_t size, 
                             void **out, size_t *outsize)
{
    const struct dp_vs_route_conf *cf;
    struct dp_vs_route_conf_array *array;
    size_t nroute, hash;
    struct route_entry *entry;
    int off = 0;

    if (conf && size >= sizeof(*cf))
        cf = conf;
    else
        cf = NULL;

    /* FIXME: route number may change, need bigger lock ! */
    nroute = rte_atomic32_read(&this_num_routes);

    *outsize = sizeof(struct dp_vs_route_conf_array) + \
               nroute * sizeof(struct dp_vs_route_conf);
    *out = rte_calloc_socket(NULL, 1, *outsize, 0, rte_socket_id());
    if (!(*out))
        return EDPVS_NOMEM;
    array = *out;
    array->nroute = nroute;

    /* XXX: again, the lock is not safe ! */
    for (hash = 0; hash < LOCAL_ROUTE_TAB_SIZE; hash++) {
        list_for_each_entry(entry, &this_local_route_table[hash], list) {
            if (off >= nroute)
                break;
            route_fill_conf(AF_INET, &array->routes[off++], entry);
        }
    }

    list_for_each_entry(entry, &this_net_route_table, list) {
        if (off >= nroute)
            break;
        route_fill_conf(AF_INET, &array->routes[off++], entry);
    }

    return EDPVS_OK;
}

static int route_msg_process(bool add, struct dpvs_msg *msg)
{
    struct dp_vs_route_conf *cf;
    int err;

    assert(msg);
    if (msg->len != sizeof(struct dp_vs_route_conf)) {
        RTE_LOG(ERR, ROUTE, "%s: bad message.\n", __func__);
        return EDPVS_INVAL;
    }

    /* set route config */
    cf = (struct dp_vs_route_conf *)msg->data;
    if (add)
        err = route_add_lcore(&cf->dst.in, cf->plen, cf->flags,
                              &cf->via.in, netif_port_get_by_name(cf->ifname), 
                              &cf->src.in, cf->mtu, cf->metric);
    else
        err = route_del_lcore(&cf->dst.in, cf->plen, cf->flags,
                              &cf->via.in, netif_port_get_by_name(cf->ifname), 
                              &cf->src.in, cf->mtu, cf->metric);
    if (err != EDPVS_OK)
        RTE_LOG(ERR, ROUTE, "%s: fail to %s route: %s.\n", 
                __func__, add ? "add" : "del", dpvs_strerror(err));

    return err;
}

static int route_add_msg_cb(struct dpvs_msg *msg)
{
    return route_msg_process(true, msg);
}

static int route_del_msg_cb(struct dpvs_msg *msg)
{
    return route_msg_process(false, msg);
}

static struct dpvs_sockopts route_sockopts = {
    .version        = SOCKOPT_VERSION,
    .set_opt_min    = SOCKOPT_SET_ROUTE_ADD,
    .set_opt_max    = SOCKOPT_SET_ROUTE_FLUSH,
    .set            = route_sockopt_set,
    .get_opt_min    = SOCKOPT_GET_ROUTE_SHOW,
    .get_opt_max    = SOCKOPT_GET_ROUTE_SHOW,
    .get            = route_sockopt_get,
};

static int route_lcore_init(void *arg)
{
    int i;

    if (!rte_lcore_is_enabled(rte_lcore_id()))
        return EDPVS_DISABLED;

    for (i = 0; i < LOCAL_ROUTE_TAB_SIZE; i++)
        INIT_LIST_HEAD(&this_local_route_table[i]);
    INIT_LIST_HEAD(&this_net_route_table);

    return EDPVS_OK;
}

static int route_lcore_term(void *arg)
{
    if (!rte_lcore_is_enabled(rte_lcore_id()))
        return EDPVS_DISABLED;

    return route_flush();
}

int route_init(void)
{
    int err;
    lcoreid_t cid;
    struct dpvs_msg_type msg_type;

    rte_atomic32_set(&this_num_routes, 0);

    /* master core also need routes */
    rte_eal_mp_remote_launch(route_lcore_init, NULL, CALL_MASTER);
    RTE_LCORE_FOREACH_SLAVE(cid) {
        if ((err = rte_eal_wait_lcore(cid)) < 0) {
            RTE_LOG(WARNING, ROUTE, "%s: lcore %d: %s.\n", 
                    __func__, cid, dpvs_strerror(err));
            return err;
        }
    }

    memset(&msg_type, 0, sizeof(struct dpvs_msg_type));
    msg_type.type   = MSG_TYPE_ROUTE_ADD;
    msg_type.mode   = DPVS_MSG_MULTICAST;
    msg_type.cid    = rte_lcore_id();
    msg_type.unicast_msg_cb = route_add_msg_cb;
    err = msg_type_mc_register(&msg_type);
    if (err != EDPVS_OK) {
        RTE_LOG(ERR, ROUTE, "%s: fail to register msg.\n", __func__);
        return err;
    }

    memset(&msg_type, 0, sizeof(struct dpvs_msg_type));
    msg_type.type   = MSG_TYPE_ROUTE_DEL;
    msg_type.mode   = DPVS_MSG_MULTICAST;
    msg_type.cid    = rte_lcore_id();
    msg_type.unicast_msg_cb = route_del_msg_cb;
    err = msg_type_mc_register(&msg_type);
    if (err != EDPVS_OK) {
        RTE_LOG(ERR, ROUTE, "%s: fail to register msg.\n", __func__);
        return err;
    }

    if ((err = sockopt_register(&route_sockopts)) != EDPVS_OK)
        return err;

    return EDPVS_OK;
}

int route_term(void)
{
    int err;
    lcoreid_t cid;

    if ((err = sockopt_unregister(&route_sockopts)) != EDPVS_OK)
        return err;
    
    rte_eal_mp_remote_launch(route_lcore_term, NULL, CALL_MASTER);
    RTE_LCORE_FOREACH_SLAVE(cid) {
        if ((err = rte_eal_wait_lcore(cid)) < 0) {
            RTE_LOG(WARNING, ROUTE, "%s: lcore %d: %s.\n", 
                    __func__, cid, dpvs_strerror(err));
        }
    }

    return EDPVS_OK;
}