/* *****************************************************************************
 * Cluster Messages API
 *
 * Facil supports a message oriented API for use for Inter Process Communication
 * (IPC), publish/subscribe patterns, horizontal scaling and similar use-cases.
 **************************************************************************** */

// #include "facil_cluster.h"

#include "spnlock.inc"

#include "facil.h"
#include "fio_mem.h"

#include "fio_llist.h"
#include "fio_tmpfile.h"
#include "fiobj4sock.h"

#include <sys/types.h>

#include <signal.h>

#ifndef H_FACIL_CLUSTER_H
/* *****************************************************************************
 * Cluster Messages and Pub/Sub
 **************************************************************************** */

/** An opaque subscription type. */
typedef struct subscription_s subscription_s;

/** A pub/sub engine data structure. See details later on. */
typedef struct pubsub_engine_s pubsub_engine_s;

/** The default engine (settable). Initial default is FACIL_PUBSUB_CLUSTER. */
extern pubsub_engine_s *FACIL_PUBSUB_DEFAULT;
/** Used to publish the message to all clients in the cluster. */
#define FACIL_PUBSUB_CLUSTER ((pubsub_engine_s *)1)
/** Used to publish the message only within the current process. */
#define FACIL_PUBSUB_PROCESS ((pubsub_engine_s *)2)
/** Used to publish the message except within the current process. */
#define FACIL_PUBSUB_SIBLINGS ((pubsub_engine_s *)3)

/** Message structure, with an integer filter as well as a channel filter. */
typedef struct facil_msg_s {
  /** A unique message type. Negative values are reserved, 0 == pub/sub. */
  int32_t filter;
  /** A channel name, allowing for pub/sub patterns. */
  FIOBJ channel;
  /** The actual message. */
  FIOBJ msg;
  /** The `udata1` argument associated with the subscription. */
  void *udata1;
  /** The `udata1` argument associated with the subscription. */
  void *udata2;
} facil_msg_s;

/**
 * Pattern matching callback type - should return 0 unless channel matches
 * pattern.
 */
typedef int (*facil_match_fn)(FIOBJ pattern, FIOBJ channel);

/** possible arguments for the facil_subscribe method. */
typedef struct {
  /**
   * If `filter` is set, all messages that match the filter's numerical value
   * will be forwarded to the subscription's callback.
   *
   * Subscriptions can either require a match by filter or match by channel.
   * This will match the subscription by filter.
   */
  int32_t filter;
  /**
   * If `channel` is set, all messages where `filter == 0` and the channel is an
   * exact match will be forwarded to the subscription's callback.
   *
   * Subscriptions can either require a match by filter or match by channel.
   * This will match the subscription by channel (only messages with no `filter`
   * will be received.
   */
  FIOBJ channel;
  /**
   * The the `match` function allows pattern matching for channel names.
   *
   * When using a match function, the channel name is considered to be a pattern
   * and each pub/sub message (a message where filter == 0) will be tested
   * against that pattern.
   *
   * Using pattern subscriptions extensively could become a performance concern,
   * since channel names are tested against each distinct pattern rather than
   * using a hashmap for possible name matching.
   */
  facil_match_fn match;
  /**
   * The callback will be called for each message forwarded to the subscription.
   */
  void (*callback)(facil_msg_s *msg);
  /** An optional callback for when a subscription is fully canceled. */
  void (*on_unsubscribe)(void *udata1, void *udata2);
  /** The udata values are ignored and made available to the callback. */
  void *udata1;
  /** The udata values are ignored and made available to the callback. */
  void *udata2;
} subscribe_args_s;

/** Publishing and on_message callback arguments. */
typedef struct facil_publish_args_s {
  /** The pub/sub engine that should be used to farward this message. */
  pubsub_engine_s const *engine;
  /** A unique message type. Negative values are reserved, 0 == pub/sub. */
  int32_t filter;
  /** The pub/sub target channnel. */
  FIOBJ channel;
  /** The pub/sub message. */
  FIOBJ message;
} facil_publish_args_s;

/**
 * Subscribes to either a filter OR a channel (never both).
 *
 * Returns a subscription pointer on success or NULL on failure.
 *
 * See `subscribe_args_s` for details.
 */
subscription_s *facil_subscribe(subscribe_args_s args);

/**
 * Subscribes to a channel (enforces filter == 0).
 *
 * Returns a subscription pointer on success or NULL on failure.
 *
 * See `subscribe_args_s` for details.
 */
subscription_s *facil_subscribe_pubsub(subscribe_args_s args);

/**
 * Cancels an existing subscriptions - actual effects might be delayed, for
 * example, if the subscription's callback is running in another thread.
 */
void facil_unsubscribe(subscription_s *subscription);

/**
 * This helper returns a temporary handle to an existing subscription's channel
 * or filter.
 *
 * To keep the handle beyond the lifetime of the subscription, use `fiobj_dup`.
 */
FIOBJ facil_subscription_channel(subscription_s *subscription);

/**
 * Publishes a message to the relevant subscribers (if any).
 *
 * See `facil_publish_args_s` for details.
 *
 * By default the message is sent using the FACIL_PUBSUB_CLUSTER engine (all
 * processes, including the calling process).
 *
 * To limit the message only to other processes (exclude the calling process),
 * use the FACIL_PUBSUB_SIBLINGS engine.
 *
 * To limit the message only to the calling process, use the
 * FACIL_PUBSUB_PROCESS engine.
 *
 * To publish messages to the pub/sub layer, the `.filter` argument MUST be
 * equal to 0 or missing.
 */
void facil_publish(facil_publish_args_s args);

/** Finds the message's metadata by it's type ID. Returns the data or NULL. */
void *facil_message_metadata(facil_msg_s *msg, intptr_t type_id);

/**
 * Defers the current callback, so it will be called again for the message.
 */
void facil_message_defer(facil_msg_s *msg);

/**
 * Signals all workers to shutdown, which might invoke a respawning of the
 * workers unless the shutdown signal was received.
 *
 * NOT signal safe.
 */
void facil_cluster_signal_children(void);

/* *****************************************************************************
 * Cluster / Pub/Sub Middleware and Extensions ("Engines")
 **************************************************************************** */

/** Contains message metadata, set by message extensions. */
typedef struct facil_msg_metadata_s facil_msg_metadata_s;
struct facil_msg_metadata_s {
  /** The type ID should be used to identify the metadata's actual structure. */
  intptr_t type_id;
  /**
   * This method will be called by facil.io to cleanup the metadata resources.
   *
   * Don't alter / call this method, this data is reserved.
   */
  void (*on_finish)(facil_msg_s *msg, facil_msg_metadata_s *self);
  /** The pointer to be returned by the `facil_message_metadata` function. */
  void *metadata;
  /** RESERVED for internal use (Metadata linked list). */
  facil_msg_metadata_s *next;
};

/**
 * It's possible to attach metadata to facil.io messages before they are
 * published.
 *
 * This allows, for example, messages to be encoded as network packets for
 * outgoing protocols (i.e., encoding for WebSocket transmissions), improving
 * performance in large network based broadcasting.
 *
 * The callback should return a pointer to a valid metadata object.
 *
 * Since the cluster messaging system serializes objects to JSON (unless both
 * the channel and the data are String objects), the pre-serialized data is
 * available to the callback as the `raw_ch` and `raw_msg` arguments.
 *
 * To remove a callback, set the `remove` flag to true (`1`).
 */
