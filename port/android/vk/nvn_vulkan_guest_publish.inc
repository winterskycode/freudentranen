// At frame boundaries wait only for real guest publication and acquire-token consumption; later pure GPU work may remain in flight.
typedef struct guest_publish_result {
    uint64_t queued_ticket,required_ticket,gap,wait_ns;
} guest_publish_result;
static guest_publish_result guest_publish_last;
static uint64_t guest_publish_calls,guest_publish_gaps,guest_publish_max_gap,guest_publish_wait_ns;
