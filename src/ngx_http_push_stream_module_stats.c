/*
 * ngx_http_push_stream_module_stats.c
 *
 * Dimensional connection counters.
 *
 * A location can declare any number of independent counter groups:
 *
 *     push_stream_stat_counter <group> <key-template> [max=N] [keys=N];
 *
 * <key-template> is an nginx complex value, rendered ONCE when a subscriber
 * connection is registered. The rendered string (sanitised, capped at `max`
 * bytes, default NGX_HTTP_PUSH_STREAM_STAT_DEFAULT_KEY_LEN) is the key inside
 * that group. Each (group, key) pair is one bucket holding:
 *
 *     active  - currently-connected count (gauge), live inc at connect / dec at
 *               disconnect, exactly like channel->subscribers. Drifts up only if
 *               a worker dies hard (missed decrement); GET ...?reset=active zeroes
 *               all gauges.
 *     total   - connections ever opened (monotonic, +1 per connect), survives
 *               "nginx -s reload".
 *
 * Buckets live in shared memory on ngx_http_push_stream_shm_data_s (the struct
 * the reload branch of ngx_http_push_stream_init_shm_zone reuses), are never
 * freed, and a "$ps_transport" variable is provided so the transport can be
 * composed into templates. When a group hits its key cap new keys fold into a
 * single "__overflow__" bucket for that group.
 *
 * The endpoint:
 *
 *     location = /stats/live { push_stream_stats_dimensions; }
 *
 *     { "generated": <epoch>,
 *       "counters": { "<group>": { "<key>": {"a":<active>,"t":<total>}, ... }, ... } }
 */

static ngx_int_t ngx_http_push_stream_stats_dimensions_handler(ngx_http_request_t *r);

static const ngx_str_t NGX_HTTP_PUSH_STREAM_STAT_OVERFLOW_KEY = ngx_string("__overflow__");
static ngx_str_t       NGX_HTTP_PUSH_STREAM_STAT_VAR_TRANSPORT = ngx_string("ps_transport");


static ngx_int_t
ngx_http_push_stream_stat_bucket_cmp(ngx_http_push_stream_stat_bucket_t *a, ngx_http_push_stream_stat_bucket_t *b)
{
    ngx_int_t rc;

    if (a->group.len != b->group.len) {
        return (a->group.len < b->group.len) ? -1 : 1;
    }
    if ((rc = ngx_memcmp(a->group.data, b->group.data, a->group.len)) != 0) {
        return rc;
    }
    if (a->key.len != b->key.len) {
        return (a->key.len < b->key.len) ? -1 : 1;
    }
    return ngx_memcmp(a->key.data, b->key.data, a->key.len);
}


static void
ngx_http_push_stream_stats_rbtree_insert(ngx_rbtree_node_t *temp, ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel)
{
    ngx_rbtree_node_t                  **p;

    for ( ;; ) {
        if (node->key != temp->key) {
            p = (node->key < temp->key) ? &temp->left : &temp->right;
        } else {
            p = (ngx_http_push_stream_stat_bucket_cmp((ngx_http_push_stream_stat_bucket_t *) node,
                                                      (ngx_http_push_stream_stat_bucket_t *) temp) < 0)
                ? &temp->left : &temp->right;
        }

        if (*p == sentinel) {
            break;
        }
        temp = *p;
    }

    *p = node;
    node->parent = temp;
    node->left = sentinel;
    node->right = sentinel;
    ngx_rbt_red(node);
}


static ngx_http_push_stream_stat_bucket_t *
ngx_http_push_stream_stat_bucket_find(ngx_http_push_stream_shm_data_t *d, uint32_t hash, ngx_str_t *group, ngx_str_t *key)
{
    ngx_rbtree_node_t                  *node = d->stats_tree.root;
    ngx_rbtree_node_t                  *sentinel = d->stats_tree.sentinel;
    ngx_http_push_stream_stat_bucket_t  probe;
    ngx_int_t                           rc;

    probe.group = *group;
    probe.key = *key;

    while (node != sentinel) {
        if (hash != node->key) {
            node = (hash < node->key) ? node->left : node->right;
            continue;
        }
        rc = ngx_http_push_stream_stat_bucket_cmp(&probe, (ngx_http_push_stream_stat_bucket_t *) node);
        if (rc == 0) {
            return (ngx_http_push_stream_stat_bucket_t *) node;
        }
        node = (rc < 0) ? node->left : node->right;
    }
    return NULL;
}