void facil_message_metadata_set(
    facil_msg_metadata_s *(*callback)(facil_msg_s *msg, FIOBJ raw_ch,
                                      FIOBJ raw_msg),
    int remove);

/**
 * facil.io can be linked with external Pub/Sub services using "engines".
 *
 * Only unfiltered messages and subscriptions (where filter == 0) will be
 * forwarded to external Pub/Sub services.
 *
 * Engines MUST provide the listed function pointers and should be registered
 * using the `pubsub_engine_register` function.
 *
 * Engines should deregister, before being destroyed, by using the
 * `pubsub_engine_deregister` function.
 *
 * When an engine received a message to publish, it should call the
 * `pubsub_publish` function with the engine to which the message is forwarded.
 * i.e.:
 *
 *       pubsub_publish(
 *           .engine = FACIL_PROCESS_ENGINE,
 *           .channel = channel_name,
 *           .message = msg_body );
 *
 * Engines MUST NOT free any of the FIOBJ objects they receive.
 *
 */
struct pubsub_engine_s {
  /** Should subscribe channel. Failures are ignored. */
  void (*subscribe)(const pubsub_engine_s *eng, FIOBJ channel,
                    facil_match_fn match);
  /** Should unsubscribe channel. Failures are ignored. */
  void (*unsubscribe)(const pubsub_engine_s *eng, FIOBJ channel,
                      facil_match_fn match);
  /** Should return 0 on success and -1 on failure. */
  int (*publish)(const pubsub_engine_s *eng, FIOBJ channel, FIOBJ msg);
  /**
   * facil.io will call this callback whenever starting, or restarting, the
   * reactor.
   *
   * This will be called when facil.io starts (the master process).
   *
   * This will also be called when forking, after facil.io closes all
   * connections and claim to shut down (running all deferred event).
   */
  void (*on_startup)(const pubsub_engine_s *eng);
};

/** Attaches an engine, so it's callback can be called by facil.io. */
void facil_pubsub_attach(pubsub_engine_s *engine);

/** Detaches an engine, so it could be safely destroyed. */
void facil_pubsub_detach(pubsub_engine_s *engine);

/**
 * Engines can ask facil.io to call the `subscribe` callback for all active
 * channels.
 *
 * This allows engines that lost their connection to their Pub/Sub service to
 * resubscribe all the currently active channels with the new connection.
 *
 * CAUTION: This is an evented task... try not to free the engine's memory while
 * resubscriptions are under way...
 */
void facil_pubsub_reattach(pubsub_engine_s *eng);

#endif

/* *****************************************************************************
 * Data Structures - Clients / Subscriptions data
 **************************************************************************** */

typedef enum cluster_message_type_e {
  CLUSTER_MESSAGE_FORWARD,
  CLUSTER_MESSAGE_JSON,
  CLUSTER_MESSAGE_SHUTDOWN,
  CLUSTER_MESSAGE_ERROR,
  CLUSTER_MESSAGE_PING,
} cluster_message_type_e;

#define FIO_HASH_KEY_TYPE FIOBJ
#define FIO_HASH_KEY_INVALID FIOBJ_INVALID
#define FIO_HASH_KEY2UINT(key) (fiobj_obj2hash((key)))
#define FIO_HASH_COMPARE_KEYS(k1, k2) (fiobj_iseq((k1), (k2)))
#define FIO_HASH_KEY_ISINVALID(key) ((key) == FIOBJ_INVALID)
#define FIO_HASH_KEY_COPY(key) (fiobj_dup((key)))
#define FIO_HASH_KEY_DESTROY(key) fiobj_free((key))

#include "fio_ary.h"
#include "fio_hashmap.h"

typedef struct {
  fio_hash_s channels;
  spn_lock_i lock;
} collection_s;
typedef struct {
  fio_ary_s ary;
  spn_lock_i lock;
} collection_ary_s;

typedef struct {
  FIOBJ id;
  fio_ls_embd_s subscriptions;
  collection_s *parent;
  spn_lock_i lock;
} channel_s;

typedef struct {
  channel_s ch;
  facil_match_fn match;
} pattern_s;

struct subscription_s {
  fio_ls_embd_s node;
  channel_s *parent;
  void (*callback)(facil_msg_s *msg);
  void (*on_unsubscribe)(void *udata1, void *udata2);
  void *udata1;
  void *udata2;
  /** reference counter. */
  uintptr_t ref;
  /** prevents the callback from running concurrently for multiple messages. */
  spn_lock_i lock;
};

typedef struct {
  facil_msg_s msg;
  facil_msg_metadata_s *meta;
  uintptr_t ref;
} facil_msg_internal_s;

typedef struct {
  cluster_message_type_e type;
  /** A unique message type. Negative values are reserved, 0 == pub/sub. */
  int32_t filter;
  /** A channel name, allowing for pub/sub patterns. */
  FIOBJ channel;
  /** The actual message. */
  FIOBJ msg;
} facil_msg_str_s;

#define COLLECTION_INIT                                                        \
  { .channels = FIO_HASH_INIT, .lock = SPN_LOCK_INIT }

struct {
  collection_s filters;
  collection_s pubsub;
  collection_s patterns;
  collection_s engines;
  collection_ary_s meta;
} postoffice = {
    .filters = COLLECTION_INIT,
    .pubsub = COLLECTION_INIT,
    .patterns = COLLECTION_INIT,
};

/** The default engine (settable). */
pubsub_engine_s *FACIL_PUBSUB_DEFAULT = FACIL_PUBSUB_CLUSTER;

/* *****************************************************************************
Engine handling and Management
***************************************************************************** */

/* runs in lock(!) let'm all know */
static void pubsub_on_channel_create(channel_s *ch, facil_match_fn match) {
  spn_lock(&postoffice.engines.lock);
  FIO_HASH_FOR_LOOP(&postoffice.engines.channels, e_) {
    if (!e_ || !e_->obj)
      continue;
    pubsub_engine_s *e = e_->obj;
    e->subscribe(e, ch->id, match);
  }
  spn_unlock(&postoffice.engines.lock);
}

/* runs in lock(!) let'm all know */
static void pubsub_on_channel_destroy(channel_s *ch, facil_match_fn match) {
  spn_lock(&postoffice.engines.lock);
  FIO_HASH_FOR_LOOP(&postoffice.engines.channels, e_) {
    if (!e_ || !e_->obj)
      continue;
    pubsub_engine_s *e = e_->obj;
    e->unsubscribe(e, ch->id, match);
  }
  spn_unlock(&postoffice.engines.lock);
}

/* *****************************************************************************
 * Freeing subscriptions / channels
 **************************************************************************** */

/* to be used for reference counting (subtructing) */
static inline void subscription_free(subscription_s *s) {
  if (spn_sub(&s->ref, 1)) {
    return;
  }
  if (s->on_unsubscribe) {
    s->on_unsubscribe(s->udata1, s->udata2);
  }
  fio_free(s);
}
/* to be used for reference counting (increasing) */
static inline subscription_s *subscription_dup(subscription_s *s) {
  spn_add(&s->ref, 1);
  return s;
}
static void subscription_free_later(void *s, void *ignr) {
  subscription_free(s);
  (void)ignr;
}

