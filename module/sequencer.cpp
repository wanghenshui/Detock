#include "module/sequencer.h"

#include <glog/logging.h>

#include <algorithm>

#include "common/clock.h"
#include "common/json_utils.h"
#include "common/proto_utils.h"
#include "paxos/simulated_multi_paxos.h"

using std::move;

namespace slog {

using internal::Batch;
using internal::Request;
using std::chrono::milliseconds;

Sequencer::Sequencer(const std::shared_ptr<zmq::context_t>& context, const ConfigurationPtr& config,
                     const MetricsRepositoryManagerPtr& metrics_manager, milliseconds poll_timeout)
    : NetworkedModule(context, config, config->sequencer_port(), kSequencerChannel, metrics_manager, poll_timeout),
      batcher_(std::make_shared<Batcher>(context, config, metrics_manager, poll_timeout)),
      batcher_runner_(std::static_pointer_cast<Module>(batcher_)) {
  int num_machines = config->num_partitions() * config->num_replicas();
  for (int i = 0; i < num_machines; i++) {
    deviations_.emplace_back(100);
  }
}

void Sequencer::Initialize() { batcher_runner_.StartInNewThread(); }

void Sequencer::OnInternalRequestReceived(EnvelopePtr&& env) {
  auto request = env->mutable_request();
  switch (request->type_case()) {
    case Request::kForwardTxn:
      ProcessForwardRequest(move(env));
      break;
    case Request::kPing:
      ProcessPingRequest(move(env));
      break;
    case Request::kStats:
      Send(move(env), kBatcherChannel);
      break;
    default:
      LOG(ERROR) << "Unexpected request type received: \"" << CASE_NAME(request->type_case(), Request) << "\"";
      break;
  }
}

void Sequencer::ProcessForwardRequest(EnvelopePtr&& env) {
  auto now = slog_clock::now().time_since_epoch().count();
  auto txn = env->mutable_request()->mutable_forward_txn()->mutable_txn();
  auto txn_internal = txn->mutable_internal();

  RECORD(txn_internal, TransactionEvent::ENTER_SEQUENCER);

  txn_internal->set_mh_arrive_at_home_time(now);

  if (config()->bypass_mh_orderer() && config()->synchronized_batching()) {
    int64_t dev = txn_internal->timestamp() - now;
    if (dev <= 0) {
      VLOG(3) << "Txn " << txn_internal->id() << " has a timestamp " << (now - txn_internal->timestamp()) / 1000
              << " us in the past";

#if !defined(LOCK_MANAGER_DDR)
      // If not using DDR, restart the transaction
      txn->set_status(TransactionStatus::ABORTED);
      txn->set_abort_reason("restarted");
#endif

      // Put to batch immediately
      txn_internal->set_mh_enter_local_batch_time(now);
      Send(move(env), kBatcherChannel);
    } else {
      VLOG(3) << "Txn " << txn_internal->id() << " has a timestamp " << (txn_internal->timestamp() - now) / 1000
              << " us into the future";

      RECORD_WITH_TIME(txn_internal, TransactionEvent::EXPECTED_WAIT_TIME_UNTIL_ENTER_LOCAL_BATCH,
                       txn_internal->timestamp() - now);

      // Put into a sorted buffer and wait until local clock reaches the txn's timestamp.
      // Send a signal to the batcher if the earliest time in the buffer has changed, so that
      // the batcher is rescheduled to wake up at this ealier time
      bool signal_needed = batcher_->BufferFutureTxn(env->mutable_request()->mutable_forward_txn()->release_txn());
      if (signal_needed) {
        auto env = NewEnvelope();
        env->mutable_request()->mutable_signal();
        Send(move(env), kBatcherChannel);
      }
    }
    deviations_[env->from()].Add(dev);
  } else {
    // Put to batch immediately
    txn_internal->set_mh_enter_local_batch_time(now);
    Send(move(env), kBatcherChannel);
  }
}

void Sequencer::ProcessPingRequest(EnvelopePtr&& env) {
  auto pong_env = NewEnvelope();
  auto pong = pong_env->mutable_response()->mutable_pong();
  pong->set_src_send_time(env->request().ping().src_send_time());
  pong->set_dev(deviations_[env->from()].avg());
  pong->set_dst(env->request().ping().dst());
  Send(move(pong_env), env->from(), kForwarderChannel);
}

}  // namespace slog