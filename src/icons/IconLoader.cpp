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

constexpr char kMagic[4] = {'I', 'C', 'N', '2'};  // 1 was 32 pixels wide: such a file is read as no icon
constexpr size_t kPixelBytes = static_cast<size_t>(kIconSize) * kIconSize * 4;

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

std::shared_ptr<const TableIcon> makeIcon(const std::string & url, std::vector<uint32_t> pixels) {
  auto icon = std::make_shared<TableIcon>();
  icon->id = hashOf(url);
  icon->width = icon->height = kIconSize;
  icon->pixels = std::make_shared<const std::vector<uint32_t>>(std::move(pixels));
  return icon;
}

// An image's bytes as kIconSize x kIconSize straight RGBA, the picture fitted and centred; empty if it is no image.
std::vector<uint32_t> decode(const std::string & bytes) {
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
  if (w > 8192 || h > 8192 || channels > 4) return {};
  // Four channels first: grey, grey and alpha and colour all become RGBA.
  std::vector<unsigned char> rgba(static_cast<size_t>(w) * h * 4);
  const unsigned char * in = source->getData();
  for (size_t i = 0; i < static_cast<size_t>(w) * h; i++) {
    const unsigned char * p = in + i * channels;
    unsigned char * q = &rgba[i * 4];
    if (channels >= 3) q[0] = p[0], q[1] = p[1], q[2] = p[2], q[3] = channels == 4 ? p[3] : 255;
    else q[0] = q[1] = q[2] = p[0], q[3] = channels == 2 ? p[1] : 255;
  }
  canvas::ImageData straight(rgba.data(), w, h, 4);
  const double fit = std::min(static_cast<double>(kIconSize) / w, static_cast<double>(kIconSize) / h);
  const unsigned tw = std::clamp(static_cast<unsigned>(w * fit + 0.5), 1u, static_cast<unsigned>(kIconSize));
  const unsigned th = std::clamp(static_cast<unsigned>(h * fit + 0.5), 1u, static_cast<unsigned>(kIconSize));
  auto scaled = straight.scale(tw, th);
  if (!scaled || scaled->getNumChannels() != 4) return {};
  std::vector<uint32_t> out(static_cast<size_t>(kIconSize) * kIconSize, 0);  // transparent
  const unsigned x0 = (kIconSize - tw) / 2, y0 = (kIconSize - th) / 2;
  const unsigned char * s = scaled->getData();
  for (unsigned y = 0; y < th; y++)
    for (unsigned x = 0; x < tw; x++) std::memcpy(&out[(y0 + y) * kIconSize + x0 + x], s + (static_cast<size_t>(y) * tw + x) * 4, 4);
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
  memory_.emplace_front(url, std::move(icon));
  in_memory_[url] = memory_.begin();
  while (memory_.size() > options_.max_memory_icons) {
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
  std::vector<uint32_t> pixels;
  if (r.ok()) pixels = decode(r.body);
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
    if (pixels.empty()) {  // a 404 or other refusal, or no image
      failed_.insert(url);
      return false;
    }
  }
  auto icon = makeIcon(url, std::move(pixels));
  toDisk(url, *icon);
  remember(url, std::move(icon));
  return true;
}

std::shared_ptr<const TableIcon> IconLoader::fromDisk(const std::string & url) {
  if (options_.cache_dir.empty()) return nullptr;
  const fs::path path = options_.cache_dir / (hex(hashOf(url)) + ".icn");
  std::ifstream in(path, std::ios::binary);
  char magic[4];
  if (!in.read(magic, 4) || std::memcmp(magic, kMagic, 4) != 0) return nullptr;
  std::vector<uint32_t> pixels(static_cast<size_t>(kIconSize) * kIconSize);
  if (!in.read(reinterpret_cast<char *>(pixels.data()), static_cast<std::streamsize>(kPixelBytes))) return nullptr;
  in.close();
  std::error_code ec;
  fs::last_write_time(path, fs::file_time_type::clock::now(), ec);  // used now: the last to be trimmed
  return makeIcon(url, std::move(pixels));
}

// Only the loader's thread touches the disk.
void IconLoader::toDisk(const std::string & url, const TableIcon & icon) {
  if (options_.cache_dir.empty()) return;
  std::error_code ec;
  fs::create_directories(options_.cache_dir, ec);
  const fs::path path = options_.cache_dir / (hex(hashOf(url)) + ".icn");
  const fs::path part = path.string() + ".part";
  {
    std::ofstream out(part, std::ios::binary | std::ios::trunc);
    out.write(kMagic, 4);
    out.write(reinterpret_cast<const char *>(icon.pixels->data()), static_cast<std::streamsize>(kPixelBytes));
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
    if (!entry.is_regular_file(ec) || entry.path().extension() != ".icn") continue;
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
