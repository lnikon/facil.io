#include "facil.h"
#include "pubsub.h"

static void reporter_subscribe(const pubsub_engine_s *eng, fiobj_s *channel,
                               uint8_t use_pattern) {
  fprintf(stderr, "(%u) + subscribing to %s\n", getpid(),
          fiobj_obj2cstr(channel).data);
  (void)eng;
  (void)use_pattern;
}
static void reporter_unsubscribe(const pubsub_engine_s *eng, fiobj_s *channel,
                                 uint8_t use_pattern) {
  fprintf(stderr, "(%u) - unsubscribing to %s\n", getpid(),
          fiobj_obj2cstr(channel).data);
  (void)eng;
  (void)use_pattern;
}
static int reporter_publish(const pubsub_engine_s *eng, fiobj_s *channel,
                            fiobj_s *msg) {
  fprintf(stderr, "(%u) publishing to %s\n", getpid(),
          fiobj_obj2cstr(channel).data);
  (void)eng;
  (void)msg;
  return 0;
}

pubsub_engine_s REPORTER = {
    .subscribe = reporter_subscribe,
    .unsubscribe = reporter_unsubscribe,
    .publish = reporter_publish,
};

void my_on_message(pubsub_message_s *msg) {
  fio_cstr_s s = fiobj_obj2cstr(msg->channel);
  fprintf(stderr, "Got message from %s, with subscription %p\n", s.data,
          (void *)fiobj_obj2num(msg->message));
  pubsub_sub_pt sub =
      pubsub_find_sub(.channel = msg->channel, .on_message = my_on_message,
                      .udata1 = msg->udata1, .udata2 = msg->udata2);
  pubsub_unsubscribe(sub);
}

void perfrom_sub(void *a, void *b) {
  if (defer_fork_pid()) {
    (void)a;
  } else {
    fiobj_s *ch = fiobj_sym_new("my channel", 10);
    fiobj_s *msg = fiobj_num_new(
        (intptr_t)pubsub_subscribe(.channel = ch, .on_message = my_on_message,
                                   .udata1 = a, .udata2 = b));
    pubsub_publish(.channel = ch, .message = msg);
    fiobj_free(msg);
    fiobj_free(ch);
  }
}

int main(void) {
  pubsub_engine_register(&REPORTER);
  defer(perfrom_sub, NULL, NULL);
  facil_run(.threads = 4, .processes = 4);
  pubsub_engine_deregister(&REPORTER);
  return 0;
}