/* free a channel (if it's empty) */
static inline void channel_destroy(channel_s *c) {
  spn_lock(&c->parent->lock);
  if (fio_ls_embd_any(&c->subscriptions)) {
    spn_unlock(&c->parent->lock);
    return;
  }
  fio_hash_insert(&c->parent->channels, c->id, NULL);
  if ((fio_hash_count(&c->parent->channels) << 1) <=
          fio_hash_capa(&c->parent->channels) &&
      fio_hash_capa(&c->parent->channels) > 512) {
    fio_hash_compact(&c->parent->channels);
  }
  spn_unlock(&c->parent->lock);
  pubsub_on_channel_destroy(
      c, (c->parent == &postoffice.patterns ? ((pattern_s *)c)->match : NULL));
  fio_free(c);
}

/* cancel a subscription */
static void subscription_destroy(void *s_, void *ignore) {
  if (!s_) { /* ignore NULL subscriptions. */
    return;
  }
  subscription_s *s = s_;
  if (spn_trylock(&s->parent->lock)) {
    defer(subscription_destroy, s_, ignore);
    return;
  }
  fio_ls_embd_remove(&s->node);
  if (fio_ls_embd_is_empty(&s->parent->subscriptions)) {
    spn_unlock(&s->parent->lock);
    channel_destroy(s->parent);
  } else {
    spn_unlock(&s->parent->lock);
  }
  subscription_free(s);
  (void)ignore;
}

/* *****************************************************************************
 * Creating subscriptions
 **************************************************************************** */

/** Creates a new subscription object, returning NULL on error. */
static inline subscription_s *subscription_create(subscribe_args_s args) {
  if (!args.callback || (!args.channel && !args.filter)) {
    return NULL;
  }
  collection_s *collection;
  if (args.filter) {
    /* either a filter OR a channel can be subscribed to. */
    args.channel = fiobj_num_new((uintptr_t)args.filter);
    collection = &postoffice.filters;
  } else {
    if (args.match) {
      collection = &postoffice.patterns;
    } else {
      collection = &postoffice.pubsub;
    }
    if (FIOBJ_TYPE_IS(args.channel, FIOBJ_T_STRING)) {
      /* Hash values are cached, so it can be computed outside the lock */
      fiobj_str_freeze(args.channel);
      fiobj_obj2hash(args.channel);
    }
  }
  /* allocate and initialize subscription object */
  subscription_s *s = fio_malloc(sizeof(*s));
  if (!s) {
    perror("FATAL ERROR: (pubsub) can't allocate memory for subscription");
    exit(errno);
  }
  *s = (subscription_s){
      .node = (fio_ls_embd_s)FIO_LS_INIT(s->node),
      .parent = NULL,
      .callback = args.callback,
      .udata1 = args.udata1,
      .udata2 = args.udata2,
      .ref = 1,
      .lock = SPN_LOCK_INIT,
  };
  /* seek existing channel or create one */
  spn_lock(&collection->lock);
  channel_s *ch = fio_hash_find(&collection->channels, args.channel);
  if (!ch) {
    if (args.match) {
      /* pattern subscriptions */
      ch = fio_malloc(sizeof(pattern_s));
      if (!ch) {
        perror("FATAL ERROR: (pubsub) can't allocate memory for pattern");
        exit(errno);
      }
      ((pattern_s *)ch)->match = args.match;
    } else {
      /* channel subscriptions */
      ch = fio_malloc(sizeof(*ch));
      if (!ch) {
        perror("FATAL ERROR: (pubsub) can't allocate memory for channel");
        exit(errno);
      }
    }
    *ch = (channel_s){
        .id = fiobj_dup(args.channel),
        .subscriptions = (fio_ls_embd_s)FIO_LS_INIT(ch->subscriptions),
        .parent = collection,
        .lock = SPN_LOCK_INIT,
    };
    subscription_s *old =
        fio_hash_insert(&collection->channels, args.channel, ch);
    if (old && old->on_unsubscribe) {
      defer(subscription_free_later, old, NULL);
    }
    if (!args.filter) {
      pubsub_on_channel_create(ch, args.match);
    }
  }
  /* add subscription to filter / channel / pattern */
  s->parent = ch;
  spn_lock(&ch->lock);
  fio_ls_embd_push(&ch->subscriptions, &s->node);
  spn_unlock(&ch->lock);
  spn_unlock(&collection->lock);
  if (args.filter) {
    fiobj_free(args.channel);
  }
  return s;
}

/* *****************************************************************************
 * Publishing to the subsriptions
 **************************************************************************** */

/** frees the internal message data */
static inline void internal_message_free(facil_msg_internal_s *msg) {
  if (spn_sub(&msg->ref, 1))
    return;
  facil_msg_metadata_s *meta = msg->meta;
  while (meta) {
    facil_msg_metadata_s *tmp = meta;
    meta = meta->next;
    if (tmp->on_finish) {
      tmp->on_finish(&msg->msg, tmp);
    }
  }
  fiobj_free(msg->msg.channel);
  fiobj_free(msg->msg.msg);
  fio_free(msg);
}

/* defers the callback (mark only) */
static inline void defer_subscription_callback(facil_msg_s *msg_) {
  facil_msg_internal_s *msg = (facil_msg_internal_s *)msg_;
  msg->ref = 1;
}

/** Finds the message's metadata by it's type ID. */
void *facil_message_metadata(facil_msg_s *msg, intptr_t type_id) {
  facil_msg_metadata_s *exists = ((facil_msg_internal_s *)msg)->meta;
  while (exists && exists->type_id != type_id) {
    exists = exists->next;
  }
  return exists->metadata;
}

/* performs the actual callback */
static void perform_subscription_callback(void *s_, void *msg_) {
  subscription_s *s = s_;
  if (spn_trylock(&s->lock)) {
    defer(perform_subscription_callback, s_, msg_);
    return;
  }
  facil_msg_internal_s *msg = (facil_msg_internal_s *)msg_;
  facil_msg_internal_s m = {
      .msg =
          {
              .channel = msg->msg.channel,
              .msg = msg->msg.msg,
              .udata1 = s->udata1,
              .udata2 = s->udata2,
          },
      .meta = msg->meta,
      .ref = 0,
  };
  s->callback((facil_msg_s *)&m);
  spn_unlock(&s->lock);
  if (m.ref) {
    defer(perform_subscription_callback, s_, msg_);
    return;
  }
  internal_message_free(msg);
  subscription_free(s);
}

/* publishes a message to a channel, managing the reference counts */
static void publish2channel(channel_s *ch, facil_msg_internal_s *msg) {
  if (!ch || !msg) {
    return;
  }
  spn_lock(&ch->lock);
  FIO_LS_EMBD_FOR(&ch->subscriptions, pos) {
    subscription_s *s = FIO_LS_EMBD_OBJ(subscription_s, node, pos);
    if (!s) {
      continue;
    }
    subscription_dup(s);
    spn_add(&msg->ref, 1);
    defer(perform_subscription_callback, s, msg);
  }
  spn_unlock(&ch->lock);
}

