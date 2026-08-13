// -*- mode: c++; c-basic-offset: 2; indent-tabs-mode: nil; -*-
// GIF-Viewer with HTTP API for dynamic content
// Features:
// - Fetches GIF from HTTP API
// - Fetches JSON with text and image metadata
// - Configurable JSON keys to display
// - Dynamic content refresh when "img" key changes
// - Shows "no-signal.gif" on HTTP failure
// - Shows "idle.gif" when JSON returns null for "img"

#include "led-matrix.h"
#include "graphics.h"

#include <Magick++.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <vector>
#include <string>
#include <map>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <cstring>
#include <sstream>
#include <thread>
#include <mutex>
#include <chrono>

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
} g_config;

// ============= GLOBAL VARIABLES =============
volatile bool interrupt_received = false;
std::mutex g_api_mutex;
enum ApiStatus {
  STATUS_VALID,        // Valid data from API
  STATUS_HTTP_FAILED,  // HTTP request failed -> show no-signal
  STATUS_JSON_NULL     // JSON returned null for img -> show idle
};
struct {
  std::string current_img_key;
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
      fprintf(stderr, "API JSON fetch failed, showing no-signal\n");
    } else {
      try {
        json j = json::parse(json_str);
        
        std::lock_guard<std::mutex> lock(g_api_mutex);
        
        std::string img_key = "unknown";
        if (j.contains("img")) {
          auto img_val = j["img"];
          if (!img_val.is_null()) {
            img_key = img_val.dump();
          } else {
            g_api_data.status = STATUS_JSON_NULL;
            fprintf(stderr, "API returned 'img': null, showing idle\n");
            std::this_thread::sleep_for(std::chrono::milliseconds(g_config.api_poll_interval_ms));
            continue;
          }
        }
        
        std::string formatted_text;
        for (size_t i = 0; i < g_config.json_keys.size(); ++i) {
          const auto& key = g_config.json_keys[i];
          if (j.contains(key)) {
            auto val = j[key];
            formatted_text += key + ": " + val.dump();
          }
          if (i < g_config.json_keys.size() - 1) {
            formatted_text += "; ";
          }
        }
        
        if (img_key != g_api_data.current_img_key) {
          g_api_data.current_img_key = img_key;
          g_api_data.current_text = formatted_text;
          g_api_data.status = STATUS_VALID;
          printf("API update: img=%s, text=%s\n", img_key.c_str(), formatted_text.c_str());
        } else {
          g_api_data.current_text = formatted_text;
          g_api_data.status = STATUS_VALID;
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
  
  FILE* f = fopen(output_file.c_str(), "wb");
  if (!f) {
    fprintf(stderr, "Failed to open file %s for writing\n", output_file.c_str());
    return false;
  }
  
  fwrite(data.c_str(), 1, data.size(), f);
  fclose(f);
  printf("Downloaded GIF to %s (%zu bytes)\n", output_file.c_str(), data.size());
  return true;
}

// ============= NO-SIGNAL IMAGE =============
bool CreateNoSignalImage(const std::string& filename, int width, int height) {
  try {
    Magick::Image img(Magick::Geometry(width, height), Magick::Color("black"));
    Magick::Quantum half_width = width / 2;
    Magick::Quantum half_height = height / 2;
    img.strokeColor("red");
    img.fillColor(Magick::Color("transparent"));
    img.draw(Magick::DrawableCircle(half_width, half_height, half_width - 10, half_height));
    img.draw(Magick::DrawableLine(half_width - 15, half_height - 15, half_width + 15, half_height + 15));
    img.draw(Magick::DrawableLine(half_width - 15, half_height + 15, half_width + 15, half_height - 15));
    img.write(filename);
    return true;
  } catch (Magick::Error& e) {
    fprintf(stderr, "Error creating no-signal image: %s\n", e.what());
    return false;
  }
}

// ============= GIF FRAMES =============
struct GifFrame {
  std::vector<Color> pixels;
  int delay_ms;
};

bool LoadGifFrames(const char* filename, 
                   int width, 
                   int height, 
                   int bar_height,
                   std::vector<GifFrame>& frames) {
  try {
    std::vector<Magick::Image> images;
    Magick::readImages(&images, filename);
    
    if (images.empty()) {
      fprintf(stderr, "No frames in GIF found\n");
      return false;
    }
    
    std::vector<Magick::Image> coalesced;
    Magick::coalesceImages(&coalesced, images.begin(), images.end());
    
    int gif_height = height - bar_height;
    
    for (size_t i = 0; i < coalesced.size(); ++i) {
      Magick::Image& img = coalesced[i];
      
      img.scale(Magick::Geometry(width, gif_height));
      
      GifFrame frame;
      frame.pixels.resize(width * height);
      frame.delay_ms = static_cast<int>(img.animationDelay() * 10);
      
      for (int y = 0; y < gif_height; ++y) {
        for (int x = 0; x < width; ++x) {
          Magick::Color c = img.pixelColor(x, y);
          size_t idx = y * width + x;
          frame.pixels[idx] = Color(
            ScaleQuantumToChar(c.redQuantum()),
            ScaleQuantumToChar(c.greenQuantum()),
            ScaleQuantumToChar(c.blueQuantum())
          );
        }
      }
      
      for (int y = gif_height; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
          size_t idx = y * width + x;
          frame.pixels[idx] = Color(0, 0, 0);
        }
      }
      
      frames.push_back(frame);
      
      if (i % 10 == 0) {
        printf("  Frame %zu/%zu loaded\n", i, coalesced.size());
      }
    }
    
    printf("Total %zu frames loaded\n", frames.size());
    return true;
    
  } catch (Magick::Error& error) {
    fprintf(stderr, "Error loading GIF: %s\n", error.what());
    return false;
  }
}

void CopyFrameToCanvas(const GifFrame& frame, 
                       FrameCanvas* canvas, 
                       int width, 
                       int height) {
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const Color& c = frame.pixels[y * width + x];
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
    "  --gif-speed <int>            GIF scroll speed in pixels/frame (default: 2)\n"
    "  --text-speed <int>           Text scroll speed in pixels/frame (default: 2)\n"
    "  --gif-multiplier <float>     GIF animation speed multiplier (default: 1.0)\n"
    "  --api-interval <ms>          API poll interval in milliseconds (default: 5000)\n",
    prog);
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
        key.erase(key.find_last_not_of(" \t") + 1);
        g_config.json_keys.push_back(key);
      }
    } else if (strcmp(argv[i], "--gif-speed") == 0 && i + 1 < argc) {
      g_config.gif_scroll_speed = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--text-speed") == 0 && i + 1 < argc) {
      g_config.text_scroll_speed = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--gif-multiplier") == 0 && i + 1 < argc) {
      g_config.gif_speed_multiplier = atof(argv[++i]);
    } else if (strcmp(argv[i], "--api-interval") == 0 && i + 1 < argc) {
      g_config.api_poll_interval_ms = atoi(argv[++i]);
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
  printf("  api_interval=%dms\n", g_config.api_poll_interval_ms);
  
  return true;
}

