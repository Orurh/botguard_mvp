#include "agent/uds_client.h"
#include "agent/uds_protocol.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>

namespace {

namespace agent = botguard::agent;
namespace execution = botguard::execution;
namespace risk = botguard::risk;

[[nodiscard]] risk::OrderIntent Intent() noexcept {
  return risk::OrderIntent{
      .strategy_id = 7,
      .client_order_id = 1001,
      .market_id = 42,
      .side = risk::Side::kBuy,
      .price = {.micros_per_unit = 250'000},
      .quantity = {.microunits = 1'500'000},
      .market_data_received_at = risk::MonotonicClock::now(),
  };
}

TEST(UdsProtocolTest, SubmitIntentRoundTripsWithoutCallerControlledSafetyState) {
  const auto encoded = agent::EncodeSubmitIntent(Intent());
  EXPECT_EQ(encoded[32], std::byte{1});
  const auto decoded = agent::DecodeSubmitIntent(encoded);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->strategy_id, 7U);
  EXPECT_EQ(decoded->client_order_id, 1001U);
  EXPECT_EQ(decoded->market_id, 42U);
  EXPECT_EQ(decoded->side, risk::Side::kBuy);
  EXPECT_EQ(decoded->price.micros_per_unit, 250'000);
  EXPECT_EQ(decoded->quantity.microunits, 1'500'000);
  EXPECT_EQ(decoded->market_data_received_at, risk::MonotonicClock::time_point{});
}

TEST(UdsProtocolTest, SideUsesExplicitProtocolV1Values) {
  auto buy = Intent();
  auto sell = Intent();
  sell.side = risk::Side::kSell;
  EXPECT_EQ(agent::EncodeSubmitIntent(buy)[32], std::byte{1});
  EXPECT_EQ(agent::EncodeSubmitIntent(sell)[32], std::byte{2});
}

TEST(UdsProtocolTest, MaxLossSinceBaselineRetainsProtocolV1WireBit) {
  risk::RiskDecision decision;
  decision.AddReason(risk::RejectReason::kMaxLossSinceBaselineExceeded);
  const execution::SubmissionResult result{
      .status = execution::SubmissionStatus::kRiskRejected,
      .risk_decision = decision,
  };

  const auto encoded = agent::EncodeSubmitResponse(agent::MakeSubmitResponse(result));
  const auto decoded = agent::DecodeSubmitResponse(encoded);

  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->reject_reason_mask, std::uint64_t{1} << 16U);
  EXPECT_TRUE(agent::HasRejectReason(*decoded, agent::WireRejectReason::kMaxLossSinceBaselineExceeded));
}

TEST(UdsProtocolTest, RejectsWrongSizeVersionSideAndReservedBytes) {
  auto encoded = agent::EncodeSubmitIntent(Intent());
  EXPECT_EQ(agent::DecodeSubmitIntent(std::span<const std::byte>(encoded.data(), encoded.size() - 1)).error(),
            agent::ProtocolError::kInvalidSize);

  encoded = agent::EncodeSubmitIntent(Intent());
  encoded[5] = std::byte{2};
  EXPECT_EQ(agent::DecodeSubmitIntent(encoded).error(), agent::ProtocolError::kUnsupportedVersion);

  encoded = agent::EncodeSubmitIntent(Intent());
  encoded[32] = std::byte{9};
  EXPECT_EQ(agent::DecodeSubmitIntent(encoded).error(), agent::ProtocolError::kInvalidSide);

  encoded = agent::EncodeSubmitIntent(Intent());
  encoded[39] = std::byte{1};
  EXPECT_EQ(agent::DecodeSubmitIntent(encoded).error(), agent::ProtocolError::kInvalidReservedBytes);
}

TEST(UdsProtocolTest, SubmissionResponsePreservesStatusOutcomeNotionalAndReasons) {
  risk::RiskDecision decision;
  decision.SetOrderNotional(risk::Money{.micros = 375'000});
  decision.AddReason(risk::RejectReason::kDuplicateClientOrderId);
  decision.AddReason(risk::RejectReason::kKillSwitchActive);
  const execution::SubmissionResult result{
      .status = execution::SubmissionStatus::kRiskRejected,
      .risk_decision = decision,
      .submit_outcome = std::nullopt,
  };

  const auto response = agent::MakeSubmitResponse(result);
  const auto decoded = agent::DecodeSubmitResponse(agent::EncodeSubmitResponse(response));
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->status, agent::ResponseStatus::kRiskRejected);
  EXPECT_EQ(decoded->submit_outcome, agent::WireSubmitOutcome::kNone);
  EXPECT_EQ(decoded->order_notional.micros, 375'000);
  EXPECT_EQ(decoded->reject_reason_count, 2U);
  EXPECT_EQ(decoded->reject_reason_mask, (std::uint64_t{1} << 3U) | (std::uint64_t{1} << 0U));
  EXPECT_TRUE(agent::HasRejectReason(*decoded, agent::WireRejectReason::kDuplicateClientOrderId));
  EXPECT_TRUE(agent::HasRejectReason(*decoded, agent::WireRejectReason::kKillSwitchActive));
}

TEST(UdsProtocolTest, WireOutcomeDoesNotDependOnInternalEnumOrdinal) {
  agent::SubmitResponse response{
      .status = agent::ResponseStatus::kSubmitted,
      .submit_outcome = agent::WireSubmitOutcome::kUnknown,
  };
  const auto encoded = agent::EncodeSubmitResponse(response);
  EXPECT_EQ(encoded[10], std::byte{4});
}

TEST(UdsProtocolTest, RejectsResponseWithInconsistentReasonCount) {
  agent::SubmitResponse response{
      .status = agent::ResponseStatus::kRiskRejected,
      .submit_outcome = agent::WireSubmitOutcome::kNone,
      .order_notional = {},
      .reject_reason_mask = std::uint64_t{1} << 3U,
      .reject_reason_count = 2,
  };
  EXPECT_EQ(agent::DecodeSubmitResponse(agent::EncodeSubmitResponse(response)).error(), agent::ProtocolError::kInvalidRejectReasons);
}

TEST(UdsProtocolTest, StatusAndKillFramesHaveStableTypesAndRoundTrip) {
  const auto get_status = agent::EncodeGetStatus();
  const auto kill = agent::EncodeKill();
  EXPECT_EQ(get_status[7], std::byte{3});
  EXPECT_EQ(kill[7], std::byte{5});
  EXPECT_TRUE(agent::DecodeEmptyRequest(get_status, agent::MessageType::kGetStatus).has_value());
  EXPECT_TRUE(agent::DecodeEmptyRequest(kill, agent::MessageType::kKill).has_value());

  const agent::StatusSnapshot original{
      .agent_state = agent::AgentState::kKilled,
      .kill_switch_active = true,
      .gate_state = agent::WireGateState::kReady,
      .durable_order_count = 7,
      .active_reservation_count = 3,
      .total_reserved_exposure = {.micros = 1'250'000},
  };
  const auto encoded = agent::EncodeStatusResponse(original, agent::MessageType::kKillResponse);
  EXPECT_EQ(encoded[7], std::byte{6});
  const auto decoded = agent::DecodeStatusResponse(encoded, agent::MessageType::kKillResponse);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->agent_state, agent::AgentState::kKilled);
  EXPECT_TRUE(decoded->kill_switch_active);
  EXPECT_EQ(decoded->gate_state, agent::WireGateState::kReady);
  EXPECT_EQ(decoded->durable_order_count, 7U);
  EXPECT_EQ(decoded->active_reservation_count, 3U);
  EXPECT_EQ(decoded->total_reserved_exposure.micros, 1'250'000);
}

TEST(UdsProtocolTest, ResumeResultAndStatusRoundTripWithStableWireValues) {
  const auto request = agent::EncodeResume();
  EXPECT_EQ(request[7], std::byte{7});
  EXPECT_TRUE(agent::DecodeEmptyRequest(request, agent::MessageType::kResume).has_value());

  const agent::ResumeResponse original{
      .result = agent::WireResumeResult::kRecoveryFailed,
      .status =
          agent::StatusSnapshot{
              .agent_state = agent::AgentState::kRecoveryRequired,
              .kill_switch_active = true,
              .gate_state = agent::WireGateState::kReconciliationRequired,
              .durable_order_count = 2,
              .active_reservation_count = 2,
              .total_reserved_exposure = {.micros = 3'000'000},
          },
  };
  const auto encoded = agent::EncodeResumeResponse(original);
  EXPECT_EQ(encoded[7], std::byte{8});
  EXPECT_EQ(encoded[8], std::byte{3});
  const auto decoded = agent::DecodeResumeResponse(encoded);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->result, agent::WireResumeResult::kRecoveryFailed);
  EXPECT_EQ(decoded->status.agent_state, agent::AgentState::kRecoveryRequired);
  EXPECT_TRUE(decoded->status.kill_switch_active);
  EXPECT_EQ(decoded->status.durable_order_count, 2U);
  EXPECT_EQ(decoded->status.total_reserved_exposure.micros, 3'000'000);
}

TEST(UdsClientTest, RejectsNonPositiveTimeoutBeforeTransportAccess) {
  const auto zero = agent::GetStatusOverUds("/does/not/matter", agent::UdsClientOptions{.timeout = std::chrono::milliseconds::zero()});
  ASSERT_FALSE(zero.has_value());
  EXPECT_EQ(zero.error(), agent::UdsClientError::kInvalidTimeout);
  const auto negative = agent::GetStatusOverUds("/does/not/matter", agent::UdsClientOptions{.timeout = std::chrono::milliseconds{-1}});
  ASSERT_FALSE(negative.has_value());
  EXPECT_EQ(negative.error(), agent::UdsClientError::kInvalidTimeout);
}

}  // namespace
