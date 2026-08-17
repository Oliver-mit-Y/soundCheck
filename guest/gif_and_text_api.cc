// -*- mode: c++; c-basic-offset: 2; indent-tabs-mode: nil; -*-
//
// Show an API-provided GIF/image full-screen on an RGB matrix and overlay a
// small scrolling text bar from a JSON API.

#include "led-matrix.h"
#include "graphics.h"

#include <Magick++.h>
#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <getopt.h>
#include <map>
#include <memory>
#include <mutex>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <string>
#include <sys/time.h>
#include <thread>
#include <time.h>
#include <vector>

using rgb_matrix::Color;
using rgb_matrix::FrameCanvas;
using rgb_matrix::RGBMatrix;

volatile bool interrupt_received = false;
static void InterruptHandler(int signo) {
  interrupt_received = true;
}

typedef int64_t tmillis_t;

static tmillis_t GetTimeInMillis() {
  struct timeval tp;
  gettimeofday(&tp, NULL);
  return tp.tv_sec * 1000 + tp.tv_usec / 1000;
}

static void SleepMillis(tmillis_t milli_seconds) {
  if (milli_seconds <= 0) return;
  struct timespec ts;
  ts.tv_sec = milli_seconds / 1000;
  ts.tv_nsec = (milli_seconds % 1000) * 1000000;
  nanosleep(&ts, NULL);
}

static void add_micros(struct timespec *accumulator, long micros) {
  const long billion = 1000000000;
  const int64_t nanos = (int64_t) micros * 1000;
  accumulator->tv_sec += nanos / billion;
  accumulator->tv_nsec += nanos % billion;
  while (accumulator->tv_nsec > billion) {
    accumulator->tv_nsec -= billion;
    accumulator->tv_sec += 1;
  }
}

struct Pixel {
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

struct Frame {
  std::vector<Pixel> pixels;
  tmillis_t delay_ms;
};

struct Animation {
  std::vector<Frame> frames;
  int width;
  int height;
};

enum SignalStatus {
  SIGNAL_NORMAL = 0,
  SIGNAL_ERROR = 1,
  SIGNAL_LOADING = 2
};

struct SharedState {
  std::mutex mutex;
  std::shared_ptr<Animation> animation;
  std::string text = "error";
  SignalStatus signal_status = SIGNAL_ERROR;
  uint64_t generation = 0;
};

struct Config {
  std::string json_url;
  std::string gif_url;
  std::string idle_image;
  std::vector<std::string> keys;
  std::string font_file;
  int text_bar_height = 8;
  float scroll_speed = 7.0f;
  int gif_delay_override_ms = -1;
  int poll_ms = 1000;
  int letter_spacing = 0;
  bool pre_processing = true;
  Color text_color = Color(255, 255, 255);
  Color bar_color = Color(0, 0, 0);
};

static std::string Preview(const std::string &value, size_t max_len) {
  std::string out = value.substr(0, std::min(max_len, value.size()));
  for (size_t i = 0; i < out.size(); ++i) {
    if (out[i] == '\n' || out[i] == '\r' || out[i] == '\t') out[i] = ' ';
  }
  if (value.size() > max_len) out.append("...");
  return out;
}

static std::string JoinKeys(const std::vector<std::string> &keys) {
  std::string out;
  for (size_t i = 0; i < keys.size(); ++i) {
    if (i > 0) out.append(",");
    out.append(keys[i]);
  }
  return out;
}

static size_t CurlWriteCallback(void *contents, size_t size, size_t nmemb,
                                void *userp) {
  const size_t total = size * nmemb;
  std::string *target = reinterpret_cast<std::string*>(userp);
  target->append(reinterpret_cast<const char*>(contents), total);
  return total;
}

static bool FetchUrl(const std::string &url, std::string *body,
                     std::string *err) {
  CURL *curl = curl_easy_init();
  if (curl == NULL) {
    *err = "curl init failed";
    return false;
  }

  body->clear();
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 2000L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 5000L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, body);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "gif-and-text-api/1.0");

  const CURLcode res = curl_easy_perform(curl);
  long response_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
  curl_easy_cleanup(curl);

  if (res != CURLE_OK) {
    *err = curl_easy_strerror(res);
    return false;
  }
  if (response_code >= 400) {
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "http status %ld", response_code);
    *err = buffer;
    return false;
  }
  fprintf(stderr, "[fetch] ok url=%s http=%ld bytes=%zu\n",
          url.c_str(), response_code, body->size());
  return true;
}

