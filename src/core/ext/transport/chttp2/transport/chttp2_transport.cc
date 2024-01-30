//
// Copyright 2018 gRPC authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "src/core/ext/transport/chttp2/transport/chttp2_transport.h"

#include <inttypes.h>
#include <limits.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/container/flat_hash_map.h"
#include "absl/hash/hash.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/meta/type_traits.h"
#include "absl/random/random.h"
#include "absl/status/status.h"
#include "absl/strings/cord.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "absl/types/optional.h"
#include "absl/types/variant.h"

#include <grpc/event_engine/event_engine.h>
#include <grpc/grpc.h>
#include <grpc/impl/channel_arg_names.h>
#include <grpc/impl/connectivity_state.h>
#include <grpc/slice_buffer.h>
#include <grpc/status.h>
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/port_platform.h>
#include <grpc/support/time.h>

#include "src/core/ext/transport/chttp2/transport/context_list_entry.h"
#include "src/core/ext/transport/chttp2/transport/flow_control.h"
#include "src/core/ext/transport/chttp2/transport/frame_data.h"
#include "src/core/ext/transport/chttp2/transport/frame_goaway.h"
#include "src/core/ext/transport/chttp2/transport/frame_rst_stream.h"
#include "src/core/ext/transport/chttp2/transport/hpack_encoder.h"
#include "src/core/ext/transport/chttp2/transport/http2_settings.h"
#include "src/core/ext/transport/chttp2/transport/http_trace.h"
#include "src/core/ext/transport/chttp2/transport/internal.h"
#include "src/core/ext/transport/chttp2/transport/legacy_frame.h"
#include "src/core/ext/transport/chttp2/transport/max_concurrent_streams_policy.h"
#include "src/core/ext/transport/chttp2/transport/ping_abuse_policy.h"
#include "src/core/ext/transport/chttp2/transport/ping_callbacks.h"
#include "src/core/ext/transport/chttp2/transport/ping_rate_policy.h"
#include "src/core/ext/transport/chttp2/transport/varint.h"
#include "src/core/ext/transport/chttp2/transport/write_size_policy.h"
#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/config/config_vars.h"
#include "src/core/lib/event_engine/extensions/tcp_trace.h"
#include "src/core/lib/event_engine/query_extensions.h"
#include "src/core/lib/experiments/experiments.h"
#include "src/core/lib/gprpp/bitset.h"
#include "src/core/lib/gprpp/crash.h"
#include "src/core/lib/gprpp/debug_location.h"
#include "src/core/lib/gprpp/ref_counted.h"
#include "src/core/lib/gprpp/status_helper.h"
#include "src/core/lib/gprpp/time.h"
#include "src/core/lib/iomgr/combiner.h"
#include "src/core/lib/iomgr/error.h"
#include "src/core/lib/iomgr/ev_posix.h"
#include "src/core/lib/iomgr/event_engine_shims/endpoint.h"
#include "src/core/lib/iomgr/exec_ctx.h"
#include "src/core/lib/iomgr/iomgr_fwd.h"
#include "src/core/lib/iomgr/port.h"
#include "src/core/lib/promise/poll.h"
#include "src/core/lib/resource_quota/arena.h"
#include "src/core/lib/resource_quota/memory_quota.h"
#include "src/core/lib/resource_quota/resource_quota.h"
#include "src/core/lib/resource_quota/trace.h"
#include "src/core/lib/slice/slice.h"
#include "src/core/lib/slice/slice_buffer.h"
#include "src/core/lib/slice/slice_internal.h"
#include "src/core/lib/transport/bdp_estimator.h"
#include "src/core/lib/transport/connectivity_state.h"
#include "src/core/lib/transport/error_utils.h"
#include "src/core/lib/transport/http2_errors.h"
#include "src/core/lib/transport/metadata_batch.h"
#include "src/core/lib/transport/metadata_info.h"
#include "src/core/lib/transport/status_conversion.h"
#include "src/core/lib/transport/transport.h"
#include "src/core/telemetry/call_tracer.h"
#include "src/core/telemetry/stats.h"
#include "src/core/telemetry/stats_data.h"
#include "src/core/telemetry/tcp_tracer.h"
#include "src/core/util/http_client/parser.h"
#include "src/core/util/string.h"
#include "src/core/util/useful.h"

#define DEFAULT_CONNECTION_WINDOW_TARGET (1024 * 1024)
#define MAX_WINDOW 0x7fffffffu
#define MAX_WRITE_BUFFER_SIZE (64 * 1024 * 1024)

#define KEEPALIVE_TIME_BACKOFF_MULTIPLIER 2

#define DEFAULT_MAX_PENDING_INDUCED_FRAMES 10000

#define GRPC_ARG_HTTP2_PING_ON_RST_STREAM_PERCENT \
  "grpc.http2.ping_on_rst_stream_percent"

static grpc_core::Duration g_default_client_keepalive_time =
    grpc_core::Duration::Infinity();
static grpc_core::Duration g_default_client_keepalive_timeout =
    grpc_core::Duration::Seconds(20);
static grpc_core::Duration g_default_server_keepalive_time =
    grpc_core::Duration::Hours(2);
static grpc_core::Duration g_default_server_keepalive_timeout =
    grpc_core::Duration::Seconds(20);
static bool g_default_client_keepalive_permit_without_calls = false;
static bool g_default_server_keepalive_permit_without_calls = false;

// EXPERIMENTAL: control tarpitting in chttp2
#define GRPC_ARG_HTTP_ALLOW_TARPIT "grpc.http.tarpit"
#define GRPC_ARG_HTTP_TARPIT_MIN_DURATION_MS "grpc.http.tarpit_min_duration_ms"
#define GRPC_ARG_HTTP_TARPIT_MAX_DURATION_MS "grpc.http.tarpit_max_duration_ms"

#define MAX_CLIENT_STREAM_ID 0x7fffffffu
grpc_core::TraceFlag grpc_keepalive_trace(false, "http_keepalive");

// forward declarations of various callbacks that we'll build closures around
static void write_action_begin_locked(
    grpc_core::RefCountedPtr<grpc_chttp2_transport>, grpc_error_handle error);
static void write_action(grpc_chttp2_transport* t);
static void write_action_end(grpc_core::RefCountedPtr<grpc_chttp2_transport>,
                             grpc_error_handle error);
static void write_action_end_locked(
    grpc_core::RefCountedPtr<grpc_chttp2_transport>, grpc_error_handle error);

static void read_action(grpc_core::RefCountedPtr<grpc_chttp2_transport>,
                        grpc_error_handle error);
static void read_action_locked(grpc_core::RefCountedPtr<grpc_chttp2_transport>,
                               grpc_error_handle error);
static void continue_read_action_locked(
    grpc_core::RefCountedPtr<grpc_chttp2_transport> t);

static void close_from_api(grpc_chttp2_transport* t, grpc_chttp2_stream* s,
                           grpc_error_handle error, bool tarpit);

// Start new streams that have been created if we can
static void maybe_start_some_streams(grpc_chttp2_transport* t);

static void connectivity_state_set(grpc_chttp2_transport* t,
                                   grpc_connectivity_state state,
                                   const absl::Status& status,
                                   const char* reason);

static void benign_reclaimer_locked(
    grpc_core::RefCountedPtr<grpc_chttp2_transport>, grpc_error_handle error);
static void destructive_reclaimer_locked(
    grpc_core::RefCountedPtr<grpc_chttp2_transport>, grpc_error_handle error);

static void post_benign_reclaimer(grpc_chttp2_transport* t);
static void post_destructive_reclaimer(grpc_chttp2_transport* t);

static void close_transport_locked(grpc_chttp2_transport* t,
                                   grpc_error_handle error);
static void end_all_the_calls(grpc_chttp2_transport* t,
                              grpc_error_handle error);

static void start_bdp_ping(grpc_core::RefCountedPtr<grpc_chttp2_transport>,
                           grpc_error_handle error);
static void finish_bdp_ping(grpc_core::RefCountedPtr<grpc_chttp2_transport>,
                            grpc_error_handle error);
static void start_bdp_ping_locked(
    grpc_core::RefCountedPtr<grpc_chttp2_transport>, grpc_error_handle error);
static void finish_bdp_ping_locked(
    grpc_core::RefCountedPtr<grpc_chttp2_transport>, grpc_error_handle error);
static void next_bdp_ping_timer_expired(grpc_chttp2_transport* t);
static void next_bdp_ping_timer_expired_locked(
    grpc_core::RefCountedPtr<grpc_chttp2_transport> tp,
    GRPC_UNUSED grpc_error_handle error);

static void cancel_pings(grpc_chttp2_transport* t, grpc_error_handle error);
static void send_ping_locked(grpc_chttp2_transport* t,
                             grpc_closure* on_initiate, grpc_closure* on_ack);
static void retry_initiate_ping_locked(
    grpc_core::RefCountedPtr<grpc_chttp2_transport> t,
    GRPC_UNUSED grpc_error_handle error);

// keepalive-relevant functions
static void init_keepalive_ping(
    grpc_core::RefCountedPtr<grpc_chttp2_transport> t);
static void init_keepalive_ping_locked(
    grpc_core::RefCountedPtr<grpc_chttp2_transport> t,
    GRPC_UNUSED grpc_error_handle error);
static void finish_keepalive_ping(
    grpc_core::RefCountedPtr<grpc_chttp2_transport> t, grpc_error_handle error);
static void finish_keepalive_ping_locked(
    grpc_core::RefCountedPtr<grpc_chttp2_transport> t, grpc_error_handle error);
static void maybe_reset_keepalive_ping_timer_locked(grpc_chttp2_transport* t);

static void send_goaway(grpc_chttp2_transport* t, grpc_error_handle error,
                        bool immediate_disconnect_hint);

// Timeout for getting an ack back on settings changes
#define GRPC_ARG_SETTINGS_TIMEOUT "grpc.http2.settings_timeout"

namespace {

using TaskHandle = ::grpc_event_engine::experimental::EventEngine::TaskHandle;

grpc_core::CallTracerAnnotationInterface* CallTracerIfSampled(
    grpc_chttp2_stream* s) {
  if (!grpc_core::IsTraceRecordCallopsEnabled()) {
    return nullptr;
  }
  auto* call_tracer =
      s->arena->GetContext<grpc_core::CallTracerAnnotationInterface>();
  if (call_tracer == nullptr || !call_tracer->IsSampled()) {
    return nullptr;
  }
  return call_tracer;
}

std::shared_ptr<grpc_core::TcpTracerInterface> TcpTracerIfSampled(
    grpc_chttp2_stream* s) {
  if (!grpc_core::IsTraceRecordCallopsEnabled()) {
    return nullptr;
  }
  auto* call_attempt_tracer =
      s->arena->GetContext<grpc_core::CallTracerInterface>();
  if (call_attempt_tracer == nullptr || !call_attempt_tracer->IsSampled()) {
    return nullptr;
  }
  return call_attempt_tracer->StartNewTcpTrace();
}

grpc_core::WriteTimestampsCallback g_write_timestamps_callback = nullptr;
grpc_core::CopyContextFn g_get_copied_context_fn = nullptr;
}  // namespace

namespace grpc_core {

namespace {
// Initialize a grpc_closure \a c to call \a Fn with \a t and \a error. Holds
// the passed in reference to \a t until it's moved into Fn.
template <void (*Fn)(RefCountedPtr<grpc_chttp2_transport>, grpc_error_handle)>
grpc_closure* InitTransportClosure(RefCountedPtr<grpc_chttp2_transport> t,
                                   grpc_closure* c) {
  GRPC_CLOSURE_INIT(
      c,
      [](void* tp, grpc_error_handle error) {
        Fn(RefCountedPtr<grpc_chttp2_transport>(
               static_cast<grpc_chttp2_transport*>(tp)),
           std::move(error));
      },
      t.release(), nullptr);
  return c;
}
}  // namespace

namespace {
TestOnlyGlobalHttp2TransportInitCallback test_only_init_callback = nullptr;
TestOnlyGlobalHttp2TransportDestructCallback test_only_destruct_callback =
    nullptr;
bool test_only_disable_transient_failure_state_notification = false;
}  // namespace

void TestOnlySetGlobalHttp2TransportInitCallback(
    TestOnlyGlobalHttp2TransportInitCallback callback) {
  test_only_init_callback = callback;
}

void TestOnlySetGlobalHttp2TransportDestructCallback(
    TestOnlyGlobalHttp2TransportDestructCallback callback) {
  test_only_destruct_callback = callback;
}

void TestOnlyGlobalHttp2TransportDisableTransientFailureStateNotification(
    bool disable) {
  test_only_disable_transient_failure_state_notification = disable;
}

void GrpcHttp2SetWriteTimestampsCallback(WriteTimestampsCallback fn) {
  g_write_timestamps_callback = fn;
}

void GrpcHttp2SetCopyContextFn(CopyContextFn fn) {
  g_get_copied_context_fn = fn;
}

WriteTimestampsCallback GrpcHttp2GetWriteTimestampsCallback() {
  return g_write_timestamps_callback;
}

CopyContextFn GrpcHttp2GetCopyContextFn() { return g_get_copied_context_fn; }

// For each entry in the passed ContextList, it executes the function set using
// GrpcHttp2SetWriteTimestampsCallback method with each context in the list
// and \a ts. It also deletes/frees up the passed ContextList after this
// operation.
void ForEachContextListEntryExecute(void* arg, Timestamps* ts,
                                    grpc_error_handle error) {
  ContextList* context_list = reinterpret_cast<ContextList*>(arg);
  if (!context_list) {
    return;
  }
  for (auto it = context_list->begin(); it != context_list->end(); it++) {
    ContextListEntry& entry = (*it);
    if (ts) {
      ts->byte_offset = static_cast<uint32_t>(entry.ByteOffsetInStream());
    }
    g_write_timestamps_callback(entry.TraceContext(), ts, error);
  }
  delete context_list;
}

HttpAnnotation::HttpAnnotation(Type type, gpr_timespec time)
    : CallTracerAnnotationInterface::Annotation(
          CallTracerAnnotationInterface::AnnotationType::kHttpTransport),
      type_(type),
      time_(time) {}

std::string HttpAnnotation::ToString() const {
  std::string s = "HttpAnnotation type: ";
  switch (type_) {
    case Type::kStart:
      absl::StrAppend(&s, "Start");
      break;
    case Type::kHeadWritten:
      absl::StrAppend(&s, "HeadWritten");
      break;
    case Type::kEnd:
      absl::StrAppend(&s, "End");
      break;
    default:
      absl::StrAppend(&s, "Unknown");
  }
  absl::StrAppend(&s, " time: ", gpr_format_timespec(time_));
  if (transport_stats_.has_value()) {
    absl::StrAppend(&s, " transport:[", transport_stats_->ToString(), "]");
  }
  if (stream_stats_.has_value()) {
    absl::StrAppend(&s, " stream:[", stream_stats_->ToString(), "]");
  }
  return s;
}

}  // namespace grpc_core

//
// CONSTRUCTION/DESTRUCTION/REFCOUNTING
//

grpc_chttp2_transport::~grpc_chttp2_transport() {
  size_t i;

  cancel_pings(this, GRPC_ERROR_CREATE("Transport destroyed"));

  event_engine.reset();

  if (channelz_socket != nullptr) {
    channelz_socket.reset();
  }

  if (ep != nullptr) grpc_endpoint_destroy(ep);

  grpc_slice_buffer_destroy(&qbuf);

  grpc_error_handle error = GRPC_ERROR_CREATE("Transport destroyed");
  // ContextList::Execute follows semantics of a callback function and does not
  // take a ref on error
  if (context_list != nullptr) {
    grpc_core::ForEachContextListEntryExecute(context_list, nullptr, error);
  }
  context_list = nullptr;

  grpc_slice_buffer_destroy(&read_buffer);
  grpc_chttp2_goaway_parser_destroy(&goaway_parser);

  for (i = 0; i < STREAM_LIST_COUNT; i++) {
    CHECK_EQ(lists[i].head, nullptr);
    CHECK_EQ(lists[i].tail, nullptr);
  }

  CHECK(stream_map.empty());
  GRPC_COMBINER_UNREF(combiner, "chttp2_transport");

  while (write_cb_pool) {
    grpc_chttp2_write_cb* next = write_cb_pool->next;
    gpr_free(write_cb_pool);
    write_cb_pool = next;
  }

  gpr_free(ping_acks);
  if (grpc_core::test_only_destruct_callback != nullptr) {
    grpc_core::test_only_destruct_callback();
  }
}

