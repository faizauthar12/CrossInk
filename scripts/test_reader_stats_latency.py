#!/usr/bin/env python3
"""Exercise production reader interval methods with slow storage and a fake clock."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def method(source, signature):
    start = source.index(signature)
    cursor = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[cursor] == '{') - (source[cursor] == '}')
        cursor += 1
    return source[start:cursor] + '\n'


FIXTURE = r'''
#include <cassert>
#include <cstdint>
#include <iostream>
#include <optional>
#include "ReadingSessionStats.h"
#include <CrossPointSettings.h>
#include <HalClock.h>
#define LOG_ERR(...) ((void)0)
#define LOG_DBG(...) ((void)0)
unsigned long nowMs=1000;
unsigned long millis(){return nowMs;}
int storageCalls=0, panelCalls=0;
bool failCheckpoint=false;
BookReadingStats savedBook;
GlobalReadingStats savedGlobal;
extern bool statsLocked;
void storage(){assert(statsLocked);++storageCalls;nowMs+=3000;halClock.second+=3;}
bool BookReadingStats::save(const std::string&)const {
  storage(); if(failCheckpoint)return false; savedBook=*this;return true;
}
bool GlobalReadingStats::save()const {storage();savedGlobal=*this;return true;}
bool GlobalReadingStats::loadForUpload(GlobalReadingStats& s){s=savedGlobal;return true;}
GlobalReadingStats GlobalReadingStats::load(){return savedGlobal;}
uint32_t GlobalReadingStats::localResetRevision(){return 0;}
struct IntervalSink {
  ReadingStatsDateTime begin, lastEnd;
  uint8_t offset=255,lastOffset=255;
  uint32_t accepted=0,rev=0;
  void start(const ReadingStatsDateTime& t,uint8_t o){begin=t;offset=o;}
  bool accept(const ReadingStatsDateTime& end,uint8_t o,uint32_t seconds,uint32_t=0){
    assert(isValidReadingStatsInterval(begin,end,offset,o,seconds));
    lastEnd=end;lastOffset=o;accepted+=seconds;++rev;storage();return true;
  }
  bool accept(const char*,const ReadingStatsDateTime& end,uint8_t o,uint32_t seconds){
    return accept(end,o,seconds);
  }
  uint32_t revision()const{return rev;}
};
struct Book {std::string getPath()const{return "/book";}std::string getCachePath()const{return "/cache";}};
bool statsLocked=false;
int statsLockAcquires=0;
struct RenderLock {
  template<class T> explicit RenderLock(T&){assert(!statsLocked);statsLocked=true;++statsLockAcquires;}
  ~RenderLock(){statsLocked=false;}
  static bool heldByCurrentTask(){return statsLocked;}
};
struct Reader {
  Book book;Book* epub=&book;Book* xtc=&book;
  BookReadingStats stats;GlobalReadingStats globalStats;
  ReadingSessionDates readingSessionDates;
  DeferredReadingStatsInterval deferredReadingStats;
  IntervalSink dailyReadingSession,readingSessionJournal;
  uint32_t sessionReadingSeconds=0,statsCheckpointRevision=0;
  unsigned long pageShownAtMs=1000;
  bool activeFootnotePreview=false,acceptTime=true;
  bool currentPageReadingSecondsForStats(uint32_t& seconds,const char*)const {
    seconds=pageShownAtMs && acceptTime ? (nowMs-pageShownAtMs)/1000 : 0;return seconds>0;
  }
  void recordCurrentPageReadingTime(const char*,bool=false);
  void flushDeferredReadingStats();
  void startDailyReadingInterval();
  void render(bool error=false);
};
'''

TESTS = r'''
int main(){
  Reader r;
  halClock.second=0;
  r.startDailyReadingInterval();
  nowMs=11000;halClock.second=10;
  const auto inputAt=millis();
  r.recordCurrentPageReadingTime("touch",true);
  assert(storageCalls==0 && millis()==inputAt && r.sessionReadingSeconds==10);
  assert(r.pageShownAtMs==0 && r.deferredReadingStats.seconds==10);
  // Rendering shows the page before any SD work. The accepted end remains 10s
  // even though the clock advances while four slow persistence calls run.
  r.render();
  assert(panelCalls==1 && r.dailyReadingSession.accepted==10);
  assert(r.readingSessionJournal.lastEnd.second==10);
  assert(r.dailyReadingSession.lastEnd.second==10);
  assert(r.dailyReadingSession.begin.second==10);
  assert(r.deferredReadingStats.seconds==0);
  const auto written=storageCalls;
  { RenderLock lock(r); r.flushDeferredReadingStats(); }assert(storageCalls==written);
  // Time spent viewing the new page during storage remains a valid interval.
  r.recordCurrentPageReadingTime("front",true);
  assert(storageCalls==written && r.deferredReadingStats.seconds==12);
  r.render(true);  // Error/end-screen early returns must also drain.
  assert(r.deferredReadingStats.seconds==0 && r.dailyReadingSession.accepted==22);
  // A restart/exit before the requested render drains the pending interval once.
  nowMs=1000;halClock.second=0;storageCalls=0;
  Reader exit;
  exit.startDailyReadingInterval();nowMs=11000;halClock.second=10;
  exit.recordCurrentPageReadingTime("side",true);
  exit.recordCurrentPageReadingTime("restart_checkpoint");
  assert(exit.dailyReadingSession.accepted==10 && storageCalls==4);
  exit.recordCurrentPageReadingTime("session_commit");assert(storageCalls==4);
  // A changed setting after capture cannot change the accepted offset.
  nowMs=1000;halClock.second=0;storageCalls=0;
  Reader offset;offset.startDailyReadingInterval();nowMs=11000;halClock.second=10;
  offset.recordCurrentPageReadingTime("touch",true);SETTINGS.clockUtcOffsetQ=52;
  offset.render(true);assert(offset.dailyReadingSession.lastOffset==48);
  SETTINGS.clockUtcOffsetQ=48;
  // Existing checkpoint failures remain unacknowledged and can retry.
  nowMs=1000;halClock.second=0;storageCalls=0;
  Reader failed;failed.startDailyReadingInterval();nowMs=11000;halClock.second=10;
  failed.recordCurrentPageReadingTime("touch",true);failCheckpoint=true;
  failed.render(true);assert(failed.statsCheckpointRevision==0);
  assert(failed.dailyReadingSession.accepted==10);failCheckpoint=false;
  // An invalid/idle interval never schedules storage.
  Reader idle;idle.acceptTime=false;storageCalls=0;
  idle.recordCurrentPageReadingTime("front",true);idle.render(true);
  assert(storageCalls==0 && idle.sessionReadingSeconds==0);
  std::cout<<"deferred reading stats checks passed\n";
}
'''

for kind in ('Epub', 'Xtc'):
    source = (ROOT / f'src/activities/reader/{kind}ReaderActivity.cpp').read_text()
    methods = ''.join(method(source, f'void {kind}ReaderActivity::{name}(')
                      for name in ('recordCurrentPageReadingTime', 'flushDeferredReadingStats',
                                   'startDailyReadingInterval'))
    render = method(source, f'void {kind}ReaderActivity::render(')
    # Execute the production early-return guard around a fake display operation.
    guard = render[render.index('{')+1:render.index('  } flushStats{*this};')+len('  } flushStats{*this};')]
    methods += 'void Reader::render(bool error){ RenderLock renderLock(*this);' + guard + r'''
      ++panelCalls;
      if(error)return;
      pageShownAtMs=millis();startDailyReadingInterval();
    }
    '''
    methods = methods.replace(f'{kind}ReaderActivity', 'Reader')
    # Keep page-turn integration covered as well as the extracted methods.
    if kind == 'Epub':
        turn = method(source, 'void EpubReaderActivity::pageTurn(')
        assert turn.count('recordCurrentPageReadingTime(source, /*deferStorage=*/true)') == 2
        assert source.index('startDailyReadingInterval();', source.index('lastRenderCompleteMs = millis();')) > source.index('renderContents(std::move(p)')
    else:
        for reason in ('side_long_press', 'page_back', 'page_forward'):
            assert f'recordCurrentPageReadingTime("{reason}", /*deferStorage=*/true)' in source
        assert render.index('renderPage(pageToRender)') < render.index('startDailyReadingInterval()')
    with tempfile.TemporaryDirectory(prefix='reader-stats-latency-') as tmp:
        cpp = Path(tmp) / 'test.cpp'
        cpp.write_text(FIXTURE + methods + TESTS)
        exe = Path(tmp) / 'test'
        includes = ['test/daily_reading_stats/stubs', 'test/sync_files/stubs', 'lib/Memory',
                    'src/activities/reader', 'lib']
        subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Wno-unused-parameter',
                        *[f'-I{ROOT / p}' for p in includes], str(cpp),
                        str(ROOT / 'src/activities/reader/ReadingSessionStats.cpp'),
                        str(ROOT / 'src/activities/reader/ReadingStatsUtils.cpp'), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print(f'{kind}: production page-turn persistence order passed')
