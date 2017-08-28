#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include "common/mongo/bson_impl.h"
#include "common/mongo/codec_impl.h"
#include "common/mongo/proxy.h"
#include "common/stats/stats_impl.h"

#include "test/mocks/access_log/mocks.h"
#include "test/mocks/event/mocks.h"
#include "test/mocks/filesystem/mocks.h"
#include "test/mocks/network/mocks.h"
#include "test/mocks/runtime/mocks.h"
#include "test/test_common/printers.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace Envoy {
using testing::AnyNumber;
using testing::AtLeast;
using testing::Invoke;
using testing::NiceMock;
using testing::Return;
using testing::_;

namespace Mongo {

class MockDecoder : public Decoder {
public:
  MOCK_METHOD1(onData, void(Buffer::Instance& data));
};

class TestStatStore : public Stats::IsolatedStoreImpl {
public:
  MOCK_METHOD2(deliverHistogramToSinks, void(const std::string& name, uint64_t value));
  MOCK_METHOD2(deliverTimingToSinks, void(const std::string& name, std::chrono::milliseconds ms));
};

class TestProxyFilter : public ProxyFilter {
public:
  using ProxyFilter::ProxyFilter;

  // ProxyFilter
  DecoderPtr createDecoder(DecoderCallbacks& callbacks) override {
    callbacks_ = &callbacks;
    return DecoderPtr{decoder_};
  }

  MockDecoder* decoder_{new MockDecoder()};
  DecoderCallbacks* callbacks_{};
};

class MongoProxyFilterTest : public testing::Test {
public:
  MongoProxyFilterTest() { setup(); }

  void setup() {
    ON_CALL(runtime_.snapshot_, featureEnabled("mongo.proxy_enabled", 100))
        .WillByDefault(Return(true));
    ON_CALL(runtime_.snapshot_, featureEnabled("mongo.connection_logging_enabled", 100))
        .WillByDefault(Return(true));
    ON_CALL(runtime_.snapshot_, featureEnabled("mongo.logging_enabled", 100))
        .WillByDefault(Return(true));

    EXPECT_CALL(log_manager_, createAccessLog(_)).WillOnce(Return(file_));
    access_log_.reset(new AccessLog("test", log_manager_));
  }

  void initializeFilter() {
    filter_.reset(new TestProxyFilter("test.", store_, runtime_, access_log_, fault_config_));
    filter_->initializeReadFilterCallbacks(read_filter_callbacks_);
    filter_->onNewConnection();
  }

  void setupDelayFault(uint64_t duration_ms, uint32_t delay_percent, bool enable_fault) {
    const std::string json_fault_config_template = R"EOF(
    {{
      "fixed_delay": {{
        "percent": {},
        "duration_ms": {}
      }}
    }}
    )EOF";
    const std::string json_config =
        fmt::format(json_fault_config_template, delay_percent, duration_ms);
    Json::ObjectSharedPtr config = Json::Factory::loadFromString(json_config);

    fault_config_.reset(new FaultConfig(*config));

    EXPECT_CALL(runtime_.snapshot_, featureEnabled(_, _)).Times(AnyNumber());
    EXPECT_CALL(runtime_.snapshot_, featureEnabled("mongo.fault.delay.percent", delay_percent))
        .WillOnce(Return(enable_fault));

    if (enable_fault) {
      EXPECT_CALL(runtime_.snapshot_, getInteger("mongo.fault.delay.duration_ms", duration_ms))
          .WillOnce(Return(duration_ms));
    }
  }

  Buffer::OwnedImpl fake_data_;
  NiceMock<TestStatStore> store_;
  NiceMock<Runtime::MockLoader> runtime_;
  NiceMock<Event::MockDispatcher> dispatcher_;
  std::shared_ptr<Filesystem::MockFile> file_{new NiceMock<Filesystem::MockFile>()};
  AccessLogSharedPtr access_log_;
  FaultConfigSharedPtr fault_config_;
  std::unique_ptr<TestProxyFilter> filter_;
  NiceMock<Network::MockReadFilterCallbacks> read_filter_callbacks_;
  Envoy::AccessLog::MockAccessLogManager log_manager_;
};