static void read_channel_args(grpc_chttp2_transport* t,
                              const grpc_core::ChannelArgs& channel_args,
                              bool is_client) {
  const int initial_sequence_number =
      channel_args.GetInt(GRPC_ARG_HTTP2_INITIAL_SEQUENCE_NUMBER).value_or(-1);
  if (initial_sequence_number > 0) {
    if ((t->next_stream_id & 1) != (initial_sequence_number & 1)) {
      gpr_log(GPR_ERROR, "%s: low bit must be %d on %s",
              GRPC_ARG_HTTP2_INITIAL_SEQUENCE_NUMBER, t->next_stream_id & 1,
              is_client ? "client" : "server");
    } else {
      t->next_stream_id = static_cast<uint32_t>(initial_sequence_number);
    }
  }

  const int max_hpack_table_size =
      channel_args.GetInt(GRPC_ARG_HTTP2_HPACK_TABLE_SIZE_ENCODER).value_or(-1);
  if (max_hpack_table_size >= 0) {
    t->hpack_compressor.SetMaxUsableSize(max_hpack_table_size);
  }

  t->write_buffer_size =
      std::max(0, channel_args.GetInt(GRPC_ARG_HTTP2_WRITE_BUFFER_SIZE)
                      .value_or(grpc_core::chttp2::kDefaultWindow));
  t->keepalive_time =
      std::max(grpc_core::Duration::Milliseconds(1),
               channel_args.GetDurationFromIntMillis(GRPC_ARG_KEEPALIVE_TIME_MS)
                   .value_or(t->is_client ? g_default_client_keepalive_time
                                          : g_default_server_keepalive_time));
  t->keepalive_timeout = std::max(
      grpc_core::Duration::Zero(),
      channel_args.GetDurationFromIntMillis(GRPC_ARG_KEEPALIVE_TIMEOUT_MS)
          .value_or(t->keepalive_time == grpc_core::Duration::Infinity()
                        ? grpc_core::Duration::Infinity()
                        : (t->is_client ? g_default_client_keepalive_timeout
                                        : g_default_server_keepalive_timeout)));
  t->ping_timeout = std::max(
      grpc_core::Duration::Zero(),
      channel_args.GetDurationFromIntMillis(GRPC_ARG_PING_TIMEOUT_MS)
          .value_or(t->keepalive_time == grpc_core::Duration::Infinity()
                        ? grpc_core::Duration::Infinity()
                        : grpc_core::Duration::Minutes(1)));
  if (t->is_client) {
    t->keepalive_permit_without_calls =
        channel_args.GetBool(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS)
            .value_or(grpc_core::IsKeepaliveFixEnabled()
                          ? g_default_client_keepalive_permit_without_calls
                          : false);
  } else {
    t->keepalive_permit_without_calls =
        channel_args.GetBool(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS)
            .value_or(grpc_core::IsKeepaliveServerFixEnabled()
                          ? g_default_server_keepalive_permit_without_calls
                          : false);
  }

  t->settings_timeout =
      channel_args.GetDurationFromIntMillis(GRPC_ARG_SETTINGS_TIMEOUT)
          .value_or(std::max(t->keepalive_timeout * 2,
                             grpc_core::Duration::Minutes(1)));

  // Only send the prefered rx frame size http2 setting if we are instructed
  // to auto size the buffers allocated at tcp level and we also can adjust
  // sending frame size.
  t->enable_preferred_rx_crypto_frame_advertisement =
      channel_args
          .GetBool(GRPC_ARG_EXPERIMENTAL_HTTP2_PREFERRED_CRYPTO_FRAME_SIZE)
          .value_or(false);

  const auto max_requests_per_read =
      channel_args.GetInt("grpc.http2.max_requests_per_read");
  if (max_requests_per_read.has_value()) {
    t->max_requests_per_read =
        grpc_core::Clamp(*max_requests_per_read, 1, 10000);
  } else {
    t->max_requests_per_read = 32;
  }

  if (channel_args.GetBool(GRPC_ARG_ENABLE_CHANNELZ)
          .value_or(GRPC_ENABLE_CHANNELZ_DEFAULT)) {
    t->channelz_socket =
        grpc_core::MakeRefCounted<grpc_core::channelz::SocketNode>(
            std::string(grpc_endpoint_get_local_address(t->ep)),
            std::string(t->peer_string.as_string_view()),
            absl::StrCat(t->GetTransportName(), " ",
                         t->peer_string.as_string_view()),
            channel_args
                .GetObjectRef<grpc_core::channelz::SocketNode::Security>());
  }

  t->ack_pings = channel_args.GetBool("grpc.http2.ack_pings").value_or(true);

  t->allow_tarpit =
      channel_args.GetBool(GRPC_ARG_HTTP_ALLOW_TARPIT).value_or(true);
  t->min_tarpit_duration_ms =
      channel_args
          .GetDurationFromIntMillis(GRPC_ARG_HTTP_TARPIT_MIN_DURATION_MS)
          .value_or(grpc_core::Duration::Milliseconds(100))
          .millis();
  t->max_tarpit_duration_ms =
      channel_args
          .GetDurationFromIntMillis(GRPC_ARG_HTTP_TARPIT_MAX_DURATION_MS)
          .value_or(grpc_core::Duration::Seconds(1))
          .millis();
  t->max_header_list_size_soft_limit =
      grpc_core::GetSoftLimitFromChannelArgs(channel_args);

  int value;
  if (!is_client) {
    value = channel_args.GetInt(GRPC_ARG_MAX_CONCURRENT_STREAMS).value_or(-1);
    if (value >= 0) {
      t->settings.mutable_local().SetMaxConcurrentStreams(value);
      t->max_concurrent_streams_policy.SetTarget(value);
    }
  } else if (channel_args.Contains(GRPC_ARG_MAX_CONCURRENT_STREAMS)) {
    gpr_log(GPR_DEBUG, "%s is not available on clients",
            GRPC_ARG_MAX_CONCURRENT_STREAMS);
  }
  value =
      channel_args.GetInt(GRPC_ARG_HTTP2_HPACK_TABLE_SIZE_DECODER).value_or(-1);
  if (value >= 0) {
    t->settings.mutable_local().SetHeaderTableSize(value);
  }
  t->settings.mutable_local().SetMaxHeaderListSize(
      grpc_core::GetHardLimitFromChannelArgs(channel_args));
  value = channel_args.GetInt(GRPC_ARG_HTTP2_MAX_FRAME_SIZE).value_or(-1);
  if (value >= 0) {
    t->settings.mutable_local().SetMaxFrameSize(value);
  }
  value =
      channel_args.GetInt(GRPC_ARG_HTTP2_STREAM_LOOKAHEAD_BYTES).value_or(-1);
  if (value >= 0) {
    t->settings.mutable_local().SetInitialWindowSize(value);
  }
  value = channel_args.GetInt(GRPC_ARG_HTTP2_ENABLE_TRUE_BINARY).value_or(-1);
  if (value >= 0) {
    t->settings.mutable_local().SetAllowTrueBinaryMetadata(value != 0);
  }

  if (t->enable_preferred_rx_crypto_frame_advertisement) {
    t->settings.mutable_local().SetPreferredReceiveCryptoMessageSize(INT_MAX);
  }

  t->ping_on_rst_stream_percent = grpc_core::Clamp(
      channel_args.GetInt(GRPC_ARG_HTTP2_PING_ON_RST_STREAM_PERCENT)
          .value_or(1),
      0, 100);

  t->max_concurrent_streams_overload_protection =
      channel_args.GetBool(GRPC_ARG_MAX_CONCURRENT_STREAMS_OVERLOAD_PROTECTION)
          .value_or(true);
}

static void init_keepalive_pings_if_enabled_locked(
    grpc_core::RefCountedPtr<grpc_chttp2_transport> t,
    GRPC_UNUSED grpc_error_handle error) {
  DCHECK(error.ok());
  if (t->keepalive_time != grpc_core::Duration::Infinity()) {
    t->keepalive_state = GRPC_CHTTP2_KEEPALIVE_STATE_WAITING;
    t->keepalive_ping_timer_handle =
        t->event_engine->RunAfter(t->keepalive_time, [t = t->Ref()]() mutable {
          grpc_core::ApplicationCallbackExecCtx callback_exec_ctx;
          grpc_core::ExecCtx exec_ctx;
          init_keepalive_ping(std::move(t));
        });
  } else {
    // Use GRPC_CHTTP2_KEEPALIVE_STATE_DISABLED to indicate there are no
    // inflight keepalive timers
    t->keepalive_state = GRPC_CHTTP2_KEEPALIVE_STATE_DISABLED;
  }
}

using grpc_event_engine::experimental::QueryExtension;
using grpc_event_engine::experimental::TcpTraceExtension;

grpc_chttp2_transport::grpc_chttp2_transport(
    const grpc_core::ChannelArgs& channel_args, grpc_endpoint* ep,
    bool is_client)
    : ep(ep),
      peer_string(
          grpc_core::Slice::FromCopiedString(grpc_endpoint_get_peer(ep))),
      memory_owner(channel_args.GetObject<grpc_core::ResourceQuota>()
                       ->memory_quota()
                       ->CreateMemoryOwner()),
      self_reservation(
          memory_owner.MakeReservation(sizeof(grpc_chttp2_transport))),
      event_engine(
          channel_args
              .GetObjectRef<grpc_event_engine::experimental::EventEngine>()),
      combiner(grpc_combiner_create(event_engine)),
      state_tracker(is_client ? "client_transport" : "server_transport",
                    GRPC_CHANNEL_READY),
      next_stream_id(is_client ? 1 : 2),
      ping_abuse_policy(channel_args),
      ping_rate_policy(channel_args, is_client),
      flow_control(
          peer_string.as_string_view(),
          channel_args.GetBool(GRPC_ARG_HTTP2_BDP_PROBE).value_or(true),
          &memory_owner),
      deframe_state(is_client ? GRPC_DTS_FH_0 : GRPC_DTS_CLIENT_PREFIX_0),
      is_client(is_client) {
  context_list = new grpc_core::ContextList();

  if (channel_args.GetBool(GRPC_ARG_TCP_TRACING_ENABLED).value_or(false) &&
      grpc_event_engine::experimental::grpc_is_event_engine_endpoint(ep)) {
    auto epte = QueryExtension<TcpTraceExtension>(
        grpc_event_engine::experimental::grpc_get_wrapped_event_engine_endpoint(
            ep));
    if (epte != nullptr) {
      epte->InitializeAndReturnTcpTracer();
    }
  }

  CHECK(strlen(GRPC_CHTTP2_CLIENT_CONNECT_STRING) ==
        GRPC_CHTTP2_CLIENT_CONNECT_STRLEN);

  grpc_slice_buffer_init(&read_buffer);
  if (is_client) {
    grpc_slice_buffer_add(
        outbuf.c_slice_buffer(),
        grpc_slice_from_copied_string(GRPC_CHTTP2_CLIENT_CONNECT_STRING));
  }
  grpc_slice_buffer_init(&qbuf);
  grpc_chttp2_goaway_parser_init(&goaway_parser);

  // configure http2 the way we like it
  if (is_client) {
    settings.mutable_local().SetEnablePush(false);
    settings.mutable_local().SetMaxConcurrentStreams(0);
  }
  settings.mutable_local().SetMaxHeaderListSize(DEFAULT_MAX_HEADER_LIST_SIZE);
  settings.mutable_local().SetAllowTrueBinaryMetadata(true);

  read_channel_args(this, channel_args, is_client);

  // Initially allow *UP TO* MAX_CONCURRENT_STREAMS incoming before we start
  // blanket cancelling them.
  num_incoming_streams_before_settings_ack =
      settings.local().max_concurrent_streams();

  grpc_core::ExecCtx exec_ctx;
  combiner->Run(
      grpc_core::InitTransportClosure<init_keepalive_pings_if_enabled_locked>(
          Ref(), &init_keepalive_ping_locked),
      absl::OkStatus());

  if (flow_control.bdp_probe()) {
    bdp_ping_blocked = true;
    grpc_chttp2_act_on_flowctl_action(flow_control.PeriodicUpdate(), this,
                                      nullptr);
  }

  grpc_chttp2_initiate_write(this, GRPC_CHTTP2_INITIATE_WRITE_INITIAL_WRITE);
  post_benign_reclaimer(this);
  if (grpc_core::test_only_init_callback != nullptr) {
    grpc_core::test_only_init_callback();
  }

#ifdef GRPC_POSIX_SOCKET_TCP
  closure_barrier_may_cover_write =
      grpc_event_engine_run_in_background() &&
              grpc_core::IsScheduleCancellationOverWriteEnabled()
          ? 0
          : CLOSURE_BARRIER_MAY_COVER_WRITE;
#endif
}

static void destroy_transport_locked(void* tp, grpc_error_handle /*error*/) {
  grpc_core::RefCountedPtr<grpc_chttp2_transport> t(
      static_cast<grpc_chttp2_transport*>(tp));
  t->destroying = 1;
  close_transport_locked(
      t.get(),
      grpc_error_set_int(GRPC_ERROR_CREATE("Transport destroyed"),
                         grpc_core::StatusIntProperty::kOccurredDuringWrite,
                         t->write_state));
  t->memory_owner.Reset();
}

void grpc_chttp2_transport::Orphan() {
  combiner->Run(GRPC_CLOSURE_CREATE(destroy_transport_locked, this, nullptr),
                absl::OkStatus());
}

static void close_transport_locked(grpc_chttp2_transport* t,
                                   grpc_error_handle error) {
  end_all_the_calls(t, error);
  cancel_pings(t, error);
  if (t->closed_with_error.ok()) {
    if (!grpc_error_has_clear_grpc_status(error)) {
      error =
          grpc_error_set_int(error, grpc_core::StatusIntProperty::kRpcStatus,
                             GRPC_STATUS_UNAVAILABLE);
    }
    if (t->write_state != GRPC_CHTTP2_WRITE_STATE_IDLE) {
      if (t->close_transport_on_writes_finished.ok()) {
        t->close_transport_on_writes_finished =
            GRPC_ERROR_CREATE("Delayed close due to in-progress write");
      }
      t->close_transport_on_writes_finished =
          grpc_error_add_child(t->close_transport_on_writes_finished, error);
      return;
    }
    CHECK(!error.ok());
    t->closed_with_error = error;
    connectivity_state_set(t, GRPC_CHANNEL_SHUTDOWN, absl::Status(),
                           "close_transport");
    if (t->keepalive_ping_timeout_handle != TaskHandle::kInvalid) {
      t->event_engine->Cancel(std::exchange(t->keepalive_ping_timeout_handle,
                                            TaskHandle::kInvalid));
    }
    if (t->settings_ack_watchdog != TaskHandle::kInvalid) {
      t->event_engine->Cancel(
          std::exchange(t->settings_ack_watchdog, TaskHandle::kInvalid));
    }
    if (t->delayed_ping_timer_handle != TaskHandle::kInvalid &&
        t->event_engine->Cancel(t->delayed_ping_timer_handle)) {
      t->delayed_ping_timer_handle = TaskHandle::kInvalid;
    }
    if (t->next_bdp_ping_timer_handle != TaskHandle::kInvalid &&
        t->event_engine->Cancel(t->next_bdp_ping_timer_handle)) {
      t->next_bdp_ping_timer_handle = TaskHandle::kInvalid;
    }
    switch (t->keepalive_state) {
      case GRPC_CHTTP2_KEEPALIVE_STATE_WAITING:
        if (t->keepalive_ping_timer_handle != TaskHandle::kInvalid &&
            t->event_engine->Cancel(t->keepalive_ping_timer_handle)) {
          t->keepalive_ping_timer_handle = TaskHandle::kInvalid;
        }
        break;
      case GRPC_CHTTP2_KEEPALIVE_STATE_PINGING:
        if (t->keepalive_ping_timer_handle != TaskHandle::kInvalid &&
            t->event_engine->Cancel(t->keepalive_ping_timer_handle)) {
          t->keepalive_ping_timer_handle = TaskHandle::kInvalid;
        }
        break;
      case GRPC_CHTTP2_KEEPALIVE_STATE_DYING:
      case GRPC_CHTTP2_KEEPALIVE_STATE_DISABLED:
        // keepalive timers are not set in these two states
        break;
    }

    // flush writable stream list to avoid dangling references
    grpc_chttp2_stream* s;
    while (grpc_chttp2_list_pop_writable_stream(t, &s)) {
      GRPC_CHTTP2_STREAM_UNREF(s, "chttp2_writing:close");
    }
    CHECK(t->write_state == GRPC_CHTTP2_WRITE_STATE_IDLE);
    if (t->interested_parties_until_recv_settings != nullptr) {
      grpc_endpoint_delete_from_pollset_set(
          t->ep, t->interested_parties_until_recv_settings);
      t->interested_parties_until_recv_settings = nullptr;
    }
    grpc_core::MutexLock lock(&t->ep_destroy_mu);
    grpc_endpoint_destroy(t->ep);
    t->ep = nullptr;
  }
  if (t->notify_on_receive_settings != nullptr) {
    if (t->interested_parties_until_recv_settings != nullptr) {
      grpc_endpoint_delete_from_pollset_set(
          t->ep, t->interested_parties_until_recv_settings);
      t->interested_parties_until_recv_settings = nullptr;
    }
    grpc_core::ExecCtx::Run(DEBUG_LOCATION, t->notify_on_receive_settings,
                            error);
    t->notify_on_receive_settings = nullptr;
  }
  if (t->notify_on_close != nullptr) {
    grpc_core::ExecCtx::Run(DEBUG_LOCATION, t->notify_on_close, error);
    t->notify_on_close = nullptr;
  }
}

#ifndef NDEBUG
void grpc_chttp2_stream_ref(grpc_chttp2_stream* s, const char* reason) {
  grpc_stream_ref(s->refcount, reason);
}
void grpc_chttp2_stream_unref(grpc_chttp2_stream* s, const char* reason) {
  grpc_stream_unref(s->refcount, reason);
}
#else
void grpc_chttp2_stream_ref(grpc_chttp2_stream* s) {
  grpc_stream_ref(s->refcount);
}
void grpc_chttp2_stream_unref(grpc_chttp2_stream* s) {
  grpc_stream_unref(s->refcount);
}
#endif

grpc_chttp2_stream::grpc_chttp2_stream(grpc_chttp2_transport* t,
                                       grpc_stream_refcount* refcount,
                                       const void* server_data,
                                       grpc_core::Arena* arena)
    : t(t->Ref()),
      refcount([refcount]() {
// We reserve one 'active stream' that's dropped when the stream is
//   read-closed. The others are for Chttp2IncomingByteStreams that are
//   actively reading
// We do this here to avoid cache misses.
#ifndef NDEBUG
        grpc_stream_ref(refcount, "chttp2");
#else
        grpc_stream_ref(refcount);
#endif
        return refcount;
      }()),
      arena(arena),
      flow_control(&t->flow_control) {
  t->streams_allocated.fetch_add(1, std::memory_order_relaxed);
  if (server_data) {
    id = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(server_data));
    if (grpc_http_trace.enabled()) {
      VLOG(2) << "HTTP:" << t << "/" << this << " creating accept stream " << id
              << " [from " << server_data << "]";
    }
    *t->accepting_stream = this;
    t->stream_map.emplace(id, this);
    post_destructive_reclaimer(t);
  }

  grpc_slice_buffer_init(&frame_storage);
  grpc_slice_buffer_init(&flow_controlled_buffer);
}

