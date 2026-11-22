#include "icons/IconLoader.h"

#include <canvas/Image.h>
#include <canvas/ImageLoadingException.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace fs = std::filesystem;
using cellui::TableIcon;

namespace {


// A hash that is the same in every run, for the file name and the picture's id.
uint64_t hashOf(const std::string & s) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
  return h;
}

std::string hex(uint64_t v) {
  static const char digits[] = "0123456789abcdef";
  std::string out(16, '0');
  for (int i = 15; i >= 0; i--, v >>= 4) out[static_cast<size_t>(i)] = digits[v & 15];
  return out;
}

// A picture as it came, straight RGBA at its own size.
struct Decoded {
  int width = 0, height = 0;
  std::vector<uint32_t> pixels;
};

std::shared_ptr<const TableIcon> makeIcon(const std::string & url, Decoded picture) {
  auto icon = std::make_shared<TableIcon>();
  icon->id = hashOf(url);
  icon->width = picture.width;
  icon->height = picture.height;
  icon->pixels = std::make_shared<const std::vector<uint32_t>>(std::move(picture.pixels));
  return icon;
}

size_t bytesOf(const TableIcon & icon) { return icon.pixels->size() * sizeof(uint32_t); }

// The bytes of an image file as RGBA at its own size; empty (no pixels) if it is no image.
Decoded decode(const std::string & bytes) {
  std::unique_ptr<canvas::ImageData> source;
  try {
    canvas::Image image(1.0f);
    image.decode(bytes.data(), bytes.size());
    source = image.moveDataPtr();
  } catch (...) {
    return {};
  }
  if (!source || !source->isValid() || source->getPixelType() != canvas::PixelType::UINT8) return {};
  const unsigned w = source->getWidth(), h = source->getHeight(), channels = source->getNumChannels();
  if (w > 4096 || h > 4096 || channels > 4) return {};
  Decoded out;
  out.width = static_cast<int>(w);
  out.height = static_cast<int>(h);
  out.pixels.resize(static_cast<size_t>(w) * h);
  const unsigned char * in = source->getData();
  for (size_t i = 0; i < out.pixels.size(); i++) {
    const unsigned char * p = in + i * channels;
    unsigned char q[4];
    if (channels >= 3) q[0] = p[0], q[1] = p[1], q[2] = p[2], q[3] = channels == 4 ? p[3] : 255;  // grey, grey and alpha, colour
    else q[0] = q[1] = q[2] = p[0], q[3] = channels == 2 ? p[1] : 255;
    std::memcpy(&out.pixels[i], q, 4);
  }
  return out;
}

}  // namespace

fs::path defaultIconCacheDir() {
  std::error_code ec;
#ifdef _WIN32
  if (const char * base = std::getenv("LOCALAPPDATA")) return fs::path(base) / "stocks" / "icons";
#else
  if (const char * base = std::getenv("XDG_CACHE_HOME"); base && *base) return fs::path(base) / "stocks" / "icons";
  if (const char * home = std::getenv("HOME"); home && *home) return fs::path(home) / ".cache" / "stocks" / "icons";
#endif
  return {};
}

IconLoader::IconLoader(std::unique_ptr<Http> http, Options options, std::function<void()> loaded)
    : http_(std::move(http)), options_(std::move(options)), loaded_(std::move(loaded)) {
  burst_size_ = std::max<size_t>(1, options_.burst);
  thread_ = std::thread([this] { run(); });
}

IconLoader::~IconLoader() {
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
  }
  wake_.notify_all();
  thread_.join();
}

std::shared_ptr<const TableIcon> IconLoader::get(const std::string & url) {
  std::lock_guard lock(mutex_);
  auto at = in_memory_.find(url);
  if (at == in_memory_.end()) return nullptr;
  memory_.splice(memory_.begin(), memory_, at->second);
  return at->second->second;
}

void IconLoader::want(const std::vector<std::string> & urls) {
  {
    std::lock_guard lock(mutex_);
    queue_.clear();
    std::unordered_set<std::string> seen;
    for (const std::string & url : urls)
      if (!url.empty() && !in_memory_.count(url) && !failed_.count(url) && seen.insert(url).second) queue_.push_back(url);
  }
  wake_.notify_all();
}

bool IconLoader::refused() const {
  std::lock_guard lock(mutex_);
  return refused_;
}

void IconLoader::waitIdle() {
  std::unique_lock lock(mutex_);
  idle_.wait(lock, [&] { return queue_.empty() && !busy_; });
}

void IconLoader::remember(const std::string & url, std::shared_ptr<const TableIcon> icon) {
  std::lock_guard lock(mutex_);
  if (in_memory_.count(url)) return;
  memory_bytes_ += bytesOf(*icon);
  memory_.emplace_front(url, std::move(icon));
  in_memory_[url] = memory_.begin();
  while (memory_bytes_ > options_.max_memory_bytes && memory_.size() > 1) {  // the newest stays, however large
    memory_bytes_ -= bytesOf(*memory_.back().second);
    in_memory_.erase(memory_.back().first);
    memory_.pop_back();
  }
}

