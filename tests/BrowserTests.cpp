#include "TestFramework.h"

#include "Browser.h"
#include "CommandDescriptions.h"

#include <cellui/Application.h>
#include <cellui/TestBackend.h>

using namespace cellui;

namespace {

struct Fixture {
  test::TestBackend * backend;
  std::unique_ptr<Application> app;
  std::unique_ptr<Browser> browser;
  Fixture() {
    auto owned = std::make_unique<test::TestBackend>(24, 100);
    backend = owned.get();
    app = Application::adopt(std::move(owned));
    browser = std::make_unique<Browser>(*app);
  }
  void press(int key, unsigned mods = 0) {
    app->handle(BackendEvent::makeInput(keyInput(key, mods, Input::Kind::PRESS)));
    app->present();
  }
  void type(const std::string & s) {
    for (char c : s) press(c);
  }
  std::string text() {
    app->present();
    return backend->text();
  }
};

}  // namespace

TEST(every_command_has_a_description) {
  Fixture f;
  CHECK(!f.browser->commandNames().empty());
  for (const std::string & name : f.browser->commandNames()) {
    if (commandDescription(name).empty()) std::fprintf(stderr, "  no description: %s\n", name.c_str());
    CHECK(!commandDescription(name).empty());
  }
}

TEST(meta_x_opens_the_palette_and_a_chosen_command_runs) {
  Fixture f;
  CHECK(!f.browser->paletteOpen());
  f.press('x', kModAlt);
  CHECK(f.browser->paletteOpen());
  CHECK(f.text().find("show-currencies") != std::string::npos);  // the list is as tall as the screen allows
  f.type("sort-by-pr");
  CHECK(f.text().find("sort-by-price") != std::string::npos && f.text().find("sort-by-name") == std::string::npos);
  f.press(Key::Enter);
  CHECK(!f.browser->paletteOpen());
  CHECK(f.browser->table().sortColumn() == 3 && f.browser->table().sortOrder() == SortOrder::ASCENDING);
}

TEST(the_palette_takes_every_key_while_open_and_escape_closes_it_without_running) {
  Fixture f;
  f.press('x', kModAlt);
  f.press('q');  // typed into the query, not a quit
  CHECK(f.browser->paletteOpen());
  f.press('g', kModCtrl);  // cancel (a lone Escape waits for the key after it, in case it is a Meta prefix)
  CHECK(!f.browser->paletteOpen());
  CHECK(f.browser->table().sortColumn() == -1);
}

TEST(the_palette_lists_the_bindings_of_the_commands) {
  Fixture f;
  f.press('x', kModAlt);
  std::string text = f.text();
  // The shortest binding of each: M-x opens the palette, q quits.
  auto row = [&](const std::string & name) {
    size_t at = text.find("\n " + name);
    CHECK(at != std::string::npos);
    size_t end = text.find('\n', at + 1);
    return text.substr(at + 1, end - at - 1);
  };
  CHECK(row("command-palette").find("M-x") != std::string::npos);
  std::string quit = row("quit");
  CHECK(quit.back() == 'q');
}

TEST(preset_commands_choose_the_tag_and_fill_the_table) {
  Fixture f;
  CHECK(f.browser->tags().selected() == 0);
  CHECK(f.browser->execute("show-currencies"));
  CHECK(f.browser->tags().selected() == 2);
  CHECK(f.browser->table().model()->text(f.browser->table().rowAt(0), 0) == "EUR");
  CHECK(f.browser->execute("previous-preset"));
  CHECK(f.browser->tags().selected() == 1);
  CHECK(!f.browser->execute("no-such-command"));
}

TEST(sort_commands_reverse_and_clear) {
  Fixture f;
  CHECK(f.browser->execute("sort-by-name"));
  CHECK(f.browser->table().sortColumn() == 2);
  CHECK(f.browser->execute("reverse-sort"));
  CHECK(f.browser->table().sortOrder() == SortOrder::DESCENDING);
  CHECK(f.browser->execute("clear-sort"));
  CHECK(f.browser->table().sortColumn() == -1);
}

TEST(q_and_ctrl_x_ctrl_c_quit) {
  {
    Fixture f;
    f.backend->pushKey('q');
    CHECK(f.app->run() == 0);  // ends because the key quit; a hang here is the failure
  }
  {
    Fixture f;
    f.backend->pushKey('x', kModCtrl);
    f.backend->pushKey('c', kModCtrl);
    CHECK(f.app->run() == 0);
  }
}

TEST(the_status_line_says_what_a_preset_shows) {
  Fixture f;
  f.browser->execute("show-nasdaq-helsinki");
  CHECK(f.text().find("Showing Nasdaq Helsinki") != std::string::npos);
}