grpc_chttp2_stream::~grpc_chttp2_stream() {
  t->streams_allocated.fetch_sub(1, std::memory_order_relaxed);
  grpc_chttp2_list_remove_stalled_by_stream(t.get(), this);
  grpc_chttp2_list_remove_stalled_by_transport(t.get(), this);

  if (t->channelz_socket != nullptr) {
    if ((t->is_client && eos_received) || (!t->is_client && eos_sent)) {
      t->channelz_socket->RecordStreamSucceeded();
    } else {
      t->channelz_socket->RecordStreamFailed();
    }
  }

  CHECK((write_closed && read_closed) || id == 0);
  if (id != 0) {
    CHECK_EQ(t->stream_map.count(id), 0u);
  }

  grpc_slice_buffer_destroy(&frame_storage);

  for (int i = 0; i < STREAM_LIST_COUNT; i++) {
    if (GPR_UNLIKELY(included.is_set(i))) {
      grpc_core::Crash(absl::StrFormat("%s stream %d still included in list %d",
                                       t->is_client ? "client" : "server", id,
                                       i));
    }
  }

  CHECK_EQ(send_initial_metadata_finished, nullptr);
  CHECK_EQ(send_trailing_metadata_finished, nullptr);
  CHECK_EQ(recv_initial_metadata_ready, nullptr);
  CHECK_EQ(recv_message_ready, nullptr);
  CHECK_EQ(recv_trailing_metadata_finished, nullptr);
  grpc_slice_buffer_destroy(&flow_controlled_buffer);
  grpc_core::ExecCtx::Run(DEBUG_LOCATION, destroy_stream_arg, absl::OkStatus());
}

void grpc_chttp2_transport::InitStream(grpc_stream* gs,
                                       grpc_stream_refcount* refcount,
                                       const void* server_data,
                                       grpc_core::Arena* arena) {
  new (gs) grpc_chttp2_stream(this, refcount, server_data, arena);
}

static void destroy_stream_locked(void* sp, grpc_error_handle /*error*/) {
  grpc_chttp2_stream* s = static_cast<grpc_chttp2_stream*>(sp);
  s->~grpc_chttp2_stream();
}

void grpc_chttp2_transport::DestroyStream(grpc_stream* gs,
                                          grpc_closure* then_schedule_closure) {
  grpc_chttp2_stream* s = reinterpret_cast<grpc_chttp2_stream*>(gs);

  s->destroy_stream_arg = then_schedule_closure;
  combiner->Run(
      GRPC_CLOSURE_INIT(&s->destroy_stream, destroy_stream_locked, s, nullptr),
      absl::OkStatus());
}

grpc_chttp2_stream* grpc_chttp2_parsing_accept_stream(grpc_chttp2_transport* t,
                                                      uint32_t id) {
  if (t->accept_stream_cb == nullptr) {
    return nullptr;
  }
  grpc_chttp2_stream* accepting = nullptr;
  CHECK_EQ(t->accepting_stream, nullptr);
  t->accepting_stream = &accepting;
  t->accept_stream_cb(t->accept_stream_cb_user_data, t,
                      reinterpret_cast<void*>(id));
  t->accepting_stream = nullptr;
  return accepting;
}

//
// OUTPUT PROCESSING
//

static const char* write_state_name(grpc_chttp2_write_state st) {
  switch (st) {
    case GRPC_CHTTP2_WRITE_STATE_IDLE:
      return "IDLE";
    case GRPC_CHTTP2_WRITE_STATE_WRITING:
      return "WRITING";
    case GRPC_CHTTP2_WRITE_STATE_WRITING_WITH_MORE:
      return "WRITING+MORE";
  }
  GPR_UNREACHABLE_CODE(return "UNKNOWN");
}

static void set_write_state(grpc_chttp2_transport* t,
                            grpc_chttp2_write_state st, const char* reason) {
  GRPC_CHTTP2_IF_TRACING(
      gpr_log(GPR_INFO, "W:%p %s [%s] state %s -> %s [%s]", t,
              t->is_client ? "CLIENT" : "SERVER",
              std::string(t->peer_string.as_string_view()).c_str(),
              write_state_name(t->write_state), write_state_name(st), reason));
  t->write_state = st;
  // If the state is being reset back to idle, it means a write was just
  // finished. Make sure all the run_after_write closures are scheduled.
  //
  // This is also our chance to close the transport if the transport was marked
  // to be closed after all writes finish (for example, if we received a go-away
  // from peer while we had some pending writes)
  if (st == GRPC_CHTTP2_WRITE_STATE_IDLE) {
    grpc_core::ExecCtx::RunList(DEBUG_LOCATION, &t->run_after_write);
    if (!t->close_transport_on_writes_finished.ok()) {
      grpc_error_handle err = t->close_transport_on_writes_finished;
      t->close_transport_on_writes_finished = absl::OkStatus();
      close_transport_locked(t, err);
    }
  }
}

void grpc_chttp2_initiate_write(grpc_chttp2_transport* t,
                                grpc_chttp2_initiate_write_reason reason) {
  switch (t->write_state) {
    case GRPC_CHTTP2_WRITE_STATE_IDLE:
      set_write_state(t, GRPC_CHTTP2_WRITE_STATE_WRITING,
                      grpc_chttp2_initiate_write_reason_string(reason));
      // Note that the 'write_action_begin_locked' closure is being scheduled
      // on the 'finally_scheduler' of t->combiner. This means that
      // 'write_action_begin_locked' is called only *after* all the other
      // closures (some of which are potentially initiating more writes on the
      // transport) are executed on the t->combiner.
      //
      // The reason for scheduling on finally_scheduler is to make sure we batch
      // as many writes as possible. 'write_action_begin_locked' is the function
      // that gathers all the relevant bytes (which are at various places in the
      // grpc_chttp2_transport structure) and append them to 'outbuf' field in
      // grpc_chttp2_transport thereby batching what would have been potentially
      // multiple write operations.
      //
      // Also, 'write_action_begin_locked' only gathers the bytes into outbuf.
      // It does not call the endpoint to write the bytes. That is done by the
      // 'write_action' (which is scheduled by 'write_action_begin_locked')
      t->combiner->FinallyRun(
          grpc_core::InitTransportClosure<write_action_begin_locked>(
              t->Ref(), &t->write_action_begin_locked),
          absl::OkStatus());
      break;
    case GRPC_CHTTP2_WRITE_STATE_WRITING:
      set_write_state(t, GRPC_CHTTP2_WRITE_STATE_WRITING_WITH_MORE,
                      grpc_chttp2_initiate_write_reason_string(reason));
      break;
    case GRPC_CHTTP2_WRITE_STATE_WRITING_WITH_MORE:
      break;
  }
}

void grpc_chttp2_mark_stream_writable(grpc_chttp2_transport* t,
                                      grpc_chttp2_stream* s) {
  if (t->closed_with_error.ok() && grpc_chttp2_list_add_writable_stream(t, s)) {
    GRPC_CHTTP2_STREAM_REF(s, "chttp2_writing:become");
  }
}

static const char* begin_writing_desc(bool partial) {
  if (partial) {
    return "begin partial write in background";
  } else {
    return "begin write in current thread";
  }
}

static void write_action_begin_locked(
    grpc_core::RefCountedPtr<grpc_chttp2_transport> t,
    grpc_error_handle /*error_ignored*/) {
  CHECK(t->write_state != GRPC_CHTTP2_WRITE_STATE_IDLE);
  grpc_chttp2_begin_write_result r;
  if (!t->closed_with_error.ok()) {
    r.writing = false;
  } else {
    r = grpc_chttp2_begin_write(t.get());
  }
  if (r.writing) {
    set_write_state(t.get(),
                    r.partial ? GRPC_CHTTP2_WRITE_STATE_WRITING_WITH_MORE
                              : GRPC_CHTTP2_WRITE_STATE_WRITING,
                    begin_writing_desc(r.partial));
    write_action(t.get());
    if (t->reading_paused_on_pending_induced_frames) {
      CHECK_EQ(t->num_pending_induced_frames, 0u);
      // We had paused reading, because we had many induced frames (SETTINGS
      // ACK, PINGS ACK and RST_STREAMS) pending in t->qbuf. Now that we have
      // been able to flush qbuf, we can resume reading.
      GRPC_CHTTP2_IF_TRACING(gpr_log(
          GPR_INFO,
          "transport %p : Resuming reading after being paused due to too "
          "many unwritten SETTINGS ACK, PINGS ACK and RST_STREAM frames",
          t.get()));
      t->reading_paused_on_pending_induced_frames = false;
      continue_read_action_locked(std::move(t));
    }
  } else {
    set_write_state(t.get(), GRPC_CHTTP2_WRITE_STATE_IDLE,
                    "begin writing nothing");
  }
}

static void write_action(grpc_chttp2_transport* t) {
  void* cl = t->context_list;
  if (!t->context_list->empty()) {
    // Transfer the ownership of the context list to the endpoint and create and
    // associate a new context list with the transport.
    // The old context list is stored in the cl local variable which is passed
    // to the endpoint. Its upto the endpoint to manage its lifetime.
    t->context_list = new grpc_core::ContextList();
  } else {
    // t->cl is Empty. There is nothing to trace in this endpoint_write. set cl
    // to nullptr.
    cl = nullptr;
  }
  // Choose max_frame_size as the prefered rx crypto frame size indicated by the
  // peer.
  int max_frame_size =
      t->settings.peer().preferred_receive_crypto_message_size();
  // Note: max frame size is 0 if the remote peer does not support adjusting the
  // sending frame size.
  if (max_frame_size == 0) {
    max_frame_size = INT_MAX;
  }
  if (GRPC_TRACE_FLAG_ENABLED(grpc_ping_trace)) {
    LOG(INFO) << (t->is_client ? "CLIENT" : "SERVER") << "[" << t << "]: Write "
              << t->outbuf.Length() << " bytes";
  }
  t->write_size_policy.BeginWrite(t->outbuf.Length());
  grpc_endpoint_write(t->ep, t->outbuf.c_slice_buffer(),
                      grpc_core::InitTransportClosure<write_action_end>(
                          t->Ref(), &t->write_action_end_locked),
                      cl, max_frame_size);
}

static void write_action_end(grpc_core::RefCountedPtr<grpc_chttp2_transport> t,
                             grpc_error_handle error) {
  auto* tp = t.get();
  if (GRPC_TRACE_FLAG_ENABLED(grpc_ping_trace)) {
    LOG(INFO) << (t->is_client ? "CLIENT" : "SERVER") << "[" << t.get()
              << "]: Finish write";
  }
  tp->combiner->Run(grpc_core::InitTransportClosure<write_action_end_locked>(
                        std::move(t), &tp->write_action_end_locked),
                    error);
}

// Callback from the grpc_endpoint after bytes have been written by calling
// sendmsg
static void write_action_end_locked(
    grpc_core::RefCountedPtr<grpc_chttp2_transport> t,
    grpc_error_handle error) {
  t->write_size_policy.EndWrite(error.ok());

  bool closed = false;
  if (!error.ok()) {
    close_transport_locked(t.get(), error);
    closed = true;
  }

  if (t->sent_goaway_state == GRPC_CHTTP2_FINAL_GOAWAY_SEND_SCHEDULED) {
    t->sent_goaway_state = GRPC_CHTTP2_FINAL_GOAWAY_SENT;
    closed = true;
    if (t->stream_map.empty()) {
      close_transport_locked(t.get(), GRPC_ERROR_CREATE("goaway sent"));
    }
  }

  switch (t->write_state) {
    case GRPC_CHTTP2_WRITE_STATE_IDLE:
      GPR_UNREACHABLE_CODE(break);
    case GRPC_CHTTP2_WRITE_STATE_WRITING:
      set_write_state(t.get(), GRPC_CHTTP2_WRITE_STATE_IDLE, "finish writing");
      break;
    case GRPC_CHTTP2_WRITE_STATE_WRITING_WITH_MORE:
      set_write_state(t.get(), GRPC_CHTTP2_WRITE_STATE_WRITING,
                      "continue writing");
      // If the transport is closed, we will retry writing on the endpoint
      // and next write may contain part of the currently serialized frames.
      // So, we should only call the run_after_write callbacks when the next
      // write finishes, or the callbacks will be invoked when the stream is
      // closed.
      if (!closed) {
        grpc_core::ExecCtx::RunList(DEBUG_LOCATION, &t->run_after_write);
      }
      t->combiner->FinallyRun(
          grpc_core::InitTransportClosure<write_action_begin_locked>(
              t, &t->write_action_begin_locked),
          absl::OkStatus());
      break;
  }

  grpc_chttp2_end_write(t.get(), error);
}

// Cancel out streams that haven't yet started if we have received a GOAWAY
static void cancel_unstarted_streams(grpc_chttp2_transport* t,
                                     grpc_error_handle error, bool tarpit) {
  grpc_chttp2_stream* s;
  while (grpc_chttp2_list_pop_waiting_for_concurrency(t, &s)) {
    s->trailing_metadata_buffer.Set(
        grpc_core::GrpcStreamNetworkState(),
        grpc_core::GrpcStreamNetworkState::kNotSentOnWire);
    grpc_chttp2_cancel_stream(t, s, error, tarpit);
  }
}

void grpc_chttp2_add_incoming_goaway(grpc_chttp2_transport* t,
                                     uint32_t goaway_error,
                                     uint32_t last_stream_id,
                                     absl::string_view goaway_text) {
  t->goaway_error = grpc_error_set_int(
      grpc_error_set_int(
          grpc_core::StatusCreate(
              absl::StatusCode::kUnavailable,
              absl::StrFormat("GOAWAY received; Error code: %u; Debug Text: %s",
                              goaway_error, goaway_text),
              DEBUG_LOCATION, {}),
          grpc_core::StatusIntProperty::kHttp2Error,
          static_cast<intptr_t>(goaway_error)),
      grpc_core::StatusIntProperty::kRpcStatus, GRPC_STATUS_UNAVAILABLE);

  GRPC_CHTTP2_IF_TRACING(
      gpr_log(GPR_INFO, "transport %p got goaway with last stream id %d", t,
              last_stream_id));
  // We want to log this irrespective of whether http tracing is enabled if we
  // received a GOAWAY with a non NO_ERROR code.
  if (goaway_error != GRPC_HTTP2_NO_ERROR) {
    gpr_log(GPR_INFO, "%s: Got goaway [%d] err=%s",
            std::string(t->peer_string.as_string_view()).c_str(), goaway_error,
            grpc_core::StatusToString(t->goaway_error).c_str());
  }
  if (t->is_client) {
    cancel_unstarted_streams(t, t->goaway_error, false);
    // Cancel all unseen streams
    std::vector<grpc_chttp2_stream*> to_cancel;
    for (auto id_stream : t->stream_map) {
      if (id_stream.first > last_stream_id) {
        to_cancel.push_back(id_stream.second);
      }
    }
    for (auto s : to_cancel) {
      s->trailing_metadata_buffer.Set(
          grpc_core::GrpcStreamNetworkState(),
          grpc_core::GrpcStreamNetworkState::kNotSeenByServer);
      grpc_chttp2_cancel_stream(s->t.get(), s, s->t->goaway_error, false);
    }
  }
  absl::Status status = grpc_error_to_absl_status(t->goaway_error);
  // When a client receives a GOAWAY with error code ENHANCE_YOUR_CALM and debug
  // data equal to "too_many_pings", it should log the occurrence at a log level
  // that is enabled by default and double the configured KEEPALIVE_TIME used
  // for new connections on that channel.
  if (GPR_UNLIKELY(t->is_client &&
                   goaway_error == GRPC_HTTP2_ENHANCE_YOUR_CALM &&
                   goaway_text == "too_many_pings")) {
    gpr_log(GPR_ERROR,
            "%s: Received a GOAWAY with error code ENHANCE_YOUR_CALM and debug "
            "data equal to \"too_many_pings\". Current keepalive time (before "
            "throttling): %s",
            std::string(t->peer_string.as_string_view()).c_str(),
            t->keepalive_time.ToString().c_str());
    constexpr int max_keepalive_time_millis =
        INT_MAX / KEEPALIVE_TIME_BACKOFF_MULTIPLIER;
    int64_t throttled_keepalive_time =
        t->keepalive_time.millis() > max_keepalive_time_millis
            ? INT_MAX
            : t->keepalive_time.millis() * KEEPALIVE_TIME_BACKOFF_MULTIPLIER;
    status.SetPayload(grpc_core::kKeepaliveThrottlingKey,
                      absl::Cord(std::to_string(throttled_keepalive_time)));
  }
  // lie: use transient failure from the transport to indicate goaway has been
  // received.
  if (!grpc_core::test_only_disable_transient_failure_state_notification) {
    connectivity_state_set(t, GRPC_CHANNEL_TRANSIENT_FAILURE, status,
                           "got_goaway");
  }
}