/* d->stats_mutex held. Find-or-create the per-group key-count node. */
static ngx_http_push_stream_stat_group_t *
ngx_http_push_stream_stat_group_get(ngx_http_push_stream_shm_data_t *d, ngx_slab_pool_t *shpool, ngx_str_t *group)
{
    ngx_queue_t                       *q;
    ngx_http_push_stream_stat_group_t  *g;

    for (q = ngx_queue_head(&d->stats_groups); q != ngx_queue_sentinel(&d->stats_groups); q = ngx_queue_next(q)) {
        g = ngx_queue_data(q, ngx_http_push_stream_stat_group_t, queue);
        if ((g->name.len == group->len) && (ngx_memcmp(g->name.data, group->data, group->len) == 0)) {
            return g;
        }
    }

    g = ngx_slab_alloc(shpool, sizeof(ngx_http_push_stream_stat_group_t));
    if (g == NULL) {
        return NULL;
    }
    g->name.data = ngx_slab_alloc(shpool, group->len ? group->len : 1);
    if (g->name.data == NULL) {
        ngx_slab_free(shpool, g);
        return NULL;
    }
    ngx_memcpy(g->name.data, group->data, group->len);
    g->name.len = group->len;
    g->nkeys = 0;
    ngx_queue_insert_tail(&d->stats_groups, &g->queue);
    return g;
}


/*
 * Find (or create) the (group, key) bucket and bump active + total.
 * Self-locking on d->stats_mutex; the rare create path also takes shpool->mutex
 * via self-locking ngx_slab_alloc (lock order: stats_mutex -> shpool->mutex,
 * and nothing ever takes them the other way round). Returns the bucket, or NULL
 * on shared-memory exhaustion. `max_keys` is applied PER GROUP.
 */
static ngx_http_push_stream_stat_bucket_t *
ngx_http_push_stream_stat_bucket_touch(ngx_http_push_stream_shm_data_t *d, ngx_slab_pool_t *shpool,
    ngx_str_t *group, ngx_str_t *key, ngx_uint_t max_keys)
{
    uint32_t                             crc;
    ngx_http_push_stream_stat_bucket_t  *b;
    ngx_http_push_stream_stat_group_t   *g;
    size_t                               klen;
    ngx_flag_t                           is_overflow;

    crc = ngx_crc32_long(group->data, group->len);
    crc ^= ngx_crc32_long(key->data, key->len);

    is_overflow = (key->len == NGX_HTTP_PUSH_STREAM_STAT_OVERFLOW_KEY.len)
        && (ngx_memcmp(key->data, NGX_HTTP_PUSH_STREAM_STAT_OVERFLOW_KEY.data, key->len) == 0);

    ngx_shmtx_lock(&d->stats_mutex);

    b = ngx_http_push_stream_stat_bucket_find(d, crc, group, key);

    if (b == NULL) {
        g = ngx_http_push_stream_stat_group_get(d, shpool, group);
        if (g == NULL) {
            ngx_shmtx_unlock(&d->stats_mutex);
            return NULL;
        }

        if ((max_keys > 0) && !is_overflow && (g->nkeys >= max_keys)) {
            ngx_shmtx_unlock(&d->stats_mutex);
            return ngx_http_push_stream_stat_bucket_touch(d, shpool, group,
                (ngx_str_t *) &NGX_HTTP_PUSH_STREAM_STAT_OVERFLOW_KEY, 0);
        }

        klen = key->len ? key->len : 1;

        b = ngx_slab_alloc(shpool, sizeof(ngx_http_push_stream_stat_bucket_t));
        if (b == NULL) {
            ngx_shmtx_unlock(&d->stats_mutex);
            return NULL;
        }
        ngx_memzero(b, sizeof(ngx_http_push_stream_stat_bucket_t));

        b->group.data = ngx_slab_alloc(shpool, group->len ? group->len : 1);
        b->key.data = ngx_slab_alloc(shpool, klen);
        if ((b->group.data == NULL) || (b->key.data == NULL)) {
            if (b->group.data != NULL) ngx_slab_free(shpool, b->group.data);
            if (b->key.data != NULL) ngx_slab_free(shpool, b->key.data);
            ngx_slab_free(shpool, b);
            ngx_shmtx_unlock(&d->stats_mutex);
            return NULL;
        }

        ngx_memcpy(b->group.data, group->data, group->len);
        b->group.len = group->len;
        ngx_memcpy(b->key.data, key->data, key->len);
        b->key.len = key->len;
        b->node.key = crc;

        ngx_rbtree_insert(&d->stats_tree, &b->node);
        ngx_queue_insert_tail(&d->stats_queue, &b->queue);
        d->stats_buckets++;
        g->nkeys++;
    }

    b->active++;
    b->total++;

    ngx_shmtx_unlock(&d->stats_mutex);
    return b;
}