TEST_F(MongoProxyFilterTest, DelayFaults) {
  const uint64_t delay = 10;
  setupDelayFault(delay, 50, true);
  initializeFilter();

  Event::MockTimer* delay_timer =
      new Event::MockTimer(&read_filter_callbacks_.connection_.dispatcher_);
  EXPECT_CALL(*delay_timer, enableTimer(std::chrono::milliseconds(delay)));
  EXPECT_CALL(*file_, write(_)).Times(AtLeast(1));

  EXPECT_CALL(*filter_->decoder_, onData(_)).WillOnce(Invoke([&](Buffer::Instance&) -> void {
    QueryMessagePtr message(new QueryMessageImpl(0, 0));
    message->fullCollectionName("db.test");
    message->flags(0b1110010);
    message->query(Bson::DocumentImpl::create());
    filter_->callbacks_->decodeQuery(std::move(message));
  }));

  EXPECT_EQ(Network::FilterStatus::StopIteration, filter_->onData(fake_data_));
  EXPECT_EQ(1U, store_.counter("test.op_query").value());

  // Requests during active delay.
  EXPECT_CALL(*filter_->decoder_, onData(_)).WillOnce(Invoke([&](Buffer::Instance&) -> void {
    QueryMessagePtr message(new QueryMessageImpl(0, 0));
    message->fullCollectionName("db.test");
    message->flags(0b1110010);
    message->query(Bson::DocumentImpl::create());
    filter_->callbacks_->decodeQuery(std::move(message));
  }));

  EXPECT_EQ(Network::FilterStatus::StopIteration, filter_->onData(fake_data_));
  EXPECT_EQ(2U, store_.counter("test.op_query").value());

  EXPECT_CALL(*filter_->decoder_, onData(_)).WillOnce(Invoke([&](Buffer::Instance&) -> void {
    GetMoreMessagePtr message(new GetMoreMessageImpl(0, 0));
    message->fullCollectionName("db.test");
    message->cursorId(1);
    filter_->callbacks_->decodeGetMore(std::move(message));
  }));
  EXPECT_EQ(Network::FilterStatus::StopIteration, filter_->onData(fake_data_));
  EXPECT_EQ(1U, store_.counter("test.op_get_more").value());

  EXPECT_CALL(read_filter_callbacks_, continueReading());
  delay_timer->callback_();
  EXPECT_EQ(1U, store_.counter("test.delays_injected").value());
}

TEST_F(MongoProxyFilterTest, DelayFaultsRuntimeDisabled) {
  setupDelayFault(10, 50, false);
  initializeFilter();

  EXPECT_CALL(dispatcher_, createTimer_(_)).Times(0);
  EXPECT_CALL(*file_, write(_)).Times(AtLeast(1));

  EXPECT_CALL(*filter_->decoder_, onData(_)).WillOnce(Invoke([&](Buffer::Instance&) -> void {
    QueryMessagePtr message(new QueryMessageImpl(0, 0));
    message->fullCollectionName("db.test");
    message->flags(0b1110010);
    message->query(Bson::DocumentImpl::create());
    filter_->callbacks_->decodeQuery(std::move(message));
  }));

  EXPECT_EQ(Network::FilterStatus::Continue, filter_->onData(fake_data_));
  EXPECT_EQ(0U, store_.counter("test.delays_injected").value());
}

