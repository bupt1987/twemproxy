/*
 * twemproxy - A fast and lightweight proxy for memcached protocol.
 * Copyright (C) 2011 Twitter, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <stdlib.h>

#include <nc_core.h>
#include <nc_server.h>
#include <nc_hashkit.h>
#include <hash_map.h>

#define KINGDOM_CONTINUUM_ADDITION   10  /* # extra slots to build into continuum */
#define KINGDOM_POINTS_PER_SERVER    1
#define MAX_SERVER_NAME_LEN          86
#define KINGDOM_SPLIT_CHAR (':')

rstatus_t
server_name_update(struct server_pool *pool) {
    uint32_t nserver;             /* # server - live and dead */
    uint32_t nlive_server;        /* # live server */
    uint32_t pointer_per_server;  /* pointers per server proportional to weight */
    uint32_t pointer_counter;     /* # pointers on continuum */
    uint32_t points_per_server;   /* points per server */
    uint32_t continuum_index;     /* continuum index */
    uint32_t continuum_addition;  /* extra space in the continuum */
    uint32_t server_index;        /* server index */
    int64_t now;                  /* current timestamp in usec */
    int error;

    now = nc_usec_now();
    if (now < 0) {
        return NC_ERROR;
    }

    ASSERT(!pool->auto_eject_hosts);

    nserver = array_n(&pool->server);
    nlive_server = 0;
    pool->next_rebuild = 0LL;

    for (server_index = 0; server_index < nserver; server_index++) {
        nlive_server++;
    }

    pool->nlive_server = nlive_server;

    if (nlive_server == 0) {
        ASSERT(pool->continuum != NULL);
        ASSERT(pool->ncontinuum != 0);

        log_debug(LOG_DEBUG, "no live servers for pool %"PRIu32" '%.*s'",
                  pool->idx, pool->name.len, pool->name.data);

        return NC_OK;
    }
    log_debug(LOG_DEBUG, "%"PRIu32" of %"PRIu32" servers are live for pool "
              "%"PRIu32" '%.*s'", nlive_server, nserver, pool->idx,
              pool->name.len, pool->name.data);

    continuum_addition = KINGDOM_CONTINUUM_ADDITION;
    points_per_server = KINGDOM_POINTS_PER_SERVER;

    /*
     * Allocate the continuum for the pool, the first time, and every time we
     * add a new server to the pool
     */
    if (nlive_server > pool->nserver_continuum) {
        struct continuum *continuum;
        uint32_t nserver_continuum = nlive_server + KINGDOM_CONTINUUM_ADDITION;
        uint32_t ncontinuum = nserver_continuum * KINGDOM_POINTS_PER_SERVER;

        continuum = nc_realloc(pool->continuum, sizeof(*continuum) * ncontinuum);
        if (continuum == NULL) {
            return NC_ENOMEM;
        }

        pool->continuum = continuum;
        pool->nserver_continuum = nserver_continuum;
        /* pool->ncontinuum is initialized later as it could be <= ncontinuum */
    }

    pool->hm_server = hashmap_new();

    /* update the continuum with the servers that are live */
    continuum_index = 0;
    pointer_counter = 0;
    for (server_index = 0; server_index < nserver; server_index++) {
        struct server *server = array_get(&pool->server, server_index);

        ASSERT(server->name.len > 0);

        pointer_per_server = 1;

        pool->continuum[continuum_index].index = server_index;
        pool->continuum[continuum_index++].value = 0;

        pointer_counter += pointer_per_server;

        error = hashmap_put(pool->hm_server, server->name.data, server_index);
        ASSERT(error == MAP_OK);

        log_debug(LOG_VERB, "add server %s in hm_server", server->name.data);

    }
    pool->ncontinuum = pointer_counter;

    log_debug(LOG_VERB, "updated pool %"PRIu32" '%.*s' with %"PRIu32" of "
              "%"PRIu32" servers live in %"PRIu32" slots and %"PRIu32" "
              "active points in %"PRIu32" slots", pool->idx,
              pool->name.len, pool->name.data, nlive_server, nserver,
              pool->nserver_continuum, pool->ncontinuum,
              (pool->nserver_continuum + continuum_addition) * points_per_server);

    return NC_OK;

}

uint32_t
server_name_dispatch(struct continuum *continuum, uint32_t ncontinuum, uint32_t hash) {
    ASSERT(continuum != NULL);
    ASSERT(ncontinuum != 0);

    if (hash >= ncontinuum) {
        return ncontinuum;
    }

    return hash;
}

uint32_t
hash_server_name(struct server_pool *pool, const char *key, size_t key_length) {

    int x;
    bool found = false;
    const char *tmp = key;

    for (x = 0; x < key_length; x++) {
        if (*key == KINGDOM_SPLIT_CHAR) {
            found = true;
            break;
        }
        *key++;
    }

    if (!found) {
        log_error("key do not match (^\\w+:.+)");
        return pool->ncontinuum;
    }

    char server_name[MAX_SERVER_NAME_LEN] = "";
    snprintf(server_name, MAX_SERVER_NAME_LEN, "%.*s", x, tmp);

    uint32_t index;
    int error = hashmap_get(pool->hm_server, server_name, &index);

    if (error != MAP_OK) {
        log_error("do not found match server by key: %.*s", key_length, tmp);
        return pool->ncontinuum;
    }

    return index;
}