/*
 * Filter a rendered key to a JSON- and separator-safe alphabet
 * ([0-9A-Za-z] plus _ - . : | /), cap at `max` bytes, never empty.
 */
static ngx_int_t
ngx_http_push_stream_stat_sanitize_key(ngx_pool_t *pool, ngx_str_t *in, ngx_uint_t max, ngx_str_t *out)
{
    ngx_uint_t   i, n;
    u_char       c, *p;

    n = in->len;
    if (n > max) {
        n = max;
    }

    p = ngx_pnalloc(pool, n ? n : 1);
    if (p == NULL) {
        return NGX_ERROR;
    }

    for (i = 0; i < n; i++) {
        c = in->data[i];
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
            || c == '_' || c == '-' || c == '.' || c == ':' || c == '|' || c == '/')
        {
            p[i] = c;
        } else {
            p[i] = '_';
        }
    }

    if (n == 0) {
        p[0] = '_';
        n = 1;
    }

    out->data = p;
    out->len = n;
    return NGX_OK;
}


/* replace every "~channel~" in `in` with `chan`, into a fresh pool buffer.
   Returns `in` unchanged when the token is absent (common: per=connection). */
static void
ngx_http_push_stream_stat_expand_channel(ngx_pool_t *pool, ngx_str_t *in, ngx_str_t *chan, ngx_str_t *out)
{
    static const ngx_str_t  tok = ngx_string("~channel~");
    u_char                 *p, *s, *end, *w;
    ngx_uint_t              hits = 0;

    *out = *in;

    if (in->len < tok.len) {
        return;
    }

    end = in->data + in->len;
    for (p = in->data; p + tok.len <= end; ) {
        if (ngx_memcmp(p, tok.data, tok.len) == 0) {
            hits++;
            p += tok.len;
        } else {
            p++;
        }
    }
    if (hits == 0) {
        return;
    }

    w = ngx_pnalloc(pool, in->len + hits * (chan->len > tok.len ? chan->len - tok.len : 0) + 1);
    if (w == NULL) {
        return; /* out stays = in */
    }
    out->data = w;

    for (s = in->data; s < end; ) {
        if ((s + tok.len <= end) && (ngx_memcmp(s, tok.data, tok.len) == 0)) {
            w = ngx_cpymem(w, chan->data, chan->len);
            s += tok.len;
        } else {
            *w++ = *s++;
        }
    }
    out->len = w - out->data;
}


/*
 * Resolve one counter's key for this request (+ optional channel id for the
 * ~channel~ token) and bump its bucket, appending the bucket pointer to
 * buckets[*n]. Shared by the per=connection and per=channel paths.
 */
static void
ngx_http_push_stream_stat_apply(ngx_http_request_t *r, ngx_http_push_stream_main_conf_t *mcf,
    ngx_http_push_stream_stat_counter_t *cc, ngx_str_t *channel_id,
    ngx_http_push_stream_stat_bucket_t **buckets, ngx_uint_t *n)
{
    ngx_http_push_stream_shm_data_t     *d = mcf->shm_data;
    ngx_slab_pool_t                     *shpool = mcf->shpool;
    ngx_uint_t                           maxlen  = cc->max_key_len ? cc->max_key_len : NGX_HTTP_PUSH_STREAM_STAT_DEFAULT_KEY_LEN;
    ngx_uint_t                           maxkeys = cc->max_keys ? cc->max_keys : mcf->stat_max_keys;
    ngx_str_t                            rendered, expanded, key;
    ngx_http_push_stream_stat_bucket_t  *b;

    if (ngx_http_complex_value(r, &cc->key_template, &rendered) != NGX_OK) {
        return;
    }

    if (channel_id != NULL) {
        ngx_http_push_stream_stat_expand_channel(r->pool, &rendered, channel_id, &expanded);
    } else {
        expanded = rendered;
    }

    if (ngx_http_push_stream_stat_sanitize_key(r->pool, &expanded, maxlen, &key) != NGX_OK) {
        return;
    }

    b = ngx_http_push_stream_stat_bucket_touch(d, shpool, &cc->group, &key, maxkeys);
    if (b != NULL) {
        buckets[(*n)++] = b;
    }
}