static void publish2process(int32_t filter, FIOBJ channel, FIOBJ msg,
                            cluster_message_type_e type) {
  facil_msg_internal_s *m = fio_malloc(sizeof(*m));
  if (!m) {
    perror("FATAL ERROR: (pubsub) can't allocate memory for message data");
    exit(errno);
  }
  *m = (facil_msg_internal_s){
      .msg =
          {
              .filter = filter,
              .channel = fiobj_dup(channel),
              .msg = fiobj_dup(msg),

          },
      .ref = 1,
  };
  if (type == CLUSTER_MESSAGE_JSON) {
    FIOBJ org_ch = m->msg.channel;
    FIOBJ org_msg = m->msg.msg;
    if (org_ch) {
      fio_cstr_s str = fiobj_obj2cstr(org_ch);
      fiobj_json2obj(&m->msg.channel, str.data, str.length);
    }
    if (org_msg) {
      fio_cstr_s str = fiobj_obj2cstr(org_msg);
      fiobj_json2obj(&m->msg.msg, str.data, str.length);
    }
    if (!m->msg.channel) {
      m->msg.channel = fiobj_dup(org_ch);
    }
    if (!m->msg.msg) {
      m->msg.msg = fiobj_dup(org_msg);
    }
    spn_lock(&postoffice.meta.lock);
    FIO_ARY_FOR(&postoffice.meta.ary, pos) {
      if (pos.obj == NULL)
        continue;
      facil_msg_metadata_s *ret =
          ((facil_msg_metadata_s * (*)(facil_msg_s * msg, FIOBJ raw_ch,
                                       FIOBJ raw_msg))(uintptr_t)pos.obj)(
              &m->msg, org_ch, org_msg);
      if (ret) {
        ret->next = m->meta;
        m->meta = ret;
      }
    }
    spn_unlock(&postoffice.meta.lock);
    fiobj_free(org_ch);
    fiobj_free(org_msg);
  } else {
  }
  if (filter) {
    FIOBJ key = fiobj_num_new((uintptr_t)filter);
    spn_lock(&postoffice.filters.lock);
    channel_s *ch = fio_hash_find(&postoffice.filters.channels, key);
    publish2channel(ch, m);
    spn_unlock(&postoffice.filters.lock);
    internal_message_free(m);
    return;
  }
  /* exact match */
  spn_lock(&postoffice.pubsub.lock);
  channel_s *ch = fio_hash_find(&postoffice.pubsub.channels, channel);
  publish2channel(ch, m);
  spn_unlock(&postoffice.pubsub.lock);
  /* test patterns */
  spn_lock(&postoffice.patterns.lock);
  FIO_HASH_FOR_LOOP(&postoffice.patterns.channels, p) {
    if (!p->obj) {
      continue;
    }
    pattern_s *pattern = p->obj;
    if (pattern->match(pattern->ch.id, channel)) {
      publish2channel(&pattern->ch, m);
    }
  }
  spn_unlock(&postoffice.patterns.lock);
  internal_message_free(m);
}

/** Prepares the message to be published. */
static inline facil_msg_str_s prepare_message(int32_t filter, FIOBJ ch,
                                              FIOBJ msg) {
  facil_msg_str_s m = {
      .channel = ch,
      .msg = msg,
      .type = CLUSTER_MESSAGE_FORWARD,
      .filter = filter,
  };
  if ((!ch || FIOBJ_TYPE_IS(ch, FIOBJ_T_STRING)) &&
      (!msg || FIOBJ_TYPE_IS(msg, FIOBJ_T_STRING))) {
    /* nothing to do */
  } else {
    m.type = CLUSTER_MESSAGE_JSON;
    if (ch) {
      m.channel = fiobj_obj2json(ch, 0);
    }
    if (msg) {
      m.msg = fiobj_obj2json(msg, 0);
    }
  }
  fiobj_dup(m.channel);
  fiobj_dup(m.msg);
  return m;
}

static void facil_send2cluster(int32_t filter, FIOBJ ch, FIOBJ msg,
                               cluster_message_type_e type);

/** Publishes a message to all processes (including this one) */
static inline void publish_msg2all(int32_t filter, FIOBJ ch, FIOBJ msg) {
  facil_msg_str_s m = prepare_message(filter, ch, msg);
  facil_send2cluster(m.filter, m.channel, m.msg, m.type);
  publish2process(m.filter, m.channel, m.msg, m.type);
  fiobj_free(m.channel);
  fiobj_free(m.msg);
}

/** Publishes a message within the current process (only this one) */
static inline void publish_msg2local(int32_t filter, FIOBJ ch, FIOBJ msg) {
  facil_msg_str_s m = prepare_message(filter, ch, msg);
  publish2process(m.filter, m.channel, m.msg, m.type);
  fiobj_free(m.channel);
  fiobj_free(m.msg);
}

/** Publishes a message to other processes (excluding this one) */
static inline void publish_msg2cluster(int32_t filter, FIOBJ ch, FIOBJ msg) {
  facil_msg_str_s m = prepare_message(filter, ch, msg);
  facil_send2cluster(m.filter, m.channel, m.msg, m.type);
  fiobj_free(m.channel);
  fiobj_free(m.msg);
}

/* *****************************************************************************
 * Data Structures - Core Structures
 **************************************************************************** */

#define CLUSTER_READ_BUFFER 16384

typedef struct cluster_pr_s {
  protocol_s protocol;
  FIOBJ channel;
  FIOBJ msg;
  void (*handler)(struct cluster_pr_s *pr);
  void (*sender)(FIOBJ data);
  intptr_t uuid;
  uint32_t exp_channel;
  uint32_t exp_msg;
  uint32_t type;
  int32_t filter;
  uint32_t length;
  uint8_t buffer[CLUSTER_READ_BUFFER];
} cluster_pr_s;

struct cluster_data_s {
  intptr_t listener;
  intptr_t client;
  fio_ls_s clients;
  fio_hash_s subscribers;
  spn_lock_i lock;
  char name[128];
} cluster_data = {.clients = FIO_LS_INIT(cluster_data.clients),
                  .subscribers = FIO_HASH_INIT,
                  .lock = SPN_LOCK_INIT};

static void cluster_data_cleanup(int delete_file) {
  if (delete_file && cluster_data.name[0]) {
#if DEBUG
    fprintf(stderr, "* INFO: (%d) CLUSTER UNLINKING\n", getpid());
#endif
    unlink(cluster_data.name);
  }
  while (fio_ls_any(&cluster_data.clients)) {
    intptr_t uuid = (intptr_t)fio_ls_pop(&cluster_data.clients);
    if (uuid > 0) {
      sock_close(uuid);
    }
  }
  cluster_data = (struct cluster_data_s){
      .lock = SPN_LOCK_INIT,
      .clients = (fio_ls_s)FIO_LS_INIT(cluster_data.clients),
  };
}

