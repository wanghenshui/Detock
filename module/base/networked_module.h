#pragma once

#include <array>
#include <vector>
#include <zmq.hpp>

#include "common/constants.h"
#include "common/metrics.h"
#include "common/types.h"
#include "connection/broker.h"
#include "connection/poller.h"
#include "connection/sender.h"
#include "connection/zmq_utils.h"
#include "module/base/module.h"
#include "proto/internal.pb.h"

namespace slog {

struct ChannelOption {
  ChannelOption(Channel channel, bool recv_raw = true) : channel(channel), recv_raw(recv_raw) {}
  Channel channel;
  bool recv_raw;
};

/**
 * Base class for modules that can send and receive in internal messages.
 */
class NetworkedModule : public Module {
 public:
  NetworkedModule(const std::string& name, const std::shared_ptr<Broker>& broker, ChannelOption chopt,
                  const MetricsRepositoryManagerPtr& metrics_manager,
                  std::optional<std::chrono::milliseconds> poll_timeout);
  ~NetworkedModule();

  MetricsRepositoryManager& metrics_manager() { return *metrics_manager_; }

 protected:
  virtual void Initialize(){};

  virtual void OnInternalRequestReceived(EnvelopePtr&& env) = 0;

  virtual void OnInternalResponseReceived(EnvelopePtr&& /* env */) {}

  // Returns true if useful work was done
  virtual bool OnCustomSocket() { return false; }

  void AddCustomSocket(zmq::socket_t&& new_socket);
  zmq::socket_t& GetCustomSocket(size_t i);
  void SetMainVsCustomSocketWeights(std::array<int, 2> weights) { weights_ = weights; }

  inline static EnvelopePtr NewEnvelope() { return std::make_unique<internal::Envelope>(); }
  void Send(const internal::Envelope& env, MachineId to_machine_id, Channel to_channel);
  void Send(EnvelopePtr&& env, MachineId to_machine_id, Channel to_channel);
  void Send(EnvelopePtr&& env, Channel to_channel);
  void Send(const internal::Envelope& env, const std::vector<MachineId>& to_machine_ids, Channel to_channel);
  void Send(EnvelopePtr&& env, const std::vector<MachineId>& to_machine_ids, Channel to_channel);

  void NewTimedCallback(microseconds timeout, std::function<void()>&& cb);
  void ClearTimedCallbacks();

  const std::shared_ptr<zmq::context_t> context() const { return context_; }
  const ConfigurationPtr& config() const { return config_; }

  Channel channel() const { return channel_; }

 private:
  void SetUp() final;
  bool Loop() final;

  std::shared_ptr<zmq::context_t> context_;
  ConfigurationPtr config_;
  MetricsRepositoryManagerPtr metrics_manager_;
  Channel channel_;
  zmq::socket_t pull_socket_;
  std::vector<zmq::socket_t> custom_sockets_;
  Sender sender_;
  Poller poller_;
  int recv_retries_start_;
  int recv_retries_;

  // Weights for the main socket and the custom sockets
  std::array<int, 2> weights_;
  std::array<int, 2> counters_;
  uint8_t current_;

  std::string debug_info_;

  std::atomic<uint64_t> work_ = 0;
};

}  // namespace slog