static void maybe_start_some_streams(grpc_chttp2_transport* t) {
  grpc_chttp2_stream* s;
  // maybe cancel out streams that haven't yet started if we have received a
  // GOAWAY
  if (!t->goaway_error.ok()) {
    cancel_unstarted_streams(t, t->goaway_error, false);
    return;
  }
  // start streams where we have free grpc_chttp2_stream ids and free
  // * concurrency
  while (t->next_stream_id <= MAX_CLIENT_STREAM_ID &&
         t->stream_map.size() < t->settings.peer().max_concurrent_streams() &&
         grpc_chttp2_list_pop_waiting_for_concurrency(t, &s)) {
    // safe since we can't (legally) be parsing this stream yet
    GRPC_CHTTP2_IF_TRACING(gpr_log(
        GPR_INFO,
        "HTTP:%s: Transport %p allocating new grpc_chttp2_stream %p to id %d",
        t->is_client ? "CLI" : "SVR", t, s, t->next_stream_id));

    CHECK_EQ(s->id, 0u);
    s->id = t->next_stream_id;
    t->next_stream_id += 2;

    if (t->next_stream_id >= MAX_CLIENT_STREAM_ID) {
      connectivity_state_set(t, GRPC_CHANNEL_TRANSIENT_FAILURE,
                             absl::Status(absl::StatusCode::kUnavailable,
                                          "Transport Stream IDs exhausted"),
                             "no_more_stream_ids");
    }

    t->stream_map.emplace(s->id, s);
    post_destructive_reclaimer(t);
    grpc_chttp2_mark_stream_writable(t, s);
    grpc_chttp2_initiate_write(t, GRPC_CHTTP2_INITIATE_WRITE_START_NEW_STREAM);
  }
  // cancel out streams that will never be started
  if (t->next_stream_id >= MAX_CLIENT_STREAM_ID) {
    while (grpc_chttp2_list_pop_waiting_for_concurrency(t, &s)) {
      s->trailing_metadata_buffer.Set(
          grpc_core::GrpcStreamNetworkState(),
          grpc_core::GrpcStreamNetworkState::kNotSentOnWire);
      grpc_chttp2_cancel_stream(
          t, s,
          grpc_error_set_int(GRPC_ERROR_CREATE("Stream IDs exhausted"),
                             grpc_core::StatusIntProperty::kRpcStatus,
                             GRPC_STATUS_UNAVAILABLE),
          false);
    }
  }
}

static grpc_closure* add_closure_barrier(grpc_closure* closure) {
  closure->next_data.scratch += CLOSURE_BARRIER_FIRST_REF_BIT;
  return closure;
}

static void null_then_sched_closure(grpc_closure** closure) {
  grpc_closure* c = *closure;
  *closure = nullptr;
  // null_then_schedule_closure might be run during a start_batch which might
  // subsequently examine the batch for more operations contained within.
  // However, the closure run might make it back to the call object, push a
  // completion, have the application see it, and make a new operation on the
  // call which recycles the batch BEFORE the call to start_batch completes,
  // forcing a race.
  grpc_core::ExecCtx::Run(DEBUG_LOCATION, c, absl::OkStatus());
}

void grpc_chttp2_complete_closure_step(grpc_chttp2_transport* t,
                                       grpc_closure** pclosure,
                                       grpc_error_handle error,
                                       const char* desc,
                                       grpc_core::DebugLocation whence) {
  grpc_closure* closure = *pclosure;
  *pclosure = nullptr;
  if (closure == nullptr) {
    return;
  }
  closure->next_data.scratch -= CLOSURE_BARRIER_FIRST_REF_BIT;
  if (GRPC_TRACE_FLAG_ENABLED(grpc_http_trace)) {
    gpr_log(
        GPR_INFO,
        "complete_closure_step: t=%p %p refs=%d flags=0x%04x desc=%s err=%s "
        "write_state=%s whence=%s:%d",
        t, closure,
        static_cast<int>(closure->next_data.scratch /
                         CLOSURE_BARRIER_FIRST_REF_BIT),
        static_cast<int>(closure->next_data.scratch %
                         CLOSURE_BARRIER_FIRST_REF_BIT),
        desc, grpc_core::StatusToString(error).c_str(),
        write_state_name(t->write_state), whence.file(), whence.line());
  }

  if (!error.ok()) {
    grpc_error_handle cl_err =
        grpc_core::internal::StatusMoveFromHeapPtr(closure->error_data.error);
    if (cl_err.ok()) {
      cl_err = GRPC_ERROR_CREATE(absl::StrCat(
          "Error in HTTP transport completing operation: ", desc,
          " write_state=", write_state_name(t->write_state),
          " refs=", closure->next_data.scratch / CLOSURE_BARRIER_FIRST_REF_BIT,
          " flags=", closure->next_data.scratch % CLOSURE_BARRIER_FIRST_REF_BIT,
          " peer_address=", t->peer_string.as_string_view()));
    }
    cl_err = grpc_error_add_child(cl_err, error);
    closure->error_data.error = grpc_core::internal::StatusAllocHeapPtr(cl_err);
  }
  if (closure->next_data.scratch < CLOSURE_BARRIER_FIRST_REF_BIT) {
    if ((t->write_state == GRPC_CHTTP2_WRITE_STATE_IDLE) ||
        !(closure->next_data.scratch & CLOSURE_BARRIER_MAY_COVER_WRITE)) {
      // Using GRPC_CLOSURE_SCHED instead of GRPC_CLOSURE_RUN to avoid running
      // closures earlier than when it is safe to do so.
      grpc_error_handle run_error =
          grpc_core::internal::StatusMoveFromHeapPtr(closure->error_data.error);
      closure->error_data.error = 0;
      grpc_core::ExecCtx::Run(DEBUG_LOCATION, closure, run_error);
    } else {
      grpc_closure_list_append(&t->run_after_write, closure);
    }
  }
}

static bool contains_non_ok_status(grpc_metadata_batch* batch) {
  return batch->get(grpc_core::GrpcStatusMetadata()).value_or(GRPC_STATUS_OK) !=
         GRPC_STATUS_OK;
}

static void log_metadata(const grpc_metadata_batch* md_batch, uint32_t id,
                         bool is_client, bool is_initial) {
  VLOG(2) << "--metadata--";
  const std::string prefix = absl::StrCat(
      "HTTP:", id, is_initial ? ":HDR" : ":TRL", is_client ? ":CLI:" : ":SVR:");
  md_batch->Log([&prefix](absl::string_view key, absl::string_view value) {
    VLOG(2) << absl::StrCat(prefix, key, ": ", value);
  });
}

static void perform_stream_op_locked(void* stream_op,
                                     grpc_error_handle /*error_ignored*/) {
  grpc_transport_stream_op_batch* op =
      static_cast<grpc_transport_stream_op_batch*>(stream_op);
  grpc_chttp2_stream* s =
      static_cast<grpc_chttp2_stream*>(op->handler_private.extra_arg);
  grpc_transport_stream_op_batch_payload* op_payload = op->payload;
  grpc_chttp2_transport* t = s->t.get();

  s->traced = op->is_traced;
  s->call_tracer = CallTracerIfSampled(s);
  s->tcp_tracer = TcpTracerIfSampled(s);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_http_trace)) {
    gpr_log(GPR_INFO,
            "perform_stream_op_locked[s=%p; op=%p]: %s; on_complete = %p", s,
            op, grpc_transport_stream_op_batch_string(op, false).c_str(),
            op->on_complete);
    if (op->send_initial_metadata) {
      log_metadata(op_payload->send_initial_metadata.send_initial_metadata,
                   s->id, t->is_client, true);
    }
    if (op->send_trailing_metadata) {
      log_metadata(op_payload->send_trailing_metadata.send_trailing_metadata,
                   s->id, t->is_client, false);
    }
  }

  grpc_closure* on_complete = op->on_complete;
  // on_complete will be null if and only if there are no send ops in the batch.
  if (on_complete != nullptr) {
    // This batch has send ops. Use final_data as a barrier until enqueue time;
    // the initial counter is dropped at the end of this function.
    on_complete->next_data.scratch = CLOSURE_BARRIER_FIRST_REF_BIT;
    on_complete->error_data.error = 0;
  }

  if (op->cancel_stream) {
    grpc_chttp2_cancel_stream(t, s, op_payload->cancel_stream.cancel_error,
                              op_payload->cancel_stream.tarpit);
  }

  if (op->send_initial_metadata) {
    if (s->call_tracer != nullptr) {
      s->call_tracer->RecordAnnotation(
          grpc_core::HttpAnnotation(grpc_core::HttpAnnotation::Type::kStart,
                                    gpr_now(GPR_CLOCK_REALTIME))
              .Add(s->t->flow_control.stats())
              .Add(s->flow_control.stats()));
    }
    if (t->is_client && t->channelz_socket != nullptr) {
      t->channelz_socket->RecordStreamStartedFromLocal();
    }
    CHECK_EQ(s->send_initial_metadata_finished, nullptr);
    on_complete->next_data.scratch |= t->closure_barrier_may_cover_write;

    s->send_initial_metadata_finished = add_closure_barrier(on_complete);
    s->send_initial_metadata =
        op_payload->send_initial_metadata.send_initial_metadata;
    if (t->is_client) {
      s->deadline = std::min(
          s->deadline,
          s->send_initial_metadata->get(grpc_core::GrpcTimeoutMetadata())
              .value_or(grpc_core::Timestamp::InfFuture()));
    }
    if (contains_non_ok_status(s->send_initial_metadata)) {
      s->seen_error = true;
    }
    if (!s->write_closed) {
      if (t->is_client) {
        if (t->closed_with_error.ok()) {
          CHECK_EQ(s->id, 0u);
          grpc_chttp2_list_add_waiting_for_concurrency(t, s);
          maybe_start_some_streams(t);
        } else {
          s->trailing_metadata_buffer.Set(
              grpc_core::GrpcStreamNetworkState(),
              grpc_core::GrpcStreamNetworkState::kNotSentOnWire);
          grpc_chttp2_cancel_stream(
              t, s,
              grpc_error_set_int(
                  GRPC_ERROR_CREATE_REFERENCING("Transport closed",
                                                &t->closed_with_error, 1),
                  grpc_core::StatusIntProperty::kRpcStatus,
                  GRPC_STATUS_UNAVAILABLE),
              false);
        }
      } else {
        CHECK_NE(s->id, 0u);
        grpc_chttp2_mark_stream_writable(t, s);
        if (!(op->send_message &&
              (op->payload->send_message.flags & GRPC_WRITE_BUFFER_HINT))) {
          grpc_chttp2_initiate_write(
              t, GRPC_CHTTP2_INITIATE_WRITE_SEND_INITIAL_METADATA);
        }
      }
    } else {
      s->send_initial_metadata = nullptr;
      grpc_chttp2_complete_closure_step(
          t, &s->send_initial_metadata_finished,
          GRPC_ERROR_CREATE_REFERENCING(
              "Attempt to send initial metadata after stream was closed",
              &s->write_closed_error, 1),
          "send_initial_metadata_finished");
    }
  }

  if (op->send_message) {
    t->num_messages_in_next_write++;
    grpc_core::global_stats().IncrementHttp2SendMessageSize(
        op->payload->send_message.send_message->Length());
    on_complete->next_data.scratch |= t->closure_barrier_may_cover_write;
    s->send_message_finished = add_closure_barrier(op->on_complete);
    const uint32_t flags = op_payload->send_message.flags;
    if (s->write_closed) {
      op->payload->send_message.stream_write_closed = true;
      // We should NOT return an error here, so as to avoid a cancel OP being
      // started. The surface layer will notice that the stream has been closed
      // for writes and fail the send message op.
      grpc_chttp2_complete_closure_step(t, &s->send_message_finished,
                                        absl::OkStatus(),
                                        "fetching_send_message_finished");
    } else {
      uint8_t* frame_hdr = grpc_slice_buffer_tiny_add(
          &s->flow_controlled_buffer, GRPC_HEADER_SIZE_IN_BYTES);
      frame_hdr[0] = (flags & GRPC_WRITE_INTERNAL_COMPRESS) != 0;
      size_t len = op_payload->send_message.send_message->Length();
      frame_hdr[1] = static_cast<uint8_t>(len >> 24);
      frame_hdr[2] = static_cast<uint8_t>(len >> 16);
      frame_hdr[3] = static_cast<uint8_t>(len >> 8);
      frame_hdr[4] = static_cast<uint8_t>(len);

      if (grpc_core::IsHttp2StatsFixEnabled()) {
        s->stats.outgoing.framing_bytes += GRPC_HEADER_SIZE_IN_BYTES;
        s->stats.outgoing.data_bytes +=
            op_payload->send_message.send_message->Length();
      }
      s->next_message_end_offset =
          s->flow_controlled_bytes_written +
          static_cast<int64_t>(s->flow_controlled_buffer.length) +
          static_cast<int64_t>(len);
      if (flags & GRPC_WRITE_BUFFER_HINT) {
        s->next_message_end_offset -= t->write_buffer_size;
        s->write_buffering = true;
      } else {
        s->write_buffering = false;
      }

      grpc_slice* const slices =
          op_payload->send_message.send_message->c_slice_buffer()->slices;
      grpc_slice* const end =
          slices + op_payload->send_message.send_message->Count();
      for (grpc_slice* slice = slices; slice != end; slice++) {
        grpc_slice_buffer_add(&s->flow_controlled_buffer,
                              grpc_core::CSliceRef(*slice));
      }

      int64_t notify_offset = s->next_message_end_offset;
      if (notify_offset <= s->flow_controlled_bytes_written) {
        grpc_chttp2_complete_closure_step(t, &s->send_message_finished,
                                          absl::OkStatus(),
                                          "fetching_send_message_finished");
      } else {
        grpc_chttp2_write_cb* cb = t->write_cb_pool;
        if (cb == nullptr) {
          cb = static_cast<grpc_chttp2_write_cb*>(gpr_malloc(sizeof(*cb)));
        } else {
          t->write_cb_pool = cb->next;
        }
        cb->call_at_byte = notify_offset;
        cb->closure = s->send_message_finished;
        s->send_message_finished = nullptr;
        grpc_chttp2_write_cb** list = flags & GRPC_WRITE_THROUGH
                                          ? &s->on_write_finished_cbs
                                          : &s->on_flow_controlled_cbs;
        cb->next = *list;
        *list = cb;
      }

      if (s->id != 0 &&
          (!s->write_buffering ||
           s->flow_controlled_buffer.length > t->write_buffer_size)) {
        grpc_chttp2_mark_stream_writable(t, s);
        grpc_chttp2_initiate_write(t, GRPC_CHTTP2_INITIATE_WRITE_SEND_MESSAGE);
      }
    }
  }

  if (op->send_trailing_metadata) {
    CHECK_EQ(s->send_trailing_metadata_finished, nullptr);
    on_complete->next_data.scratch |= t->closure_barrier_may_cover_write;
    s->send_trailing_metadata_finished = add_closure_barrier(on_complete);
    s->send_trailing_metadata =
        op_payload->send_trailing_metadata.send_trailing_metadata;
    s->sent_trailing_metadata_op = op_payload->send_trailing_metadata.sent;
    s->write_buffering = false;
    if (contains_non_ok_status(s->send_trailing_metadata)) {
      s->seen_error = true;
    }
    if (s->write_closed) {
      s->send_trailing_metadata = nullptr;
      s->sent_trailing_metadata_op = nullptr;
      grpc_chttp2_complete_closure_step(
          t, &s->send_trailing_metadata_finished,
          op->payload->send_trailing_metadata.send_trailing_metadata->empty()
              ? absl::OkStatus()
              : GRPC_ERROR_CREATE("Attempt to send trailing metadata after "
                                  "stream was closed"),
          "send_trailing_metadata_finished");
    } else if (s->id != 0) {
      // TODO(ctiller): check if there's flow control for any outstanding
      //   bytes before going writable
      grpc_chttp2_mark_stream_writable(t, s);
      grpc_chttp2_initiate_write(
          t, GRPC_CHTTP2_INITIATE_WRITE_SEND_TRAILING_METADATA);
    }
  }

  if (op->recv_initial_metadata) {
    CHECK_EQ(s->recv_initial_metadata_ready, nullptr);
    s->recv_initial_metadata_ready =
        op_payload->recv_initial_metadata.recv_initial_metadata_ready;
    s->recv_initial_metadata =
        op_payload->recv_initial_metadata.recv_initial_metadata;
    s->trailing_metadata_available =
        op_payload->recv_initial_metadata.trailing_metadata_available;
    if (s->parsed_trailers_only && s->trailing_metadata_available != nullptr) {
      *s->trailing_metadata_available = true;
    }
    grpc_chttp2_maybe_complete_recv_initial_metadata(t, s);
  }

  if (op->recv_message) {
    CHECK_EQ(s->recv_message_ready, nullptr);
    s->recv_message_ready = op_payload->recv_message.recv_message_ready;
    s->recv_message = op_payload->recv_message.recv_message;
    s->recv_message->emplace();
    s->recv_message_flags = op_payload->recv_message.flags;
    s->call_failed_before_recv_message =
        op_payload->recv_message.call_failed_before_recv_message;
    grpc_chttp2_maybe_complete_recv_trailing_metadata(t, s);
  }

  if (op->recv_trailing_metadata) {
    CHECK_EQ(s->collecting_stats, nullptr);
    s->collecting_stats = op_payload->recv_trailing_metadata.collect_stats;
    CHECK_EQ(s->recv_trailing_metadata_finished, nullptr);
    s->recv_trailing_metadata_finished =
        op_payload->recv_trailing_metadata.recv_trailing_metadata_ready;
    s->recv_trailing_metadata =
        op_payload->recv_trailing_metadata.recv_trailing_metadata;
    s->final_metadata_requested = true;
    grpc_chttp2_maybe_complete_recv_trailing_metadata(t, s);
  }

  if (on_complete != nullptr) {
    grpc_chttp2_complete_closure_step(t, &on_complete, absl::OkStatus(),
                                      "op->on_complete");
  }

  GRPC_CHTTP2_STREAM_UNREF(s, "perform_stream_op");
}

