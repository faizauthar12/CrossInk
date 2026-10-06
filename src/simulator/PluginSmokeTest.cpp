#ifdef SIMULATOR
#include "PluginSmokeTest.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>

#include <cstdlib>
#include <filesystem>

#include "activities/ActivityManager.h"
#include "activities/plugins/PluginCatalogActivity.h"
#include "network/CrossPointWebServer.h"
#include "util/PluginHttp.h"

extern ActivityManager activityManager;
extern GfxRenderer renderer;
extern MappedInputManager mappedInputManager;

struct PluginCatalogSmokeTest {
  static void require(bool ok, const char* message) {
    if (!ok) {
      LOG_ERR("PLUGINTEST", "%s", message);
      std::_Exit(1);
    }
  }
  static void capture(PluginCatalogActivity& activity, const char* name) {
    RenderLock lock;
    activity.render(std::move(lock));
    if (const char* directory = std::getenv("CROSSINK_PLUGIN_CAPTURES")) {
      std::filesystem::create_directories(directory);
      const auto path = std::filesystem::path(directory) / (std::string(name) + ".pgm");
      FILE* file = std::fopen(path.c_str(), "wb");
      require(file != nullptr, "capture open");
      const int width = renderer.getScreenWidth(), height = renderer.getScreenHeight();
      std::fprintf(file, "P5\n%d %d\n255\n", width, height);
      for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) std::fputc(renderer.isPixelBlack(x, y) ? 0 : 255, file);
      std::fclose(file);
    }
  }
  static void select(PluginCatalogActivity& activity, const char* name) {
    RenderLock lock;
    activity.enterPluginPicker();
    for (size_t i = 0; i < activity.installedPlugins.size(); ++i) {
      if (activity.installedPlugins[i].name == name) {
        activity.activateIndex(i);
        return;
      }
    }
    require(false, "fixture plugin missing");
  }
  static void tick() {
    static int phase = 0;
    static int bundleUpdates = 0;
    static std::unique_ptr<CrossPointWebServer> server;
    if (phase == 0) {
      WiFi.mode(WIFI_STA);
      WiFi.begin("Plugin fixture", "");
      activityManager.replaceActivity(makeUniqueNoThrow<PluginCatalogActivity>(renderer, mappedInputManager));
      ++phase;
      return;
    }
    if (!activityManager.isCurrentActivityNamed("PluginCatalog")) return;
    auto& activity = *static_cast<PluginCatalogActivity*>(activityManager.simulatorCurrentActivity());
    CatalogActivity::State state;
    {
      RenderLock lock;
      state = activity.state;
    }
    require(state != CatalogActivity::State::ERROR, "catalog entered error state");
    switch (phase) {
      case 1: {
        capture(activity, "plugins");
        select(activity, "json");
        ++phase;
        break;
      }
      case 2: {
        if (state != CatalogActivity::State::BROWSING) return;
        {
          RenderLock lock;
          require(activity.itemCount == 3, "JSON row count");
        }
        capture(activity, "catalog");
        require(pluginhttp::saveTokenToFile("/plugins/json/token.json", "access_token", "old"), "token save");
        require(!pluginhttp::saveTokenToFile("/plugins/json/token.json", "access_token", std::string(2035, 'x')),
                "oversized token accepted");
        std::string token;
        require(pluginhttp::loadTokenFromFile("/plugins/json/token.json", "access_token", token) && token == "old",
                "oversized token replaced previous value");
        // Exercise the published touch target, then the normal main-loop download.
        freeink::ui::Interaction hit;
        bool found = false;
        int tapX = 0, tapY = 0;
        {
          RenderLock lock;
          for (int y = 0; y < renderer.getScreenHeight(); y += 4) {
            const int x = renderer.getScreenWidth() / 2;
            if (activity.app.hitPublished(x, y, PluginCatalogActivity::ACTION_ROW, hit) && hit.value == 0) {
              tapX = x;
              tapY = hit.rect.y + hit.rect.height / 2;
              found = true;
              break;
            }
          }
        }
        require(found, "first catalog row has no touch target");
        mappedInputManager.simulatorInjectTouchDown(tapX, tapY);
        activity.loop();
        mappedInputManager.simulatorClearInputFrame();
        mappedInputManager.simulatorInjectTouchRelease(tapX, tapY);
        activity.loop();
        mappedInputManager.simulatorClearInputFrame();
        ++phase;
        break;
      }
      case 3: {
        if (state != CatalogActivity::State::DONE) return;
        require(Storage.exists("/Books/Fixture.txt"), "download missing");
        capture(activity, "download-complete");
        select(activity, "xml");
        ++phase;
        break;
      }
      case 4: {
        if (state != CatalogActivity::State::BROWSING) return;
        {
          RenderLock lock;
          require(activity.itemCount == 200, "XML row count");
          activity.moveSelectionTo(199);
        }
        capture(activity, "xml-last-page");
        select(activity, "bundles");
        ++phase;
        break;
      }
      case 5: {
        if (state != CatalogActivity::State::BROWSING) return;
        {
          RenderLock lock;
          require(activity.itemCount == 1, "bundle row count");
          activity.downloadItem(0);
        }
        ++phase;
        break;
      }
      case 6: {
        if (state != CatalogActivity::State::DONE) return;
        require(Storage.exists("/plugins/installed/.config.json"), "bundle lost hidden config");
        require(Storage.exists("/plugins/installed/.secrets/value"), "bundle lost hidden folder");
        require(Storage.exists("/plugins/installed/plugin.js"), "bundle file missing");
        require(!Storage.exists("/.crosspoint/plugin-install.txt"), "bundle journal not cleaned");
        require(!Storage.exists("/plugins/.installed.plugin-old"), "bundle backup not cleaned");
        if (++bundleUpdates < 2) {
          select(activity, "bundles");
          phase = 5;
          break;
        }
        select(activity, "auth");
        ++phase;
        break;
      }
      case 7: {
        if (state != CatalogActivity::State::NO_TOKEN) return;
        {
          RenderLock lock;
          activity.beginAuth();
        }
        ++phase;
        break;
      }
      case 8: {
        if (state != CatalogActivity::State::AUTH) return;
        capture(activity, "sign-in");
        ++phase;
        break;
      }
      case 9: {
        if (state != CatalogActivity::State::BROWSING) return;
        std::string token;
        require(
            pluginhttp::loadTokenFromFile("/plugins/auth/token.json", "access_token", token) && token == "signed-in",
            "device-code token was not persisted");
        capture(activity, "signed-in");
        server = makeUniqueNoThrow<CrossPointWebServer>();
        require(server != nullptr, "web server allocation");
        server->begin();
        LOG_INF("PLUGINTEST", "Catalog, TLS download, tokens and bundle checks passed; web API ready");
        ++phase;
        break;
      }
      case 10:
        server->handleClient();
        if (Storage.exists("/plugin-web-done")) {
          server->stop();
          LOG_INF("PLUGINTEST", "Simulator plugin smoke test passed");
          std::_Exit(0);
        }
        break;
    }
  }
};
void runPluginSmokeTestTick() { PluginCatalogSmokeTest::tick(); }
#endif