static int cluster_init(void) {
  cluster_data_cleanup(0);
  /* create a unique socket name */
  char *tmp_folder = getenv("TMPDIR");
  uint32_t tmp_folder_len = 0;
  if (!tmp_folder || ((tmp_folder_len = (uint32_t)strlen(tmp_folder)) > 100)) {
#ifdef P_tmpdir
    tmp_folder = P_tmpdir;
    if (tmp_folder)
      tmp_folder_len = (uint32_t)strlen(tmp_folder);
#else
    tmp_folder = "/tmp/";
    tmp_folder_len = 5;
#endif
  }
  if (tmp_folder_len >= 100) {
    tmp_folder_len = 0;
  }
  if (tmp_folder_len) {
    memcpy(cluster_data.name, tmp_folder, tmp_folder_len);
    if (cluster_data.name[tmp_folder_len - 1] != '/')
      cluster_data.name[tmp_folder_len++] = '/';
  }
  memcpy(cluster_data.name + tmp_folder_len, "facil-io-sock-", 14);
  tmp_folder_len += 14;
  tmp_folder_len += fio_ltoa(cluster_data.name + tmp_folder_len, getpid(), 8);
  cluster_data.name[tmp_folder_len] = 0;

  /* remove if existing */
  unlink(cluster_data.name);
  return 0;
}

/* *****************************************************************************
 * Cluster Protocol callbacks
 **************************************************************************** */

#ifdef __BIG_ENDIAN__
inline static uint32_t cluster_str2uint32(uint8_t *str) {
  return ((str[0] & 0xFF) | ((((uint32_t)str[1]) << 8) & 0xFF00) |
          ((((uint32_t)str[2]) << 16) & 0xFF0000) |
          ((((uint32_t)str[3]) << 24) & 0xFF000000));
}
inline static void cluster_uint2str(uint8_t *dest, uint32_t i) {
  dest[0] = i & 0xFF;
  dest[1] = (i >> 8) & 0xFF;
  dest[2] = (i >> 16) & 0xFF;
  dest[3] = (i >> 24) & 0xFF;
}
#else
inline static uint32_t cluster_str2uint32(uint8_t *str) {
  return (((((uint32_t)str[0]) << 24) & 0xFF000000) |
          ((((uint32_t)str[1]) << 16) & 0xFF0000) |
          ((((uint32_t)str[2]) << 8) & 0xFF00) | (str[3] & 0xFF));
}
inline static void cluster_uint2str(uint8_t *dest, uint32_t i) {
  dest[0] = (i >> 24) & 0xFF;
  dest[1] = (i >> 16) & 0xFF;
  dest[2] = (i >> 8) & 0xFF;
  dest[3] = i & 0xFF;
}
#endif

typedef struct cluster_msg_s {
  facil_msg_s message;
  size_t ref;
} cluster_msg_s;

static inline FIOBJ cluster_wrap_message(uint32_t ch_len, uint32_t msg_len,
                                         uint32_t type, int32_t filter,
                                         uint8_t *ch_data, uint8_t *msg_data) {
  FIOBJ buf = fiobj_str_buf(ch_len + msg_len + 16);
  fio_cstr_s f = fiobj_obj2cstr(buf);
  cluster_uint2str(f.bytes, ch_len);
  cluster_uint2str(f.bytes + 4, msg_len);
  cluster_uint2str(f.bytes + 8, type);
  cluster_uint2str(f.bytes + 12, (uint32_t)filter);
  if (ch_len && ch_data) {
    memcpy(f.bytes + 16, ch_data, ch_len);
  }
  if (msg_len && msg_data) {
    memcpy(f.bytes + 16 + ch_len, msg_data, msg_len);
  }
  fiobj_str_resize(buf, ch_len + msg_len + 16);
  return buf;
}

static uint8_t cluster_on_shutdown(intptr_t uuid, protocol_s *pr_) {
  cluster_pr_s *p = (cluster_pr_s *)pr_;
  p->sender(
      cluster_wrap_message(0, 0, CLUSTER_MESSAGE_SHUTDOWN, 0, NULL, NULL));
  return 255;
  (void)pr_;
  (void)uuid;
}

static void cluster_on_data(intptr_t uuid, protocol_s *pr_) {
  cluster_pr_s *c = (cluster_pr_s *)pr_;
  ssize_t i =
      sock_read(uuid, c->buffer + c->length, CLUSTER_READ_BUFFER - c->length);
  if (i <= 0)
    return;
  c->length += i;
  i = 0;
  do {
    if (!c->exp_channel && !c->exp_msg) {
      if (c->length - i < 16)
        break;
      c->exp_channel = cluster_str2uint32(c->buffer + i);
      c->exp_msg = cluster_str2uint32(c->buffer + i + 4);
      c->type = cluster_str2uint32(c->buffer + i + 8);
      c->filter = (int32_t)cluster_str2uint32(c->buffer + i + 12);
      if (c->exp_channel) {
        if (c->exp_channel >= (1024 * 1024 * 16)) {
          fprintf(stderr,
                  "FATAL ERROR: (%d) cluster message name too long (16Mb "
                  "limit): %u\n",
                  getpid(), (unsigned int)c->exp_channel);
          exit(1);
          return;
        }
        c->channel = fiobj_str_buf(c->exp_channel);
      }
      if (c->exp_msg) {
        if (c->exp_msg >= (1024 * 1024 * 64)) {
          fprintf(stderr,
                  "FATAL ERROR: (%d) cluster message data too long (64Mb "
                  "limit): %u\n",
                  getpid(), (unsigned int)c->exp_msg);
          exit(1);
          return;
        }
        c->msg = fiobj_str_buf(c->exp_msg);
      }
      i += 16;
    }
    if (c->exp_channel) {
      if (c->exp_channel + i > c->length) {
        fiobj_str_write(c->channel, (char *)c->buffer + i,
                        (size_t)(c->length - i));
        c->exp_channel -= (c->length - i);
        i = c->length;
        break;
      } else {
        fiobj_str_write(c->channel, (char *)c->buffer + i, c->exp_channel);
        i += c->exp_channel;
        c->exp_channel = 0;
      }
    }
    if (c->exp_msg) {
      if (c->exp_msg + i > c->length) {
        fiobj_str_write(c->msg, (char *)c->buffer + i, (size_t)(c->length - i));
        c->exp_msg -= (c->length - i);
        i = c->length;
        break;
      } else {
        fiobj_str_write(c->msg, (char *)c->buffer + i, c->exp_msg);
        i += c->exp_msg;
        c->exp_msg = 0;
      }
    }
    c->handler(c);
    fiobj_free(c->msg);
    fiobj_free(c->channel);
    c->msg = FIOBJ_INVALID;
    c->channel = FIOBJ_INVALID;
  } while (c->length > i);
  c->length -= i;
  if (c->length) {
    memmove(c->buffer, c->buffer + i, c->length);
  }
  (void)pr_;
}

static void cluster_ping(intptr_t uuid, protocol_s *pr_) {
  FIOBJ ping = cluster_wrap_message(0, 0, CLUSTER_MESSAGE_PING, 0, NULL, NULL);
  fiobj_send_free(uuid, ping);
  (void)pr_;
}

static void cluster_data_cleanup(int delete_file);

static void cluster_on_close(intptr_t uuid, protocol_s *pr_) {
  cluster_pr_s *c = (cluster_pr_s *)pr_;
  if (facil_parent_pid() == getpid()) {
    /* a child was lost, respawning is handled elsewhere. */
    spn_lock(&cluster_data.lock);
    FIO_LS_FOR(&cluster_data.clients, pos) {
      if (pos->obj == (void *)uuid) {
        fio_ls_remove(pos);
        break;
      }
    }
    spn_unlock(&cluster_data.lock);
  } else if (cluster_data.client == uuid) {
    /* no shutdown message received - parent crashed. */
    if (c->type != CLUSTER_MESSAGE_SHUTDOWN && facil_is_running()) {
      if (FACIL_PRINT_STATE) {
        fprintf(stderr, "* FATAL ERROR: (%d) Parent Process crash detected!\n",
                getpid());
      }
      facil_core_callback_force(FIO_CALL_ON_PARENT_CRUSH);
      cluster_data_cleanup(1);
      kill(getpid(), SIGINT);
    }
  }
  fiobj_free(c->msg);
  fiobj_free(c->channel);
  fio_free(c);
  (void)uuid;
}