void
ngx_http_push_stream_stat_register(ngx_http_request_t *r, ngx_http_push_stream_subscriber_t *sub)
{
    ngx_http_push_stream_loc_conf_t      *cf = ngx_http_get_module_loc_conf(r, ngx_http_push_stream_module);
    ngx_http_push_stream_main_conf_t     *mcf = ngx_http_get_module_main_conf(r, ngx_http_push_stream_module);
    ngx_http_push_stream_stat_counter_t  *cc;
    ngx_uint_t                            i, n = 0;

    sub->stat_buckets = NULL;
    sub->stat_buckets_n = 0;

    if ((cf->stat_counters == NULL) || (cf->stat_counters == NGX_CONF_UNSET_PTR)
        || (cf->stat_counters->nelts == 0))
    {
        return;
    }

    sub->stat_buckets = ngx_pcalloc(r->pool,
        sizeof(ngx_http_push_stream_stat_bucket_t *) * cf->stat_counters->nelts);
    if (sub->stat_buckets == NULL) {
        return;
    }

    cc = cf->stat_counters->elts;
    for (i = 0; i < cf->stat_counters->nelts; i++) {
        if (cc[i].per_channel) {
            continue;   /* handled per subscription, see stat_subscription_register */
        }
        ngx_http_push_stream_stat_apply(r, mcf, &cc[i], NULL, sub->stat_buckets, &n);
    }

    sub->stat_buckets_n = n;
}


void
ngx_http_push_stream_stat_subscription_register(ngx_http_push_stream_subscription_t *subscription)
{
    ngx_http_request_t                  *r;
    ngx_http_push_stream_loc_conf_t      *cf;
    ngx_http_push_stream_main_conf_t     *mcf;
    ngx_http_push_stream_stat_counter_t  *cc;
    ngx_uint_t                            i, n = 0;

    subscription->stat_buckets = NULL;
    subscription->stat_buckets_n = 0;

    if ((subscription->subscriber == NULL) || (subscription->subscriber->request == NULL)
        || (subscription->channel == NULL))
    {
        return;
    }

    r = subscription->subscriber->request;
    cf = ngx_http_get_module_loc_conf(r, ngx_http_push_stream_module);

    if ((cf->stat_counters == NULL) || (cf->stat_counters == NGX_CONF_UNSET_PTR)
        || (cf->stat_counters->nelts == 0))
    {
        return;
    }

    mcf = ngx_http_get_module_main_conf(r, ngx_http_push_stream_module);

    subscription->stat_buckets = ngx_pcalloc(r->pool,
        sizeof(ngx_http_push_stream_stat_bucket_t *) * cf->stat_counters->nelts);
    if (subscription->stat_buckets == NULL) {
        return;
    }

    cc = cf->stat_counters->elts;
    for (i = 0; i < cf->stat_counters->nelts; i++) {
        if (!cc[i].per_channel) {
            continue;
        }
        ngx_http_push_stream_stat_apply(r, mcf, &cc[i], &subscription->channel->id,
            subscription->stat_buckets, &n);
    }

    subscription->stat_buckets_n = n;
}


void
ngx_http_push_stream_stat_subscription_unregister(ngx_http_push_stream_subscription_t *subscription)
{
    ngx_http_push_stream_main_conf_t   *mcf;
    ngx_http_push_stream_shm_data_t    *d;
    ngx_http_push_stream_stat_bucket_t *b;
    ngx_uint_t                          i;

    if ((subscription == NULL) || (subscription->stat_buckets == NULL) || (subscription->stat_buckets_n == 0)) {
        return;
    }
    if ((subscription->subscriber == NULL) || (subscription->subscriber->request == NULL)) {
        return;
    }

    mcf = ngx_http_get_module_main_conf(subscription->subscriber->request, ngx_http_push_stream_module);
    d = mcf->shm_data;

    ngx_shmtx_lock(&d->stats_mutex);
    for (i = 0; i < subscription->stat_buckets_n; i++) {
        b = subscription->stat_buckets[i];
        if ((b != NULL) && (b->active > 0)) {
            b->active--;
        }
    }
    ngx_shmtx_unlock(&d->stats_mutex);

    subscription->stat_buckets = NULL;
    subscription->stat_buckets_n = 0;
}