TEST_F(MongoProxyFilterTest, Stats) {
  initializeFilter();

  EXPECT_CALL(*file_, write(_)).Times(AtLeast(1));

  EXPECT_CALL(*filter_->decoder_, onData(_)).WillOnce(Invoke([&](Buffer::Instance&) -> void {
    QueryMessagePtr message(new QueryMessageImpl(0, 0));
    message->fullCollectionName("db.test");
    message->flags(0b1110010);
    message->query(Bson::DocumentImpl::create());
    filter_->callbacks_->decodeQuery(std::move(message));
  }));
  filter_->onData(fake_data_);

  EXPECT_CALL(store_, deliverHistogramToSinks("test.collection.test.query.reply_num_docs", 1));
  EXPECT_CALL(store_, deliverHistogramToSinks("test.collection.test.query.reply_size", 22));
  EXPECT_CALL(store_, deliverTimingToSinks("test.collection.test.query.reply_time_ms", _));

  EXPECT_CALL(*filter_->decoder_, onData(_)).WillOnce(Invoke([&](Buffer::Instance&) -> void {
    ReplyMessagePtr message(new ReplyMessageImpl(0, 0));
    message->flags(0b11);
    message->cursorId(1);
    message->documents().push_back(Bson::DocumentImpl::create()->addString("hello", "world"));
    filter_->callbacks_->decodeReply(std::move(message));

  }));
  filter_->onWrite(fake_data_);

  EXPECT_EQ(1U, store_.counter("test.op_query").value());
  EXPECT_EQ(1U, store_.counter("test.op_query_tailable_cursor").value());
  EXPECT_EQ(1U, store_.counter("test.op_query_no_cursor_timeout").value());
  EXPECT_EQ(1U, store_.counter("test.op_query_await_data").value());
  EXPECT_EQ(1U, store_.counter("test.op_query_exhaust").value());
  EXPECT_EQ(1U, store_.counter("test.op_query_no_max_time").value());
  EXPECT_EQ(1U, store_.counter("test.op_query_scatter_get").value());

  EXPECT_EQ(1U, store_.counter("test.collection.test.query.total").value());
  EXPECT_EQ(1U, store_.counter("test.collection.test.query.scatter_get").value());

  EXPECT_EQ(1U, store_.counter("test.op_reply").value());
  EXPECT_EQ(1U, store_.counter("test.op_reply_cursor_not_found").value());
  EXPECT_EQ(1U, store_.counter("test.op_reply_query_failure").value());
  EXPECT_EQ(1U, store_.counter("test.op_reply_valid_cursor").value());

  EXPECT_CALL(*filter_->decoder_, onData(_)).WillOnce(Invoke([&](Buffer::Instance&) -> void {
    GetMoreMessagePtr message(new GetMoreMessageImpl(0, 0));
    message->fullCollectionName("db.test");
    message->cursorId(1);
    filter_->callbacks_->decodeGetMore(std::move(message));
  }));
  filter_->onData(fake_data_);

  EXPECT_CALL(*filter_->decoder_, onData(_)).WillOnce(Invoke([&](Buffer::Instance&) -> void {
    InsertMessagePtr message(new InsertMessageImpl(0, 0));
    message->fullCollectionName("db.test");
    message->documents().push_back(Bson::DocumentImpl::create());
    filter_->callbacks_->decodeInsert(std::move(message));
  }));
  filter_->onData(fake_data_);

  EXPECT_CALL(*filter_->decoder_, onData(_)).WillOnce(Invoke([&](Buffer::Instance&) -> void {
    KillCursorsMessagePtr message(new KillCursorsMessageImpl(0, 0));
    message->numberOfCursorIds(1);
    message->cursorIds({1});
    filter_->callbacks_->decodeKillCursors(std::move(message));
  }));
  filter_->onData(fake_data_);

  EXPECT_EQ(1U, store_.counter("test.op_get_more").value());
  EXPECT_EQ(1U, store_.counter("test.op_insert").value());
  EXPECT_EQ(1U, store_.counter("test.op_kill_cursors").value());
  EXPECT_EQ(0U, store_.counter("test.delays_injected").value());
}

TEST_F(MongoProxyFilterTest, CommandStats) {
  initializeFilter();

  EXPECT_CALL(*filter_->decoder_, onData(_)).WillOnce(Invoke([&](Buffer::Instance&) -> void {
    QueryMessagePtr message(new QueryMessageImpl(0, 0));
    message->fullCollectionName("db.$cmd");
    message->flags(0b1110010);
    message->query(Bson::DocumentImpl::create()->addString("foo", "bar"));
    filter_->callbacks_->decodeQuery(std::move(message));
  }));
  filter_->onData(fake_data_);

  EXPECT_CALL(store_, deliverHistogramToSinks("test.cmd.foo.reply_num_docs", 1));
  EXPECT_CALL(store_, deliverHistogramToSinks("test.cmd.foo.reply_size", 22));
  EXPECT_CALL(store_, deliverTimingToSinks("test.cmd.foo.reply_time_ms", _));

  EXPECT_CALL(*filter_->decoder_, onData(_)).WillOnce(Invoke([&](Buffer::Instance&) -> void {
    ReplyMessagePtr message(new ReplyMessageImpl(0, 0));
    message->flags(0b11);
    message->cursorId(1);
    message->documents().push_back(Bson::DocumentImpl::create()->addString("hello", "world"));
    filter_->callbacks_->decodeReply(std::move(message));
  }));
  filter_->onWrite(fake_data_);

  EXPECT_EQ(1U, store_.counter("test.cmd.foo.total").value());
}

