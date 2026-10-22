#include "ReadingSessionStats.h"

#include <Logging.h>
#include <Memory.h>

void commitReadingSession(BookReadingStats& book, GlobalReadingStats& global, const uint32_t seconds,
                          const ReadingStatsDateTime* sessionStart) {
  // Page intervals longer than the idle threshold are rejected before they
  // reach the session total, so seconds is active reading time.
  if (seconds >= 60) {
    book.sessionCount++;
    global.totalSessions++;
  }
  if (seconds < 10) return;
  book.totalReadingSeconds += seconds;
  global.totalReadingSeconds += seconds;
  if (!sessionStart) return;
  book.recordReadingSpan(*sessionStart, seconds);
  global.recordReadingSpan(*sessionStart, seconds);
  if (seconds >= 120 && !book.startDateManual && !book.startDate.isValid()) {
    book.startDate = sessionStart->date;
  }
}

void importBookStatsEdits(BookReadingStats& live, const BookReadingStats& disk) {
  live.isCompleted = disk.isCompleted;
  live.startDateManual = disk.startDateManual;
  live.finishedDateManual = disk.finishedDateManual;
  live.startDate = disk.startDate;
  live.finishedDate = disk.finishedDate;
}

void refreshGlobalStatsAfterOverlay(GlobalReadingStats& live, const uint32_t resetRevisionAtOpen) {
  GlobalReadingStats disk = GlobalReadingStats::load();
  if (GlobalReadingStats::localResetRevision() != resetRevisionAtOpen) {
    live = disk;
  } else {
    live.completedBooks = disk.completedBooks;
  }
}

bool saveReadingSessionCheckpoint(const BookReadingStats& book, const GlobalReadingStats& global, uint32_t seconds,
                                  const ReadingStatsDateTime* sessionStart, const std::string& cachePath) {
  // These two snapshots exceed the small task-stack budget. Allocate only at
  // checkpoint boundaries, never for every page or while rendering text.
  struct Preview {
    BookReadingStats book;
    GlobalReadingStats global;
  };
  auto preview = makeUniqueNoThrow<Preview>();
  if (!preview) {
    LOG_ERR("SessionStats", "Cannot allocate stats checkpoint");
    return false;
  }
  preview->book = book;
  preview->global = global;
  commitReadingSession(preview->book, preview->global, seconds, sessionStart);
  if (!preview->book.save(cachePath)) return false;
  preview->global.save();
  GlobalReadingStats checked;
  if (!GlobalReadingStats::loadForUpload(checked) ||
      checked.totalReadingSeconds != preview->global.totalReadingSeconds ||
      checked.totalSessions != preview->global.totalSessions) {
    LOG_ERR("SessionStats", "Global stats checkpoint could not be verified");
    return false;
  }
  return true;
}
