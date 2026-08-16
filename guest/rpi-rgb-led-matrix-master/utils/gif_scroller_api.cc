// -*- mode: c++; c-basic-offset: 2; indent-tabs-mode: nil; -*-
// GIF-Viewer with HTTP API for dynamic content
// Features:
// - Fetches GIF from HTTP API
// - Fetches JSON with text and image metadata
// - Configurable JSON keys to display
// - Dynamic content refresh when JSON changes
// - Shows "error" text on HTTP/GIF failures
// - Clears the matrix on null JSON, or shows optional idle media

#include "led-matrix.h"
#include "graphics.h"

#include <Magick++.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include <string>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <cstring>
#include <sstream>
#include <thread>
#include <mutex>
#include <chrono>
#include <algorithm>

using rgb_matrix::RGBMatrix;
using rgb_matrix::FrameCanvas;
using rgb_matrix::Canvas;
using rgb_matrix::Color;
using rgb_matrix::Font;
using rgb_matrix::DrawText;
using json = nlohmann::json;

// ============= GLOBAL CONFIG =============
const char* FONT_PATH = "fonts/7x13.bdf";

const int MATRIX_ROWS = 64;
const int MATRIX_COLS = 64;
const int CHAIN_LENGTH = 1;
const char* HARDWARE_MAPPING = "adafruit-hat-pwm";

const int TEXT_BAR_HEIGHT = 16;
const Color TEXT_COLOR(255, 255, 255);
const Color TEXT_BAR_COLOR(0, 0, 0);

// ============= MUTABLE CONFIG (via args) =============
struct Config {
  std::string api_gif_url;
  std::string api_json_url;
  std::vector<std::string> json_keys;
  int gif_scroll_speed;
  int text_scroll_speed;
  float gif_speed_multiplier;
  int api_poll_interval_ms;
  std::string font_path;
  int text_bar_height;
  std::string idle_path;
} g_config;

// ============= GLOBAL VARIABLES =============
volatile bool interrupt_received = false;
std::mutex g_api_mutex;
enum ApiStatus {
  STATUS_VALID,        // Valid data from API
  STATUS_HTTP_FAILED,  // HTTP/API/GIF request failed -> show "error"
  STATUS_JSON_NULL     // JSON returned null/no img -> blank or idle media
};
struct {
  std::string current_json_key;
  std::string current_text;
  ApiStatus status;
} g_api_data;

static void InterruptHandler(int signo) {
  interrupt_received = true;
}

// ============= HTTP HELPER =============
size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* s) {
  size_t newLength = size * nmemb;
  s->append((char*)contents, newLength);
  return newLength;
}

std::string JsonValueToString(const json& value) {
  if (value.is_string()) return value.get<std::string>();
  if (value.is_null()) return "";
  return value.dump();
}

std::string FetchUrl(const std::string& url) {
  CURL* curl = curl_easy_init();
  std::string response;
  
  if (!curl) {
    fprintf(stderr, "curl_easy_init failed\n");
    return "";
  }
  
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  
  CURLcode res = curl_easy_perform(curl);
  if (res != CURLE_OK) {
    fprintf(stderr, "curl_easy_perform failed: %s\n", curl_easy_strerror(res));
    curl_easy_cleanup(curl);
    return "";
  }
  
  curl_easy_cleanup(curl);
  return response;
}

// ============= API THREAD =============
void ApiPollerThread() {
  while (!interrupt_received) {
    std::string json_str = FetchUrl(g_config.api_json_url);
    
    if (json_str.empty()) {
      std::lock_guard<std::mutex> lock(g_api_mutex);
      g_api_data.status = STATUS_HTTP_FAILED;
      fprintf(stderr, "API JSON fetch failed, showing error text\n");
    } else {
      try {
        json j = json::parse(json_str);

        std::lock_guard<std::mutex> lock(g_api_mutex);

        if (j.is_null() || !j.is_object() || !j.contains("img") || j["img"].is_null()) {
          g_api_data.status = STATUS_JSON_NULL;
          g_api_data.current_json_key.clear();
          g_api_data.current_text.clear();
          fprintf(stderr, "API returned no active image, showing idle\n");
        } else {
          std::string img_value = JsonValueToString(j["img"]);
          std::string json_key = j.dump();
          std::string formatted_text;
          for (size_t i = 0; i < g_config.json_keys.size(); ++i) {
            const auto& key = g_config.json_keys[i];
            if (j.contains(key)) {
              formatted_text += key + ": " + JsonValueToString(j[key]);
            }
            if (i < g_config.json_keys.size() - 1) {
              formatted_text += "; ";
            }
          }

          if (json_key != g_api_data.current_json_key) {
            g_api_data.current_json_key = json_key;
            g_api_data.current_text = formatted_text;
            g_api_data.status = STATUS_VALID;
            printf("API update: img=%s, text=%s\n", img_value.c_str(), formatted_text.c_str());
          } else {
            g_api_data.current_text = formatted_text;
            g_api_data.status = STATUS_VALID;
          }
        }
      } catch (json::exception& e) {
        std::lock_guard<std::mutex> lock(g_api_mutex);
        g_api_data.status = STATUS_HTTP_FAILED;
        fprintf(stderr, "JSON parse error: %s\n", e.what());
      }
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(g_config.api_poll_interval_ms));
  }
}