void
ngx_http_push_stream_stat_unregister(ngx_http_push_stream_subscriber_t *sub)
{
    ngx_http_push_stream_main_conf_t  *mcf;
    ngx_http_push_stream_shm_data_t   *d;
    ngx_http_push_stream_stat_bucket_t *b;
    ngx_uint_t                         i;

    if ((sub == NULL) || (sub->stat_buckets == NULL) || (sub->stat_buckets_n == 0)) {
        return;
    }

    mcf = ngx_http_get_module_main_conf(sub->request, ngx_http_push_stream_module);
    d = mcf->shm_data;

    ngx_shmtx_lock(&d->stats_mutex);
    for (i = 0; i < sub->stat_buckets_n; i++) {
        b = sub->stat_buckets[i];
        if ((b != NULL) && (b->active > 0)) {
            b->active--;
        }
    }
    ngx_shmtx_unlock(&d->stats_mutex);

    sub->stat_buckets = NULL;
    sub->stat_buckets_n = 0;
}


ngx_int_t
ngx_http_push_stream_stats_init_shm(ngx_http_push_stream_shm_data_t *d)
{
    ngx_rbtree_init(&d->stats_tree, &d->stats_sentinel, ngx_http_push_stream_stats_rbtree_insert);
    ngx_queue_init(&d->stats_queue);
    ngx_queue_init(&d->stats_groups);
    d->stats_buckets = 0;

    if (ngx_http_push_stream_create_shmtx(&d->stats_mutex, &d->stats_lock, (u_char *) "push_stream_stats") != NGX_OK) {
        return NGX_ERROR;
    }
    return NGX_OK;
}


/* $ps_transport - "ws" | "lp" | "poll" | "stream" | "es" | "other" */
static ngx_int_t
ngx_http_push_stream_ps_transport_variable(ngx_http_request_t *r, ngx_http_variable_value_t *v, uintptr_t data)
{
    ngx_http_push_stream_loc_conf_t  *cf = ngx_http_get_module_loc_conf(r, ngx_http_push_stream_module);
    ngx_str_t                         s;

    switch (cf->location_type) {
    case NGX_HTTP_PUSH_STREAM_SUBSCRIBER_MODE_WEBSOCKET:   ngx_str_set(&s, "ws");     break;
    case NGX_HTTP_PUSH_STREAM_SUBSCRIBER_MODE_LONGPOLLING: ngx_str_set(&s, "lp");     break;
    case NGX_HTTP_PUSH_STREAM_SUBSCRIBER_MODE_POLLING:     ngx_str_set(&s, "poll");   break;
    case NGX_HTTP_PUSH_STREAM_SUBSCRIBER_MODE_STREAMING:   ngx_str_set(&s, "stream"); break;
    case NGX_HTTP_PUSH_STREAM_SUBSCRIBER_MODE_EVENTSOURCE: ngx_str_set(&s, "es");     break;
    default:                                               ngx_str_set(&s, "other"); break;
    }

    v->len = s.len;
    v->data = s.data;
    v->valid = 1;
    v->no_cacheable = 0;
    v->not_found = 0;
    return NGX_OK;
}


ngx_int_t
ngx_http_push_stream_stats_add_variables(ngx_conf_t *cf)
{
    ngx_http_variable_t  *var;

    var = ngx_http_add_variable(cf, &NGX_HTTP_PUSH_STREAM_STAT_VAR_TRANSPORT, 0);
    if (var == NULL) {
        return NGX_ERROR;
    }
    var->get_handler = ngx_http_push_stream_ps_transport_variable;

    return NGX_OK;
}


/* ── config directives ─────────────────────────────────────────────────── */

