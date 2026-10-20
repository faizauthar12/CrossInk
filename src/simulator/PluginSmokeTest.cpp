#ifdef SIMULATOR
#include "PluginSmokeTest.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <LibraryBuilder.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>

#include <cstdlib>
#include <filesystem>

#include "OpdsServerStore.h"
#include "activities/ActivityManager.h"
#include "activities/home/HomeActivity.h"
#include "activities/network/NetworkModeSelectionActivity.h"
#include "activities/plugins/PluginCatalogActivity.h"
#include "activities/util/DetailsActivity.h"
#include "components/TouchRegistry.h"
#include "components/UITheme.h"
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
  static void capture(Activity& activity, const char* name) {
    RenderLock lock;
    TouchRegistry::getInstance().beginFrame();
    activity.render(std::move(lock));
    TouchRegistry::getInstance().publish();
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
        activity.activateIndex(i + (activity.showPluginHubInstallRow ? 1 : 0));
        return;
      }
    }
    require(false, "fixture plugin missing");
  }
  static void tick() {
    static const uint8_t originalTheme = SETTINGS.uiTheme;
    static int phase = -4;
    static int homePass = 0;
    static int pendingRelease = -1;
    if (pendingRelease >= 0) {
      mappedInputManager.simulatorInjectRelease(static_cast<MappedInputManager::Button>(pendingRelease));
      pendingRelease = -1;
      return;
    }
    static bool detailsCancelled = false, reopenDetails = false, detailsClosing = false, detailsReady = false;
    if (activityManager.isCurrentActivityNamed("Details")) {
      if (detailsClosing) return;
      auto& details = *static_cast<DetailsActivity*>(activityManager.simulatorCurrentActivity());
      if (!detailsReady) {
        require(details.simulatorText().find("Café & catalog") != std::string::npos, "HTML details were not decoded");
        require(details.simulatorText().find("<p>") == std::string::npos, "HTML markup visible");
        require(details.simulatorPage() == 0 && details.simulatorFirstLine() == "Fixture",
                "Details did not start at the title");
        const size_t firstEnd = details.simulatorPageEnd();
        require(firstEnd < details.simulatorText().size(), "Long details did not paginate");
        capture(details, detailsCancelled ? "catalog-details-reopened" : "catalog-details");
        mappedInputManager.simulatorInjectRelease(MappedInputManager::Button::Down);
        details.loop();
        mappedInputManager.simulatorClearInputFrame();
        require(details.simulatorPageEnd() > firstEnd, "Details paging did not advance");
        detailsReady = true;
        return;
      }
      mappedInputManager.simulatorInjectRelease(detailsCancelled ? MappedInputManager::Button::Confirm
                                                                 : MappedInputManager::Button::Back);
      details.loop();
      mappedInputManager.simulatorClearInputFrame();
      reopenDetails = !detailsCancelled;
      detailsCancelled = true;
      detailsClosing = true;
      return;
    }
    detailsClosing = false;
    detailsReady = false;
    if (reopenDetails && activityManager.isCurrentActivityNamed("PluginCatalog")) {
      require(!Storage.exists("/Books/Fixture.txt"), "Cancelling details downloaded a book");
      auto& catalog = *static_cast<PluginCatalogActivity*>(activityManager.simulatorCurrentActivity());
      RenderLock lock;
      catalog.activateItem(0);
      reopenDetails = false;
      return;
    }

    if (phase == -8) {
      {
        RenderLock lock;
        SETTINGS.uiTheme = CrossPointSettings::LYRA;
        UITheme::getInstance().reload();
      }
      activityManager.goHome();
      phase = -7;
      return;
    }
    if (phase == -7) {
      require(activityManager.isCurrentActivityNamed("Home"), "Full Home menu fixture");
      auto& home = *static_cast<HomeActivity*>(activityManager.simulatorCurrentActivity());
      {
        RenderLock lock;
        home.hasReadingStats = home.hasBookmarks = true;
        home.selectorIndex = 0;
        require(home.getMenuItemCount() == 7, "OPDS still occupies a Home row");
      }
      capture(home, "home-all-actions");
#if CROSSINK_APP_CAP_TOUCH
      bool settingsVisible = false;
      {
        RenderLock lock;
        for (int y = 0; y < renderer.getScreenHeight() && !settingsVisible; y += 4)
          for (int x = 0; x < renderer.getScreenWidth(); x += 4) {
            int id = -1;
            if (TouchRegistry::getInstance().hitTest(x, y, TouchRegistry::Item, id) && id == 6) {
              settingsVisible = true;
              break;
            }
          }
      }
      require(settingsVisible, "Settings missing from first Home page");
#endif
      // Two servers exercise the existing picker without making a network request.
      if (OPDS_STORE.getCount() < 2)
        require(OPDS_STORE.addServer({"Second OPDS", "https://second.example.invalid"}), "Second OPDS fixture");
      activityManager.goToFileTransfer();
      phase = -6;
      return;
    }
    if (phase == -6 || phase == -9) {
      require(activityManager.isCurrentActivityNamed("NetworkModeSelection"), "File Transfer menu missing");
      auto& menu = *static_cast<NetworkModeSelectionActivity*>(activityManager.simulatorCurrentActivity());
      if (phase == -6) {
        for (int theme = 0; theme < CrossPointSettings::UI_THEME_COUNT; ++theme) {
          {
            RenderLock lock;
            SETTINGS.uiTheme = theme;
            UITheme::getInstance().reload();
            menu.ui.reset();
          }
          capture(menu, ("file-transfer-theme-" + std::to_string(theme)).c_str());
        }
        {
          RenderLock lock;
          SETTINGS.uiTheme = originalTheme;
          UITheme::getInstance().reload();
          menu.ui.reset();
          menu.selectedIndex = 0;
        }
        capture(menu, "file-transfer");
        mappedInputManager.simulatorInjectPress(MappedInputManager::Button::Confirm);
        pendingRelease = static_cast<int>(MappedInputManager::Button::Confirm);
        menu.loop();
        mappedInputManager.simulatorClearInputFrame();
        phase = -5;
      } else {
#if CROSSINK_APP_CAP_TOUCH
        capture(menu, "file-transfer");
        freeink::ui::Interaction hit;
        bool found = false;
        int tapX = 0, tapY = 0;
        {
          RenderLock lock;
          for (int y = 0; y < renderer.getScreenHeight() && !found; y += 4)
            for (int x = 0; x < renderer.getScreenWidth(); x += 4)
              if (menu.ui.app.hitPublished(x, y, 1, hit) && hit.value == 0) {
                found = true;
                tapX = x;
                tapY = y;
                break;
              }
        }
        require(found, "OPDS missing from File Transfer touch targets");
        mappedInputManager.simulatorInjectTouchDown(tapX, tapY);
        menu.loop();
        mappedInputManager.simulatorClearInputFrame();
        mappedInputManager.simulatorInjectTouchRelease(tapX, tapY);
        menu.loop();
        mappedInputManager.simulatorClearInputFrame();
        phase = -10;
#else
        activityManager.goHome(HomeMenuItem::FILE_TRANSFER);
        phase = 0;
#endif
      }
      return;
    }
    if (phase == -5 || phase == -10) {
      require(activityManager.isCurrentActivityNamed("OpdsServerList"), "File Transfer OPDS action failed");
      capture(*activityManager.simulatorCurrentActivity(), "opds-servers");
      mappedInputManager.simulatorInjectPress(MappedInputManager::Button::Back);
      pendingRelease = static_cast<int>(MappedInputManager::Button::Back);
      activityManager.simulatorCurrentActivity()->loop();
      mappedInputManager.simulatorClearInputFrame();
      phase = phase == -5 ? -9 : -11;
      return;
    }
    if (phase == -11) {
      require(activityManager.isCurrentActivityNamed("NetworkModeSelection"), "OPDS Back lost File Transfer");
      // Release File Transfer before starting the independent Wi-Fi fixture;
      // its normal exit intentionally reboots when networking is active.
      activityManager.goHome(HomeMenuItem::FILE_TRANSFER);
      phase = 0;
      return;
    }
    if (phase == -4) {
      SETTINGS.pluginsEnabled = 1;
      const int theme = homePass / 2;
      if (theme >= CrossPointSettings::UI_THEME_COUNT) {
        RenderLock lock;
        SETTINGS.uiTheme = originalTheme;
        UITheme::getInstance().reload();
        phase = -8;
        return;
      }
      {
        RenderLock lock;
        SETTINGS.uiTheme = theme;
        UITheme::getInstance().reload();
        if (!OPDS_STORE.hasServers())
          require(OPDS_STORE.addServer({"Fixture OPDS", "https://example.invalid"}), "OPDS fixture");
      }
      activityManager.goHome(HomeMenuItem::PLUGINS);
      phase = -3;
      return;
    }
    if (phase == -3) {
      if (!activityManager.isCurrentActivityNamed("Home")) return;
      auto& home = *static_cast<HomeActivity*>(activityManager.simulatorCurrentActivity());
      {
        RenderLock lock;
        require(home.hasPlugins, "Home lost Plugins");
        require(home.getMenuItemCount() == 5, "Home menu count lost an action");
      }
      capture(home, ("home-theme-" + std::to_string(homePass / 2)).c_str());
      if (homePass % 2 == 0 || !mappedInputManager.hasTouch()) {
        mappedInputManager.simulatorInjectRelease(MappedInputManager::Button::Confirm);
        // Carousel intentionally ignores physical Confirm when touch hardware exists.
        if (SETTINGS.uiTheme == CrossPointSettings::LYRA_CAROUSEL && mappedInputManager.hasTouchHardware()) {
          RenderLock lock;
          home.onPluginsOpen();
        } else {
          RenderLock lock;
          home.loop();
        }
        mappedInputManager.simulatorClearInputFrame();
      }
#if CROSSINK_APP_CAP_TOUCH
      else {
        int tapX = -1, tapY = -1;
        {
          RenderLock lock;
          const int target = home.minimalMenuOpen ? home.minimalMenuIndex : 2;
          for (int y = 0; y < renderer.getScreenHeight() && tapX < 0; y += 4) {
            for (int x = 0; x < renderer.getScreenWidth(); x += 4) {
              int id = -1;
              freeink::ui::Interaction hit;
              const bool found =
                  home.coverGridUi
                      ? (home.coverGridUi->app.hitPublished(x, y, 1, hit) && hit.value == home.selectorIndex)
                      : (TouchRegistry::getInstance().hitTest(x, y, TouchRegistry::Item, id) && id == target);
              if (found) {
                tapX = x;
                tapY = y;
                break;
              }
            }
          }
        }
        require(tapX >= 0, "Home Plugins has no touch target");
        mappedInputManager.simulatorInjectTouchDown(tapX, tapY);
        {
          RenderLock lock;
          home.loop();
        }
        mappedInputManager.simulatorClearInputFrame();
        mappedInputManager.simulatorInjectTouchRelease(tapX, tapY);
        {
          RenderLock lock;
          home.loop();
        }
        mappedInputManager.simulatorClearInputFrame();
      }
#endif
      phase = -2;
      return;
    }
    if (phase == -2) {
      require(activityManager.isCurrentActivityNamed("PluginCatalog"), "Home Plugins action opened wrong screen");
      auto& plugin = *static_cast<PluginCatalogActivity*>(activityManager.simulatorCurrentActivity());
      {
        RenderLock lock;
        require(plugin.fromHome, "Plugins lost Home origin");
        plugin.onBackButton();
      }
      phase = -1;
      return;
    }
    if (phase == -1) {
      require(activityManager.isCurrentActivityNamed("Home"), "Plugins Back did not return Home");
      ++homePass;
      phase = -4;
      return;
    }
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
        {
          RenderLock lock;
          SETTINGS.pluginsEnabled = 0;
          require(PluginLocations::scanPlugins().empty(), "disabled plugins discovered");
          require(PluginLocations::findPluginDir("json").empty(), "disabled plugin resolved");
          require(PluginLocations::anyPluginInstalledOnDisk(), "migration cannot see disabled plugins");
          SETTINGS.pluginsEnabled = 1;
          library::restoreLibraryIndexAfterSleep();
          require(!library::libraryIndexNeedsRefresh(), "Library fixture not clean");
        }
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
        {
          RenderLock lock;
          activity.page = 2;
          activity.hasMore = true;
          activity.releaseRows();
        }
        capture(activity, "pagination");
        {
          RenderLock lock;
          require(activity.rowItems.front().value == nullptr, "Previous Page still has trailing arrow");
          require(activity.rowItems.back().label == nullptr, "Next Page is not in the right-aligned slot");
          activity.page = 1;
          activity.hasMore = false;
          activity.releaseRows();
        }
        capture(activity, "catalog");
        require(pluginhttp::saveTokenToFile("/plugins/json/token.json", "access_token", "old"), "token save");
        require(!pluginhttp::saveTokenToFile("/plugins/json/token.json", "access_token", std::string(2035, 'x')),
                "oversized token accepted");
        std::string token;
        require(pluginhttp::loadTokenFromFile("/plugins/json/token.json", "access_token", token) && token == "old",
                "oversized token replaced previous value");
#if CROSSINK_APP_CAP_TOUCH
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
#else
        {
          RenderLock lock;
          activity.downloadItem(0);
        }
#endif
        ++phase;
        break;
      }
      case 3: {
        if (state != CatalogActivity::State::DONE) return;
        require(Storage.exists("/Books/Fixture.txt"), "download missing");
        require(library::libraryIndexNeedsRefresh(), "plugin download did not refresh Library");
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
        require(!Storage.exists("/.crosspoint/plugins/installed"), "bundle duplicated plugin in a different root");
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