static inline protocol_s *
cluster_alloc(intptr_t uuid, void (*handler)(struct cluster_pr_s *pr),
              void (*sender)(FIOBJ data)) {
  cluster_pr_s *p = fio_mmap(sizeof(*p));
  if (!p) {
    perror("FATAL ERROR: Cluster protocol allocation failed");
    exit(errno);
  }
  p->protocol = (protocol_s){
      .service = "_facil.io_cluster_",
      .ping = cluster_ping,
      .on_close = cluster_on_close,
      .on_shutdown = cluster_on_shutdown,
      .on_data = cluster_on_data,
  };
  p->uuid = uuid;
  p->handler = handler;
  p->sender = sender;
  return &p->protocol;
}

/* *****************************************************************************
 * Master (server) IPC Connections
 **************************************************************************** */

static void cluster_server_sender(FIOBJ data) {
  spn_lock(&cluster_data.lock);
  FIO_LS_FOR(&cluster_data.clients, pos) {
    if ((intptr_t)pos->obj > 0) {
      fiobj_send_free((intptr_t)pos->obj, fiobj_dup(data));
    }
  }
  spn_unlock(&cluster_data.lock);
  fiobj_free(data);
}

static void cluster_server_handler(struct cluster_pr_s *pr) {
  /* what to do? */
  switch ((cluster_message_type_e)pr->type) {
  case CLUSTER_MESSAGE_FORWARD: /* fallthrough */
  case CLUSTER_MESSAGE_JSON: {
    fio_cstr_s cs = fiobj_obj2cstr(pr->channel);
    fio_cstr_s ms = fiobj_obj2cstr(pr->msg);
    cluster_server_sender(cluster_wrap_message(cs.len, ms.len, pr->type,
                                               pr->filter, cs.bytes, ms.bytes));
    publish2process(pr->filter, pr->channel, pr->msg,
                    (cluster_message_type_e)pr->type);
    break;
  }
  case CLUSTER_MESSAGE_SHUTDOWN: /* fallthrough */
  case CLUSTER_MESSAGE_ERROR:    /* fallthrough */
  case CLUSTER_MESSAGE_PING:     /* fallthrough */
  default:
    break;
  }
}

/** Called when a ne client is available */
static void cluster_listen_accept(intptr_t uuid, protocol_s *protocol) {
  (void)protocol;
  /* prevent `accept` backlog in parent */
  intptr_t client;
  while ((client = sock_accept(uuid)) != -1) {
    if (facil_attach(client, cluster_alloc(client, cluster_server_handler,
                                           cluster_server_sender))) {
      perror("FATAL ERROR: (facil.io) failed to attach cluster client");
      exit(errno);
    }
    spn_lock(&cluster_data.lock);
    fio_ls_push(&cluster_data.clients, (void *)client);
    spn_unlock(&cluster_data.lock);
  }
}
/** Called when the connection was closed, but will not run concurrently */
static void cluster_listen_on_close(intptr_t uuid, protocol_s *protocol) {
  free(protocol);
  cluster_data.listener = -1;
  if (facil_parent_pid() == getpid()) {
#if DEBUG
    fprintf(stderr, "* INFO: (%d) stopped listening for cluster connections\n",
            getpid());
#endif
    kill(0, SIGINT);
  }
  (void)uuid;
}
/** called when a connection's timeout was reached */
static void cluster_listen_ping(intptr_t uuid, protocol_s *protocol) {
  sock_touch(uuid);
  (void)protocol;
}

static uint8_t cluster_listen_on_shutdown(intptr_t uuid, protocol_s *pr_) {
  return 255;
  (void)pr_;
  (void)uuid;
}

static void facil_listen2cluster(void *ignore) {
  /* this is called for each `fork`, but we only need this to run once. */
  spn_lock(&cluster_data.lock);
  cluster_init();
  cluster_data.listener = sock_listen(cluster_data.name, NULL);
  spn_unlock(&cluster_data.lock);
  if (cluster_data.listener < 0) {
    perror("FATAL ERROR: (facil.io cluster) failed to open cluster socket.\n"
           "             check file permissions");
    exit(errno);
  }
  protocol_s *p = malloc(sizeof(*p));
  *p = (protocol_s){
      .service = "_facil.io_listen4cluster_",
      .on_data = cluster_listen_accept,
      .on_shutdown = cluster_listen_on_shutdown,
      .ping = cluster_listen_ping,
      .on_close = cluster_listen_on_close,
  };
  if (!p) {
    perror("FATAL ERROR: (facil.io) couldn't allocate cluster server");
    exit(errno);
  }
  if (facil_attach(cluster_data.listener, p)) {
    perror(
        "FATAL ERROR: (facil.io) couldn't attach cluster server to facil.io");
    exit(errno);
  }
#if DEBUG
  fprintf(stderr, "* INFO: (%d) Listening to cluster: %s\n", getpid(),
          cluster_data.name);
#endif
  (void)ignore;
}

static void facil_cluster_cleanup(void *ignore) {
  /* cleanup the cluster data */
  cluster_data_cleanup(facil_parent_pid() == getpid());
  (void)ignore;
}

/* *****************************************************************************
 * Worker (client) IPC connections
 **************************************************************************** */

static void cluster_client_handler(struct cluster_pr_s *pr) {
  /* what to do? */
  switch ((cluster_message_type_e)pr->type) {
  case CLUSTER_MESSAGE_FORWARD: /* fallthrough */
  case CLUSTER_MESSAGE_JSON:
    publish2process(pr->filter, pr->channel, pr->msg,
                    (cluster_message_type_e)pr->type);
    break;
  case CLUSTER_MESSAGE_SHUTDOWN:
    kill(getpid(), SIGINT);
  case CLUSTER_MESSAGE_ERROR: /* fallthrough */
  case CLUSTER_MESSAGE_PING:  /* fallthrough */
  default:
    break;
  }
}
static void cluster_client_sender(FIOBJ data) {
  fiobj_send_free(cluster_data.client, data);
}

/** The address of the server we are connecting to. */
// char *address;
/** The port on the server we are connecting to. */
// char *port;
/**
 * The `on_connect` callback should return a pointer to a protocol object
 * that will handle any connection related events.
 *
 * Should either call `facil_attach` or close the connection.
 */
void facil_cluster_on_connect(intptr_t uuid, void *udata) {
  cluster_data.client = uuid;
  if (facil_attach(uuid, cluster_alloc(uuid, cluster_client_handler,
                                       cluster_client_sender))) {
    perror("FATAL ERROR: (facil.io) failed to attach cluster connection");
    kill(facil_parent_pid(), SIGINT);
    exit(errno);
  }
  (void)udata;
}
/**
 * The `on_fail` is called when a socket fails to connect. The old sock UUID
 * is passed along.
 */