// ============= GIF HELPER =============
bool DownloadAndSaveGif(const std::string& url, const std::string& output_file) {
  std::string data = FetchUrl(url);
  if (data.empty()) {
    fprintf(stderr, "Failed to download GIF from %s\n", url.c_str());
    return false;
  }

  std::string download_file = output_file + ".download";
  FILE* f = fopen(download_file.c_str(), "wb");
  if (!f) {
    fprintf(stderr, "Failed to open file %s for writing\n", download_file.c_str());
    return false;
  }
  
  fwrite(data.c_str(), 1, data.size(), f);
  fclose(f);

  if (rename(download_file.c_str(), output_file.c_str()) != 0) {
    fprintf(stderr, "Failed to replace %s\n", output_file.c_str());
    remove(download_file.c_str());
    return false;
  }

  printf("Downloaded GIF to %s (%zu bytes)\n", output_file.c_str(), data.size());
  return true;
}

// ============= ON-DEMAND GIF HELPERS =============
std::string IndexedFrameName(const char* filename, int index) {
  return std::string(filename) + "[" + std::to_string(index) + "]";
}

bool PingGif(const char* filename, int& out_frame_count, std::vector<int>& out_delays, int& out_width, int& out_height) {
  try {
    out_frame_count = 0;
    out_width = 0;
    out_height = 0;
    out_delays.clear();

    for (int index = 0; index < 10000; ++index) {
      Magick::Image img;
      try {
        img.ping(IndexedFrameName(filename, index));
      } catch (Magick::Error&) {
        if (index == 0) {
          img.ping(filename);
          out_frame_count = 1;
          out_width = static_cast<int>(img.columns());
          out_height = static_cast<int>(img.rows());
          int delay = static_cast<int>(img.animationDelay() * 10);
          out_delays.push_back(delay > 0 ? delay : 100);
          return true;
        }
        break;
      }

      if (index == 0) {
        out_width = static_cast<int>(img.columns());
        out_height = static_cast<int>(img.rows());
      }
      int delay = static_cast<int>(img.animationDelay() * 10);
      out_delays.push_back(delay > 0 ? delay : 100);
      out_frame_count++;
    }

    return out_frame_count > 0;
  } catch (Magick::Error& err) {
    fprintf(stderr, "Ping error: %s\n", err.what());
    return false;
  }
}

bool LoadGifFrameAtIndex(const char* filename, int index, int width, int height, int bar_height, std::vector<Color>& out_pixels, int& out_delay_ms) {
  try {
    (void)bar_height;
    std::string fname = IndexedFrameName(filename, index);
    Magick::Image img;
    try {
      img.read(fname);
    } catch (Magick::Error&) {
      if (index != 0) throw;
      img.read(filename);
    }
    out_delay_ms = static_cast<int>(img.animationDelay() * 10);
    if (out_delay_ms <= 0) out_delay_ms = 100;

    img.scale(Magick::Geometry(width, height));

    out_pixels.assign(width * height, Color(0,0,0));

    int read_w = std::min(static_cast<int>(img.columns()), width);
    int read_h = std::min(static_cast<int>(img.rows()), height);

    for (int y = 0; y < read_h; ++y) {
      for (int x = 0; x < read_w; ++x) {
        Magick::Color c = img.pixelColor(x, y);
        size_t idx = y * width + x;
        out_pixels[idx] = Color(
          ScaleQuantumToChar(c.redQuantum()),
          ScaleQuantumToChar(c.greenQuantum()),
          ScaleQuantumToChar(c.blueQuantum())
        );
      }
    }

    return true;
  } catch (Magick::Error& err) {
    fprintf(stderr, "Load frame %d error: %s\n", index, err.what());
    return false;
  }
}