TEST_F(MongoProxyFilterTest, CallingFunctionStats) {
  initializeFilter();

  std::string json = R"EOF(
    {
      "hostname":"api-production-iad-canary",
      "httpUniqueId":"VqqX7H8AAQEAAE@8EUkAAAAR",
      "callingFunction":"getByMongoId"
    }
  )EOF";

  EXPECT_CALL(*filter_->decoder_, onData(_)).WillOnce(Invoke([&](Buffer::Instance&) -> void {
    QueryMessagePtr message(new QueryMessageImpl(0, 0));
    message->fullCollectionName("db.test");
    message->flags(0b1110010);
    message->query(Bson::DocumentImpl::create()->addString("$comment", std::move(json)));
    filter_->callbacks_->decodeQuery(std::move(message));
  }));
  filter_->onData(fake_data_);

  EXPECT_EQ(1U, store_.counter("test.collection.test.query.total").value());
  EXPECT_EQ(1U, store_.counter("test.collection.test.query.scatter_get").value());
  EXPECT_EQ(1U, store_.counter("test.collection.test.callsite.getByMongoId.query.total").value());
  EXPECT_EQ(1U,
            store_.counter("test.collection.test.callsite.getByMongoId.query.scatter_get").value());

  EXPECT_CALL(store_, deliverHistogramToSinks("test.collection.test.query.reply_num_docs", 1));
  EXPECT_CALL(store_, deliverHistogramToSinks("test.collection.test.query.reply_size", 22));
  EXPECT_CALL(store_, deliverTimingToSinks("test.collection.test.query.reply_time_ms", _));
  EXPECT_CALL(store_, deliverHistogramToSinks(
                          "test.collection.test.callsite.getByMongoId.query.reply_num_docs", 1));
  EXPECT_CALL(store_, deliverHistogramToSinks(
                          "test.collection.test.callsite.getByMongoId.query.reply_size", 22));
  EXPECT_CALL(store_, deliverTimingToSinks(
                          "test.collection.test.callsite.getByMongoId.query.reply_time_ms", _));

  EXPECT_CALL(*filter_->decoder_, onData(_)).WillOnce(Invoke([&](Buffer::Instance&) -> void {
    ReplyMessagePtr message(new ReplyMessageImpl(0, 0));
    message->flags(0b11);
    message->cursorId(1);
    message->documents().push_back(Bson::DocumentImpl::create()->addString("hello", "world"));
    filter_->callbacks_->decodeReply(std::move(message));

  }));
  filter_->onWrite(fake_data_);
}

TEST_F(MongoProxyFilterTest, MultiGet) {
  initializeFilter();

  EXPECT_CALL(*filter_->decoder_, onData(_)).WillOnce(Invoke([&](Buffer::Instance&) -> void {
    QueryMessagePtr message(new QueryMessageImpl(0, 0));
    message->fullCollectionName("db.test");
    message->flags(0b1110010);
    message->query(Bson::DocumentImpl::create()->addDocument(
        "_id", Bson::DocumentImpl::create()->addArray("$in", Bson::DocumentImpl::create())));
    filter_->callbacks_->decodeQuery(std::move(message));
  }));
  filter_->onData(fake_data_);

  EXPECT_EQ(1U, store_.counter("test.op_query_multi_get").value());
  EXPECT_EQ(1U, store_.counter("test.collection.test.query.multi_get").value());
}

TEST_F(MongoProxyFilterTest, MaxTime) {
  initializeFilter();

  EXPECT_CALL(*filter_->decoder_, onData(_)).WillOnce(Invoke([&](Buffer::Instance&) -> void {
    QueryMessagePtr message(new QueryMessageImpl(0, 0));
    message->fullCollectionName("db.test");
    message->flags(0b1110010);
    message->query(Bson::DocumentImpl::create()->addInt32("$maxTimeMS", 100));
    filter_->callbacks_->decodeQuery(std::move(message));
  }));
  filter_->onData(fake_data_);

  EXPECT_EQ(0U, store_.counter("test.op_query_no_max_time").value());
}

TEST_F(MongoProxyFilterTest, DecodeError) {
  initializeFilter();

  EXPECT_CALL(*filter_->decoder_, onData(_)).WillOnce(Invoke([&](Buffer::Instance&) -> void {
    throw EnvoyException("bad decode");
  }));
  filter_->onData(fake_data_);

  // Should not call decode again.
  filter_->onData(fake_data_);

  EXPECT_EQ(1U, store_.counter("test.decoding_error").value());
}