void IconLoader::run() {
  std::unique_lock lock(mutex_);
  while (true) {
    wake_.wait(lock, [&] { return stop_ || !queue_.empty(); });
    if (stop_) return;
    const std::string url = queue_.front();
    queue_.pop_front();
    busy_ = true;
    lock.unlock();
    bool used_network = false;
    const bool arrived = load(url, used_network);
    if (arrived && loaded_) loaded_();
    lock.lock();
    busy_ = false;
    // Requests come in bursts of about `burst`, each followed by a pause (both a little random); a destructor cuts it short.
    if (used_network && ++sent_ >= burst_size_) {
      sent_ = 0;
      burst_size_ = std::max<size_t>(1, options_.burst * (80 + random_() % 41) / 100);
      const auto pause = options_.burst_pause * (50 + random_() % 101) / 100;
      if (pause.count() > 0) wake_.wait_for(lock, pause, [&] { return stop_; });
    }
    if (queue_.empty()) idle_.notify_all();
  }
}

bool IconLoader::load(const std::string & url, bool & used_network) {
  {
    std::lock_guard lock(mutex_);
    if (in_memory_.count(url) || failed_.count(url)) return false;
  }
  if (auto icon = fromDisk(url)) {
    remember(url, std::move(icon));
    return true;
  }
  {
    std::lock_guard lock(mutex_);
    if (refused_ || std::chrono::steady_clock::now() < offline_until_) return false;
  }
  used_network = true;
  HttpResult r = http_->get(url, {{"Accept", "image/*"}});
  // The mutex is held only to change state: decoding and the disk are the thread's own, so get() never waits for them.
  Decoded picture;
  if (r.ok()) picture = decode(r.body);
  {
    std::lock_guard lock(mutex_);
    if (r.status == 403 || r.status == 429) {
      refused_ = true;
      return false;
    }
    if (r.status == 0 || r.status >= 500) {
      offline_until_ = std::chrono::steady_clock::now() + options_.offline_pause;
      return false;
    }
    if (picture.pixels.empty()) {  // a 404 or other refusal, or no image
      failed_.insert(url);
      return false;
    }
  }
  toDisk(url, r.body);  // the file as it came: nothing is lost to the cache
  remember(url, makeIcon(url, std::move(picture)));
  return true;
}

std::shared_ptr<const TableIcon> IconLoader::fromDisk(const std::string & url) {
  if (options_.cache_dir.empty()) return nullptr;
  const fs::path path = options_.cache_dir / (hex(hashOf(url)) + ".img");
  std::string bytes;
  {
    std::ifstream in(path, std::ios::binary);
    if (!in) return nullptr;
    bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  Decoded picture = decode(bytes);
  if (picture.pixels.empty()) return nullptr;  // damaged: fetched again
  std::error_code ec;
  fs::last_write_time(path, fs::file_time_type::clock::now(), ec);  // used now: the last to be trimmed
  return makeIcon(url, std::move(picture));
}

// Only the loader's thread touches the disk.
void IconLoader::toDisk(const std::string & url, const std::string & bytes) {
  if (options_.cache_dir.empty()) return;
  std::error_code ec;
  fs::create_directories(options_.cache_dir, ec);
  const fs::path path = options_.cache_dir / (hex(hashOf(url)) + ".img");
  const fs::path part = path.string() + ".part";
  {
    std::ofstream out(part, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) {
      out.close();
      fs::remove(part, ec);
      return;
    }
  }
  fs::rename(part, path, ec);  // a run stopped half way leaves no half file under the real name
  if (++disk_writes_ % 50 == 1) trimDisk();
}

// The files over the cap go, least recently used first, down to 80 % of it.
void IconLoader::trimDisk() {
  std::error_code ec;
  struct File {
    fs::path path;
    fs::file_time_type time;
    uintmax_t size;
  };
  std::vector<File> files;
  uintmax_t total = 0;
  for (const auto & entry : fs::directory_iterator(options_.cache_dir, ec)) {
    if (!entry.is_regular_file(ec) || entry.path().extension() != ".img" && entry.path().extension() != ".icn") continue;
    files.push_back({entry.path(), entry.last_write_time(ec), entry.file_size(ec)});
    total += files.back().size;
  }
  if (total <= options_.max_disk_bytes) return;
  std::sort(files.begin(), files.end(), [](const File & a, const File & b) { return a.time < b.time; });
  const uintmax_t goal = options_.max_disk_bytes / 10 * 8;
  for (const File & f : files) {
    if (total <= goal) break;
    if (fs::remove(f.path, ec)) total -= f.size;
  }
}