void grpc_chttp2_transport::PerformStreamOp(
    grpc_stream* gs, grpc_transport_stream_op_batch* op) {
  grpc_chttp2_stream* s = reinterpret_cast<grpc_chttp2_stream*>(gs);

  if (!is_client) {
    if (op->send_initial_metadata) {
      CHECK(!op->payload->send_initial_metadata.send_initial_metadata
                 ->get(grpc_core::GrpcTimeoutMetadata())
                 .has_value());
    }
    if (op->send_trailing_metadata) {
      CHECK(!op->payload->send_trailing_metadata.send_trailing_metadata
                 ->get(grpc_core::GrpcTimeoutMetadata())
                 .has_value());
    }
  }

  if (GRPC_TRACE_FLAG_ENABLED(grpc_http_trace)) {
    gpr_log(GPR_INFO, "perform_stream_op[s=%p; op=%p]: %s", s, op,
            grpc_transport_stream_op_batch_string(op, false).c_str());
  }

  GRPC_CHTTP2_STREAM_REF(s, "perform_stream_op");
  op->handler_private.extra_arg = gs;
  combiner->Run(GRPC_CLOSURE_INIT(&op->handler_private.closure,
                                  perform_stream_op_locked, op, nullptr),
                absl::OkStatus());
}

static void cancel_pings(grpc_chttp2_transport* t, grpc_error_handle error) {
  GRPC_CHTTP2_IF_TRACING(gpr_log(GPR_INFO, "%p CANCEL PINGS: %s", t,
                                 grpc_core::StatusToString(error).c_str()));
  // callback remaining pings: they're not allowed to call into the transport,
  //   and maybe they hold resources that need to be freed
  t->ping_callbacks.CancelAll(t->event_engine.get());
}

namespace {
class PingClosureWrapper {
 public:
  explicit PingClosureWrapper(grpc_closure* closure) : closure_(closure) {}
  PingClosureWrapper(const PingClosureWrapper&) = delete;
  PingClosureWrapper& operator=(const PingClosureWrapper&) = delete;
  PingClosureWrapper(PingClosureWrapper&& other) noexcept
      : closure_(other.Take()) {}
  PingClosureWrapper& operator=(PingClosureWrapper&& other) noexcept {
    std::swap(closure_, other.closure_);
    return *this;
  }
  ~PingClosureWrapper() {
    if (closure_ != nullptr) {
      grpc_core::ExecCtx::Run(DEBUG_LOCATION, closure_, absl::CancelledError());
    }
  }

  void operator()() {
    grpc_core::ExecCtx::Run(DEBUG_LOCATION, Take(), absl::OkStatus());
  }

 private:
  grpc_closure* Take() { return std::exchange(closure_, nullptr); }

  grpc_closure* closure_ = nullptr;
};
}  // namespace

static void send_ping_locked(grpc_chttp2_transport* t,
                             grpc_closure* on_initiate, grpc_closure* on_ack) {
  if (!t->closed_with_error.ok()) {
    grpc_core::ExecCtx::Run(DEBUG_LOCATION, on_initiate, t->closed_with_error);
    grpc_core::ExecCtx::Run(DEBUG_LOCATION, on_ack, t->closed_with_error);
    return;
  }
  t->ping_callbacks.OnPing(PingClosureWrapper(on_initiate),
                           PingClosureWrapper(on_ack));
}

// Specialized form of send_ping_locked for keepalive ping. If there is already
// a ping in progress, the keepalive ping would piggyback onto that ping,
// instead of waiting for that ping to complete and then starting a new ping.
static void send_keepalive_ping_locked(
    grpc_core::RefCountedPtr<grpc_chttp2_transport> t) {
  if (!t->closed_with_error.ok()) {
    t->combiner->Run(
        grpc_core::InitTransportClosure<finish_keepalive_ping_locked>(
            t->Ref(), &t->finish_keepalive_ping_locked),
        t->closed_with_error);
    return;
  }
  t->ping_callbacks.OnPingAck(
      PingClosureWrapper(grpc_core::InitTransportClosure<finish_keepalive_ping>(
          t->Ref(), &t->finish_keepalive_ping_locked)));
}

void grpc_chttp2_retry_initiate_ping(
    grpc_core::RefCountedPtr<grpc_chttp2_transport> t) {
  auto tp = t.get();
  tp->combiner->Run(grpc_core::InitTransportClosure<retry_initiate_ping_locked>(
                        std::move(t), &tp->retry_initiate_ping_locked),
                    absl::OkStatus());
}

static void retry_initiate_ping_locked(
    grpc_core::RefCountedPtr<grpc_chttp2_transport> t,
    GRPC_UNUSED grpc_error_handle error) {
  DCHECK(error.ok());
  CHECK(t->delayed_ping_timer_handle != TaskHandle::kInvalid);
  t->delayed_ping_timer_handle = TaskHandle::kInvalid;
  grpc_chttp2_initiate_write(t.get(),
                             GRPC_CHTTP2_INITIATE_WRITE_RETRY_SEND_PING);
}

void grpc_chttp2_ack_ping(grpc_chttp2_transport* t, uint64_t id) {
  if (!t->ping_callbacks.AckPing(id, t->event_engine.get())) {
    gpr_log(GPR_DEBUG, "Unknown ping response from %s: %" PRIx64,
            std::string(t->peer_string.as_string_view()).c_str(), id);
    return;
  }
  if (t->ping_callbacks.ping_requested()) {
    grpc_chttp2_initiate_write(t, GRPC_CHTTP2_INITIATE_WRITE_CONTINUE_PINGS);
  }
}

void grpc_chttp2_keepalive_timeout(
    grpc_core::RefCountedPtr<grpc_chttp2_transport> t) {
  t->combiner->Run(
      grpc_core::NewClosure([t](grpc_error_handle) {
        gpr_log(GPR_INFO, "%s: Keepalive timeout. Closing transport.",
                std::string(t->peer_string.as_string_view()).c_str());
        send_goaway(
            t.get(),
            grpc_error_set_int(GRPC_ERROR_CREATE("keepalive_timeout"),
                               grpc_core::StatusIntProperty::kHttp2Error,
                               GRPC_HTTP2_ENHANCE_YOUR_CALM),
            /*immediate_disconnect_hint=*/true);
        close_transport_locked(
            t.get(),
            grpc_error_set_int(GRPC_ERROR_CREATE("keepalive timeout"),
                               grpc_core::StatusIntProperty::kRpcStatus,
                               GRPC_STATUS_UNAVAILABLE));
      }),
      absl::OkStatus());
}

void grpc_chttp2_ping_timeout(
    grpc_core::RefCountedPtr<grpc_chttp2_transport> t) {
  t->combiner->Run(
      grpc_core::NewClosure([t](grpc_error_handle) {
        gpr_log(GPR_INFO, "%s: Ping timeout. Closing transport.",
                std::string(t->peer_string.as_string_view()).c_str());
        send_goaway(
            t.get(),
            grpc_error_set_int(GRPC_ERROR_CREATE("ping_timeout"),
                               grpc_core::StatusIntProperty::kHttp2Error,
                               GRPC_HTTP2_ENHANCE_YOUR_CALM),
            /*immediate_disconnect_hint=*/true);
        close_transport_locked(
            t.get(),
            grpc_error_set_int(GRPC_ERROR_CREATE("ping timeout"),
                               grpc_core::StatusIntProperty::kRpcStatus,
                               GRPC_STATUS_UNAVAILABLE));
      }),
      absl::OkStatus());
}

void grpc_chttp2_settings_timeout(
    grpc_core::RefCountedPtr<grpc_chttp2_transport> t) {
  t->combiner->Run(
      grpc_core::NewClosure([t](grpc_error_handle) {
        gpr_log(GPR_INFO, "%s: Settings timeout. Closing transport.",
                std::string(t->peer_string.as_string_view()).c_str());
        send_goaway(
            t.get(),
            grpc_error_set_int(GRPC_ERROR_CREATE("settings_timeout"),
                               grpc_core::StatusIntProperty::kHttp2Error,
                               GRPC_HTTP2_SETTINGS_TIMEOUT),
            /*immediate_disconnect_hint=*/true);
        close_transport_locked(
            t.get(),
            grpc_error_set_int(GRPC_ERROR_CREATE("settings timeout"),
                               grpc_core::StatusIntProperty::kRpcStatus,
                               GRPC_STATUS_UNAVAILABLE));
      }),
      absl::OkStatus());
}

namespace {

// Fire and forget (deletes itself on completion). Does a graceful shutdown by
// sending a GOAWAY frame with the last stream id set to 2^31-1, sending a ping
// and waiting for an ack (effective waiting for an RTT) and then sending a
// final GOAWAY frame with an updated last stream identifier. This helps ensure
// that a connection can be cleanly shut down without losing requests.
// In the event, that the client does not respond to the ping for some reason,
// we add a 20 second deadline, after which we send the second goaway.
class GracefulGoaway : public grpc_core::RefCounted<GracefulGoaway> {
 public:
  static void Start(grpc_chttp2_transport* t) { new GracefulGoaway(t); }

 private:
  using TaskHandle = ::grpc_event_engine::experimental::EventEngine::TaskHandle;

  explicit GracefulGoaway(grpc_chttp2_transport* t) : t_(t->Ref()) {
    t->sent_goaway_state = GRPC_CHTTP2_GRACEFUL_GOAWAY;
    grpc_chttp2_goaway_append((1u << 31) - 1, 0, grpc_empty_slice(), &t->qbuf);
    t->keepalive_timeout =
        std::min(t->keepalive_timeout, grpc_core::Duration::Seconds(20));
    t->ping_timeout =
        std::min(t->ping_timeout, grpc_core::Duration::Seconds(20));
    send_ping_locked(
        t, nullptr, GRPC_CLOSURE_INIT(&on_ping_ack_, OnPingAck, this, nullptr));
    grpc_chttp2_initiate_write(t, GRPC_CHTTP2_INITIATE_WRITE_GOAWAY_SENT);
  }

  void MaybeSendFinalGoawayLocked() {
    if (t_->sent_goaway_state != GRPC_CHTTP2_GRACEFUL_GOAWAY) {
      // We already sent the final GOAWAY.
      return;
    }
    if (t_->destroying || !t_->closed_with_error.ok()) {
      GRPC_CHTTP2_IF_TRACING(
          gpr_log(GPR_INFO,
                  "transport:%p %s peer:%s Transport already shutting down. "
                  "Graceful GOAWAY abandoned.",
                  t_.get(), t_->is_client ? "CLIENT" : "SERVER",
                  std::string(t_->peer_string.as_string_view()).c_str()));
      return;
    }
    // Ping completed. Send final goaway.
    GRPC_CHTTP2_IF_TRACING(
        gpr_log(GPR_INFO,
                "transport:%p %s peer:%s Graceful shutdown: Ping received. "
                "Sending final GOAWAY with stream_id:%d",
                t_.get(), t_->is_client ? "CLIENT" : "SERVER",
                std::string(t_->peer_string.as_string_view()).c_str(),
                t_->last_new_stream_id));
    t_->sent_goaway_state = GRPC_CHTTP2_FINAL_GOAWAY_SEND_SCHEDULED;
    grpc_chttp2_goaway_append(t_->last_new_stream_id, 0, grpc_empty_slice(),
                              &t_->qbuf);
    grpc_chttp2_initiate_write(t_.get(),
                               GRPC_CHTTP2_INITIATE_WRITE_GOAWAY_SENT);
  }

  static void OnPingAck(void* arg, grpc_error_handle /* error */) {
    auto* self = static_cast<GracefulGoaway*>(arg);
    self->t_->combiner->Run(
        GRPC_CLOSURE_INIT(&self->on_ping_ack_, OnPingAckLocked, self, nullptr),
        absl::OkStatus());
  }

  static void OnPingAckLocked(void* arg, grpc_error_handle /* error */) {
    auto* self = static_cast<GracefulGoaway*>(arg);
    self->MaybeSendFinalGoawayLocked();
    self->Unref();
  }

  const grpc_core::RefCountedPtr<grpc_chttp2_transport> t_;
  grpc_closure on_ping_ack_;
};

}  // namespace

static void send_goaway(grpc_chttp2_transport* t, grpc_error_handle error,
                        bool immediate_disconnect_hint) {
  grpc_http2_error_code http_error;
  std::string message;
  grpc_error_get_status(error, grpc_core::Timestamp::InfFuture(), nullptr,
                        &message, &http_error, nullptr);
  if (!t->is_client && http_error == GRPC_HTTP2_NO_ERROR &&
      !immediate_disconnect_hint) {
    // Do a graceful shutdown.
    if (t->sent_goaway_state == GRPC_CHTTP2_NO_GOAWAY_SEND) {
      GracefulGoaway::Start(t);
    } else {
      // Graceful GOAWAY is already in progress.
    }
  } else if (t->sent_goaway_state == GRPC_CHTTP2_NO_GOAWAY_SEND ||
             t->sent_goaway_state == GRPC_CHTTP2_GRACEFUL_GOAWAY) {
    // We want to log this irrespective of whether http tracing is enabled
    gpr_log(GPR_DEBUG, "%s %s: Sending goaway last_new_stream_id=%d err=%s",
            std::string(t->peer_string.as_string_view()).c_str(),
            t->is_client ? "CLIENT" : "SERVER", t->last_new_stream_id,
            grpc_core::StatusToString(error).c_str());
    t->sent_goaway_state = GRPC_CHTTP2_FINAL_GOAWAY_SEND_SCHEDULED;
    grpc_chttp2_goaway_append(
        t->last_new_stream_id, static_cast<uint32_t>(http_error),
        grpc_slice_from_cpp_string(std::move(message)), &t->qbuf);
  } else {
    // Final GOAWAY has already been sent.
  }
  grpc_chttp2_initiate_write(t, GRPC_CHTTP2_INITIATE_WRITE_GOAWAY_SENT);
}

void grpc_chttp2_exceeded_ping_strikes(grpc_chttp2_transport* t) {
  send_goaway(t,
              grpc_error_set_int(GRPC_ERROR_CREATE("too_many_pings"),
                                 grpc_core::StatusIntProperty::kHttp2Error,
                                 GRPC_HTTP2_ENHANCE_YOUR_CALM),
              /*immediate_disconnect_hint=*/true);
  // The transport will be closed after the write is done
  close_transport_locked(
      t, grpc_error_set_int(GRPC_ERROR_CREATE("Too many pings"),
                            grpc_core::StatusIntProperty::kRpcStatus,
                            GRPC_STATUS_UNAVAILABLE));
}

void grpc_chttp2_reset_ping_clock(grpc_chttp2_transport* t) {
  if (!t->is_client) {
    t->ping_abuse_policy.ResetPingStrikes();
  }
  t->ping_rate_policy.ResetPingsBeforeDataRequired();
}

static void perform_transport_op_locked(void* stream_op,
                                        grpc_error_handle /*error_ignored*/) {
  grpc_transport_op* op = static_cast<grpc_transport_op*>(stream_op);
  grpc_core::RefCountedPtr<grpc_chttp2_transport> t(
      static_cast<grpc_chttp2_transport*>(op->handler_private.extra_arg));

  if (!op->goaway_error.ok()) {
    send_goaway(t.get(), op->goaway_error, /*immediate_disconnect_hint=*/false);
  }

  if (op->set_accept_stream) {
    t->accept_stream_cb = op->set_accept_stream_fn;
    t->accept_stream_cb_user_data = op->set_accept_stream_user_data;
    t->registered_method_matcher_cb = op->set_registered_method_matcher_fn;
  }

  if (op->bind_pollset) {
    grpc_endpoint_add_to_pollset(t->ep, op->bind_pollset);
  }

  if (op->bind_pollset_set) {
    grpc_endpoint_add_to_pollset_set(t->ep, op->bind_pollset_set);
  }

  if (op->send_ping.on_initiate != nullptr || op->send_ping.on_ack != nullptr) {
    send_ping_locked(t.get(), op->send_ping.on_initiate, op->send_ping.on_ack);
    grpc_chttp2_initiate_write(t.get(),
                               GRPC_CHTTP2_INITIATE_WRITE_APPLICATION_PING);
  }

  if (op->start_connectivity_watch != nullptr) {
    t->state_tracker.AddWatcher(op->start_connectivity_watch_state,
                                std::move(op->start_connectivity_watch));
  }
  if (op->stop_connectivity_watch != nullptr) {
    t->state_tracker.RemoveWatcher(op->stop_connectivity_watch);
  }

  if (!op->disconnect_with_error.ok()) {
    send_goaway(t.get(), op->disconnect_with_error,
                /*immediate_disconnect_hint=*/true);
    close_transport_locked(t.get(), op->disconnect_with_error);
  }

  grpc_core::ExecCtx::Run(DEBUG_LOCATION, op->on_consumed, absl::OkStatus());
}

void grpc_chttp2_transport::PerformOp(grpc_transport_op* op) {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_http_trace)) {
    gpr_log(GPR_INFO, "perform_transport_op[t=%p]: %s", this,
            grpc_transport_op_string(op).c_str());
  }
  op->handler_private.extra_arg = this;
  Ref().release()->combiner->Run(
      GRPC_CLOSURE_INIT(&op->handler_private.closure,
                        perform_transport_op_locked, op, nullptr),
      absl::OkStatus());
}

//
// INPUT PROCESSING - GENERAL
//

void grpc_chttp2_maybe_complete_recv_initial_metadata(grpc_chttp2_transport* t,
                                                      grpc_chttp2_stream* s) {
  if (s->recv_initial_metadata_ready != nullptr &&
      s->published_metadata[0] != GRPC_METADATA_NOT_PUBLISHED) {
    if (s->seen_error) {
      grpc_slice_buffer_reset_and_unref(&s->frame_storage);
    }
    *s->recv_initial_metadata = std::move(s->initial_metadata_buffer);
    s->recv_initial_metadata->Set(grpc_core::PeerString(),
                                  t->peer_string.Ref());
    // If we didn't receive initial metadata from the wire and instead faked a
    // status (due to stream cancellations for example), let upper layers know
    // that trailing metadata is immediately available.
    if (s->trailing_metadata_available != nullptr &&
        s->published_metadata[0] != GRPC_METADATA_PUBLISHED_FROM_WIRE &&
        s->published_metadata[1] == GRPC_METADATA_SYNTHESIZED_FROM_FAKE) {
      *s->trailing_metadata_available = true;
      s->trailing_metadata_available = nullptr;
    }
    if (t->registered_method_matcher_cb != nullptr) {
      t->registered_method_matcher_cb(t->accept_stream_cb_user_data,
                                      s->recv_initial_metadata);
    }
    null_then_sched_closure(&s->recv_initial_metadata_ready);
  }
}

