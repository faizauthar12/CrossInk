#include <gtest/gtest.h>

#include "ReadingSessionStats.h"
namespace {
BookReadingStats savedBook;
GlobalReadingStats savedGlobal;
bool saveFails = false;
}  // namespace
bool BookReadingStats::save(const std::string&) const {
  if (saveFails) return false;
  savedBook = *this;
  return true;
}
void GlobalReadingStats::save() const { savedGlobal = *this; }
bool GlobalReadingStats::loadForUpload(GlobalReadingStats& out) {
  out = savedGlobal;
  return true;
}
GlobalReadingStats GlobalReadingStats::load() { return savedGlobal; }
uint32_t GlobalReadingStats::localResetRevision() { return 0; }
void BookReadingStats::recordReadingSpan(const ReadingStatsDateTime&, uint32_t) {}
void GlobalReadingStats::recordReadingSpan(const ReadingStatsDateTime&, uint32_t) {}
bool ReadingStatsDate::isValid() const { return false; }
TEST(SessionCheckpoint, OverlayPreviewsNeverConsumeOrDoubleCountTheLiveSession) {
  BookReadingStats book;
  GlobalReadingStats global;
  ASSERT_TRUE(saveReadingSessionCheckpoint(book, global, 30, nullptr, "/cache"));
  EXPECT_EQ(savedBook.totalReadingSeconds, 30u);
  EXPECT_EQ(savedGlobal.totalSessions, 0u);
  EXPECT_EQ(book.totalReadingSeconds, 0u);
  EXPECT_EQ(global.totalReadingSeconds, 0u);
  ASSERT_TRUE(saveReadingSessionCheckpoint(book, global, 60, nullptr, "/cache"));
  EXPECT_EQ(savedBook.totalReadingSeconds, 60u);
  EXPECT_EQ(savedGlobal.totalSessions, 1u);
  ASSERT_TRUE(saveReadingSessionCheckpoint(book, global, 60, nullptr, "/cache"));
  EXPECT_EQ(savedGlobal.totalReadingSeconds, 60u);
  EXPECT_EQ(savedGlobal.totalSessions, 1u);
  commitReadingSession(book, global, 60, nullptr);
  EXPECT_EQ(book.totalReadingSeconds, savedBook.totalReadingSeconds);
  EXPECT_EQ(global.totalSessions, savedGlobal.totalSessions);
}
TEST(SessionCheckpoint, ShortSegmentsKeepTheExistingWholeSessionThreshold) {
  BookReadingStats book;
  GlobalReadingStats global;
  ASSERT_TRUE(saveReadingSessionCheckpoint(book, global, 6, nullptr, "/cache"));
  EXPECT_EQ(savedGlobal.totalReadingSeconds, 0u);
  ASSERT_TRUE(saveReadingSessionCheckpoint(book, global, 12, nullptr, "/cache"));
  EXPECT_EQ(savedGlobal.totalReadingSeconds, 12u);
  EXPECT_EQ(global.totalReadingSeconds, 0u);
}
TEST(SessionCheckpoint, FailedBookWritesAreNotAcknowledged) {
  BookReadingStats book;
  GlobalReadingStats global;
  saveFails = true;
  EXPECT_FALSE(saveReadingSessionCheckpoint(book, global, 60, nullptr, "/cache"));
  saveFails = false;
}
