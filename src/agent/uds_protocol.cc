#include "uds_protocol.h"

#include <bit>
#include <type_traits>

namespace botguard::agent {
namespace {

template <typename Integer, std::size_t Size>
void Put(std::array<std::byte, Size>& frame, std::size_t offset, Integer value) noexcept {
  using Unsigned = std::make_unsigned_t<Integer>;
  auto bits = static_cast<Unsigned>(value);
  for (std::size_t index = 0; index < sizeof(Integer); ++index) {
    frame[offset + sizeof(Integer) - index - 1] = static_cast<std::byte>(bits & 0xFFU);
    bits >>= 8U;
  }
}

template <typename Integer>
[[nodiscard]] Integer Get(std::span<const std::byte> frame, std::size_t offset) noexcept {
  using Unsigned = std::make_unsigned_t<Integer>;
  Unsigned bits{};
  for (std::size_t index = 0; index < sizeof(Integer); ++index) {
    bits = static_cast<Unsigned>((bits << 8U) | std::to_integer<unsigned int>(frame[offset + index]));
  }
  return static_cast<Integer>(bits);
}

void PutHeader(std::span<std::byte> frame, MessageType type) noexcept {
  const auto put = [&frame]<typename Integer>(std::size_t offset, Integer value) {
    using Unsigned = std::make_unsigned_t<Integer>;
    auto bits = static_cast<Unsigned>(value);
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
      frame[offset + sizeof(Integer) - index - 1] = static_cast<std::byte>(bits & 0xFFU);
      bits >>= 8U;
    }
  };
  put(0, kProtocolMagic);
  put(4, kProtocolVersion);
  put(6, static_cast<std::uint16_t>(type));
}

[[nodiscard]] std::optional<MessageType> ToMessageType(std::uint16_t raw) noexcept {
  switch (raw) {
    case 1:
      return MessageType::kSubmitIntent;
    case 2:
      return MessageType::kSubmitResponse;
    case 3:
      return MessageType::kGetStatus;
    case 4:
      return MessageType::kStatusResponse;
    case 5:
      return MessageType::kKill;
    case 6:
      return MessageType::kKillResponse;
    case 7:
      return MessageType::kResume;
    case 8:
      return MessageType::kResumeResponse;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<ResponseStatus> ToResponseStatus(execution::SubmissionStatus status) noexcept {
  switch (status) {
    case execution::SubmissionStatus::kRiskRejected:
      return ResponseStatus::kRiskRejected;
    case execution::SubmissionStatus::kVenuePreflightRejected:
      return ResponseStatus::kVenuePreflightRejected;
    case execution::SubmissionStatus::kPreSubmitPersistenceFailed:
      return ResponseStatus::kPreSubmitPersistenceFailed;
    case execution::SubmissionStatus::kAbortedBeforeSubmit:
      return ResponseStatus::kAbortedBeforeSubmit;
    case execution::SubmissionStatus::kPreSubmitAbortPersistenceFailed:
      return ResponseStatus::kPreSubmitAbortPersistenceFailed;
    case execution::SubmissionStatus::kSubmitted:
      return ResponseStatus::kSubmitted;
    case execution::SubmissionStatus::kPostSubmitPersistenceFailed:
      return ResponseStatus::kPostSubmitPersistenceFailed;
    case execution::SubmissionStatus::kRegistryTransitionFailed:
      return ResponseStatus::kRegistryTransitionFailed;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<ResponseStatus> ToResponseStatus(std::uint16_t raw) noexcept {
  switch (raw) {
    case 1:
      return ResponseStatus::kRiskRejected;
    case 2:
      return ResponseStatus::kVenuePreflightRejected;
    case 3:
      return ResponseStatus::kPreSubmitPersistenceFailed;
    case 4:
      return ResponseStatus::kAbortedBeforeSubmit;
    case 5:
      return ResponseStatus::kPreSubmitAbortPersistenceFailed;
    case 6:
      return ResponseStatus::kSubmitted;
    case 7:
      return ResponseStatus::kPostSubmitPersistenceFailed;
    case 8:
      return ResponseStatus::kRegistryTransitionFailed;
    case 100:
      return ResponseStatus::kInvalidRequest;
    case 101:
      return ResponseStatus::kInternalError;
  }
  return std::nullopt;
}

[[nodiscard]] WireSubmitOutcome ToWireOutcome(const std::optional<execution::SubmitOutcome>& outcome) noexcept {
  if (!outcome)
    return WireSubmitOutcome::kNone;
  switch (*outcome) {
    case execution::SubmitOutcome::kOpen:
      return WireSubmitOutcome::kOpen;
    case execution::SubmitOutcome::kFilled:
      return WireSubmitOutcome::kFilled;
    case execution::SubmitOutcome::kRejected:
      return WireSubmitOutcome::kRejected;
    case execution::SubmitOutcome::kUnknown:
      return WireSubmitOutcome::kUnknown;
  }
  return WireSubmitOutcome::kNone;
}

[[nodiscard]] std::optional<WireSubmitOutcome> ToWireOutcome(std::uint8_t raw) noexcept {
  switch (raw) {
    case 0:
      return WireSubmitOutcome::kNone;
    case 1:
      return WireSubmitOutcome::kOpen;
    case 2:
      return WireSubmitOutcome::kFilled;
    case 3:
      return WireSubmitOutcome::kRejected;
    case 4:
      return WireSubmitOutcome::kUnknown;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<WireRejectReason> ToWireReason(risk::RejectReason reason) noexcept {
  switch (reason) {
    case risk::RejectReason::kKillSwitchActive:
      return WireRejectReason::kKillSwitchActive;
    case risk::RejectReason::kInvalidPrice:
      return WireRejectReason::kInvalidPrice;
    case risk::RejectReason::kInvalidQuantity:
      return WireRejectReason::kInvalidQuantity;
    case risk::RejectReason::kDuplicateClientOrderId:
      return WireRejectReason::kDuplicateClientOrderId;
    case risk::RejectReason::kReconciliationRequired:
      return WireRejectReason::kReconciliationRequired;
    case risk::RejectReason::kAccountStateUnavailable:
      return WireRejectReason::kAccountStateUnavailable;
    case risk::RejectReason::kStaleAccountState:
      return WireRejectReason::kStaleAccountState;
    case risk::RejectReason::kAccountStateVersionRegressed:
      return WireRejectReason::kAccountStateVersionRegressed;
    case risk::RejectReason::kAccountStateChangedBeforeSubmit:
      return WireRejectReason::kAccountStateChangedBeforeSubmit;
    case risk::RejectReason::kMarketDataUnavailable:
      return WireRejectReason::kMarketDataUnavailable;
    case risk::RejectReason::kMarketDataChangedBeforeSubmit:
      return WireRejectReason::kMarketDataChangedBeforeSubmit;
    case risk::RejectReason::kStaleMarketData:
      return WireRejectReason::kStaleMarketData;
    case risk::RejectReason::kNotionalOverflow:
      return WireRejectReason::kNotionalOverflow;
    case risk::RejectReason::kMaxOrderNotionalExceeded:
      return WireRejectReason::kMaxOrderNotionalExceeded;
    case risk::RejectReason::kMaxMarketExposureExceeded:
      return WireRejectReason::kMaxMarketExposureExceeded;
    case risk::RejectReason::kMaxTotalExposureExceeded:
      return WireRejectReason::kMaxTotalExposureExceeded;
    case risk::RejectReason::kMaxLossSinceBaselineExceeded:
      return WireRejectReason::kMaxLossSinceBaselineExceeded;
    case risk::RejectReason::kCount:
      break;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<AgentState> ToAgentState(std::uint8_t raw) noexcept {
  switch (raw) {
    case 1:
      return AgentState::kReady;
    case 2:
      return AgentState::kKilled;
    case 3:
      return AgentState::kRecoveryRequired;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<WireGateState> ToGateState(std::uint8_t raw) noexcept {
  switch (raw) {
    case 1:
      return WireGateState::kReady;
    case 2:
      return WireGateState::kReconciliationRequired;
  }
  return std::nullopt;
}

}  // namespace

std::expected<FrameHeader, ProtocolError> DecodeHeader(std::span<const std::byte> frame) noexcept {
  if (frame.size() < kHeaderFrameSize)
    return std::unexpected(ProtocolError::kInvalidSize);
  if (Get<std::uint32_t>(frame, 0) != kProtocolMagic)
    return std::unexpected(ProtocolError::kInvalidMagic);
  if (Get<std::uint16_t>(frame, 4) != kProtocolVersion)
    return std::unexpected(ProtocolError::kUnsupportedVersion);
  const auto type = ToMessageType(Get<std::uint16_t>(frame, 6));
  if (!type)
    return std::unexpected(ProtocolError::kInvalidMessageType);
  return FrameHeader{.message_type = *type};
}

std::array<std::byte, kSubmitIntentFrameSize> EncodeSubmitIntent(const risk::OrderIntent& intent) noexcept {
  std::array<std::byte, kSubmitIntentFrameSize> frame{};
  PutHeader(frame, MessageType::kSubmitIntent);
  Put(frame, 8, intent.strategy_id);
  Put(frame, 16, intent.client_order_id);
  Put(frame, 24, intent.market_id);
  frame[32] = static_cast<std::byte>(intent.side == risk::Side::kBuy ? WireSide::kBuy : WireSide::kSell);
  Put(frame, 40, intent.price.micros_per_unit);
  Put(frame, 48, intent.quantity.microunits);
  return frame;
}

std::expected<risk::OrderIntent, ProtocolError> DecodeSubmitIntent(std::span<const std::byte> frame) noexcept {
  if (frame.size() != kSubmitIntentFrameSize)
    return std::unexpected(ProtocolError::kInvalidSize);
  const auto header = DecodeHeader(frame);
  if (!header)
    return std::unexpected(header.error());
  if (header->message_type != MessageType::kSubmitIntent)
    return std::unexpected(ProtocolError::kInvalidMessageType);
  for (std::size_t index = 33; index < 40; ++index) {
    if (frame[index] != std::byte{})
      return std::unexpected(ProtocolError::kInvalidReservedBytes);
  }
  const auto raw_side = std::to_integer<std::uint8_t>(frame[32]);
  risk::Side side{};
  if (raw_side == static_cast<std::uint8_t>(WireSide::kBuy))
    side = risk::Side::kBuy;
  else if (raw_side == static_cast<std::uint8_t>(WireSide::kSell))
    side = risk::Side::kSell;
  else
    return std::unexpected(ProtocolError::kInvalidSide);
  return risk::OrderIntent{
      .strategy_id = Get<risk::StrategyId>(frame, 8),
      .client_order_id = Get<risk::ClientOrderId>(frame, 16),
      .market_id = Get<risk::MarketId>(frame, 24),
      .side = side,
      .price = {.micros_per_unit = Get<std::int64_t>(frame, 40)},
      .quantity = {.microunits = Get<std::int64_t>(frame, 48)},
      .market_data_received_at = {},
  };
}

SubmitResponse MakeSubmitResponse(const execution::SubmissionResult& result) noexcept {
  std::uint64_t mask{};
  std::uint8_t count{};
  for (const auto reason : result.risk_decision.Reasons()) {
    if (const auto wire = ToWireReason(reason)) {
      mask |= std::uint64_t{1} << static_cast<std::uint8_t>(*wire);
      ++count;
    }
  }
  return SubmitResponse{
      .status = ToResponseStatus(result.status).value_or(ResponseStatus::kInternalError),
      .submit_outcome = ToWireOutcome(result.submit_outcome),
      .order_notional = result.risk_decision.OrderNotional(),
      .reject_reason_mask = mask,
      .reject_reason_count = count,
  };
}

std::array<std::byte, kSubmitResponseFrameSize> EncodeSubmitResponse(const SubmitResponse& response) noexcept {
  std::array<std::byte, kSubmitResponseFrameSize> frame{};
  PutHeader(frame, MessageType::kSubmitResponse);
  Put(frame, 8, static_cast<std::uint16_t>(response.status));
  frame[10] = static_cast<std::byte>(response.submit_outcome);
  frame[11] = static_cast<std::byte>(response.reject_reason_count);
  Put(frame, 16, response.order_notional.micros);
  Put(frame, 24, response.reject_reason_mask);
  return frame;
}

std::expected<SubmitResponse, ProtocolError> DecodeSubmitResponse(std::span<const std::byte> frame) noexcept {
  if (frame.size() != kSubmitResponseFrameSize)
    return std::unexpected(ProtocolError::kInvalidSize);
  const auto header = DecodeHeader(frame);
  if (!header)
    return std::unexpected(header.error());
  if (header->message_type != MessageType::kSubmitResponse)
    return std::unexpected(ProtocolError::kInvalidMessageType);
  for (std::size_t index = 12; index < 16; ++index) {
    if (frame[index] != std::byte{})
      return std::unexpected(ProtocolError::kInvalidReservedBytes);
  }
  const auto status = ToResponseStatus(Get<std::uint16_t>(frame, 8));
  if (!status)
    return std::unexpected(ProtocolError::kInvalidStatus);
  const auto outcome = ToWireOutcome(std::to_integer<std::uint8_t>(frame[10]));
  if (!outcome)
    return std::unexpected(ProtocolError::kInvalidSubmitOutcome);
  const auto reason_count = std::to_integer<std::uint8_t>(frame[11]);
  const auto reason_mask = Get<std::uint64_t>(frame, 24);
  const auto valid_reason_bits = (std::uint64_t{1} << static_cast<std::uint8_t>(WireRejectReason::kCount)) - 1U;
  if ((reason_mask & ~valid_reason_bits) != 0 || std::popcount(reason_mask) != reason_count) {
    return std::unexpected(ProtocolError::kInvalidRejectReasons);
  }
  return SubmitResponse{
      .status = *status,
      .submit_outcome = *outcome,
      .order_notional = {.micros = Get<std::int64_t>(frame, 16)},
      .reject_reason_mask = reason_mask,
      .reject_reason_count = reason_count,
  };
}

std::array<std::byte, kGetStatusFrameSize> EncodeGetStatus() noexcept {
  std::array<std::byte, kGetStatusFrameSize> frame{};
  PutHeader(frame, MessageType::kGetStatus);
  return frame;
}

std::array<std::byte, kKillFrameSize> EncodeKill() noexcept {
  std::array<std::byte, kKillFrameSize> frame{};
  PutHeader(frame, MessageType::kKill);
  return frame;
}

std::array<std::byte, kResumeFrameSize> EncodeResume() noexcept {
  std::array<std::byte, kResumeFrameSize> frame{};
  PutHeader(frame, MessageType::kResume);
  return frame;
}

std::expected<void, ProtocolError> DecodeEmptyRequest(std::span<const std::byte> frame, MessageType expected) noexcept {
  if (frame.size() != kHeaderFrameSize)
    return std::unexpected(ProtocolError::kInvalidSize);
  const auto header = DecodeHeader(frame);
  if (!header)
    return std::unexpected(header.error());
  if (header->message_type != expected)
    return std::unexpected(ProtocolError::kInvalidMessageType);
  return {};
}

std::array<std::byte, kStatusResponseFrameSize> EncodeStatusResponse(const StatusSnapshot& status, MessageType response_type) noexcept {
  std::array<std::byte, kStatusResponseFrameSize> frame{};
  PutHeader(frame, response_type);
  frame[8] = static_cast<std::byte>(status.agent_state);
  frame[9] = status.kill_switch_active ? std::byte{1} : std::byte{};
  frame[10] = static_cast<std::byte>(status.gate_state);
  Put(frame, 16, status.durable_order_count);
  Put(frame, 24, status.active_reservation_count);
  Put(frame, 32, status.total_reserved_exposure.micros);
  return frame;
}

std::expected<StatusSnapshot, ProtocolError> DecodeStatusResponse(std::span<const std::byte> frame, MessageType expected) noexcept {
  if (frame.size() != kStatusResponseFrameSize)
    return std::unexpected(ProtocolError::kInvalidSize);
  const auto header = DecodeHeader(frame);
  if (!header)
    return std::unexpected(header.error());
  if (header->message_type != expected || (expected != MessageType::kStatusResponse && expected != MessageType::kKillResponse)) {
    return std::unexpected(ProtocolError::kInvalidMessageType);
  }
  for (std::size_t index = 11; index < 16; ++index) {
    if (frame[index] != std::byte{})
      return std::unexpected(ProtocolError::kInvalidReservedBytes);
  }
  const auto state = ToAgentState(std::to_integer<std::uint8_t>(frame[8]));
  if (!state)
    return std::unexpected(ProtocolError::kInvalidAgentState);
  const auto raw_kill = std::to_integer<std::uint8_t>(frame[9]);
  if (raw_kill > 1)
    return std::unexpected(ProtocolError::kInvalidBoolean);
  const auto gate = ToGateState(std::to_integer<std::uint8_t>(frame[10]));
  if (!gate)
    return std::unexpected(ProtocolError::kInvalidGateState);
  return StatusSnapshot{
      .agent_state = *state,
      .kill_switch_active = raw_kill == 1,
      .gate_state = *gate,
      .durable_order_count = Get<std::uint64_t>(frame, 16),
      .active_reservation_count = Get<std::uint64_t>(frame, 24),
      .total_reserved_exposure = {.micros = Get<std::int64_t>(frame, 32)},
  };
}

std::array<std::byte, kResumeResponseFrameSize> EncodeResumeResponse(const ResumeResponse& response) noexcept {
  std::array<std::byte, kResumeResponseFrameSize> frame{};
  PutHeader(frame, MessageType::kResumeResponse);
  frame[8] = static_cast<std::byte>(response.result);
  frame[16] = static_cast<std::byte>(response.status.agent_state);
  frame[17] = response.status.kill_switch_active ? std::byte{1} : std::byte{};
  frame[18] = static_cast<std::byte>(response.status.gate_state);
  Put(frame, 24, response.status.durable_order_count);
  Put(frame, 32, response.status.active_reservation_count);
  Put(frame, 40, response.status.total_reserved_exposure.micros);
  return frame;
}

std::expected<ResumeResponse, ProtocolError> DecodeResumeResponse(std::span<const std::byte> frame) noexcept {
  if (frame.size() != kResumeResponseFrameSize)
    return std::unexpected(ProtocolError::kInvalidSize);
  const auto header = DecodeHeader(frame);
  if (!header)
    return std::unexpected(header.error());
  if (header->message_type != MessageType::kResumeResponse)
    return std::unexpected(ProtocolError::kInvalidMessageType);
  for (std::size_t index = 9; index < 16; ++index) {
    if (frame[index] != std::byte{})
      return std::unexpected(ProtocolError::kInvalidReservedBytes);
  }
  for (std::size_t index = 19; index < 24; ++index) {
    if (frame[index] != std::byte{})
      return std::unexpected(ProtocolError::kInvalidReservedBytes);
  }
  WireResumeResult result{};
  switch (std::to_integer<std::uint8_t>(frame[8])) {
    case 1:
      result = WireResumeResult::kReady;
      break;
    case 2:
      result = WireResumeResult::kAlreadyReady;
      break;
    case 3:
      result = WireResumeResult::kRecoveryFailed;
      break;
    case 4:
      result = WireResumeResult::kPersistenceFailed;
      break;
    default:
      return std::unexpected(ProtocolError::kInvalidResumeResult);
  }
  const auto state = ToAgentState(std::to_integer<std::uint8_t>(frame[16]));
  if (!state)
    return std::unexpected(ProtocolError::kInvalidAgentState);
  const auto raw_kill = std::to_integer<std::uint8_t>(frame[17]);
  if (raw_kill > 1)
    return std::unexpected(ProtocolError::kInvalidBoolean);
  const auto gate = ToGateState(std::to_integer<std::uint8_t>(frame[18]));
  if (!gate)
    return std::unexpected(ProtocolError::kInvalidGateState);
  return ResumeResponse{
      .result = result,
      .status =
          StatusSnapshot{
              .agent_state = *state,
              .kill_switch_active = raw_kill == 1,
              .gate_state = *gate,
              .durable_order_count = Get<std::uint64_t>(frame, 24),
              .active_reservation_count = Get<std::uint64_t>(frame, 32),
              .total_reserved_exposure = {.micros = Get<std::int64_t>(frame, 40)},
          },
  };
}

bool HasRejectReason(const SubmitResponse& response, WireRejectReason reason) noexcept {
  const auto bit = static_cast<std::uint8_t>(reason);
  return bit < static_cast<std::uint8_t>(WireRejectReason::kCount) && (response.reject_reason_mask & (std::uint64_t{1} << bit)) != 0;
}

}  // namespace botguard::agent
