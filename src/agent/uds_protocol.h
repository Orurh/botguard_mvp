#pragma once

#include "execution/order_submission_coordinator.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>

namespace botguard::agent {

inline constexpr std::uint32_t kProtocolMagic = 0x42475544U;  // "BGUD"
inline constexpr std::uint16_t kProtocolVersion = 1;
inline constexpr std::size_t kHeaderFrameSize = 8;
inline constexpr std::size_t kSubmitIntentFrameSize = 56;
inline constexpr std::size_t kSubmitResponseFrameSize = 32;
inline constexpr std::size_t kGetStatusFrameSize = kHeaderFrameSize;
inline constexpr std::size_t kKillFrameSize = kHeaderFrameSize;
inline constexpr std::size_t kStatusResponseFrameSize = 40;
inline constexpr std::size_t kResumeFrameSize = kHeaderFrameSize;
inline constexpr std::size_t kResumeResponseFrameSize = 48;
inline constexpr std::size_t kMaxRequestFrameSize = 256;

enum class MessageType : std::uint16_t {
  kSubmitIntent = 1,
  kSubmitResponse = 2,
  kGetStatus = 3,
  kStatusResponse = 4,
  kKill = 5,
  kKillResponse = 6,
  kResume = 7,
  kResumeResponse = 8,
};

// Wire values are explicit protocol-v1 ABI and must never be renumbered.
enum class WireSide : std::uint8_t {
  kBuy = 1,
  kSell = 2,
};

enum class WireSubmitOutcome : std::uint8_t {
  kNone = 0,
  kOpen = 1,
  kFilled = 2,
  kRejected = 3,
  kUnknown = 4,
};

enum class WireRejectReason : std::uint8_t {
  kKillSwitchActive = 0,
  kInvalidPrice = 1,
  kInvalidQuantity = 2,
  kDuplicateClientOrderId = 3,
  kReconciliationRequired = 4,
  kAccountStateUnavailable = 5,
  kStaleAccountState = 6,
  kAccountStateVersionRegressed = 7,
  kAccountStateChangedBeforeSubmit = 8,
  kMarketDataUnavailable = 9,
  kMarketDataChangedBeforeSubmit = 10,
  kStaleMarketData = 11,
  kNotionalOverflow = 12,
  kMaxOrderNotionalExceeded = 13,
  kMaxMarketExposureExceeded = 14,
  kMaxTotalExposureExceeded = 15,
  // Numeric value 16 is retained from protocol-v1's former daily-loss label.
  kMaxLossSinceBaselineExceeded = 16,
  kCount = 17,
};

enum class ResponseStatus : std::uint16_t {
  kRiskRejected = 1,
  kVenuePreflightRejected = 2,
  kPreSubmitPersistenceFailed = 3,
  kAbortedBeforeSubmit = 4,
  kPreSubmitAbortPersistenceFailed = 5,
  kSubmitted = 6,
  kPostSubmitPersistenceFailed = 7,
  kRegistryTransitionFailed = 8,
  kInvalidRequest = 100,
  kInternalError = 101,
};

enum class AgentState : std::uint8_t {
  kReady = 1,
  kKilled = 2,
  kRecoveryRequired = 3,
};

enum class WireGateState : std::uint8_t {
  kReady = 1,
  kReconciliationRequired = 2,
};

enum class WireResumeResult : std::uint8_t {
  kReady = 1,
  kAlreadyReady = 2,
  kRecoveryFailed = 3,
  kPersistenceFailed = 4,
};

enum class ProtocolError : std::uint8_t {
  kInvalidSize,
  kInvalidMagic,
  kUnsupportedVersion,
  kInvalidMessageType,
  kInvalidReservedBytes,
  kInvalidSide,
  kInvalidStatus,
  kInvalidSubmitOutcome,
  kInvalidRejectReasons,
  kInvalidAgentState,
  kInvalidGateState,
  kInvalidBoolean,
  kInvalidResumeResult,
};

struct FrameHeader {
  MessageType message_type{MessageType::kSubmitIntent};
};

struct SubmitResponse {
  ResponseStatus status{ResponseStatus::kInternalError};
  WireSubmitOutcome submit_outcome{WireSubmitOutcome::kNone};
  risk::Money order_notional{};
  std::uint64_t reject_reason_mask{};
  std::uint8_t reject_reason_count{};
};

struct StatusSnapshot {
  AgentState agent_state{AgentState::kRecoveryRequired};
  bool kill_switch_active{true};
  WireGateState gate_state{WireGateState::kReconciliationRequired};
  std::uint64_t durable_order_count{};
  std::uint64_t active_reservation_count{};
  risk::Money total_reserved_exposure{};
};

struct ResumeResponse {
  WireResumeResult result{WireResumeResult::kRecoveryFailed};
  StatusSnapshot status{};
};

[[nodiscard]] std::expected<FrameHeader, ProtocolError> DecodeHeader(std::span<const std::byte> frame) noexcept;

[[nodiscard]] std::array<std::byte, kSubmitIntentFrameSize> EncodeSubmitIntent(const risk::OrderIntent& intent) noexcept;
[[nodiscard]] std::expected<risk::OrderIntent, ProtocolError> DecodeSubmitIntent(std::span<const std::byte> frame) noexcept;

[[nodiscard]] SubmitResponse MakeSubmitResponse(const execution::SubmissionResult& result) noexcept;
[[nodiscard]] std::array<std::byte, kSubmitResponseFrameSize> EncodeSubmitResponse(const SubmitResponse& response) noexcept;
[[nodiscard]] std::expected<SubmitResponse, ProtocolError> DecodeSubmitResponse(std::span<const std::byte> frame) noexcept;

[[nodiscard]] std::array<std::byte, kGetStatusFrameSize> EncodeGetStatus() noexcept;
[[nodiscard]] std::array<std::byte, kKillFrameSize> EncodeKill() noexcept;
[[nodiscard]] std::array<std::byte, kResumeFrameSize> EncodeResume() noexcept;
[[nodiscard]] std::expected<void, ProtocolError> DecodeEmptyRequest(std::span<const std::byte> frame, MessageType expected) noexcept;

[[nodiscard]] std::array<std::byte, kStatusResponseFrameSize> EncodeStatusResponse(const StatusSnapshot& status,
                                                                                   MessageType response_type) noexcept;
[[nodiscard]] std::expected<StatusSnapshot, ProtocolError> DecodeStatusResponse(std::span<const std::byte> frame,
                                                                                MessageType expected) noexcept;

[[nodiscard]] std::array<std::byte, kResumeResponseFrameSize> EncodeResumeResponse(const ResumeResponse& response) noexcept;
[[nodiscard]] std::expected<ResumeResponse, ProtocolError> DecodeResumeResponse(std::span<const std::byte> frame) noexcept;

[[nodiscard]] bool HasRejectReason(const SubmitResponse& response, WireRejectReason reason) noexcept;

}  // namespace botguard::agent