char *
ngx_http_push_stream_set_stat_counter(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_push_stream_loc_conf_t      *lcf = conf;
    ngx_http_push_stream_stat_counter_t  *sc;
    ngx_http_compile_complex_value_t      ccv;
    ngx_str_t                            *value, *a;
    ngx_int_t                             n;
    ngx_uint_t                            i;

    if (lcf->stat_counters == NGX_CONF_UNSET_PTR) {
        lcf->stat_counters = ngx_array_create(cf->pool, 4, sizeof(ngx_http_push_stream_stat_counter_t));
        if (lcf->stat_counters == NULL) {
            return NGX_CONF_ERROR;
        }
    }

    sc = ngx_array_push(lcf->stat_counters);
    if (sc == NULL) {
        return NGX_CONF_ERROR;
    }
    ngx_memzero(sc, sizeof(ngx_http_push_stream_stat_counter_t));

    value = cf->args->elts;
    sc->group = value[1];

    if (sc->group.len == 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
            "push stream module: push_stream_stat_counter group name must not be empty");
        return NGX_CONF_ERROR;
    }

    ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));
    ccv.cf = cf;
    ccv.value = &value[2];
    ccv.complex_value = &sc->key_template;
    if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    for (i = 3; i < cf->args->nelts; i++) {
        a = &value[i];
        if ((a->len > 4) && (ngx_strncmp(a->data, "max=", 4) == 0)) {
            n = ngx_atoi(a->data + 4, a->len - 4);
            if ((n == NGX_ERROR) || (n <= 0)) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                    "push stream module: invalid \"max=\" in push_stream_stat_counter");
                return NGX_CONF_ERROR;
            }
            sc->max_key_len = (ngx_uint_t) n;
        } else if ((a->len > 5) && (ngx_strncmp(a->data, "keys=", 5) == 0)) {
            n = ngx_atoi(a->data + 5, a->len - 5);
            if ((n == NGX_ERROR) || (n <= 0)) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                    "push stream module: invalid \"keys=\" in push_stream_stat_counter");
                return NGX_CONF_ERROR;
            }
            sc->max_keys = (ngx_uint_t) n;
        } else if ((a->len == sizeof("per=channel") - 1) && (ngx_strncmp(a->data, "per=channel", a->len) == 0)) {
            sc->per_channel = 1;
        } else if ((a->len == sizeof("per=connection") - 1) && (ngx_strncmp(a->data, "per=connection", a->len) == 0)) {
            sc->per_channel = 0;
        } else {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                "push stream module: unknown push_stream_stat_counter option \"%V\"", a);
            return NGX_CONF_ERROR;
        }
    }

    return NGX_CONF_OK;
}


char *
ngx_http_push_stream_stats_dimensions(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    char *rc = ngx_http_push_stream_setup_handler(cf, conf, &ngx_http_push_stream_stats_dimensions_handler);

    if (rc == NGX_CONF_OK) {
        ((ngx_http_push_stream_loc_conf_t *) conf)->location_type = NGX_HTTP_PUSH_STREAM_STATS_DIMENSIONS_MODE;
    }

    return rc;
}


/* ── endpoint ──────────────────────────────────────────────────────────── */

typedef struct {
    ngx_str_t   group;
    ngx_str_t   key;
    ngx_uint_t  active;
    ngx_uint_t  total;
} ngx_http_push_stream_stat_row_t;

static int ngx_libc_cdecl
ngx_http_push_stream_stat_row_cmp(const void *one, const void *two)
{
    const ngx_http_push_stream_stat_row_t *a = one, *b = two;
    ngx_int_t rc;

    if (a->group.len != b->group.len) {
        return (a->group.len < b->group.len) ? -1 : 1;
    }
    if ((rc = ngx_memcmp(a->group.data, b->group.data, a->group.len)) != 0) {
        return (rc < 0) ? -1 : 1;
    }
    if (a->key.len != b->key.len) {
        return (a->key.len < b->key.len) ? -1 : 1;
    }
    rc = ngx_memcmp(a->key.data, b->key.data, a->key.len);
    return (rc < 0) ? -1 : (rc > 0 ? 1 : 0);
}