void grpc_chttp2_maybe_complete_recv_message(grpc_chttp2_transport* t,
                                             grpc_chttp2_stream* s) {
  if (s->recv_message_ready == nullptr) return;

  grpc_core::chttp2::StreamFlowControl::IncomingUpdateContext upd(
      &s->flow_control);
  grpc_error_handle error;

  // Lambda is immediately invoked as a big scoped section that can be
  // exited out of at any point by returning.
  [&]() {
    if (grpc_http_trace.enabled()) {
      gpr_log(GPR_DEBUG,
              "maybe_complete_recv_message %p final_metadata_requested=%d "
              "seen_error=%d",
              s, s->final_metadata_requested, s->seen_error);
    }
    if (s->final_metadata_requested && s->seen_error) {
      grpc_slice_buffer_reset_and_unref(&s->frame_storage);
      s->recv_message->reset();
    } else {
      if (s->frame_storage.length != 0) {
        while (true) {
          CHECK_GT(s->frame_storage.length, 0u);
          int64_t min_progress_size;
          auto r = grpc_deframe_unprocessed_incoming_frames(
              s, &min_progress_size, &**s->recv_message, s->recv_message_flags);
          if (grpc_http_trace.enabled()) {
            gpr_log(GPR_DEBUG, "Deframe data frame: %s",
                    grpc_core::PollToString(r, [](absl::Status r) {
                      return r.ToString();
                    }).c_str());
          }
          if (r.pending()) {
            if (s->read_closed) {
              grpc_slice_buffer_reset_and_unref(&s->frame_storage);
              s->recv_message->reset();
              break;
            } else {
              upd.SetMinProgressSize(min_progress_size);
              return;  // Out of lambda to enclosing function
            }
          } else {
            error = std::move(r.value());
            if (!error.ok()) {
              s->seen_error = true;
              grpc_slice_buffer_reset_and_unref(&s->frame_storage);
              break;
            } else {
              if (t->channelz_socket != nullptr) {
                t->channelz_socket->RecordMessageReceived();
              }
              break;
            }
          }
        }
      } else if (s->read_closed) {
        s->recv_message->reset();
      } else {
        upd.SetMinProgressSize(GRPC_HEADER_SIZE_IN_BYTES);
        return;  // Out of lambda to enclosing function
      }
    }
    // save the length of the buffer before handing control back to application
    // threads. Needed to support correct flow control bookkeeping
    if (error.ok() && s->recv_message->has_value()) {
      null_then_sched_closure(&s->recv_message_ready);
    } else if (s->published_metadata[1] != GRPC_METADATA_NOT_PUBLISHED) {
      if (s->call_failed_before_recv_message != nullptr) {
        *s->call_failed_before_recv_message =
            (s->published_metadata[1] != GRPC_METADATA_PUBLISHED_AT_CLOSE);
      }
      null_then_sched_closure(&s->recv_message_ready);
    }
  }();

  upd.SetPendingSize(s->frame_storage.length);
  grpc_chttp2_act_on_flowctl_action(upd.MakeAction(), t, s);
}

void grpc_chttp2_maybe_complete_recv_trailing_metadata(grpc_chttp2_transport* t,
                                                       grpc_chttp2_stream* s) {
  grpc_chttp2_maybe_complete_recv_message(t, s);
  if (grpc_http_trace.enabled()) {
    gpr_log(GPR_DEBUG,
            "maybe_complete_recv_trailing_metadata cli=%d s=%p closure=%p "
            "read_closed=%d "
            "write_closed=%d %" PRIdPTR,
            t->is_client, s, s->recv_trailing_metadata_finished, s->read_closed,
            s->write_closed, s->frame_storage.length);
  }
  if (s->recv_trailing_metadata_finished != nullptr && s->read_closed &&
      s->write_closed) {
    if (s->seen_error || !t->is_client) {
      grpc_slice_buffer_reset_and_unref(&s->frame_storage);
    }
    if (s->read_closed && s->frame_storage.length == 0 &&
        s->recv_trailing_metadata_finished != nullptr) {
      grpc_transport_move_stats(&s->stats, s->collecting_stats);
      s->collecting_stats = nullptr;
      *s->recv_trailing_metadata = std::move(s->trailing_metadata_buffer);
      null_then_sched_closure(&s->recv_trailing_metadata_finished);
    }
  }
}

static grpc_chttp2_transport::RemovedStreamHandle remove_stream(
    grpc_chttp2_transport* t, uint32_t id, grpc_error_handle error) {
  grpc_chttp2_stream* s = t->stream_map.extract(id).mapped();
  DCHECK(s);
  if (t->incoming_stream == s) {
    t->incoming_stream = nullptr;
    grpc_chttp2_parsing_become_skip_parser(t);
  }

  if (t->stream_map.empty()) {
    post_benign_reclaimer(t);
    if (t->sent_goaway_state == GRPC_CHTTP2_FINAL_GOAWAY_SENT) {
      close_transport_locked(
          t, GRPC_ERROR_CREATE_REFERENCING(
                 "Last stream closed after sending GOAWAY", &error, 1));
    }
  }
  if (grpc_chttp2_list_remove_writable_stream(t, s)) {
    GRPC_CHTTP2_STREAM_UNREF(s, "chttp2_writing:remove_stream");
  }
  grpc_chttp2_list_remove_stalled_by_stream(t, s);
  grpc_chttp2_list_remove_stalled_by_transport(t, s);

  maybe_start_some_streams(t);

  if (t->is_client) return grpc_chttp2_transport::RemovedStreamHandle();
  return grpc_chttp2_transport::RemovedStreamHandle(t->Ref());
}

namespace grpc_core {
namespace {

Duration TarpitDuration(grpc_chttp2_transport* t) {
  return Duration::Milliseconds(absl::LogUniform<int>(
      absl::BitGen(), t->min_tarpit_duration_ms, t->max_tarpit_duration_ms));
}

template <typename F>
void MaybeTarpit(grpc_chttp2_transport* t, bool tarpit, F fn) {
  if (!tarpit || !t->allow_tarpit || t->is_client) {
    fn(t);
    return;
  }
  const auto duration = TarpitDuration(t);
  t->event_engine->RunAfter(
      duration, [t = t->Ref(), fn = std::move(fn)]() mutable {
        ApplicationCallbackExecCtx app_exec_ctx;
        ExecCtx exec_ctx;
        t->combiner->Run(
            NewClosure([t, fn = std::move(fn)](grpc_error_handle) mutable {
              // TODO(ctiller): this can result in not sending RST_STREAMS if a
              // request gets tarpit behind a transport close.
              if (!t->closed_with_error.ok()) return;
              fn(t.get());
            }),
            absl::OkStatus());
      });
}

}  // namespace
}  // namespace grpc_core

void grpc_chttp2_cancel_stream(grpc_chttp2_transport* t, grpc_chttp2_stream* s,
                               grpc_error_handle due_to_error, bool tarpit) {
  if (!t->is_client && !s->sent_trailing_metadata &&
      grpc_error_has_clear_grpc_status(due_to_error) &&
      !(s->read_closed && s->write_closed)) {
    close_from_api(t, s, due_to_error, tarpit);
    return;
  }

  if (!due_to_error.ok() && !s->seen_error) {
    s->seen_error = true;
  }
  if (!s->read_closed || !s->write_closed) {
    if (s->id != 0) {
      grpc_http2_error_code http_error;
      grpc_error_get_status(due_to_error, s->deadline, nullptr, nullptr,
                            &http_error, nullptr);
      grpc_core::MaybeTarpit(
          t, tarpit,
          [id = s->id, http_error,
           remove_stream_handle = grpc_chttp2_mark_stream_closed(
               t, s, 1, 1, due_to_error)](grpc_chttp2_transport* t) {
            grpc_chttp2_add_rst_stream_to_next_write(
                t, id, static_cast<uint32_t>(http_error), nullptr);
            grpc_chttp2_initiate_write(t,
                                       GRPC_CHTTP2_INITIATE_WRITE_RST_STREAM);
          });
      return;
    }
  }
  grpc_chttp2_mark_stream_closed(t, s, 1, 1, due_to_error);
}

void grpc_chttp2_fake_status(grpc_chttp2_transport* t, grpc_chttp2_stream* s,
                             grpc_error_handle error) {
  grpc_status_code status;
  std::string message;
  grpc_error_get_status(error, s->deadline, &status, &message, nullptr,
                        nullptr);
  if (status != GRPC_STATUS_OK) {
    s->seen_error = true;
  }
  // stream_global->recv_trailing_metadata_finished gives us a
  //   last chance replacement: we've received trailing metadata,
  //   but something more important has become available to signal
  //   to the upper layers - drop what we've got, and then publish
  //   what we want - which is safe because we haven't told anyone
  //   about the metadata yet
  if (s->published_metadata[1] == GRPC_METADATA_NOT_PUBLISHED ||
      s->recv_trailing_metadata_finished != nullptr ||
      !s->final_metadata_requested) {
    s->trailing_metadata_buffer.Set(grpc_core::GrpcStatusMetadata(), status);
    if (!message.empty()) {
      s->trailing_metadata_buffer.Set(
          grpc_core::GrpcMessageMetadata(),
          grpc_core::Slice::FromCopiedBuffer(message));
    }
    s->published_metadata[1] = GRPC_METADATA_SYNTHESIZED_FROM_FAKE;
    grpc_chttp2_maybe_complete_recv_trailing_metadata(t, s);
  }
}

static void add_error(grpc_error_handle error, grpc_error_handle* refs,
                      size_t* nrefs) {
  if (error.ok()) return;
  for (size_t i = 0; i < *nrefs; i++) {
    if (error == refs[i]) {
      return;
    }
  }
  refs[*nrefs] = error;
  ++*nrefs;
}

static grpc_error_handle removal_error(grpc_error_handle extra_error,
                                       grpc_chttp2_stream* s,
                                       const char* main_error_msg) {
  grpc_error_handle refs[3];
  size_t nrefs = 0;
  add_error(s->read_closed_error, refs, &nrefs);
  add_error(s->write_closed_error, refs, &nrefs);
  add_error(extra_error, refs, &nrefs);
  grpc_error_handle error;
  if (nrefs > 0) {
    error = GRPC_ERROR_CREATE_REFERENCING(main_error_msg, refs, nrefs);
  }
  return error;
}

static void flush_write_list(grpc_chttp2_transport* t,
                             grpc_chttp2_write_cb** list,
                             grpc_error_handle error) {
  while (*list) {
    grpc_chttp2_write_cb* cb = *list;
    *list = cb->next;
    grpc_chttp2_complete_closure_step(t, &cb->closure, error,
                                      "on_write_finished_cb");
    cb->next = t->write_cb_pool;
    t->write_cb_pool = cb;
  }
}

void grpc_chttp2_fail_pending_writes(grpc_chttp2_transport* t,
                                     grpc_chttp2_stream* s,
                                     grpc_error_handle error) {
  error =
      removal_error(error, s, "Pending writes failed due to stream closure");
  s->send_initial_metadata = nullptr;
  grpc_chttp2_complete_closure_step(t, &s->send_initial_metadata_finished,
                                    error, "send_initial_metadata_finished");

  s->send_trailing_metadata = nullptr;
  s->sent_trailing_metadata_op = nullptr;
  grpc_chttp2_complete_closure_step(t, &s->send_trailing_metadata_finished,
                                    error, "send_trailing_metadata_finished");

  grpc_chttp2_complete_closure_step(t, &s->send_message_finished, error,
                                    "fetching_send_message_finished");
  flush_write_list(t, &s->on_write_finished_cbs, error);
  flush_write_list(t, &s->on_flow_controlled_cbs, error);
}

grpc_chttp2_transport::RemovedStreamHandle grpc_chttp2_mark_stream_closed(
    grpc_chttp2_transport* t, grpc_chttp2_stream* s, int close_reads,
    int close_writes, grpc_error_handle error) {
  grpc_chttp2_transport::RemovedStreamHandle rsh;
  if (grpc_http_trace.enabled()) {
    gpr_log(
        GPR_DEBUG, "MARK_STREAM_CLOSED: t=%p s=%p(id=%d) %s [%s]", t, s, s->id,
        (close_reads && close_writes)
            ? "read+write"
            : (close_reads ? "read" : (close_writes ? "write" : "nothing??")),
        grpc_core::StatusToString(error).c_str());
  }
  if (s->read_closed && s->write_closed) {
    // already closed, but we should still fake the status if needed.
    grpc_error_handle overall_error = removal_error(error, s, "Stream removed");
    if (!overall_error.ok()) {
      grpc_chttp2_fake_status(t, s, overall_error);
    }
    grpc_chttp2_maybe_complete_recv_trailing_metadata(t, s);
    return rsh;
  }
  bool closed_read = false;
  bool became_closed = false;
  if (close_reads && !s->read_closed) {
    s->read_closed_error = error;
    s->read_closed = true;
    closed_read = true;
  }
  if (close_writes && !s->write_closed) {
    s->write_closed_error = error;
    s->write_closed = true;
    grpc_chttp2_fail_pending_writes(t, s, error);
  }
  if (s->read_closed && s->write_closed) {
    became_closed = true;
    grpc_error_handle overall_error = removal_error(error, s, "Stream removed");
    if (s->id != 0) {
      rsh = remove_stream(t, s->id, overall_error);
    } else {
      // Purge streams waiting on concurrency still waiting for id assignment
      grpc_chttp2_list_remove_waiting_for_concurrency(t, s);
    }
    if (!overall_error.ok()) {
      grpc_chttp2_fake_status(t, s, overall_error);
    }
  }
  if (closed_read) {
    for (int i = 0; i < 2; i++) {
      if (s->published_metadata[i] == GRPC_METADATA_NOT_PUBLISHED) {
        s->published_metadata[i] = GRPC_METADATA_PUBLISHED_AT_CLOSE;
      }
    }
    grpc_chttp2_maybe_complete_recv_initial_metadata(t, s);
    grpc_chttp2_maybe_complete_recv_message(t, s);
  }
  if (became_closed) {
    s->stats.latency =
        gpr_time_sub(gpr_now(GPR_CLOCK_MONOTONIC), s->creation_time);
    grpc_chttp2_maybe_complete_recv_trailing_metadata(t, s);
    GRPC_CHTTP2_STREAM_UNREF(s, "chttp2");
  }
  return rsh;
}