void facil_cluster_on_fail(intptr_t uuid, void *udata) {
  perror("FATAL ERROR: (facil.io) unknown cluster connection error");
  kill(facil_parent_pid(), SIGINT);
  exit(errno ? errno : 1);
  (void)udata;
  (void)uuid;
}
/** Opaque user data. */
// void *udata;
/** A non-system timeout after which connection is assumed to have failed. */
// uint8_t timeout;

static void facil_connect2cluster(void *ignore) {
  if (facil_parent_pid() != getpid()) {
    /* this is called for each child. */
    cluster_data.client =
        facil_connect(.address = cluster_data.name, .port = NULL,
                      .on_connect = facil_cluster_on_connect,
                      .on_fail = facil_cluster_on_fail);
  }
  (void)ignore;
}

static void facil_send2cluster(int32_t filter, FIOBJ ch, FIOBJ msg,
                               cluster_message_type_e type) {
  if (!facil_is_running()) {
    fprintf(stderr, "ERROR: cluster inactive, can't send message.\n");
    return;
  }
  fio_cstr_s cs = fiobj_obj2cstr(ch);
  fio_cstr_s ms = fiobj_obj2cstr(msg);
  if (cluster_data.client > 0) {
    cluster_client_sender(
        cluster_wrap_message(cs.len, ms.len, type, filter, cs.bytes, ms.bytes));
  } else {
    cluster_server_sender(
        cluster_wrap_message(cs.len, ms.len, type, filter, cs.bytes, ms.bytes));
  }
}

/* *****************************************************************************
 * Initialization
 **************************************************************************** */

static void facil_connect_after_fork(void *ignore) {
  if (facil_parent_pid() == getpid()) {
    /* prevent `accept` backlog in parent */
    cluster_listen_accept(cluster_data.listener, NULL);
  } else {
    /* this is called for each child. */
  }
  (void)ignore;
}

static void facil_cluster_in_child(void *ignore) {
  postoffice.patterns.lock = SPN_LOCK_INIT;
  postoffice.pubsub.lock = SPN_LOCK_INIT;
  postoffice.filters.lock = SPN_LOCK_INIT;
  postoffice.engines.lock = SPN_LOCK_INIT;
  postoffice.meta.lock = SPN_LOCK_INIT;
  FIO_HASH_FOR_LOOP(&postoffice.patterns.channels, pos) {
    ((channel_s *)pos->obj)->lock = SPN_LOCK_INIT;
  }
  FIO_HASH_FOR_LOOP(&postoffice.pubsub.channels, pos) {
    ((channel_s *)pos->obj)->lock = SPN_LOCK_INIT;
  }
  FIO_HASH_FOR_LOOP(&postoffice.filters.channels, pos) {
    ((channel_s *)pos->obj)->lock = SPN_LOCK_INIT;
  }
  (void)ignore;
}

static void facil_cluster_at_exit(void *ignore) {
  /* unlock all */
  facil_cluster_in_child(NULL);
  /* clear subscriptions of all types */
  while (fio_hash_count(&postoffice.patterns.channels)) {
    channel_s *ch = fio_hash_last(&postoffice.patterns.channels, NULL);
    while (fio_ls_embd_any(&ch->subscriptions)) {
      subscription_s *sub = sub =
          FIO_LS_EMBD_OBJ(subscription_s, node, ch->subscriptions.next);
      facil_unsubscribe(sub);
    }
  }
  while (fio_hash_count(&postoffice.pubsub.channels)) {
    channel_s *ch = fio_hash_last(&postoffice.pubsub.channels, NULL);
    while (fio_ls_embd_any(&ch->subscriptions)) {
      subscription_s *sub = sub =
          FIO_LS_EMBD_OBJ(subscription_s, node, ch->subscriptions.next);
      facil_unsubscribe(sub);
    }
  }
  while (fio_hash_count(&postoffice.filters.channels)) {
    channel_s *ch = fio_hash_last(&postoffice.filters.channels, NULL);
    while (fio_ls_embd_any(&ch->subscriptions)) {
      subscription_s *sub = sub =
          FIO_LS_EMBD_OBJ(subscription_s, node, ch->subscriptions.next);
      facil_unsubscribe(sub);
    }
  }

  FIO_HASH_FOR_FREE(&postoffice.patterns.channels, pos) { (void)pos; }
  FIO_HASH_FOR_FREE(&postoffice.pubsub.channels, pos) { (void)pos; }
  FIO_HASH_FOR_FREE(&postoffice.filters.channels, pos) { (void)pos; }

  /* clear engines */
  FACIL_PUBSUB_DEFAULT = FACIL_PUBSUB_CLUSTER;
  while (fio_hash_count(&postoffice.engines.channels)) {
    facil_pubsub_detach(fio_hash_last(&postoffice.engines.channels, NULL));
  }
  FIO_HASH_FOR_FREE(&postoffice.engines.channels, pos) { (void)pos; }

  /* clear meta hooks */
  fio_ary_free(&postoffice.meta.ary);
  /* perform newly created tasks */
  defer_perform();
  (void)ignore;
}

void __attribute__((constructor)) facil_cluster_initialize(void) {
  facil_core_callback_add(FIO_CALL_PRE_START, facil_listen2cluster, NULL);
  facil_core_callback_add(FIO_CALL_AFTER_FORK, facil_connect_after_fork, NULL);
  facil_core_callback_add(FIO_CALL_IN_CHILD, facil_cluster_in_child, NULL);
  facil_core_callback_add(FIO_CALL_ON_START, facil_connect2cluster, NULL);
  facil_core_callback_add(FIO_CALL_ON_FINISH, facil_cluster_cleanup, NULL);
  facil_core_callback_add(FIO_CALL_AT_EXIT, facil_cluster_at_exit, NULL);
}

/* *****************************************************************************
 * External API (old)
 **************************************************************************** */

static void facil_old_handler_callback(facil_msg_s *m) {
  void (*on_message)(int32_t, FIOBJ, FIOBJ) =
      (void (*)(int32_t, FIOBJ, FIOBJ))(uintptr_t)m->udata1;
  on_message(m->filter, m->channel, m->msg);
}

void facil_cluster_set_handler(int32_t filter,
                               void (*on_message)(int32_t id, FIOBJ ch,
                                                  FIOBJ msg)) {
  subscribe_args_s args = {
      .filter = filter,
      .callback = facil_old_handler_callback,
      .udata1 = (void *)(uintptr_t)on_message,
  };
  subscription_s *s = facil_subscribe(args);
  if (!s) {
    fprintf(stderr, "ERROR: subscription failed!\n");
  }
}

int facil_cluster_send(int32_t filter, FIOBJ ch, FIOBJ msg) {
  facil_publish_args_s args = {
      .channel = ch,
      .engine = FACIL_PUBSUB_SIBLINGS,
      .message = msg,
      .filter = filter,
  };
  facil_publish(args);
  return 0;
}

/* *****************************************************************************
 * pub/sub engines
 **************************************************************************** */

/* *****************************************************************************
 * External API
 **************************************************************************** */

/** Signals children (or self) to shutdown) - NOT signal safe. */
void facil_cluster_signal_children(void) {
  if (facil_parent_pid() != getpid()) {
    kill(getpid(), SIGINT);
    return;
  }
  cluster_server_sender(
      cluster_wrap_message(0, 0, CLUSTER_MESSAGE_SHUTDOWN, 0, NULL, NULL));
}