static ngx_int_t
ngx_http_push_stream_stats_dimensions_handler(ngx_http_request_t *r)
{
    ngx_http_push_stream_main_conf_t  *mcf = ngx_http_get_module_main_conf(r, ngx_http_push_stream_module);
    ngx_http_push_stream_shm_data_t   *d = mcf->shm_data;
    ngx_http_push_stream_stat_row_t   *rows;
    ngx_http_push_stream_stat_bucket_t *b;
    ngx_queue_t                       *q;
    ngx_str_t                         *out, arg;
    ngx_uint_t                          cap, cnt, i;
    size_t                             len;
    u_char                            *p;
    ngx_flag_t                         reset;

    ngx_http_push_stream_set_expires(r, NGX_HTTP_PUSH_STREAM_EXPIRES_EPOCH, 0);

    if (!(r->method & NGX_HTTP_GET)) {
        ngx_http_push_stream_add_response_header(r, &NGX_HTTP_PUSH_STREAM_HEADER_ALLOW, &NGX_HTTP_PUSH_STREAM_ALLOW_GET);
        return ngx_http_push_stream_send_only_header_response(r, NGX_HTTP_NOT_ALLOWED, NULL);
    }

    reset = (r->args.len > 0) && (ngx_http_arg(r, (u_char *) "reset", 5, &arg) == NGX_OK);

    if (reset) {
        ngx_shmtx_lock(&d->stats_mutex);
        for (q = ngx_queue_head(&d->stats_queue); q != ngx_queue_sentinel(&d->stats_queue); q = ngx_queue_next(q)) {
            b = ngx_queue_data(q, ngx_http_push_stream_stat_bucket_t, queue);
            b->active = 0;
        }
        ngx_shmtx_unlock(&d->stats_mutex);

        out = ngx_http_push_stream_create_str(r->pool, sizeof("{\"reset\":\"active\"}") - 1);
        if (out == NULL) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }
        out->len = ngx_sprintf(out->data, "{\"reset\":\"active\"}") - out->data;
        return ngx_http_push_stream_send_response(r, out, &NGX_HTTP_PUSH_STREAM_CONTENT_TYPE_JSON, NGX_HTTP_OK);
    }

    /* snapshot under the lock, copying key bytes into the request pool, then
       build the JSON with the lock released */
    ngx_shmtx_lock(&d->stats_mutex);
    cap = d->stats_buckets;
    rows = ngx_palloc(r->pool, sizeof(ngx_http_push_stream_stat_row_t) * (cap ? cap : 1));
    cnt = 0;
    if (rows != NULL) {
        for (q = ngx_queue_head(&d->stats_queue);
             (q != ngx_queue_sentinel(&d->stats_queue)) && (cnt < cap);
             q = ngx_queue_next(q))
        {
            u_char *gp, *kp;
            b = ngx_queue_data(q, ngx_http_push_stream_stat_bucket_t, queue);

            gp = ngx_pnalloc(r->pool, b->group.len);
            kp = ngx_pnalloc(r->pool, b->key.len);
            if ((gp == NULL) || (kp == NULL)) {
                break;
            }
            ngx_memcpy(gp, b->group.data, b->group.len);
            ngx_memcpy(kp, b->key.data, b->key.len);

            rows[cnt].group.data = gp;
            rows[cnt].group.len = b->group.len;
            rows[cnt].key.data = kp;
            rows[cnt].key.len = b->key.len;
            rows[cnt].active = b->active;
            rows[cnt].total = b->total;
            cnt++;
        }
    }
    ngx_shmtx_unlock(&d->stats_mutex);

    if (rows == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    if (cnt > 1) {
        ngx_qsort(rows, cnt, sizeof(ngx_http_push_stream_stat_row_t), ngx_http_push_stream_stat_row_cmp);
    }

    len = sizeof("{\"generated\":,\"counters\":{}}") + NGX_INT_T_LEN;
    for (i = 0; i < cnt; i++) {
        /* worst case: ,"group":{ + "key":{"a":N,"t":N} + } */
        len += rows[i].group.len + rows[i].key.len + 2 * NGX_INT_T_LEN + sizeof(",\"\":{\"\":{\"a\":,\"t\":}}");
    }

    out = ngx_http_push_stream_create_str(r->pool, len);
    if (out == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    p = ngx_sprintf(out->data, "{\"generated\":%T,\"counters\":{", ngx_time());

    for (i = 0; i < cnt; i++) {
        ngx_flag_t new_group = (i == 0)
            || (rows[i].group.len != rows[i - 1].group.len)
            || (ngx_memcmp(rows[i].group.data, rows[i - 1].group.data, rows[i].group.len) != 0);

        if (new_group) {
            if (i != 0) {
                p = ngx_cpymem(p, "},", 2);
            }
            p = ngx_sprintf(p, "\"%V\":{", &rows[i].group);
        } else {
            *p++ = ',';
        }
        p = ngx_sprintf(p, "\"%V\":{\"a\":%ui,\"t\":%ui}", &rows[i].key, rows[i].active, rows[i].total);
    }

    if (cnt != 0) {
        *p++ = '}';
    }
    *p++ = '}';
    *p++ = '}';

    out->len = p - out->data;

    return ngx_http_push_stream_send_response(r, out, &NGX_HTTP_PUSH_STREAM_CONTENT_TYPE_JSON, NGX_HTTP_OK);
}