static void close_from_api(grpc_chttp2_transport* t, grpc_chttp2_stream* s,
                           grpc_error_handle error, bool tarpit) {
  grpc_status_code grpc_status;
  std::string message;
  grpc_error_get_status(error, s->deadline, &grpc_status, &message, nullptr,
                        nullptr);

  CHECK_GE(grpc_status, 0);
  CHECK_LT((int)grpc_status, 100);

  auto remove_stream_handle = grpc_chttp2_mark_stream_closed(t, s, 1, 1, error);
  grpc_core::MaybeTarpit(
      t, tarpit,
      [error = std::move(error),
       sent_initial_metadata = s->sent_initial_metadata, id = s->id,
       grpc_status, message = std::move(message),
       remove_stream_handle =
           std::move(remove_stream_handle)](grpc_chttp2_transport* t) mutable {
        grpc_slice hdr;
        grpc_slice status_hdr;
        grpc_slice http_status_hdr;
        grpc_slice content_type_hdr;
        grpc_slice message_pfx;
        uint8_t* p;
        uint32_t len = 0;

        // Hand roll a header block.
        //   This is unnecessarily ugly - at some point we should find a more
        //   elegant solution.
        //   It's complicated by the fact that our send machinery would be dead
        //   by the time we got around to sending this, so instead we ignore
        //   HPACK compression and just write the uncompressed bytes onto the
        //   wire.
        if (!sent_initial_metadata) {
          http_status_hdr = GRPC_SLICE_MALLOC(13);
          p = GRPC_SLICE_START_PTR(http_status_hdr);
          *p++ = 0x00;
          *p++ = 7;
          *p++ = ':';
          *p++ = 's';
          *p++ = 't';
          *p++ = 'a';
          *p++ = 't';
          *p++ = 'u';
          *p++ = 's';
          *p++ = 3;
          *p++ = '2';
          *p++ = '0';
          *p++ = '0';
          CHECK(p == GRPC_SLICE_END_PTR(http_status_hdr));
          len += static_cast<uint32_t> GRPC_SLICE_LENGTH(http_status_hdr);

          content_type_hdr = GRPC_SLICE_MALLOC(31);
          p = GRPC_SLICE_START_PTR(content_type_hdr);
          *p++ = 0x00;
          *p++ = 12;
          *p++ = 'c';
          *p++ = 'o';
          *p++ = 'n';
          *p++ = 't';
          *p++ = 'e';
          *p++ = 'n';
          *p++ = 't';
          *p++ = '-';
          *p++ = 't';
          *p++ = 'y';
          *p++ = 'p';
          *p++ = 'e';
          *p++ = 16;
          *p++ = 'a';
          *p++ = 'p';
          *p++ = 'p';
          *p++ = 'l';
          *p++ = 'i';
          *p++ = 'c';
          *p++ = 'a';
          *p++ = 't';
          *p++ = 'i';
          *p++ = 'o';
          *p++ = 'n';
          *p++ = '/';
          *p++ = 'g';
          *p++ = 'r';
          *p++ = 'p';
          *p++ = 'c';
          CHECK(p == GRPC_SLICE_END_PTR(content_type_hdr));
          len += static_cast<uint32_t> GRPC_SLICE_LENGTH(content_type_hdr);
        }

        status_hdr = GRPC_SLICE_MALLOC(15 + (grpc_status >= 10));
        p = GRPC_SLICE_START_PTR(status_hdr);
        *p++ = 0x00;  // literal header, not indexed
        *p++ = 11;    // len(grpc-status)
        *p++ = 'g';
        *p++ = 'r';
        *p++ = 'p';
        *p++ = 'c';
        *p++ = '-';
        *p++ = 's';
        *p++ = 't';
        *p++ = 'a';
        *p++ = 't';
        *p++ = 'u';
        *p++ = 's';
        if (grpc_status < 10) {
          *p++ = 1;
          *p++ = static_cast<uint8_t>('0' + grpc_status);
        } else {
          *p++ = 2;
          *p++ = static_cast<uint8_t>('0' + (grpc_status / 10));
          *p++ = static_cast<uint8_t>('0' + (grpc_status % 10));
        }
        CHECK(p == GRPC_SLICE_END_PTR(status_hdr));
        len += static_cast<uint32_t> GRPC_SLICE_LENGTH(status_hdr);

        size_t msg_len = message.length();
        CHECK(msg_len <= UINT32_MAX);
        grpc_core::VarintWriter<1> msg_len_writer(
            static_cast<uint32_t>(msg_len));
        message_pfx = GRPC_SLICE_MALLOC(14 + msg_len_writer.length());
        p = GRPC_SLICE_START_PTR(message_pfx);
        *p++ = 0x00;  // literal header, not indexed
        *p++ = 12;    // len(grpc-message)
        *p++ = 'g';
        *p++ = 'r';
        *p++ = 'p';
        *p++ = 'c';
        *p++ = '-';
        *p++ = 'm';
        *p++ = 'e';
        *p++ = 's';
        *p++ = 's';
        *p++ = 'a';
        *p++ = 'g';
        *p++ = 'e';
        msg_len_writer.Write(0, p);
        p += msg_len_writer.length();
        CHECK(p == GRPC_SLICE_END_PTR(message_pfx));
        len += static_cast<uint32_t> GRPC_SLICE_LENGTH(message_pfx);
        len += static_cast<uint32_t>(msg_len);

        hdr = GRPC_SLICE_MALLOC(9);
        p = GRPC_SLICE_START_PTR(hdr);
        *p++ = static_cast<uint8_t>(len >> 16);
        *p++ = static_cast<uint8_t>(len >> 8);
        *p++ = static_cast<uint8_t>(len);
        *p++ = GRPC_CHTTP2_FRAME_HEADER;
        *p++ = GRPC_CHTTP2_DATA_FLAG_END_STREAM |
               GRPC_CHTTP2_DATA_FLAG_END_HEADERS;
        *p++ = static_cast<uint8_t>(id >> 24);
        *p++ = static_cast<uint8_t>(id >> 16);
        *p++ = static_cast<uint8_t>(id >> 8);
        *p++ = static_cast<uint8_t>(id);
        CHECK(p == GRPC_SLICE_END_PTR(hdr));

        grpc_slice_buffer_add(&t->qbuf, hdr);
        if (!sent_initial_metadata) {
          grpc_slice_buffer_add(&t->qbuf, http_status_hdr);
          grpc_slice_buffer_add(&t->qbuf, content_type_hdr);
        }
        grpc_slice_buffer_add(&t->qbuf, status_hdr);
        grpc_slice_buffer_add(&t->qbuf, message_pfx);
        grpc_slice_buffer_add(&t->qbuf,
                              grpc_slice_from_cpp_string(std::move(message)));
        grpc_chttp2_reset_ping_clock(t);
        grpc_chttp2_add_rst_stream_to_next_write(t, id, GRPC_HTTP2_NO_ERROR,
                                                 nullptr);

        grpc_chttp2_initiate_write(t,
                                   GRPC_CHTTP2_INITIATE_WRITE_CLOSE_FROM_API);
      });
}

static void end_all_the_calls(grpc_chttp2_transport* t,
                              grpc_error_handle error) {
  intptr_t http2_error;
  // If there is no explicit grpc or HTTP/2 error, set to UNAVAILABLE on server.
  if (!t->is_client && !grpc_error_has_clear_grpc_status(error) &&
      !grpc_error_get_int(error, grpc_core::StatusIntProperty::kHttp2Error,
                          &http2_error)) {
    error = grpc_error_set_int(error, grpc_core::StatusIntProperty::kRpcStatus,
                               GRPC_STATUS_UNAVAILABLE);
  }
  cancel_unstarted_streams(t, error, false);
  std::vector<grpc_chttp2_stream*> to_cancel;
  for (auto id_stream : t->stream_map) {
    to_cancel.push_back(id_stream.second);
  }
  for (auto s : to_cancel) {
    grpc_chttp2_cancel_stream(t, s, error, false);
  }
}

//
// INPUT PROCESSING - PARSING
//

template <class F>
static void WithUrgency(grpc_chttp2_transport* t,
                        grpc_core::chttp2::FlowControlAction::Urgency urgency,
                        grpc_chttp2_initiate_write_reason reason, F action) {
  switch (urgency) {
    case grpc_core::chttp2::FlowControlAction::Urgency::NO_ACTION_NEEDED:
      break;
    case grpc_core::chttp2::FlowControlAction::Urgency::UPDATE_IMMEDIATELY:
      grpc_chttp2_initiate_write(t, reason);
      ABSL_FALLTHROUGH_INTENDED;
    case grpc_core::chttp2::FlowControlAction::Urgency::QUEUE_UPDATE:
      action();
      break;
  }
}

void grpc_chttp2_act_on_flowctl_action(
    const grpc_core::chttp2::FlowControlAction& action,
    grpc_chttp2_transport* t, grpc_chttp2_stream* s) {
  WithUrgency(t, action.send_stream_update(),
              GRPC_CHTTP2_INITIATE_WRITE_STREAM_FLOW_CONTROL, [t, s]() {
                if (s->id != 0 && !s->read_closed) {
                  grpc_chttp2_mark_stream_writable(t, s);
                }
              });
  WithUrgency(t, action.send_transport_update(),
              GRPC_CHTTP2_INITIATE_WRITE_TRANSPORT_FLOW_CONTROL, []() {});
  WithUrgency(t, action.send_initial_window_update(),
              GRPC_CHTTP2_INITIATE_WRITE_SEND_SETTINGS, [t, &action]() {
                t->settings.mutable_local().SetInitialWindowSize(
                    action.initial_window_size());
              });
  WithUrgency(
      t, action.send_max_frame_size_update(),
      GRPC_CHTTP2_INITIATE_WRITE_SEND_SETTINGS, [t, &action]() {
        t->settings.mutable_local().SetMaxFrameSize(action.max_frame_size());
      });
  if (t->enable_preferred_rx_crypto_frame_advertisement) {
    WithUrgency(
        t, action.preferred_rx_crypto_frame_size_update(),
        GRPC_CHTTP2_INITIATE_WRITE_SEND_SETTINGS, [t, &action]() {
          t->settings.mutable_local().SetPreferredReceiveCryptoMessageSize(
              action.preferred_rx_crypto_frame_size());
        });
  }
}

static grpc_error_handle try_http_parsing(grpc_chttp2_transport* t) {
  grpc_http_parser parser;
  size_t i = 0;
  grpc_error_handle error;
  grpc_http_response response;

  grpc_http_parser_init(&parser, GRPC_HTTP_RESPONSE, &response);

  grpc_error_handle parse_error;
  for (; i < t->read_buffer.count && parse_error.ok(); i++) {
    parse_error =
        grpc_http_parser_parse(&parser, t->read_buffer.slices[i], nullptr);
  }
  if (parse_error.ok() &&
      (parse_error = grpc_http_parser_eof(&parser)) == absl::OkStatus()) {
    error = grpc_error_set_int(
        GRPC_ERROR_CREATE(
            absl::StrCat("Trying to connect an http1.x server (HTTP status ",
                         response.status, ")")),
        grpc_core::StatusIntProperty::kRpcStatus,
        grpc_http2_status_to_grpc_status(response.status));
  }

  grpc_http_parser_destroy(&parser);
  grpc_http_response_destroy(&response);
  return error;
}

static void read_action(grpc_core::RefCountedPtr<grpc_chttp2_transport> t,
                        grpc_error_handle error) {
  auto* tp = t.get();
  tp->combiner->Run(grpc_core::InitTransportClosure<read_action_locked>(
                        std::move(t), &tp->read_action_locked),
                    error);
}

static void read_action_parse_loop_locked(
    grpc_core::RefCountedPtr<grpc_chttp2_transport> t,
    grpc_error_handle error) {
  if (t->closed_with_error.ok()) {
    grpc_error_handle errors[3] = {error, absl::OkStatus(), absl::OkStatus()};
    size_t requests_started = 0;
    for (size_t i = 0;
         i < t->read_buffer.count && errors[1] == absl::OkStatus(); i++) {
      auto r = grpc_chttp2_perform_read(t.get(), t->read_buffer.slices[i],
                                        requests_started);
      if (auto* partial_read_size = absl::get_if<size_t>(&r)) {
        for (size_t j = 0; j < i; j++) {
          grpc_core::CSliceUnref(grpc_slice_buffer_take_first(&t->read_buffer));
        }
        grpc_slice_buffer_sub_first(
            &t->read_buffer, *partial_read_size,
            GRPC_SLICE_LENGTH(t->read_buffer.slices[0]));
        t->combiner->ForceOffload();
        auto* tp = t.get();
        tp->combiner->Run(
            grpc_core::InitTransportClosure<read_action_parse_loop_locked>(
                std::move(t), &tp->read_action_locked),
            std::move(errors[0]));
        // Early return: we queued to retry later.
        return;
      } else {
        errors[1] = std::move(absl::get<absl::Status>(r));
      }
    }
    if (errors[1] != absl::OkStatus()) {
      errors[2] = try_http_parsing(t.get());
      error = GRPC_ERROR_CREATE_REFERENCING("Failed parsing HTTP/2", errors,
                                            GPR_ARRAY_SIZE(errors));
    }

    if (t->initial_window_update != 0) {
      if (t->initial_window_update > 0) {
        grpc_chttp2_stream* s;
        while (grpc_chttp2_list_pop_stalled_by_stream(t.get(), &s)) {
          grpc_chttp2_mark_stream_writable(t.get(), s);
          grpc_chttp2_initiate_write(
              t.get(),
              GRPC_CHTTP2_INITIATE_WRITE_FLOW_CONTROL_UNSTALLED_BY_SETTING);
        }
      }
      t->initial_window_update = 0;
    }
  }

  bool keep_reading = false;
  if (error.ok() && !t->closed_with_error.ok()) {
    error = GRPC_ERROR_CREATE_REFERENCING("Transport closed",
                                          &t->closed_with_error, 1);
  }
  if (!error.ok()) {
    // If a goaway frame was received, this might be the reason why the read
    // failed. Add this info to the error
    if (!t->goaway_error.ok()) {
      error = grpc_error_add_child(error, t->goaway_error);
    }

    close_transport_locked(t.get(), error);
  } else if (t->closed_with_error.ok()) {
    keep_reading = true;
    // Since we have read a byte, reset the keepalive timer
    if (t->keepalive_state == GRPC_CHTTP2_KEEPALIVE_STATE_WAITING) {
      maybe_reset_keepalive_ping_timer_locked(t.get());
    }
  }
  grpc_slice_buffer_reset_and_unref(&t->read_buffer);

  if (keep_reading) {
    if (t->num_pending_induced_frames >= DEFAULT_MAX_PENDING_INDUCED_FRAMES) {
      t->reading_paused_on_pending_induced_frames = true;
      GRPC_CHTTP2_IF_TRACING(
          gpr_log(GPR_INFO,
                  "transport %p : Pausing reading due to too "
                  "many unwritten SETTINGS ACK and RST_STREAM frames",
                  t.get()));
    } else {
      continue_read_action_locked(std::move(t));
    }
  }
}

static void read_action_locked(
    grpc_core::RefCountedPtr<grpc_chttp2_transport> t,
    grpc_error_handle error) {
  // got an incoming read, cancel any pending keepalive timers
  t->keepalive_incoming_data_wanted = false;
  if (t->keepalive_ping_timeout_handle != TaskHandle::kInvalid) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_ping_trace) ||
        GRPC_TRACE_FLAG_ENABLED(grpc_keepalive_trace)) {
      gpr_log(GPR_INFO,
              "%s[%p]: Clear keepalive timer because data was received",
              t->is_client ? "CLIENT" : "SERVER", t.get());
    }
    t->event_engine->Cancel(
        std::exchange(t->keepalive_ping_timeout_handle, TaskHandle::kInvalid));
  }
  grpc_error_handle err = error;
  if (!err.ok()) {
    err = grpc_error_set_int(
        GRPC_ERROR_CREATE_REFERENCING("Endpoint read failed", &err, 1),
        grpc_core::StatusIntProperty::kOccurredDuringWrite, t->write_state);
  }
  std::swap(err, error);
  read_action_parse_loop_locked(std::move(t), std::move(err));
}

static void continue_read_action_locked(
    grpc_core::RefCountedPtr<grpc_chttp2_transport> t) {
  const bool urgent = !t->goaway_error.ok();
  auto* tp = t.get();
  grpc_endpoint_read(tp->ep, &tp->read_buffer,
                     grpc_core::InitTransportClosure<read_action>(
                         std::move(t), &tp->read_action_locked),
                     urgent, grpc_chttp2_min_read_progress_size(tp));
}

// t is reffed prior to calling the first time, and once the callback chain
// that kicks off finishes, it's unreffed
void schedule_bdp_ping_locked(
    grpc_core::RefCountedPtr<grpc_chttp2_transport> t) {
  auto* tp = t.get();
  tp->flow_control.bdp_estimator()->SchedulePing();
  send_ping_locked(tp,
                   grpc_core::InitTransportClosure<start_bdp_ping>(
                       tp->Ref(), &tp->start_bdp_ping_locked),
                   grpc_core::InitTransportClosure<finish_bdp_ping>(
                       std::move(t), &tp->finish_bdp_ping_locked));
  grpc_chttp2_initiate_write(tp, GRPC_CHTTP2_INITIATE_WRITE_BDP_PING);
}

static void start_bdp_ping(grpc_core::RefCountedPtr<grpc_chttp2_transport> t,
                           grpc_error_handle error) {
  grpc_chttp2_transport* tp = t.get();
  tp->combiner->Run(grpc_core::InitTransportClosure<start_bdp_ping_locked>(
                        std::move(t), &tp->start_bdp_ping_locked),
                    error);
}

static void start_bdp_ping_locked(
    grpc_core::RefCountedPtr<grpc_chttp2_transport> t,
    grpc_error_handle error) {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_http_trace)) {
    gpr_log(GPR_INFO, "%s: Start BDP ping err=%s",
            std::string(t->peer_string.as_string_view()).c_str(),
            grpc_core::StatusToString(error).c_str());
  }
  if (!error.ok() || !t->closed_with_error.ok()) {
    return;
  }
  // Reset the keepalive ping timer
  if (t->keepalive_state == GRPC_CHTTP2_KEEPALIVE_STATE_WAITING) {
    maybe_reset_keepalive_ping_timer_locked(t.get());
  }
  t->flow_control.bdp_estimator()->StartPing();
  t->bdp_ping_started = true;
}

static void finish_bdp_ping(grpc_core::RefCountedPtr<grpc_chttp2_transport> t,
                            grpc_error_handle error) {
  grpc_chttp2_transport* tp = t.get();
  tp->combiner->Run(grpc_core::InitTransportClosure<finish_bdp_ping_locked>(
                        std::move(t), &tp->finish_bdp_ping_locked),
                    error);
}

static void finish_bdp_ping_locked(
    grpc_core::RefCountedPtr<grpc_chttp2_transport> t,
    grpc_error_handle error) {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_http_trace)) {
    gpr_log(GPR_INFO, "%s: Complete BDP ping err=%s",
            std::string(t->peer_string.as_string_view()).c_str(),
            grpc_core::StatusToString(error).c_str());
  }
  if (!error.ok() || !t->closed_with_error.ok()) {
    return;
  }
  if (!t->bdp_ping_started) {
    // start_bdp_ping_locked has not been run yet. Schedule
    // finish_bdp_ping_locked to be run later.
    finish_bdp_ping(std::move(t), std::move(error));
    return;
  }
  t->bdp_ping_started = false;
  grpc_core::Timestamp next_ping =
      t->flow_control.bdp_estimator()->CompletePing();
  grpc_chttp2_act_on_flowctl_action(t->flow_control.PeriodicUpdate(), t.get(),
                                    nullptr);
  CHECK(t->next_bdp_ping_timer_handle == TaskHandle::kInvalid);
  t->next_bdp_ping_timer_handle =
      t->event_engine->RunAfter(next_ping - grpc_core::Timestamp::Now(), [t] {
        grpc_core::ApplicationCallbackExecCtx callback_exec_ctx;
        grpc_core::ExecCtx exec_ctx;
        next_bdp_ping_timer_expired(t.get());
      });
}

static void next_bdp_ping_timer_expired(grpc_chttp2_transport* t) {
  t->combiner->Run(
      grpc_core::InitTransportClosure<next_bdp_ping_timer_expired_locked>(
          t->Ref(), &t->next_bdp_ping_timer_expired_locked),
      absl::OkStatus());
}

static void next_bdp_ping_timer_expired_locked(
    grpc_core::RefCountedPtr<grpc_chttp2_transport> t,
    GRPC_UNUSED grpc_error_handle error) {
  DCHECK(error.ok());
  t->next_bdp_ping_timer_handle = TaskHandle::kInvalid;
  if (t->flow_control.bdp_estimator()->accumulator() == 0) {
    // Block the bdp ping till we receive more data.
    t->bdp_ping_blocked = true;
  } else {
    schedule_bdp_ping_locked(std::move(t));
  }
}

