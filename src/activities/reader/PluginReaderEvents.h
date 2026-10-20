#pragma once
#include <string>

#include "ReaderSession.h"

// Reader callers serialize these small fields with RenderLock. Rendering only
// records scalars; the main loop and exit path perform SD writes.
class PluginReaderEvents {
 public:
  void noteTurn(bool forward) { session.noteTurn(forward, true); }
  void rendered(int progressBasisPoints);
  void poll(const std::string& book);
  void flush(const std::string& book);
  void exit(const std::string& book, int percent);

 private:
  ReaderSession session;
  bool renderedPage = false;
  bool opened = false;
};