void CopyPixelsToCanvas(const std::vector<Color>& pixels, FrameCanvas* canvas, int width, int height) {
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const Color& c = pixels[y * width + x];
      canvas->SetPixel(x, y, c.r, c.g, c.b);
    }
  }
}



void DrawTextBar(FrameCanvas* canvas, 
                 int width, 
                 int height, 
                 int bar_height,
                 const Color& bar_color,
                 const Font& font,
                 const char* text,
                 int text_x,
                 const Color& text_color) {
  for (int y = 0; y < bar_height; ++y) {
    for (int x = 0; x < width; ++x) {
      canvas->SetPixel(x, height - bar_height + y, 
                       bar_color.r, bar_color.g, bar_color.b);
    }
  }
  
  int text_y = height - 2;
  DrawText(canvas, font, text_x, text_y, text_color, text);
  
  int approx_text_width = 0;
  for (const char* p = text; *p; ++p) {
    approx_text_width += font.CharacterWidth(*p);
  }
  
  if (text_x + approx_text_width < width) {
    DrawText(canvas, font, text_x + approx_text_width + 20, text_y, text_color, text);
  }
}

// ============= ARGUMENT PARSING =============
void PrintUsage(const char* prog) {
  fprintf(stderr, 
    "Usage: %s [options]\n"
    "  --gif-url <url>              HTTP URL to fetch GIF from\n"
    "  --json-url <url>             HTTP URL to fetch JSON from\n"
    "  --keys <key1,key2,...>       JSON keys to display (comma-separated)\n"
    "  --gif-speed <int>            Accepted for compatibility; not used\n"
    "  --text-speed <int>           Text scroll speed in pixels/frame (default: 2)\n"
    "  --gif-multiplier <float>     GIF animation speed multiplier (default: 1.0)\n"
    "  --api-interval <ms>          API poll interval in milliseconds (default: 5000)\n",
    prog);
  fprintf(stderr,
    "  --font-path <path>           Font path to use for text bar\n"
    "  --bar-height <px>            Height of the text bar in pixels (default: 16)\n"
    "  --idle-path <path>           Optional local image/GIF to show when JSON is null\n");
}

bool ParseArguments(int argc, char* argv[]) {
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--gif-url") == 0 && i + 1 < argc) {
      g_config.api_gif_url = argv[++i];
    } else if (strcmp(argv[i], "--json-url") == 0 && i + 1 < argc) {
      g_config.api_json_url = argv[++i];
    } else if (strcmp(argv[i], "--keys") == 0 && i + 1 < argc) {
      std::string keys_str = argv[++i];
      std::stringstream ss(keys_str);
      std::string key;
      while (std::getline(ss, key, ',')) {
        key.erase(0, key.find_first_not_of(" \t"));
        size_t end = key.find_last_not_of(" \t");
        if (end != std::string::npos) {
          key.erase(end + 1);
          g_config.json_keys.push_back(key);
        }
      }
    } else if (strcmp(argv[i], "--gif-speed") == 0 && i + 1 < argc) {
      g_config.gif_scroll_speed = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--text-speed") == 0 && i + 1 < argc) {
      g_config.text_scroll_speed = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--gif-multiplier") == 0 && i + 1 < argc) {
      g_config.gif_speed_multiplier = atof(argv[++i]);
    } else if (strcmp(argv[i], "--api-interval") == 0 && i + 1 < argc) {
      g_config.api_poll_interval_ms = atoi(argv[++i]);
      } else if (strcmp(argv[i], "--font-path") == 0 && i + 1 < argc) {
        g_config.font_path = argv[++i];
      } else if (strcmp(argv[i], "--bar-height") == 0 && i + 1 < argc) {
        g_config.text_bar_height = atoi(argv[++i]);
      } else if (strcmp(argv[i], "--idle-path") == 0 && i + 1 < argc) {
        g_config.idle_path = argv[++i];
    } else {
      PrintUsage(argv[0]);
      return false;
    }
  }
  
  if (g_config.api_gif_url.empty() || g_config.api_json_url.empty() || g_config.json_keys.empty()) {
    fprintf(stderr, "\nError: --gif-url, --json-url, and --keys are required\n\n");
    PrintUsage(argv[0]);
    return false;
  }
  
  printf("Config loaded:\n");
  printf("  gif_url=%s\n", g_config.api_gif_url.c_str());
  printf("  json_url=%s\n", g_config.api_json_url.c_str());
  printf("  keys=");
  for (size_t i = 0; i < g_config.json_keys.size(); ++i) {
    printf("%s%s", i > 0 ? "," : "", g_config.json_keys[i].c_str());
  }
  printf("\n  gif_speed=%d, text_speed=%d\n", g_config.gif_scroll_speed, g_config.text_scroll_speed);
    printf("  font_path=%s, bar_height=%d\n", g_config.font_path.c_str(), g_config.text_bar_height);
  if (!g_config.idle_path.empty()) printf("  idle_path=%s\n", g_config.idle_path.c_str());
  printf("  api_interval=%dms\n", g_config.api_poll_interval_ms);
  
  return true;
}