/**
 * Subscribes to either a filter OR a channel (never both).
 *
 * Returns a subscription pointer on success or NULL on failure.
 *
 * See `subscribe_args_s` for details.
 */
subscription_s *facil_subscribe(subscribe_args_s args) {
  return subscription_create(args);
}

/**
 * Subscribes to a channel (enforces filter == 0).
 *
 * Returns a subscription pointer on success or NULL on failure.
 *
 * See `subscribe_args_s` for details.
 */
subscription_s *facil_subscribe_pubsub(subscribe_args_s args) {
  args.filter = 0;
  return subscription_create(args);
}

/**
 * This helper returns a temporary handle to an existing subscription's channel
 * or filter.
 *
 * To keep the handle beyond the lifetime of the subscription, use `fiobj_dup`.
 */
FIOBJ facil_subscription_channel(subscription_s *subscription) {
  return subscription->parent->id;
}

/**
 * Cancels an existing subscriptions (actual effects might be delayed).
 */
void facil_unsubscribe(subscription_s *subscription) {
  subscription_destroy(subscription, NULL);
}

/**
 * Publishes a message to the relevant subscribers (if any).
 *
 * See `facil_publish_args_s` for details.
 *
 * By default the message is sent using the FACIL_PUBSUB_CLUSTER engine (all
 * processes, including the calling process).
 *
 * To limit the message only to other processes (exclude the calling process),
 * use the FACIL_PUBSUB_SIBLINGS engine.
 *
 * To limit the message only to the calling process, use the
 * FACIL_PUBSUB_PROCESS engine.
 *
 * To publish messages to the pub/sub layer, the `.filter` argument MUST be
 * equal to 0 or missing.
 */
void facil_publish(facil_publish_args_s args) {
  if (!args.engine) {
    args.engine = FACIL_PUBSUB_DEFAULT;
  }
  switch ((uintptr_t)args.engine) {
  case 0UL: /* fallthrough (missing default) */
  case 1UL: // ((uintptr_t)FACIL_PUBSUB_CLUSTER):
    publish_msg2all(args.filter, args.channel, args.message);
    break;
  case 2UL: // ((uintptr_t)FACIL_PUBSUB_PROCESS):
    publish_msg2local(args.filter, args.channel, args.message);
    break;
  case 3UL: // ((uintptr_t)FACIL_PUBSUB_SIBLINGS):
    publish_msg2cluster(args.filter, args.channel, args.message);
    break;
  default:
    if (args.filter != 0) {
      fprintf(stderr, "ERROR: (pub/sub) pub/sub engines can only be used for "
                      "pub/sub messages (no filter).\n");
      return;
    }
    args.engine->publish(args.engine, args.channel, args.message);
    return;
  }
}

/**
 * Defers the current callback, so it will be called again for the message.
 */
void facil_message_defer(facil_msg_s *msg) { defer_subscription_callback(msg); }

/* *****************************************************************************
 * MetaData (extension) API
 **************************************************************************** */

/**
 * It's possible to attach metadata to facil.io messages before they are
 * published.
 *
 * This allows, for example, messages to be encoded as network packets for
 * outgoing protocols (i.e., encoding for WebSocket transmissions), improving
 * performance in large network based broadcasting.
 *
 * The callback should return a pointer to a valid metadata object.
 *
 * Since the cluster messaging system serializes objects to JSON (unless both
 * the channel and the data are String objects), the pre-serialized data is
 * available to the callback as the `raw_ch` and `raw_msg` arguments.
 *
 * To remove a callback, set the `remove` flag to true (`1`).
 */
void facil_message_metadata_set(
    facil_msg_metadata_s *(*callback)(facil_msg_s *msg, FIOBJ raw_ch,
                                      FIOBJ raw_msg),
    int remove) {
  spn_lock(&postoffice.meta.lock);
  fio_ary_remove2(&postoffice.meta.ary, (void *)(uintptr_t)callback);
  if (!remove) {
    fio_ary_push(&postoffice.meta.ary, (void *)(uintptr_t)callback);
  }
  spn_unlock(&postoffice.meta.lock);
}

/* *****************************************************************************
 * Pub/Sub Engine (extension) API
 **************************************************************************** */

/** Attaches an engine, so it's callback can be called by facil.io. */
void facil_pubsub_attach(pubsub_engine_s *engine) {
  if (!engine) {
    return;
  }
  FIOBJ key = fiobj_num_new((intptr_t)engine);
  spn_lock(&postoffice.engines.lock);
  fio_hash_insert(&postoffice.engines.channels, key, engine);
  spn_unlock(&postoffice.engines.lock);
  if (engine->subscribe) {
    spn_lock(&postoffice.pubsub.lock);
    FIO_HASH_FOR_LOOP(&postoffice.pubsub.channels, i) {
      channel_s *ch = i->obj;
      engine->subscribe(engine, ch->id, NULL);
    }
    spn_unlock(&postoffice.pubsub.lock);
    spn_lock(&postoffice.patterns.lock);
    FIO_HASH_FOR_LOOP(&postoffice.patterns.channels, i) {
      channel_s *ch = i->obj;
      engine->subscribe(engine, ch->id, ((pattern_s *)ch)->match);
    }
    spn_unlock(&postoffice.patterns.lock);
  }
  fiobj_free(key);
}

/** Detaches an engine, so it could be safely destroyed. */
void facil_pubsub_detach(pubsub_engine_s *engine) {
  if (FACIL_PUBSUB_DEFAULT == engine) {
    FACIL_PUBSUB_DEFAULT = FACIL_PUBSUB_CLUSTER;
  }
  if (postoffice.engines.channels.count == 0) {
    return;
  }
  FIOBJ key = fiobj_num_new((intptr_t)engine);
  spn_lock(&postoffice.engines.lock);
  void *old = fio_hash_insert(&postoffice.engines.channels, key, NULL);
  fio_hash_compact(&postoffice.engines.channels);
  spn_unlock(&postoffice.engines.lock);
  fiobj_free(key);
#if DEBUG
  if (!old) {
    fprintf(stderr, "WARNING: (pubsub) detachment error, not registered?\n");
  }
#else
  (void)old;
#endif
}

/**
 * Engines can ask facil.io to call the `subscribe` callback for all active
 * channels.
 *
 * This allows engines that lost their connection to their Pub/Sub service to
 * resubscribe all the currently active channels with the new connection.
 *
 * CAUTION: This is an evented task... try not to free the engine's memory while
 * resubscriptions are under way...
 */
void facil_pubsub_reattach(pubsub_engine_s *engine) {
  if (engine->subscribe) {
    spn_lock(&postoffice.pubsub.lock);
    FIO_HASH_FOR_LOOP(&postoffice.pubsub.channels, i) {
      channel_s *ch = i->obj;
      engine->subscribe(engine, ch->id, NULL);
    }
    spn_unlock(&postoffice.pubsub.lock);
    spn_lock(&postoffice.patterns.lock);
    FIO_HASH_FOR_LOOP(&postoffice.patterns.channels, i) {
      channel_s *ch = i->obj;
      engine->subscribe(engine, ch->id, ((pattern_s *)ch)->match);
    }
    spn_unlock(&postoffice.patterns.lock);
  }
}