static std::string Trim(const std::string &value) {
  size_t start = 0;
  while (start < value.size() && isspace(value[start])) ++start;
  size_t end = value.size();
  while (end > start && isspace(value[end - 1])) --end;
  return value.substr(start, end - start);
}

static std::vector<std::string> SplitKeys(const std::string &keys) {
  std::vector<std::string> result;
  size_t start = 0;
  while (start <= keys.size()) {
    const size_t comma = keys.find(',', start);
    const size_t end = comma == std::string::npos ? keys.size() : comma;
    const std::string key = Trim(keys.substr(start, end - start));
    if (!key.empty()) result.push_back(key);
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return result;
}

static bool ParseColor(Color *c, const char *str) {
  return sscanf(str, "%hhu,%hhu,%hhu", &c->r, &c->g, &c->b) == 3;
}

static bool ParseBool(const char *str, bool *value) {
  if (strcasecmp(str, "true") == 0 || strcmp(str, "1") == 0 ||
      strcasecmp(str, "yes") == 0 || strcasecmp(str, "on") == 0) {
    *value = true;
    return true;
  }
  if (strcasecmp(str, "false") == 0 || strcmp(str, "0") == 0 ||
      strcasecmp(str, "no") == 0 || strcasecmp(str, "off") == 0) {
    *value = false;
    return true;
  }
  return false;
}

static bool JsonIsNull(const std::string &json) {
  return Trim(json) == "null";
}

static bool ParseJsonString(const std::string &json, size_t *pos,
                            std::string *out) {
  if (*pos >= json.size() || json[*pos] != '"') return false;
  ++(*pos);
  out->clear();
  while (*pos < json.size()) {
    const char ch = json[(*pos)++];
    if (ch == '"') return true;
    if (ch != '\\') {
      out->push_back(ch);
      continue;
    }
    if (*pos >= json.size()) return false;
    const char esc = json[(*pos)++];
    switch (esc) {
    case '"': out->push_back('"'); break;
    case '\\': out->push_back('\\'); break;
    case '/': out->push_back('/'); break;
    case 'b': out->push_back('\b'); break;
    case 'f': out->push_back('\f'); break;
    case 'n': out->push_back(' '); break;
    case 'r': out->push_back(' '); break;
    case 't': out->push_back(' '); break;
    case 'u':
      if (*pos + 4 <= json.size()) {
        out->push_back('?');
        *pos += 4;
      }
      break;
    default:
      out->push_back(esc);
      break;
    }
  }
  return false;
}

static void SkipJsonValue(const std::string &json, size_t *pos) {
  int depth = 0;
  bool in_string = false;
  bool escaped = false;
  while (*pos < json.size()) {
    const char ch = json[*pos];
    if (in_string) {
      escaped = (!escaped && ch == '\\');
      if (!escaped && ch == '"') in_string = false;
      if (escaped && ch != '\\') escaped = false;
      ++(*pos);
      continue;
    }
    if (ch == '"') {
      in_string = true;
    } else if (ch == '{' || ch == '[') {
      ++depth;
    } else if (ch == '}' || ch == ']') {
      if (depth == 0) return;
      --depth;
    } else if (depth == 0 && ch == ',') {
      return;
    }
    ++(*pos);
  }
}

static std::string ParseJsonPrimitive(const std::string &json, size_t *pos) {
  const size_t start = *pos;
  while (*pos < json.size() && json[*pos] != ',' && json[*pos] != '}') {
    ++(*pos);
  }
  return Trim(json.substr(start, *pos - start));
}

static bool ExtractFlatJsonValues(const std::string &json,
                                  std::map<std::string, std::string> *values) {
  size_t pos = 0;
  while (pos < json.size() && isspace(json[pos])) ++pos;
  if (pos >= json.size() || json[pos] != '{') return false;
  ++pos;
  while (pos < json.size()) {
    while (pos < json.size() && (isspace(json[pos]) || json[pos] == ',')) ++pos;
    if (pos < json.size() && json[pos] == '}') return true;

    std::string key;
    if (!ParseJsonString(json, &pos, &key)) return false;
    while (pos < json.size() && isspace(json[pos])) ++pos;
    if (pos >= json.size() || json[pos] != ':') return false;
    ++pos;
    while (pos < json.size() && isspace(json[pos])) ++pos;

    std::string value;
    if (pos < json.size() && json[pos] == '"') {
      if (!ParseJsonString(json, &pos, &value)) return false;
    } else if (pos < json.size() && (json[pos] == '{' || json[pos] == '[')) {
      const size_t start = pos;
      SkipJsonValue(json, &pos);
      value = Trim(json.substr(start, pos - start));
    } else {
      value = ParseJsonPrimitive(json, &pos);
    }
    (*values)[key] = value;
  }
  return false;
}

static std::string BuildScrollText(const std::string &json,
                                   const std::vector<std::string> &keys) {
  std::map<std::string, std::string> values;
  if (!ExtractFlatJsonValues(json, &values)) {
    fprintf(stderr, "[json] parse failed body='%s'\n",
            Preview(json, 180).c_str());
    return "error";
  }

  std::string out;
  for (size_t i = 0; i < keys.size(); ++i) {
    const std::map<std::string, std::string>::const_iterator it =
      values.find(keys[i]);
    if (it == values.end()) {
      fprintf(stderr, "[json] key missing: %s\n", keys[i].c_str());
      continue;
    }
    if (!out.empty()) out.append(", ");
    out.append(keys[i]).append(":").append(it->second);
  }
  fprintf(stderr, "[json] extracted text='%s'\n",
          Preview(out, 180).c_str());
  return out.empty() ? "error" : out;
}

static void PreprocessImage(Magick::Image *img, int target_width,
                            int target_height, bool pre_processing) {
  if ((int)img->columns() != target_width ||
      (int)img->rows() != target_height) {
    img->scale(Magick::Geometry(target_width, target_height));
  }
  if (pre_processing) {
    img->gamma(0.6);
  }
  img->type(Magick::TrueColorType);
}

static bool ExportFramePixels(Magick::Image *img, int target_width,
                              int target_height, Frame *frame,
                              std::string *err) {
  std::vector<uint8_t> rgb(target_width * target_height * 3);
  try {
    img->write(0, 0, target_width, target_height, "RGB", Magick::CharPixel,
               rgb.data());
  } catch (std::exception &e) {
    if (e.what()) *err = e.what();
    return false;
  }

  frame->pixels.resize(target_width * target_height);
  for (size_t i = 0, p = 0; i < frame->pixels.size(); ++i, p += 3) {
    frame->pixels[i] = Pixel{rgb[p], rgb[p + 1], rgb[p + 2]};
  }
  return true;
}

static bool FramesToAnimation(const std::vector<Magick::Image> &frames,
                              int target_width, int target_height,
                              int delay_override_ms, bool pre_processing,
                              std::shared_ptr<Animation> *animation,
                              std::string *err) {
  std::shared_ptr<Animation> result(new Animation());
  result->width = target_width;
  result->height = target_height;
  result->frames.reserve(frames.size());
  for (size_t i = 0; i < frames.size(); ++i) {
    Magick::Image img = frames[i];
    PreprocessImage(&img, target_width, target_height, pre_processing);

    Frame frame;
    frame.delay_ms = delay_override_ms >= 0
      ? delay_override_ms
      : (frames.size() > 1 ? img.animationDelay() * 10 : 1000);
    if (frame.delay_ms <= 0) frame.delay_ms = 100;

    if (!ExportFramePixels(&img, target_width, target_height, &frame, err)) {
      return false;
    }
    result->frames.push_back(frame);
  }

  *animation = result;
  return true;
}

static bool LoadImageData(const std::string &data, int target_width,
                          int target_height, int delay_override_ms,
                          bool pre_processing,
                          std::shared_ptr<Animation> *animation,
                          std::string *err) {
  const tmillis_t start_ms = GetTimeInMillis();
  std::vector<Magick::Image> source;
  std::vector<Magick::Image> frames;
  try {
    Magick::Blob blob(data.data(), data.size());
    Magick::readImages(&source, blob);
    if (source.size() > 1) {
      Magick::coalesceImages(&frames, source.begin(), source.end());
    } else {
      frames = source;
    }
  } catch (std::exception &e) {
    if (e.what()) *err = e.what();
    return false;
  }
  if (frames.empty()) {
    *err = "no image frames";
    return false;
  }
  if (!FramesToAnimation(frames, target_width, target_height,
                         delay_override_ms, pre_processing, animation, err)) {
    return false;
  }
  fprintf(stderr, "[image] decoded API image frames=%zu target=%dx%d pre_processing=%s took=%lldms\n",
          frames.size(), target_width, target_height,
          pre_processing ? "true" : "false",
          (long long)(GetTimeInMillis() - start_ms));
  return true;
}

static bool LoadImageFile(const std::string &path, int target_width,
                          int target_height, int delay_override_ms,
                          bool pre_processing,
                          std::shared_ptr<Animation> *animation,
                          std::string *err) {
  const tmillis_t start_ms = GetTimeInMillis();
  std::vector<Magick::Image> source;
  std::vector<Magick::Image> frames;
  try {
    Magick::readImages(&source, path);
    if (source.size() > 1) {
      Magick::coalesceImages(&frames, source.begin(), source.end());
    } else {
      frames = source;
    }
  } catch (std::exception &e) {
    if (e.what()) *err = e.what();
    return false;
  }
  if (frames.empty()) {
    *err = "no image frames";
    return false;
  }
  if (!FramesToAnimation(frames, target_width, target_height,
                         delay_override_ms, pre_processing, animation, err)) {
    return false;
  }
  fprintf(stderr, "[image] decoded idle image path=%s frames=%zu target=%dx%d pre_processing=%s took=%lldms\n",
          path.c_str(), frames.size(), target_width, target_height,
          pre_processing ? "true" : "false",
          (long long)(GetTimeInMillis() - start_ms));
  return true;
}

static void FillRect(FrameCanvas *canvas, int x0, int y0, int x1, int y1,
                     const Color &color) {
  canvas->SubFill(x0, y0, x1 - x0, y1 - y0, color.r, color.g, color.b);
}

static void RenderFrame(FrameCanvas *canvas, const Animation &animation,
                        size_t frame_index, int bar_y, int bar_height,
                        const Color &bar_color, bool draw_bar) {
  const Frame &frame = animation.frames[frame_index];
  for (int y = 0; y < animation.height; ++y) {
    for (int x = 0; x < animation.width; ++x) {
      const Pixel &p = frame.pixels[y * animation.width + x];
      canvas->SetPixel(x, y, p.r, p.g, p.b);
    }
  }
  if (draw_bar) {
    FillRect(canvas, 0, bar_y, animation.width, animation.height, bar_color);
  }
}

static void PollApis(const Config config, SharedState *state,
                     int width, int height) {
  std::string last_json;
  bool showing_idle = false;
  bool last_state_was_idle = false;
  std::shared_ptr<Animation> idle_animation_cache;
  fprintf(stderr, "[poll] starting json_url=%s gif_url=%s poll_ms=%d keys=%s\n",
          config.json_url.c_str(), config.gif_url.c_str(), config.poll_ms,
          JoinKeys(config.keys).c_str());
  while (!interrupt_received) {
    std::string json;
    std::string err;
    fprintf(stderr, "[poll] fetching JSON: %s\n", config.json_url.c_str());
    if (!FetchUrl(config.json_url, &json, &err)) {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->text = "error";
      state->signal_status = SIGNAL_ERROR;
      ++state->generation;
      fprintf(stderr, "JSON fetch failed: %s\n", err.c_str());
    } else {
      const bool json_is_null = JsonIsNull(json);
      const bool forced_idle_to_data_change = last_state_was_idle && !json_is_null;
      if (json != last_json || forced_idle_to_data_change) {
        fprintf(stderr, "[poll] JSON changed%s bytes=%zu preview='%s'\n",
                forced_idle_to_data_change ? " (idle->data)" : "",
                json.size(), Preview(json, 180).c_str());
        if (json_is_null) {
          fprintf(stderr, "[poll] JSON is null; using idle mode\n");
          last_json = "null";
          last_state_was_idle = true;
          bool idle_ok = config.idle_image.empty() || idle_animation_cache;
          if (!config.idle_image.empty() && !idle_animation_cache) {
            fprintf(stderr, "[idle] loading idle image: %s\n",
                    config.idle_image.c_str());
            {
              std::lock_guard<std::mutex> lock(state->mutex);
              state->signal_status = SIGNAL_LOADING;
              ++state->generation;
            }
            if (LoadImageFile(config.idle_image, width, height,
                              config.gif_delay_override_ms,
                              config.pre_processing,
                              &idle_animation_cache,
                              &err)) {
              idle_ok = true;
            } else {
              fprintf(stderr, "Idle image load failed: %s\n", err.c_str());
            }
          }
          {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->text = idle_ok ? "" : "error";
            state->signal_status = idle_ok ? SIGNAL_NORMAL : SIGNAL_ERROR;
            if (idle_animation_cache) {
              state->animation = idle_animation_cache;
              showing_idle = true;
            } else if (config.idle_image.empty()) {
              state->animation.reset();
              showing_idle = false;
            }
            ++state->generation;
          }
          fprintf(stderr, "[state] idle applied ok=%s generation=%llu\n",
                  idle_ok ? "yes" : "no",
                  (unsigned long long)state->generation);
        } else {
          std::shared_ptr<Animation> new_animation;
          std::string gif_data;
          const std::string text = BuildScrollText(json, config.keys);
          fprintf(stderr, "[poll] fetching GIF/image: %s\n",
                  config.gif_url.c_str());
          {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->signal_status = SIGNAL_LOADING;
            ++state->generation;
          }
          if (FetchUrl(config.gif_url, &gif_data, &err) &&
              LoadImageData(gif_data, width, height, config.gif_delay_override_ms,
                            config.pre_processing,
                            &new_animation, &err)) {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->text = text;
            state->animation = new_animation;
            state->signal_status = text == "error" ? SIGNAL_ERROR : SIGNAL_NORMAL;
            ++state->generation;
            showing_idle = false;
            last_json = json;
            last_state_was_idle = false;
            fprintf(stderr, "[state] active applied frames=%zu text='%s' generation=%llu\n",
                    new_animation->frames.size(), Preview(text, 120).c_str(),
                    (unsigned long long)state->generation);
          } else {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->text = "error";
            state->signal_status = SIGNAL_ERROR;
            ++state->generation;
            fprintf(stderr, "GIF update failed: %s\n", err.c_str());
          }
        }
      } else {
        fprintf(stderr, "[poll] JSON unchanged\n");
      }
    }

    const tmillis_t end = GetTimeInMillis() + config.poll_ms;
    while (!interrupt_received && GetTimeInMillis() < end) {
      SleepMillis(50);
    }
  }
}

static int usage(const char *progname) {
  fprintf(stderr, "usage: %s [options] --led-rows=32 --led-cols=64 ...\n",
          progname);
  fprintf(stderr, "Options:\n");
  fprintf(stderr,
          "\t--keys=k1,k2,k3          : JSON keys to show as key:value pairs.\n"
          "\t--json-url=URL           : URL returning a JSON object or null.\n"
          "\t--gif-url=URL            : URL returning the active GIF/image.\n"
          "\t--idle-image=PATH        : Optional image/GIF shown while JSON is null.\n"
          "\t--font=PATH              : BDF font used for the text bar.\n"
          "\t--text-bar-height=N      : Height of the bottom text bar in pixels.\n"
          "\t--scroll-speed=N         : Approximate letters per second.\n"
          "\t--gif-speed=N            : Override GIF frame delay in milliseconds.\n"
          "\t--poll-ms=N              : JSON polling interval in milliseconds.\n"
          "\t--pre-processing=true|false : Apply gamma 0.6 while loading images (default: true).\n"
          "\t--text-color=r,g,b       : Text color, default 255,255,255.\n"
          "\t--bar-color=r,g,b        : Text bar background, default 0,0,0.\n"
          "\t--letter-spacing=N       : Extra font spacing in pixels.\n");
  fprintf(stderr, "\nGeneral LED matrix options:\n");
  rgb_matrix::PrintMatrixFlags(stderr);
  return 1;
}

int main(int argc, char *argv[]) {
  Magick::InitializeMagick(*argv);
  curl_global_init(CURL_GLOBAL_DEFAULT);

  RGBMatrix::Options matrix_options;
  rgb_matrix::RuntimeOptions runtime_opt;
  runtime_opt.drop_priv_user = getenv("SUDO_UID");
  runtime_opt.drop_priv_group = getenv("SUDO_GID");
  if (!rgb_matrix::ParseOptionsFromFlags(&argc, &argv,
                                         &matrix_options, &runtime_opt)) {
    return usage(argv[0]);
  }

  Config config;
  static struct option long_options[] = {
    {"keys", required_argument, 0, 1000},
    {"json-url", required_argument, 0, 1001},
    {"gif-url", required_argument, 0, 1002},
    {"font", required_argument, 0, 1003},
    {"text-bar-height", required_argument, 0, 1004},
    {"scroll-speed", required_argument, 0, 1005},
    {"gif-speed", required_argument, 0, 1006},
    {"idle-image", required_argument, 0, 1007},
    {"poll-ms", required_argument, 0, 1008},
    {"text-color", required_argument, 0, 1009},
    {"bar-color", required_argument, 0, 1010},
    {"letter-spacing", required_argument, 0, 1011},
    {"pre-processing", required_argument, 0, 1012},
    {"help", no_argument, 0, 'h'},
    {0, 0, 0, 0}
  };

  int opt;
  int option_index = 0;
  while ((opt = getopt_long(argc, argv, "h", long_options,
                            &option_index)) != -1) {
    switch (opt) {
    case 1000: config.keys = SplitKeys(optarg); break;
    case 1001: config.json_url = optarg; break;
    case 1002: config.gif_url = optarg; break;
    case 1003: config.font_file = optarg; break;
    case 1004: config.text_bar_height = atoi(optarg); break;
    case 1005: config.scroll_speed = atof(optarg); break;
    case 1006: config.gif_delay_override_ms = atoi(optarg); break;
    case 1007: config.idle_image = optarg; break;
    case 1008: config.poll_ms = std::max(100, atoi(optarg)); break;
    case 1009:
      if (!ParseColor(&config.text_color, optarg)) return usage(argv[0]);
      break;
    case 1010:
      if (!ParseColor(&config.bar_color, optarg)) return usage(argv[0]);
      break;
    case 1011: config.letter_spacing = atoi(optarg); break;
    case 1012:
      if (!ParseBool(optarg, &config.pre_processing)) {
        fprintf(stderr, "Invalid --pre-processing value: %s\n", optarg);
        return usage(argv[0]);
      }
      break;
    case 'h':
    default:
      return usage(argv[0]);
    }
  }

  if (config.json_url.empty() || config.gif_url.empty() ||
      config.font_file.empty() || config.keys.empty()) {
    fprintf(stderr, "Need --json-url, --gif-url, --font and --keys.\n");
    return usage(argv[0]);
  }

  rgb_matrix::Font font;
  if (!font.LoadFont(config.font_file.c_str())) {
    fprintf(stderr, "Couldn't load font '%s'\n", config.font_file.c_str());
    return 1;
  }

  RGBMatrix *matrix = RGBMatrix::CreateFromOptions(matrix_options, runtime_opt);
  if (matrix == NULL) return 1;
  FrameCanvas *offscreen = matrix->CreateFrameCanvas();

  config.text_bar_height = std::max(font.height(), config.text_bar_height);
  config.text_bar_height = std::min(config.text_bar_height, matrix->height());
  const int bar_y = matrix->height() - config.text_bar_height;
  const int text_y = bar_y + std::max(0, (config.text_bar_height - font.height()) / 2);

  SharedState state;
  signal(SIGTERM, InterruptHandler);
  signal(SIGINT, InterruptHandler);

  fprintf(stderr,
          "[config] json_url=%s gif_url=%s idle_image=%s font=%s keys=%s "
          "bar_height=%d scroll_speed=%.2f gif_speed=%d poll_ms=%d "
          "pre_processing=%s\n",
          config.json_url.c_str(), config.gif_url.c_str(),
          config.idle_image.empty() ? "(none)" : config.idle_image.c_str(),
          config.font_file.c_str(), JoinKeys(config.keys).c_str(),
          config.text_bar_height, config.scroll_speed,
          config.gif_delay_override_ms, config.poll_ms,
          config.pre_processing ? "true" : "false");

  std::thread poller(PollApis, config, &state, matrix->width(), matrix->height());

  const int scroll_direction = config.scroll_speed >= 0 ? -1 : 1;
  const float abs_speed = fabs(config.scroll_speed);
  int delay_speed_usec = 1000000;
  if (abs_speed > 0) {
    delay_speed_usec = 1000000 / abs_speed / font.CharacterWidth('W');
  }

  int text_x = scroll_direction < 0 ? matrix->width() : 0;
  int text_length = 0;
  std::string current_text = "error";
  SignalStatus current_signal_status = SIGNAL_ERROR;
  std::shared_ptr<Animation> current_animation;
  uint64_t current_generation = 0;
  size_t frame_index = 0;
  tmillis_t next_gif_frame_ms = 0;
  struct timespec next_text_frame = {0, 0};

  fprintf(stderr, "Size: %dx%d. CTRL-C for exit.\n",
          matrix->width(), matrix->height());

  while (!interrupt_received) {
    {
      std::lock_guard<std::mutex> lock(state.mutex);
      if (state.generation != current_generation) {
        const bool animation_changed = state.animation != current_animation;
        current_generation = state.generation;
        current_animation = state.animation;
        current_text = state.text;
        current_signal_status = state.signal_status;
        text_x = scroll_direction < 0 ? matrix->width() : 0;
        if (animation_changed) {
          frame_index = 0;
          next_gif_frame_ms = 0;
        }
      }
    }

    const bool draw_text_bar =
      !current_text.empty() && current_text != "error";

    if (current_animation && !current_animation->frames.empty()) {
      const tmillis_t now = GetTimeInMillis();
      if (next_gif_frame_ms == 0) {
        next_gif_frame_ms = now + current_animation->frames[frame_index].delay_ms;
      } else if (now >= next_gif_frame_ms) {
        frame_index = (frame_index + 1) % current_animation->frames.size();
        next_gif_frame_ms = now + current_animation->frames[frame_index].delay_ms;
      }
      RenderFrame(offscreen, *current_animation, frame_index, bar_y,
                  config.text_bar_height, config.bar_color,
                  draw_text_bar);
    } else {
      offscreen->Fill(0, 0, 0);
      if (draw_text_bar) {
        FillRect(offscreen, 0, bar_y, matrix->width(), matrix->height(),
                 config.bar_color);
      }
    }

    if (draw_text_bar) {
      text_length = rgb_matrix::DrawText(offscreen, font, text_x,
                                         text_y + font.baseline(),
                                         config.text_color, NULL,
                                         current_text.c_str(),
                                         config.letter_spacing);
      text_x += scroll_direction;
      if ((scroll_direction < 0 && text_x + text_length < 0) ||
          (scroll_direction > 0 && text_x > matrix->width())) {
        text_x = scroll_direction < 0 ? matrix->width() : -text_length;
      }
    }

    if (current_signal_status == SIGNAL_LOADING) {
      offscreen->SetPixel(matrix->width() - 1, 0, 0, 0, 255);
    } else if (current_signal_status == SIGNAL_ERROR) {
      offscreen->SetPixel(matrix->width() - 1, 0, 255, 0, 0);
    }

    if (abs_speed > 0) {
      if (next_text_frame.tv_sec == 0 && next_text_frame.tv_nsec == 0) {
        clock_gettime(CLOCK_MONOTONIC, &next_text_frame);
      } else {
        add_micros(&next_text_frame, delay_speed_usec);
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
                        &next_text_frame, NULL);
      }
    }
    offscreen = matrix->SwapOnVSync(offscreen);
  }

  poller.join();
  matrix->Clear();
  delete matrix;
  curl_global_cleanup();
  return 0;
}