// ============= MAIN =============
int main(int argc, char* argv[]) {
  Magick::InitializeMagick(*argv);
  
  g_config.gif_scroll_speed = 2;
  g_config.text_scroll_speed = 2;
  g_config.gif_speed_multiplier = 1.0f;
  g_config.api_poll_interval_ms = 5000;
  g_config.font_path = FONT_PATH;
  g_config.text_bar_height = TEXT_BAR_HEIGHT;
  g_api_data.status = STATUS_HTTP_FAILED;
  
  if (!ParseArguments(argc, argv)) {
    return 1;
  }
  
  printf("Setup Matrix...\n");
  
  RGBMatrix::Options matrix_options;
  matrix_options.hardware_mapping = HARDWARE_MAPPING;
  matrix_options.rows = MATRIX_ROWS;
  matrix_options.cols = MATRIX_COLS;
  matrix_options.chain_length = CHAIN_LENGTH;
  matrix_options.parallel = 1;
  matrix_options.show_refresh_rate = false;

  rgb_matrix::RuntimeOptions runtime_opt;
  runtime_opt.gpio_slowdown = 2;
  
  RGBMatrix* matrix = RGBMatrix::CreateFromOptions(matrix_options, runtime_opt);
  if (matrix == NULL) {
    fprintf(stderr, "Error: Matrix could not be created\n");
    return 1;
  }
  
  signal(SIGTERM, InterruptHandler);
  signal(SIGINT, InterruptHandler);
  
  const char* temp_gif = "/tmp/dynamic.gif";
  
  printf("Starting API poller thread...\n");
  std::thread api_thread(ApiPollerThread);
  api_thread.detach();
  
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));

  int frame_count = 0;
  std::vector<int> frame_delays;
  int gif_w = 0, gif_h = 0;
  std::string media_path;
  
  printf("Loading font: %s...\n", g_config.font_path.c_str());
  Font font;
  if (!font.LoadFont(g_config.font_path.c_str())) {
    fprintf(stderr, "Error: Font could not be loaded\n");
    delete matrix;
    return 1;
  }
  
  FrameCanvas* offscreen = matrix->CreateFrameCanvas();
  
  printf("Starting animation...\n");
  
  int text_x = MATRIX_COLS;
  int frame_index = 0;
  struct timespec last_frame_time;
  clock_gettime(CLOCK_MONOTONIC, &last_frame_time);
  std::string current_json_key;
  ApiStatus last_status = STATUS_HTTP_FAILED;

  // on-demand frame buffer
  std::vector<Color> current_pixels;
  int current_frame_delay = 100;
  
  while (!interrupt_received) {
    ApiStatus api_status;
    std::string api_json_key;
    {
      std::lock_guard<std::mutex> lock(g_api_mutex);
      api_status = g_api_data.status;
      api_json_key = g_api_data.current_json_key;
    }

    bool status_changed = (api_status != last_status);
    bool json_changed = (api_status == STATUS_VALID && api_json_key != current_json_key);

    if (status_changed || json_changed) {
      last_status = api_status;
      current_json_key = api_json_key;
      current_pixels.clear();
      frame_index = 0;
      text_x = MATRIX_COLS;
      frame_count = 0;
      frame_delays.clear();
      media_path.clear();

      if (api_status == STATUS_VALID) {
        printf("Valid data, downloading GIF...\n");
        if (DownloadAndSaveGif(g_config.api_gif_url, temp_gif) &&
            PingGif(temp_gif, frame_count, frame_delays, gif_w, gif_h)) {
          media_path = temp_gif;
        } else {
          std::lock_guard<std::mutex> lock(g_api_mutex);
          g_api_data.status = STATUS_HTTP_FAILED;
          last_status = STATUS_HTTP_FAILED;
          fprintf(stderr, "GIF download or ping failed\n");
        }
      } else if (api_status == STATUS_JSON_NULL && !g_config.idle_path.empty()) {
        printf("JSON returned null, showing idle media: %s\n", g_config.idle_path.c_str());
        if (PingGif(g_config.idle_path.c_str(), frame_count, frame_delays, gif_w, gif_h)) {
          media_path = g_config.idle_path;
        } else {
          fprintf(stderr, "Idle media could not be loaded: %s\n", g_config.idle_path.c_str());
        }
      } else if (api_status == STATUS_JSON_NULL) {
        printf("JSON returned null, clearing matrix\n");
      } else {
        printf("API failed, showing error text\n");
      }

      current_frame_delay = (frame_delays.empty() ? 100 : frame_delays[0]);
      clock_gettime(CLOCK_MONOTONIC, &last_frame_time);
    }

    offscreen->Clear();

    if (last_status == STATUS_JSON_NULL && g_config.idle_path.empty()) {
      offscreen = matrix->SwapOnVSync(offscreen);
      usleep(16000);
      continue;
    }

    {
      if (frame_count > 0 && current_pixels.empty()) {
        if (!LoadGifFrameAtIndex(media_path.c_str(), frame_index, MATRIX_COLS, MATRIX_ROWS, g_config.text_bar_height, current_pixels, current_frame_delay)) {
          std::lock_guard<std::mutex> lock(g_api_mutex);
          g_api_data.status = STATUS_HTTP_FAILED;
          last_status = STATUS_HTTP_FAILED;
          current_pixels.clear();
          frame_count = 0;
          fprintf(stderr, "Frame load failed\n");
        }
      }
    }

    if (!current_pixels.empty()) CopyPixelsToCanvas(current_pixels, offscreen, MATRIX_COLS, MATRIX_ROWS);

    std::string display_text;
    bool draw_text_bar = false;
    {
      std::lock_guard<std::mutex> lock(g_api_mutex);
      if (g_api_data.status == STATUS_VALID) {
        display_text = g_api_data.current_text;
        draw_text_bar = !display_text.empty();
      } else if (g_api_data.status == STATUS_HTTP_FAILED) {
        display_text = "error";
        draw_text_bar = true;
      }
    }

    if (draw_text_bar) {
      DrawTextBar(offscreen,
            MATRIX_COLS,
            MATRIX_ROWS,
            g_config.text_bar_height,
            TEXT_BAR_COLOR,
            font,
            display_text.c_str(),
            text_x,
            TEXT_COLOR);
    }
    
    offscreen = matrix->SwapOnVSync(offscreen);
    
    if (draw_text_bar) {
      text_x -= g_config.text_scroll_speed;

      int approx_text_width = 0;
      for (const char* p = display_text.c_str(); *p; ++p) {
        approx_text_width += font.CharacterWidth(*p);
      }

      if (text_x < -approx_text_width - 20) {
        text_x = MATRIX_COLS;
      }
    }
    
    struct timespec current_time;
    clock_gettime(CLOCK_MONOTONIC, &current_time);
    
    float elapsed_ms = (current_time.tv_sec - last_frame_time.tv_sec) * 1000.0f +
                       (current_time.tv_nsec - last_frame_time.tv_nsec) / 1000000.0f;
    
    float speed_multiplier = std::max(0.1f, g_config.gif_speed_multiplier);
    float frame_delay = current_frame_delay / speed_multiplier;

    if (frame_count > 0 && elapsed_ms >= frame_delay) {
      frame_index = (frame_index + 1) % frame_count;
      current_pixels.clear();
      if (!frame_delays.empty()) current_frame_delay = frame_delays[frame_index];
      clock_gettime(CLOCK_MONOTONIC, &last_frame_time);
    }
    
    usleep(16000);
  }
  
  printf("\nAnimation ended.\n");
  
  offscreen->Clear();
  delete matrix;
  
  return 0;
}
