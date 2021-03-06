#include "common/buffer/buffer_impl.h"
#include "common/buffer/watermark_buffer.h"

#include "gtest/gtest.h"

namespace Envoy {
namespace Buffer {
namespace {

const char TEN_BYTES[] = "0123456789";

class WatermarkBufferTest : public testing::Test {
public:
  WatermarkBufferTest() { buffer_.setWatermarks(5, 10); }

  Buffer::WatermarkBuffer buffer_{[&]() -> void { ++times_low_watermark_called_; },
                                  [&]() -> void { ++times_high_watermark_called_; }};
  uint32_t times_low_watermark_called_{0};
  uint32_t times_high_watermark_called_{0};
};

TEST_F(WatermarkBufferTest, AddChar) {
  buffer_.add(TEN_BYTES, 10);
  EXPECT_EQ(0, times_high_watermark_called_);
  buffer_.add("a", 1);
  EXPECT_EQ(1, times_high_watermark_called_);
  EXPECT_EQ(11, buffer_.length());
}

TEST_F(WatermarkBufferTest, AddString) {
  buffer_.add(std::string(TEN_BYTES));
  EXPECT_EQ(0, times_high_watermark_called_);
  buffer_.add(std::string("a"));
  EXPECT_EQ(1, times_high_watermark_called_);
  EXPECT_EQ(11, buffer_.length());
}

TEST_F(WatermarkBufferTest, AddBuffer) {
  OwnedImpl first(TEN_BYTES);
  buffer_.add(first);
  EXPECT_EQ(0, times_high_watermark_called_);
  OwnedImpl second("a");
  buffer_.add(second);
  EXPECT_EQ(1, times_high_watermark_called_);
  EXPECT_EQ(11, buffer_.length());
}

TEST_F(WatermarkBufferTest, Commit) {
  buffer_.add(TEN_BYTES, 10);
  EXPECT_EQ(0, times_high_watermark_called_);
  RawSlice out;
  buffer_.reserve(10, &out, 1);
  memcpy(out.mem_, &TEN_BYTES[0], 10);
  out.len_ = 10;
  buffer_.commit(&out, 1);
  EXPECT_EQ(1, times_high_watermark_called_);
  EXPECT_EQ(20, buffer_.length());
}

TEST_F(WatermarkBufferTest, Drain) {
  // Draining from above to below the low watermark does nothing if the high
  // watermark never got hit.
  buffer_.add(TEN_BYTES, 10);
  buffer_.drain(10);
  EXPECT_EQ(0, times_high_watermark_called_);
  EXPECT_EQ(0, times_low_watermark_called_);

  // Go above the high watermark then drain down to just at the low watermark.
  buffer_.add(TEN_BYTES, 11);
  buffer_.drain(6);
  EXPECT_EQ(5, buffer_.length());
  EXPECT_EQ(0, times_low_watermark_called_);

  // Now drain below.
  buffer_.drain(1);
  EXPECT_EQ(1, times_low_watermark_called_);

  // Going back above should trigger the high again
  buffer_.add(TEN_BYTES, 10);
  EXPECT_EQ(2, times_high_watermark_called_);
}

TEST_F(WatermarkBufferTest, MoveFullBuffer) {
  buffer_.add(TEN_BYTES, 10);
  OwnedImpl data("a");

  EXPECT_EQ(0, times_high_watermark_called_);
  buffer_.move(data);
  EXPECT_EQ(1, times_high_watermark_called_);
  EXPECT_EQ(11, buffer_.length());
}

TEST_F(WatermarkBufferTest, MoveOneByte) {
  buffer_.add(TEN_BYTES, 9);
  OwnedImpl data("ab");

  buffer_.move(data, 1);
  EXPECT_EQ(0, times_high_watermark_called_);
  EXPECT_EQ(10, buffer_.length());

  buffer_.move(data, 1);
  EXPECT_EQ(1, times_high_watermark_called_);
  EXPECT_EQ(11, buffer_.length());
}

TEST_F(WatermarkBufferTest, WatermarkFdFunctions) {
  int pipe_fds[2] = {0, 0};
  ASSERT_EQ(0, pipe(pipe_fds));

  buffer_.add(TEN_BYTES, 10);
  buffer_.add(TEN_BYTES, 10);
  EXPECT_EQ(1, times_high_watermark_called_);
  EXPECT_EQ(0, times_low_watermark_called_);

  int bytes_written_total = 0;
  while (bytes_written_total < 20) {
    int bytes_written = buffer_.write(pipe_fds[1]);
    if (bytes_written < 0) {
      ASSERT_EQ(EAGAIN, errno);
    } else {
      bytes_written_total += bytes_written;
    }
  }
  EXPECT_EQ(1, times_high_watermark_called_);
  EXPECT_EQ(1, times_low_watermark_called_);
  EXPECT_EQ(0, buffer_.length());

  int bytes_read_total = 0;
  while (bytes_read_total < 20) {
    bytes_read_total += buffer_.read(pipe_fds[0], 20);
  }
  EXPECT_EQ(2, times_high_watermark_called_);
  EXPECT_EQ(20, buffer_.length());
}

TEST_F(WatermarkBufferTest, MoveWatermarks) {
  buffer_.add(TEN_BYTES, 9);
  EXPECT_EQ(0, times_high_watermark_called_);
  buffer_.setWatermarks(1, 9);
  EXPECT_EQ(0, times_high_watermark_called_);
  buffer_.setWatermarks(1, 8);
  EXPECT_EQ(1, times_high_watermark_called_);

  buffer_.setWatermarks(9, 20);
  EXPECT_EQ(0, times_low_watermark_called_);
  buffer_.setWatermarks(10, 20);
  EXPECT_EQ(1, times_low_watermark_called_);
  buffer_.setWatermarks(8, 20);
  buffer_.setWatermarks(10, 20);
  EXPECT_EQ(1, times_low_watermark_called_);
}

TEST_F(WatermarkBufferTest, GetRawSlices) {
  buffer_.add(TEN_BYTES, 10);

  RawSlice slices[2];
  ASSERT_EQ(1, buffer_.getRawSlices(&slices[0], 2));
  EXPECT_EQ(10, slices[0].len_);
  EXPECT_EQ(0, memcmp(slices[0].mem_, &TEN_BYTES[0], 10));

  void* data_pointer = buffer_.linearize(10);
  EXPECT_EQ(data_pointer, slices[0].mem_);
}

TEST_F(WatermarkBufferTest, Search) {
  buffer_.add(TEN_BYTES, 10);

  EXPECT_EQ(1, buffer_.search(&TEN_BYTES[1], 2, 0));

  EXPECT_EQ(-1, buffer_.search(&TEN_BYTES[1], 2, 5));
}

TEST_F(WatermarkBufferTest, MoveBackWithWatermarks) {
  int high_watermark_buffer1 = 0;
  int low_watermark_buffer1 = 0;
  Buffer::WatermarkBuffer buffer1{[&]() -> void { ++low_watermark_buffer1; },
                                  [&]() -> void { ++high_watermark_buffer1; }};
  buffer1.setWatermarks(5, 10);

  // Stick 20 bytes in buffer_ and expect the high watermark is hit.
  buffer_.add(TEN_BYTES, 10);
  buffer_.add(TEN_BYTES, 10);
  EXPECT_EQ(1, times_high_watermark_called_);

  // Now move 10 bytes to the new buffer.  Nothing should happen.
  buffer1.move(buffer_, 10);
  EXPECT_EQ(0, times_low_watermark_called_);
  EXPECT_EQ(0, high_watermark_buffer1);

  // Move 10 more bytes to the new buffer.  Both buffers should hit watermark callbacks.
  buffer1.move(buffer_, 10);
  EXPECT_EQ(1, times_low_watermark_called_);
  EXPECT_EQ(1, high_watermark_buffer1);

  // Now move all the data back to the original buffer.  Watermarks should trigger immediately.
  buffer_.move(buffer1);
  EXPECT_EQ(2, times_high_watermark_called_);
  EXPECT_EQ(1, low_watermark_buffer1);
}

} // namespace
} // namespace Buffer
} // namespace Envoy