TEST_F(MongoProxyFilterTest, ConcurrentQuery) {
  initializeFilter();

  EXPECT_CALL(*filter_->decoder_, onData(_)).WillOnce(Invoke([&](Buffer::Instance&) -> void {
    QueryMessagePtr message(new QueryMessageImpl(1, 0));
    message->fullCollectionName("db.test");
    message->flags(0b1110010);
    message->query(Bson::DocumentImpl::create());
    filter_->callbacks_->decodeQuery(std::move(message));

    message.reset(new QueryMessageImpl(2, 0));
    message->fullCollectionName("db.test");
    message->flags(0b1110010);
    message->query(Bson::DocumentImpl::create());
    filter_->callbacks_->decodeQuery(std::move(message));
  }));
  filter_->onData(fake_data_);
  EXPECT_EQ(2U, store_.gauge("test.op_query_active").value());

  EXPECT_CALL(*filter_->decoder_, onData(_)).WillOnce(Invoke([&](Buffer::Instance&) -> void {
    ReplyMessagePtr message(new ReplyMessageImpl(0, 1));
    message->flags(0b11);
    message->cursorId(1);
    message->documents().push_back(Bson::DocumentImpl::create()->addString("hello", "world"));
    filter_->callbacks_->decodeReply(std::move(message));

    message.reset(new ReplyMessageImpl(0, 2));
    message->flags(0b11);
    message->cursorId(1);
    message->documents().push_back(Bson::DocumentImpl::create()->addString("hello", "world"));
    filter_->callbacks_->decodeReply(std::move(message));
  }));
  filter_->onWrite(fake_data_);
  EXPECT_EQ(0U, store_.gauge("test.op_query_active").value());
}

TEST_F(MongoProxyFilterTest, EmptyActiveQueryList) {
  initializeFilter();

  EXPECT_CALL(*filter_->decoder_, onData(_)).WillOnce(Invoke([&](Buffer::Instance&) -> void {
    QueryMessagePtr message(new QueryMessageImpl(0, 0));
    message->fullCollectionName("db.$cmd");
    message->flags(0b1110010);
    message->query(Bson::DocumentImpl::create()->addString("foo", "bar"));
    filter_->callbacks_->decodeQuery(std::move(message));
  }));
  filter_->onData(fake_data_);

  EXPECT_CALL(*filter_->decoder_, onData(_)).WillOnce(Invoke([&](Buffer::Instance&) -> void {
    ReplyMessagePtr message(new ReplyMessageImpl(0, 0));
    message->flags(0b11);
    message->cursorId(1);
    message->documents().push_back(Bson::DocumentImpl::create()->addString("hello", "world"));
    filter_->callbacks_->decodeReply(std::move(message));
  }));
  filter_->onWrite(fake_data_);
  read_filter_callbacks_.connection_.raiseEvent(Network::ConnectionEvent::RemoteClose);
  EXPECT_EQ(0U, store_.counter("test.cx_destroy_local_with_active_rq").value());
  EXPECT_EQ(0U, store_.counter("test.cx_destroy_remote_with_active_rq").value());
}

TEST_F(MongoProxyFilterTest, ConnectionDestroyLocal) {
  initializeFilter();

  EXPECT_CALL(*filter_->decoder_, onData(_)).WillOnce(Invoke([&](Buffer::Instance&) -> void {
    QueryMessagePtr message(new QueryMessageImpl(0, 0));
    message->fullCollectionName("db.test");
    message->flags(0b1110010);
    message->query(Bson::DocumentImpl::create()->addDocument(
        "_id", Bson::DocumentImpl::create()->addArray("$in", Bson::DocumentImpl::create())));
    filter_->callbacks_->decodeQuery(std::move(message));
  }));
  filter_->onData(fake_data_);

  read_filter_callbacks_.connection_.raiseEvent(Network::ConnectionEvent::RemoteClose);
  EXPECT_EQ(1U, store_.counter("test.cx_destroy_local_with_active_rq").value());
  EXPECT_EQ(0U, store_.counter("test.cx_destroy_remote_with_active_rq").value());
}

TEST_F(MongoProxyFilterTest, ConnectionDestroyRemote) {
  initializeFilter();

  EXPECT_CALL(*filter_->decoder_, onData(_)).WillOnce(Invoke([&](Buffer::Instance&) -> void {
    QueryMessagePtr message(new QueryMessageImpl(0, 0));
    message->fullCollectionName("db.test");
    message->flags(0b1110010);
    message->query(Bson::DocumentImpl::create()->addDocument(
        "_id", Bson::DocumentImpl::create()->addArray("$in", Bson::DocumentImpl::create())));
    filter_->callbacks_->decodeQuery(std::move(message));
  }));
  filter_->onData(fake_data_);

  read_filter_callbacks_.connection_.raiseEvent(Network::ConnectionEvent::LocalClose);
  EXPECT_EQ(1U, store_.counter("test.cx_destroy_remote_with_active_rq").value());
  EXPECT_EQ(0U, store_.counter("test.cx_destroy_local_with_active_rq").value());
}

} // namespace Mongo
} // namespace Envoy
