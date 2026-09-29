#include "demo/simulator_runtime.h"
#include "execution/trading_day_context.h"

#include <gtest/gtest.h>

#include <sqlite3.h>

#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

namespace {

namespace agent = botguard::agent;
namespace demo = botguard::demo;
namespace execution = botguard::execution;
namespace risk = botguard::risk;

class TemporarySimulatorFiles final {
 public:
  TemporarySimulatorFiles() {
    static std::atomic_uint64_t sequence{};
    root_ = std::filesystem::temp_directory_path() /
            ("botguard_runtime_day_" + std::to_string(::getpid()) + "_" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" + std::to_string(sequence.fetch_add(1)));
    std::error_code error;
    created_ = std::filesystem::create_directory(root_, error) && !error;
  }

  ~TemporarySimulatorFiles() {
    if (created_) {
      std::error_code error;
      static_cast<void>(std::filesystem::remove_all(root_, error));
    }
  }

  [[nodiscard]] bool Created() const noexcept { return created_; }
  [[nodiscard]] std::filesystem::path Orders() const { return root_ / "orders.db"; }
  [[nodiscard]] std::filesystem::path Runtime() const { return root_ / "orders.db.runtime"; }
  [[nodiscard]] std::filesystem::path Venue() const { return root_ / "venue.db"; }

 private:
  std::filesystem::path root_;
  bool created_{};
};

class MutableSafetyInputs final : public demo::SimulatorRuntimeSafetyInputs {
 public:
  std::optional<std::int64_t> day{20'000};
  std::optional<risk::Money> equity{risk::Money::FromWholeUsd(1'000)};
  std::deque<std::optional<risk::Money>> scripted_equity;

  [[nodiscard]] std::optional<std::int64_t> CurrentUtcDay() noexcept override { return day; }

  [[nodiscard]] std::optional<risk::Money> ObserveCurrentEquity() noexcept override {
    if (scripted_equity.empty()) {
      return equity;
    }
    auto result = scripted_equity.front();
    scripted_equity.pop_front();
    return result;
  }
};

[[nodiscard]] risk::OrderIntent Intent(risk::ClientOrderId client_order_id) noexcept {
  return risk::OrderIntent{
      .strategy_id = 1,
      .client_order_id = client_order_id,
      .market_id = 10,
      .side = risk::Side::kBuy,
      .price = risk::Price::FromCents(25),
      .quantity = risk::Quantity::FromWhole(4),
      .market_data_received_at = {},
  };
}

[[nodiscard]] std::expected<std::unique_ptr<demo::SimulatorRuntime>, demo::SimulatorRuntimeError> CreateRuntime(
    const TemporarySimulatorFiles& files, const std::shared_ptr<MutableSafetyInputs>& inputs, bool crash_after_baseline = false) {
  return demo::SimulatorRuntime::Create({
      .order_database_path = files.Orders(),
      .venue_database_path = files.Venue(),
      .lose_submit_response = false,
      .safety_inputs = inputs,
      .crash_after_rollover_baseline_persisted = crash_after_baseline,
  });
}

[[nodiscard]] std::optional<execution::TradingDayContext> ReadContext(const std::filesystem::path& path) {
  sqlite3* database{};
  const auto value = path.string();
  if (sqlite3_open_v2(value.c_str(), &database, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK) {
    if (database != nullptr) {
      static_cast<void>(sqlite3_close(database));
    }
    return std::nullopt;
  }
  sqlite3_stmt* statement{};
  const auto prepared =
      sqlite3_prepare_v2(database, "SELECT trading_day_utc, opening_equity_micros FROM daily_equity_baseline WHERE singleton = 1;", -1,
                         &statement, nullptr) == SQLITE_OK;
  std::optional<execution::TradingDayContext> context;
  if (prepared && sqlite3_step(statement) == SQLITE_ROW && sqlite3_column_type(statement, 0) == SQLITE_INTEGER &&
      sqlite3_column_type(statement, 1) == SQLITE_INTEGER) {
    context = execution::TradingDayContext{
        .utc_day = sqlite3_column_int64(statement, 0),
        .baseline_equity = risk::Money{.micros = sqlite3_column_int64(statement, 1)},
    };
  }
  if (statement != nullptr) {
    static_cast<void>(sqlite3_finalize(statement));
  }
  static_cast<void>(sqlite3_close(database));
  return context;
}

TEST(SimulatorTradingDayTest, SameDaySubmitUsesFixedDurableBaseline) {
  TemporarySimulatorFiles files;
  ASSERT_TRUE(files.Created());
  auto inputs = std::make_shared<MutableSafetyInputs>();
  auto runtime = CreateRuntime(files, inputs);
  ASSERT_TRUE(runtime.has_value());
  ASSERT_EQ((*runtime)->GetStatus().agent_state, agent::AgentState::kReady);

  inputs->equity = risk::Money::FromWholeUsd(950);
  const auto submitted = (*runtime)->HandleSubmit(Intent(1001));
  EXPECT_EQ(submitted.status, execution::SubmissionStatus::kSubmitted);
  const auto context = ReadContext(files.Runtime());
  ASSERT_TRUE(context.has_value());
  EXPECT_EQ(context->utc_day, 20'000);
  EXPECT_EQ(context->baseline_equity, risk::Money::FromWholeUsd(1'000));
}

TEST(SimulatorTradingDayTest, SuccessfulRolloverPublishesFreshCapabilityAndCannotReplaceNewBaseline) {
  TemporarySimulatorFiles files;
  ASSERT_TRUE(files.Created());
  auto inputs = std::make_shared<MutableSafetyInputs>();
  auto runtime = CreateRuntime(files, inputs);
  ASSERT_TRUE(runtime.has_value());

  inputs->day = 20'001;
  inputs->equity = risk::Money::FromWholeUsd(900);
  const auto first = (*runtime)->HandleSubmit(Intent(1001));
  EXPECT_EQ(first.status, execution::SubmissionStatus::kSubmitted);
  EXPECT_EQ((*runtime)->GetStatus().agent_state, agent::AgentState::kReady);

  inputs->equity = risk::Money::FromWholeUsd(850);
  const auto second = (*runtime)->HandleSubmit(Intent(1002));
  EXPECT_EQ(second.status, execution::SubmissionStatus::kSubmitted);
  const auto context = ReadContext(files.Runtime());
  ASSERT_TRUE(context.has_value());
  EXPECT_EQ(context->utc_day, 20'001);
  EXPECT_EQ(context->baseline_equity, risk::Money::FromWholeUsd(900));
  EXPECT_EQ((*runtime)->VenueSubmitCount(), 2U);
}

TEST(SimulatorTradingDayTest, BaselinePersistenceFailureClosesAdmissionWithoutVenueSubmit) {
  TemporarySimulatorFiles files;
  ASSERT_TRUE(files.Created());
  auto inputs = std::make_shared<MutableSafetyInputs>();
  auto runtime = CreateRuntime(files, inputs);
  ASSERT_TRUE(runtime.has_value());

  sqlite3* lock{};
  const auto runtime_path = files.Runtime().string();
  ASSERT_EQ(sqlite3_open_v2(runtime_path.c_str(), &lock, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, nullptr), SQLITE_OK);
  ASSERT_EQ(sqlite3_exec(lock, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr), SQLITE_OK);
  inputs->day = 20'001;
  inputs->equity = risk::Money::FromWholeUsd(900);
  const auto blocked = (*runtime)->HandleSubmit(Intent(1001));
  EXPECT_EQ(blocked.status, execution::SubmissionStatus::kRiskRejected);
  EXPECT_TRUE(blocked.risk_decision.HasReason(risk::RejectReason::kReconciliationRequired));
  EXPECT_EQ((*runtime)->GetStatus().agent_state, agent::AgentState::kRecoveryRequired);
  EXPECT_EQ((*runtime)->VenueSubmitCount(), 0U);
  ASSERT_EQ(sqlite3_exec(lock, "ROLLBACK;", nullptr, nullptr, nullptr), SQLITE_OK);
  ASSERT_EQ(sqlite3_close(lock), SQLITE_OK);

  const auto resumed = (*runtime)->Resume();
  EXPECT_EQ(resumed.result, agent::WireResumeResult::kReady);
  const auto context = ReadContext(files.Runtime());
  ASSERT_TRUE(context.has_value());
  EXPECT_EQ(context->utc_day, 20'001);
}

TEST(SimulatorTradingDayTest, NewDayAccountRefreshFailureLeavesDurableBaselineButNoCapability) {
  TemporarySimulatorFiles files;
  ASSERT_TRUE(files.Created());
  auto inputs = std::make_shared<MutableSafetyInputs>();
  auto runtime = CreateRuntime(files, inputs);
  ASSERT_TRUE(runtime.has_value());

  inputs->day = 20'001;
  inputs->equity = risk::Money::FromWholeUsd(900);
  inputs->scripted_equity = {risk::Money::FromWholeUsd(900), std::nullopt};
  const auto blocked = (*runtime)->HandleSubmit(Intent(1001));
  EXPECT_EQ(blocked.status, execution::SubmissionStatus::kRiskRejected);
  EXPECT_EQ((*runtime)->GetStatus().agent_state, agent::AgentState::kRecoveryRequired);
  EXPECT_EQ((*runtime)->VenueSubmitCount(), 0U);
  const auto durable = ReadContext(files.Runtime());
  ASSERT_TRUE(durable.has_value());
  EXPECT_EQ(durable->utc_day, 20'001);
  EXPECT_EQ(durable->baseline_equity, risk::Money::FromWholeUsd(900));

  const auto resumed = (*runtime)->Resume();
  EXPECT_EQ(resumed.result, agent::WireResumeResult::kReady);
}

TEST(SimulatorTradingDayTest, UtcDayRegressionFailsClosedWithoutVenueSubmit) {
  TemporarySimulatorFiles files;
  ASSERT_TRUE(files.Created());
  auto inputs = std::make_shared<MutableSafetyInputs>();
  auto runtime = CreateRuntime(files, inputs);
  ASSERT_TRUE(runtime.has_value());

  inputs->day = 19'999;
  const auto blocked = (*runtime)->HandleSubmit(Intent(1001));
  EXPECT_EQ(blocked.status, execution::SubmissionStatus::kRiskRejected);
  EXPECT_EQ((*runtime)->GetStatus().agent_state, agent::AgentState::kRecoveryRequired);
  EXPECT_EQ((*runtime)->VenueSubmitCount(), 0U);
}

TEST(SimulatorTradingDayTest, ProcessExitAfterDurableRotationRestartsFromNewBaselineWithoutBlindSubmit) {
  TemporarySimulatorFiles files;
  ASSERT_TRUE(files.Created());
  {
    auto inputs = std::make_shared<MutableSafetyInputs>();
    auto runtime = CreateRuntime(files, inputs);
    ASSERT_TRUE(runtime.has_value());
  }

  const auto child = ::fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    auto inputs = std::make_shared<MutableSafetyInputs>();
    inputs->day = 20'001;
    inputs->equity = risk::Money::FromWholeUsd(900);
    static_cast<void>(CreateRuntime(files, inputs, true));
    ::_exit(87);
  }
  int status{};
  ASSERT_EQ(::waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFSIGNALED(status));
  ASSERT_EQ(WTERMSIG(status), SIGKILL);

  const auto durable = ReadContext(files.Runtime());
  ASSERT_TRUE(durable.has_value());
  EXPECT_EQ(durable->utc_day, 20'001);
  EXPECT_EQ(durable->baseline_equity, risk::Money::FromWholeUsd(900));

  auto restarted_inputs = std::make_shared<MutableSafetyInputs>();
  restarted_inputs->day = 20'001;
  restarted_inputs->equity = risk::Money::FromWholeUsd(900);
  auto restarted = CreateRuntime(files, restarted_inputs);
  ASSERT_TRUE(restarted.has_value());
  EXPECT_EQ((*restarted)->GetStatus().agent_state, agent::AgentState::kReady);
  EXPECT_EQ((*restarted)->VenueSubmitCount(), 0U);
  EXPECT_EQ((*restarted)->HandleSubmit(Intent(1001)).status, execution::SubmissionStatus::kSubmitted);
  EXPECT_EQ((*restarted)->VenueSubmitCount(), 1U);
}

}  // namespace