// ============= MAIN =============
int main(int argc, char* argv[]) {
  g_config.gif_scroll_speed = 2;
  g_config.text_scroll_speed = 2;
  g_config.gif_speed_multiplier = 1.0f;
  g_config.api_poll_interval_ms = 5000;
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
  matrix_options.drop_privileges = false;
  
  rgb_matrix::RuntimeOptions runtime_opt;
  runtime_opt.gpio_slowdown = 2;
  
  Canvas* canvas = RGBMatrix::CreateFromOptions(matrix_options, runtime_opt);
  if (canvas == NULL) {
    fprintf(stderr, "Error: Matrix could not be created\n");
    return 1;
  }
  
  signal(SIGTERM, InterruptHandler);
  signal(SIGINT, InterruptHandler);
  
  const char* temp_gif = "/tmp/dynamic.gif";
  const char* no_signal_gif = "/tmp/no_signal.gif";
  const char* idle_gif = "/tmp/idle.gif";
  
  printf("Creating no-signal and idle images...\n");
  CreateNoSignalImage(no_signal_gif, MATRIX_COLS, MATRIX_ROWS - TEXT_BAR_HEIGHT);
  CreateNoSignalImage(idle_gif, MATRIX_COLS, MATRIX_ROWS - TEXT_BAR_HEIGHT);  // User can replace with idle.* file
  
  printf("Starting API poller thread...\n");
  std::thread api_thread(ApiPollerThread);
  api_thread.detach();
  
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  
  if (!DownloadAndSaveGif(g_config.api_gif_url, temp_gif)) {
    printf("Failed to download initial GIF, using no-signal\n");
    system(("cp " + std::string(no_signal_gif) + " " + std::string(temp_gif)).c_str());
  }
  
  std::vector<GifFrame> frames;
  if (!LoadGifFrames(temp_gif, MATRIX_COLS, MATRIX_ROWS, TEXT_BAR_HEIGHT, frames)) {
    delete canvas;
    return 1;
  }
  
  printf("Loading font: %s...\n", FONT_PATH);
  Font font;
  if (!font.LoadFont(FONT_PATH)) {
    fprintf(stderr, "Error: Font could not be loaded\n");
    delete canvas;
    return 1;
  }
  
  FrameCanvas* offscreen = canvas->CreateFrameCanvas();
  
  printf("Starting animation...\n");
  
  int text_x = MATRIX_COLS;
  size_t frame_index = 0;
  struct timespec last_frame_time;
  clock_gettime(CLOCK_MONOTONIC, &last_frame_time);
  std::string current_img_key;
  ApiStatus last_status = STATUS_HTTP_FAILED;
  
  while (!interrupt_received) {
    {
      std::lock_guard<std::mutex> lock(g_api_mutex);
      
      // Check if status changed or img_key changed
      bool status_changed = (g_api_data.status != last_status);
      bool img_changed = (g_api_data.status == STATUS_VALID && g_api_data.current_img_key != current_img_key);
      
      if (status_changed || img_changed) {
        last_status = g_api_data.status;
        current_img_key = g_api_data.current_img_key;
        frames.clear();
        
        if (g_api_data.status == STATUS_VALID) {
          printf("Valid data, downloading GIF...\n");
          if (DownloadAndSaveGif(g_config.api_gif_url, temp_gif) &&
              LoadGifFrames(temp_gif, MATRIX_COLS, MATRIX_ROWS, TEXT_BAR_HEIGHT, frames)) {
            frame_index = 0;
            text_x = MATRIX_COLS;
            clock_gettime(CLOCK_MONOTONIC, &last_frame_time);
          } else {
            printf("Failed to load GIF, showing no-signal\n");
            LoadGifFrames(no_signal_gif, MATRIX_COLS, MATRIX_ROWS, TEXT_BAR_HEIGHT, frames);
          }
        } else if (g_api_data.status == STATUS_JSON_NULL) {
          printf("JSON returned null, showing idle\n");
          LoadGifFrames(idle_gif, MATRIX_COLS, MATRIX_ROWS, TEXT_BAR_HEIGHT, frames);
        } else {
          printf("HTTP failed, showing no-signal\n");
          LoadGifFrames(no_signal_gif, MATRIX_COLS, MATRIX_ROWS, TEXT_BAR_HEIGHT, frames);
        }
      }
    }
    
    if (frames.empty()) {
      usleep(100000);
      continue;
    }
    
    const GifFrame& current_frame = frames[frame_index];
    CopyFrameToCanvas(current_frame, offscreen, MATRIX_COLS, MATRIX_ROWS);
    
    std::string display_text = "Loading...";
    {
      std::lock_guard<std::mutex> lock(g_api_mutex);
      if (g_api_data.status == STATUS_VALID) {
        display_text = g_api_data.current_text;
      }
    }
    
    DrawTextBar(offscreen, 
                MATRIX_COLS, 
                MATRIX_ROWS, 
                TEXT_BAR_HEIGHT,
                TEXT_BAR_COLOR,
                font,
                display_text.c_str(),
                text_x,
                TEXT_COLOR);
    
    offscreen = canvas->SwapOnVSync(offscreen);
    
    text_x -= g_config.text_scroll_speed;
    
    int approx_text_width = 0;
    for (const char* p = display_text.c_str(); *p; ++p) {
      approx_text_width += font.CharacterWidth(*p);
    }
    
    if (text_x < -approx_text_width - 20) {
      text_x = MATRIX_COLS;
    }
    
    struct timespec current_time;
    clock_gettime(CLOCK_MONOTONIC, &current_time);
    
    float elapsed_ms = (current_time.tv_sec - last_frame_time.tv_sec) * 1000.0f +
                       (current_time.tv_nsec - last_frame_time.tv_nsec) / 1000000.0f;
    
    float frame_delay = frames[frame_index].delay_ms / g_config.gif_speed_multiplier;
    
    if (elapsed_ms >= frame_delay) {
      frame_index = (frame_index + 1) % frames.size();
      clock_gettime(CLOCK_MONOTONIC, &last_frame_time);
    }
    
    usleep(16000);
  }
  
  printf("\nAnimation ended.\n");
  
  offscreen->Clear();
  delete canvas;
  
  return 0;
}