void grpc_chttp2_config_default_keepalive_args(grpc_channel_args* args,
                                               bool is_client) {
  grpc_chttp2_config_default_keepalive_args(grpc_core::ChannelArgs::FromC(args),
                                            is_client);
}

void grpc_chttp2_config_default_keepalive_args(
    const grpc_core::ChannelArgs& channel_args, bool is_client) {
  const auto keepalive_time =
      std::max(grpc_core::Duration::Milliseconds(1),
               channel_args.GetDurationFromIntMillis(GRPC_ARG_KEEPALIVE_TIME_MS)
                   .value_or(is_client ? g_default_client_keepalive_time
                                       : g_default_server_keepalive_time));
  if (is_client) {
    g_default_client_keepalive_time = keepalive_time;
  } else {
    g_default_server_keepalive_time = keepalive_time;
  }

  const auto keepalive_timeout = std::max(
      grpc_core::Duration::Zero(),
      channel_args.GetDurationFromIntMillis(GRPC_ARG_KEEPALIVE_TIMEOUT_MS)
          .value_or(is_client ? g_default_client_keepalive_timeout
                              : g_default_server_keepalive_timeout));
  if (is_client) {
    g_default_client_keepalive_timeout = keepalive_timeout;
  } else {
    g_default_server_keepalive_timeout = keepalive_timeout;
  }

  const bool keepalive_permit_without_calls =
      channel_args.GetBool(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS)
          .value_or(is_client
                        ? g_default_client_keepalive_permit_without_calls
                        : g_default_server_keepalive_permit_without_calls);
  if (is_client) {
    g_default_client_keepalive_permit_without_calls =
        keepalive_permit_without_calls;
  } else {
    g_default_server_keepalive_permit_without_calls =
        keepalive_permit_without_calls;
  }

  grpc_core::Chttp2PingAbusePolicy::SetDefaults(channel_args);
  grpc_core::Chttp2PingRatePolicy::SetDefaults(channel_args);
}

static void init_keepalive_ping(
    grpc_core::RefCountedPtr<grpc_chttp2_transport> t) {
  auto* tp = t.get();
  tp->combiner->Run(grpc_core::InitTransportClosure<init_keepalive_ping_locked>(
                        std::move(t), &tp->init_keepalive_ping_locked),
                    absl::OkStatus());
}

static void init_keepalive_ping_locked(
    grpc_core::RefCountedPtr<grpc_chttp2_transport> t,
    GRPC_UNUSED grpc_error_handle error) {
  DCHECK(error.ok());
  CHECK(t->keepalive_state == GRPC_CHTTP2_KEEPALIVE_STATE_WAITING);
  CHECK(t->keepalive_ping_timer_handle != TaskHandle::kInvalid);
  t->keepalive_ping_timer_handle = TaskHandle::kInvalid;
  if (t->destroying || !t->closed_with_error.ok()) {
    t->keepalive_state = GRPC_CHTTP2_KEEPALIVE_STATE_DYING;
  } else {
    if (t->keepalive_permit_without_calls || !t->stream_map.empty()) {
      t->keepalive_state = GRPC_CHTTP2_KEEPALIVE_STATE_PINGING;
      send_keepalive_ping_locked(t);
      grpc_chttp2_initiate_write(t.get(),
                                 GRPC_CHTTP2_INITIATE_WRITE_KEEPALIVE_PING);
    } else {
      t->keepalive_ping_timer_handle =
          t->event_engine->RunAfter(t->keepalive_time, [t] {
            grpc_core::ApplicationCallbackExecCtx callback_exec_ctx;
            grpc_core::ExecCtx exec_ctx;
            init_keepalive_ping(t);
          });
    }
  }
}

static void finish_keepalive_ping(
    grpc_core::RefCountedPtr<grpc_chttp2_transport> t,
    grpc_error_handle error) {
  auto* tp = t.get();
  tp->combiner->Run(
      grpc_core::InitTransportClosure<finish_keepalive_ping_locked>(
          std::move(t), &tp->finish_keepalive_ping_locked),
      error);
}

static void finish_keepalive_ping_locked(
    grpc_core::RefCountedPtr<grpc_chttp2_transport> t,
    grpc_error_handle error) {
  if (t->keepalive_state == GRPC_CHTTP2_KEEPALIVE_STATE_PINGING) {
    if (error.ok()) {
      if (GRPC_TRACE_FLAG_ENABLED(grpc_http_trace) ||
          GRPC_TRACE_FLAG_ENABLED(grpc_keepalive_trace)) {
        gpr_log(GPR_INFO, "%s: Finish keepalive ping",
                std::string(t->peer_string.as_string_view()).c_str());
      }
      t->keepalive_state = GRPC_CHTTP2_KEEPALIVE_STATE_WAITING;
      CHECK(t->keepalive_ping_timer_handle == TaskHandle::kInvalid);
      t->keepalive_ping_timer_handle =
          t->event_engine->RunAfter(t->keepalive_time, [t] {
            grpc_core::ApplicationCallbackExecCtx callback_exec_ctx;
            grpc_core::ExecCtx exec_ctx;
            init_keepalive_ping(t);
          });
    }
  }
}

static void maybe_reset_keepalive_ping_timer_locked(grpc_chttp2_transport* t) {
  if (t->keepalive_ping_timer_handle != TaskHandle::kInvalid &&
      t->event_engine->Cancel(t->keepalive_ping_timer_handle)) {
    // Cancel succeeds, resets the keepalive ping timer. Note that we don't
    // need to Ref or Unref here since we still hold the Ref.
    if (GRPC_TRACE_FLAG_ENABLED(grpc_http_trace) ||
        GRPC_TRACE_FLAG_ENABLED(grpc_keepalive_trace)) {
      gpr_log(GPR_INFO, "%s: Keepalive ping cancelled. Resetting timer.",
              std::string(t->peer_string.as_string_view()).c_str());
    }
    t->keepalive_ping_timer_handle =
        t->event_engine->RunAfter(t->keepalive_time, [t = t->Ref()]() mutable {
          grpc_core::ApplicationCallbackExecCtx callback_exec_ctx;
          grpc_core::ExecCtx exec_ctx;
          init_keepalive_ping(std::move(t));
        });
  }
}

//
// CALLBACK LOOP
//

static void connectivity_state_set(grpc_chttp2_transport* t,
                                   grpc_connectivity_state state,
                                   const absl::Status& status,
                                   const char* reason) {
  GRPC_CHTTP2_IF_TRACING(gpr_log(
      GPR_INFO, "transport %p set connectivity_state=%d; status=%s; reason=%s",
      t, state, status.ToString().c_str(), reason));
  t->state_tracker.SetState(state, status, reason);
}

//
// POLLSET STUFF
//

void grpc_chttp2_transport::SetPollset(grpc_stream* /*gs*/,
                                       grpc_pollset* pollset) {
  // We don't want the overhead of acquiring the mutex unless we're
  // using the "poll" polling engine, which is the only one that
  // actually uses pollsets.
  if (strcmp(grpc_get_poll_strategy_name(), "poll") != 0) return;
  grpc_core::MutexLock lock(&ep_destroy_mu);
  if (ep != nullptr) grpc_endpoint_add_to_pollset(ep, pollset);
}

void grpc_chttp2_transport::SetPollsetSet(grpc_stream* /*gs*/,
                                          grpc_pollset_set* pollset_set) {
  // We don't want the overhead of acquiring the mutex unless we're
  // using the "poll" polling engine, which is the only one that
  // actually uses pollsets.
  if (strcmp(grpc_get_poll_strategy_name(), "poll") != 0) return;
  grpc_core::MutexLock lock(&ep_destroy_mu);
  if (ep != nullptr) grpc_endpoint_add_to_pollset_set(ep, pollset_set);
}

//
// RESOURCE QUOTAS
//

static void post_benign_reclaimer(grpc_chttp2_transport* t) {
  if (!t->benign_reclaimer_registered) {
    t->benign_reclaimer_registered = true;
    t->memory_owner.PostReclaimer(
        grpc_core::ReclamationPass::kBenign,
        [t = t->Ref()](
            absl::optional<grpc_core::ReclamationSweep> sweep) mutable {
          if (sweep.has_value()) {
            auto* tp = t.get();
            tp->active_reclamation = std::move(*sweep);
            tp->combiner->Run(
                grpc_core::InitTransportClosure<benign_reclaimer_locked>(
                    std::move(t), &tp->benign_reclaimer_locked),
                absl::OkStatus());
          }
        });
  }
}

static void post_destructive_reclaimer(grpc_chttp2_transport* t) {
  if (!t->destructive_reclaimer_registered) {
    t->destructive_reclaimer_registered = true;
    t->memory_owner.PostReclaimer(
        grpc_core::ReclamationPass::kDestructive,
        [t = t->Ref()](
            absl::optional<grpc_core::ReclamationSweep> sweep) mutable {
          if (sweep.has_value()) {
            auto* tp = t.get();
            tp->active_reclamation = std::move(*sweep);
            tp->combiner->Run(
                grpc_core::InitTransportClosure<destructive_reclaimer_locked>(
                    std::move(t), &tp->destructive_reclaimer_locked),
                absl::OkStatus());
          }
        });
  }
}

static void benign_reclaimer_locked(
    grpc_core::RefCountedPtr<grpc_chttp2_transport> t,
    grpc_error_handle error) {
  if (error.ok() && t->stream_map.empty()) {
    // Channel with no active streams: send a goaway to try and make it
    // disconnect cleanly
    if (GRPC_TRACE_FLAG_ENABLED(grpc_resource_quota_trace)) {
      gpr_log(GPR_INFO, "HTTP2: %s - send goaway to free memory",
              std::string(t->peer_string.as_string_view()).c_str());
    }
    send_goaway(t.get(),
                grpc_error_set_int(GRPC_ERROR_CREATE("Buffers full"),
                                   grpc_core::StatusIntProperty::kHttp2Error,
                                   GRPC_HTTP2_ENHANCE_YOUR_CALM),
                /*immediate_disconnect_hint=*/true);
  } else if (error.ok() && GRPC_TRACE_FLAG_ENABLED(grpc_resource_quota_trace)) {
    gpr_log(GPR_INFO,
            "HTTP2: %s - skip benign reclamation, there are still %" PRIdPTR
            " streams",
            std::string(t->peer_string.as_string_view()).c_str(),
            t->stream_map.size());
  }
  t->benign_reclaimer_registered = false;
  if (error != absl::CancelledError()) {
    t->active_reclamation.Finish();
  }
}

static void destructive_reclaimer_locked(
    grpc_core::RefCountedPtr<grpc_chttp2_transport> t,
    grpc_error_handle error) {
  t->destructive_reclaimer_registered = false;
  if (error.ok() && !t->stream_map.empty()) {
    // As stream_map is a hash map, this selects effectively a random stream.
    grpc_chttp2_stream* s = t->stream_map.begin()->second;
    if (GRPC_TRACE_FLAG_ENABLED(grpc_resource_quota_trace)) {
      gpr_log(GPR_INFO, "HTTP2: %s - abandon stream id %d",
              std::string(t->peer_string.as_string_view()).c_str(), s->id);
    }
    grpc_chttp2_cancel_stream(
        t.get(), s,
        grpc_error_set_int(GRPC_ERROR_CREATE("Buffers full"),
                           grpc_core::StatusIntProperty::kHttp2Error,
                           GRPC_HTTP2_ENHANCE_YOUR_CALM),
        false);
    if (!t->stream_map.empty()) {
      // Since we cancel one stream per destructive reclamation, if
      //   there are more streams left, we can immediately post a new
      //   reclaimer in case the resource quota needs to free more
      //   memory
      post_destructive_reclaimer(t.get());
    }
  }
  if (error != absl::CancelledError()) {
    t->active_reclamation.Finish();
  }
}

//
// MONITORING
//

const char* grpc_chttp2_initiate_write_reason_string(
    grpc_chttp2_initiate_write_reason reason) {
  switch (reason) {
    case GRPC_CHTTP2_INITIATE_WRITE_INITIAL_WRITE:
      return "INITIAL_WRITE";
    case GRPC_CHTTP2_INITIATE_WRITE_START_NEW_STREAM:
      return "START_NEW_STREAM";
    case GRPC_CHTTP2_INITIATE_WRITE_SEND_MESSAGE:
      return "SEND_MESSAGE";
    case GRPC_CHTTP2_INITIATE_WRITE_SEND_INITIAL_METADATA:
      return "SEND_INITIAL_METADATA";
    case GRPC_CHTTP2_INITIATE_WRITE_SEND_TRAILING_METADATA:
      return "SEND_TRAILING_METADATA";
    case GRPC_CHTTP2_INITIATE_WRITE_RETRY_SEND_PING:
      return "RETRY_SEND_PING";
    case GRPC_CHTTP2_INITIATE_WRITE_CONTINUE_PINGS:
      return "CONTINUE_PINGS";
    case GRPC_CHTTP2_INITIATE_WRITE_GOAWAY_SENT:
      return "GOAWAY_SENT";
    case GRPC_CHTTP2_INITIATE_WRITE_RST_STREAM:
      return "RST_STREAM";
    case GRPC_CHTTP2_INITIATE_WRITE_CLOSE_FROM_API:
      return "CLOSE_FROM_API";
    case GRPC_CHTTP2_INITIATE_WRITE_STREAM_FLOW_CONTROL:
      return "STREAM_FLOW_CONTROL";
    case GRPC_CHTTP2_INITIATE_WRITE_TRANSPORT_FLOW_CONTROL:
      return "TRANSPORT_FLOW_CONTROL";
    case GRPC_CHTTP2_INITIATE_WRITE_SEND_SETTINGS:
      return "SEND_SETTINGS";
    case GRPC_CHTTP2_INITIATE_WRITE_SETTINGS_ACK:
      return "SETTINGS_ACK";
    case GRPC_CHTTP2_INITIATE_WRITE_FLOW_CONTROL_UNSTALLED_BY_SETTING:
      return "FLOW_CONTROL_UNSTALLED_BY_SETTING";
    case GRPC_CHTTP2_INITIATE_WRITE_FLOW_CONTROL_UNSTALLED_BY_UPDATE:
      return "FLOW_CONTROL_UNSTALLED_BY_UPDATE";
    case GRPC_CHTTP2_INITIATE_WRITE_APPLICATION_PING:
      return "APPLICATION_PING";
    case GRPC_CHTTP2_INITIATE_WRITE_BDP_PING:
      return "BDP_PING";
    case GRPC_CHTTP2_INITIATE_WRITE_KEEPALIVE_PING:
      return "KEEPALIVE_PING";
    case GRPC_CHTTP2_INITIATE_WRITE_TRANSPORT_FLOW_CONTROL_UNSTALLED:
      return "TRANSPORT_FLOW_CONTROL_UNSTALLED";
    case GRPC_CHTTP2_INITIATE_WRITE_PING_RESPONSE:
      return "PING_RESPONSE";
    case GRPC_CHTTP2_INITIATE_WRITE_FORCE_RST_STREAM:
      return "FORCE_RST_STREAM";
  }
  GPR_UNREACHABLE_CODE(return "unknown");
}

size_t grpc_chttp2_transport::SizeOfStream() const {
  return sizeof(grpc_chttp2_stream);
}

bool grpc_chttp2_transport::
    HackyDisableStreamOpBatchCoalescingInConnectedChannel() const {
  return false;
}

absl::string_view grpc_chttp2_transport::GetTransportName() const {
  return "chttp2";
}

grpc_core::RefCountedPtr<grpc_core::channelz::SocketNode>
grpc_chttp2_transport_get_socket_node(grpc_core::Transport* transport) {
  grpc_chttp2_transport* t =
      reinterpret_cast<grpc_chttp2_transport*>(transport);
  return t->channelz_socket;
}

grpc_core::Transport* grpc_create_chttp2_transport(
    const grpc_core::ChannelArgs& channel_args, grpc_endpoint* ep,
    bool is_client) {
  return new grpc_chttp2_transport(channel_args, ep, is_client);
}

void grpc_chttp2_transport_start_reading(
    grpc_core::Transport* transport, grpc_slice_buffer* read_buffer,
    grpc_closure* notify_on_receive_settings,
    grpc_pollset_set* interested_parties_until_recv_settings,
    grpc_closure* notify_on_close) {
  auto t = reinterpret_cast<grpc_chttp2_transport*>(transport)->Ref();
  if (read_buffer != nullptr) {
    grpc_slice_buffer_move_into(read_buffer, &t->read_buffer);
  }
  auto* tp = t.get();
  tp->combiner->Run(
      grpc_core::NewClosure([t = std::move(t), notify_on_receive_settings,
                             interested_parties_until_recv_settings,
                             notify_on_close](grpc_error_handle) mutable {
        if (!t->closed_with_error.ok()) {
          if (notify_on_receive_settings != nullptr) {
            if (t->ep != nullptr &&
                interested_parties_until_recv_settings != nullptr) {
              grpc_endpoint_delete_from_pollset_set(
                  t->ep, interested_parties_until_recv_settings);
            }
            grpc_core::ExecCtx::Run(DEBUG_LOCATION, notify_on_receive_settings,
                                    t->closed_with_error);
          }
          if (notify_on_close != nullptr) {
            grpc_core::ExecCtx::Run(DEBUG_LOCATION, notify_on_close,
                                    t->closed_with_error);
          }
          return;
        }
        t->interested_parties_until_recv_settings =
            interested_parties_until_recv_settings;
        t->notify_on_receive_settings = notify_on_receive_settings;
        t->notify_on_close = notify_on_close;
        read_action_locked(std::move(t), absl::OkStatus());
      }),
      absl::OkStatus());
}
