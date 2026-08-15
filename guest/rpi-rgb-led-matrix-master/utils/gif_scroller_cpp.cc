// -*- mode: c++; c-basic-offset: 2; indent-tabs-mode: nil; -*-
// Hochoptimiertes GIF-Viewer-Skript mit scrollendem Text-Balken.
// Performance-Optimierungen:
// - Preprocessing aller GIF-Frames in FrameCanvas-Objekte
// - Double-Buffering mit SwapOnVSync()
// - GraphicsMagick für schnelles GIF-Laden
// - Text nur im unteren Balken (weniger Pixel-Updates)

#include "led-matrix.h"
#include "graphics.h"

#include <Magick++.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <vector>
#include <string>
#include <algorithm>

using rgb_matrix::RGBMatrix;
using rgb_matrix::FrameCanvas;
using rgb_matrix::Canvas;
using rgb_matrix::Color;
using rgb_matrix::Font;
using rgb_matrix::DrawText;

// ============= KONFIGURATION =============
const char* DEFAULT_GIF_FILE = "animation.gif";
const char* DEFAULT_TEXT = "Dies ist ein langer scrollender Text für die LED-Matrix";
const char* DEFAULT_FONT_PATH = "fonts/font.bdf";
// Matrix-Konfiguration
const int MATRIX_ROWS = 64;
const int MATRIX_COLS = 64;
const int CHAIN_LENGTH = 1;
const char* HARDWARE_MAPPING = "regular";  // oder "adafruit-hat"

// Text-Balken-/Animation-Konfiguration (can be overridden via CLI)
const Color TEXT_COLOR(255, 255, 0);  // Gelb
const Color DEFAULT_TEXT_BAR_COLOR(0, 0, 50);  // Dunkelblau für Hintergrund
const int SCROLL_SPEED = 2;  // Pixel pro Frame
const float GIF_SPEED = 1.0f;  // 1.0 = Original, 2.0 = 2× schneller

// ============= GLOBALE VARIABLEN =============
volatile bool interrupt_received = false;

static void InterruptHandler(int signo) {
  interrupt_received = true;
}

// ============= GIF FRAMES LADEN =============
// We'll only read GIF metadata (ping) up front, and load individual frames
// on-demand using the ImageMagick frame index syntax ("file.gif[3]").
bool PingGif(const char* filename, int& out_frame_count, std::vector<int>& out_delays, int& out_width, int& out_height) {
  try {
    std::vector<Magick::Image> images;
    Magick::pingImages(&images, filename);
    if (images.empty()) {
      fprintf(stderr, "Keine Frames in GIF gefunden\n");
      return false;
    }

    out_frame_count = static_cast<int>(images.size());
    out_width = images[0].columns();
    out_height = images[0].rows();
    out_delays.clear();
    for (size_t i = 0; i < images.size(); ++i) {
      out_delays.push_back(static_cast<int>(images[i].animationDelay() * 10));
    }
    printf("Ping: %d frames, size=%dx%d\n", out_frame_count, out_width, out_height);
    return true;
  } catch (Magick::Error& err) {
    fprintf(stderr, "Ping-Fehler: %s\n", err.what());
    return false;
  }
}

// Load a single frame (by index) into pixel buffer. Uses filename[index] to avoid loading all frames.
bool LoadGifFrameAtIndex(const char* filename, int index, int width, int height, int bar_height, std::vector<Color>& out_pixels, int& out_delay_ms) {
  try {
    std::string fname = std::string(filename) + "[" + std::to_string(index) + "]";
    Magick::Image img;
    Magick::readImage(&img, fname);
    out_delay_ms = static_cast<int>(img.animationDelay() * 10);

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

    // Ensure text-bar area is cleared (will be overwritten by DrawTextBar)
    for (int y = height - bar_height; y < height; ++y) {
      if (y < 0) continue;
      for (int x = 0; x < width; ++x) {
        size_t idx = y * width + x;
        out_pixels[idx] = Color(0,0,0);
      }
    }

    return true;
  } catch (Magick::Error& err) {
    fprintf(stderr, "Fehler beim Laden Frame %d: %s\n", index, err.what());
    return false;
  }
}

// ============= FRAME IN CANVAS KOPIEREN =============
void CopyPixelsToCanvas(const std::vector<Color>& pixels, FrameCanvas* canvas, int width, int height) {
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const Color& c = pixels[y * width + x];
      canvas->SetPixel(x, y, c.r, c.g, c.b);
    }
  }
}

// ============= TEXT BALKEN ZEICHNEN =============
void DrawTextBar(FrameCanvas* canvas, 
                 int width, 
                 int height, 
                 int bar_height,
                 const Color& bar_color,
                 const Font& font,
                 const char* text,
                 int text_x,
                 const Color& text_color) {
  // Hintergrund-Balken zeichnen
  for (int y = 0; y < bar_height; ++y) {
    for (int x = 0; x < width; ++x) {
      canvas->SetPixel(x, height - bar_height + y, 
                       bar_color.r, bar_color.g, bar_color.b);
    }
  }
  
  // Text zeichnen
  int text_y = height - (bar_height / 2);
  DrawText(canvas, font, text_x, text_y, text_color, text);
  
  // Zweiter Text für kontinuierliches Scrollen (wenn erster rauslä±±uft)
  int text_width = font.CharacterWidth(' ');  // Approximation
  // Besser: TextWidth berechnen (manuell)
  int approx_text_width = 0;
  for (const char* p = text; *p; ++p) {
    approx_text_width += font.CharacterWidth(*p);
  }
  
  if (text_x + approx_text_width < width) {
    DrawText(canvas, font, text_x + approx_text_width + 20, text_y, text_color, text);
  }
}

// ============= MAIN =============
int main(int argc, char* argv[]) {
  printf("Setup Matrix...\n");
  
  // Matrix konfigurieren
  RGBMatrix::Options matrix_options;
  matrix_options.hardware_mapping = HARDWARE_MAPPING;
  matrix_options.rows = MATRIX_ROWS;
  matrix_options.cols = MATRIX_COLS;
  matrix_options.chain_length = CHAIN_LENGTH;
  matrix_options.parallel = 1;
  matrix_options.show_refresh_rate = false;
  matrix_options.drop_privileges = false;  // Bessere Performance
  
  rgb_matrix::RuntimeOptions runtime_opt;
  runtime_opt.gpio_slowdown = 2;  // Für Pi 3/4 anpassen (1-4)
  
  Canvas* canvas = RGBMatrix::CreateFromOptions(matrix_options, runtime_opt);
  if (canvas == NULL) {
    fprintf(stderr, "Fehler: Matrix konnte nicht erstellt werden\n");
    return 1;
  }
  
  // Signal-Handler für sauberes Beenden
  signal(SIGTERM, InterruptHandler);
  signal(SIGINT, InterruptHandler);
  
  // --- CLI args: [gif_path] [bar_height] [font_path] [text]
  const char* gif_path = DEFAULT_GIF_FILE;
  int text_bar_height = 10;
  const char* font_path = DEFAULT_FONT_PATH;
  const char* text_ptr = DEFAULT_TEXT;
  if (argc > 1) gif_path = argv[1];
  if (argc > 2) text_bar_height = atoi(argv[2]);
  if (argc > 3) font_path = argv[3];
  if (argc > 4) text_ptr = argv[4];

  printf("Pinge GIF: %s...\n", gif_path);
  int frame_count = 0;
  std::vector<int> frame_delays;
  int gif_w = 0, gif_h = 0;
  if (!PingGif(gif_path, frame_count, frame_delays, gif_w, gif_h)) {
    delete canvas;
    return 1;
  }

  printf("Lade Font: %s...\n", font_path);
  Font font;
  if (!font.LoadFont(font_path)) {
    fprintf(stderr, "Fehler: Font konnte nicht geladen werden\n");
    delete canvas;
    return 1;
  }
  
  // Double-Buffering Canvas
  FrameCanvas* offscreen = canvas->CreateFrameCanvas();
  
  printf("Starte Animation...\n");

  // Animations-Variablen
  int text_x = MATRIX_COLS;  // Startposition Text (rechts außerhalb)
  int frame_index = 0;
  std::vector<Color> current_pixels;
  int current_frame_delay = (frame_delays.empty() ? 100 : frame_delays[0]);
  struct timespec last_frame_time;
  clock_gettime(CLOCK_MONOTONIC, &last_frame_time);

  // Track GIF file modification to allow reloading
  struct stat gif_stat;
  time_t last_mtime = 0;
  if (stat(gif_path, &gif_stat) == 0) last_mtime = gif_stat.st_mtime;

  // Pre-load first frame
  if (frame_count > 0) {
    if (!LoadGifFrameAtIndex(gif_path, frame_index, MATRIX_COLS, MATRIX_ROWS, text_bar_height, current_pixels, current_frame_delay)) {
      fprintf(stderr, "Warnung: Erstes Frame konnte nicht geladen werden\n");
    }
  }
  
  while (!interrupt_received) {
    // ============= GIF FRAME =============
    // Only load a new frame when the frame index changed (on-demand)
    if (current_pixels.empty()) {
      if (!LoadGifFrameAtIndex(gif_path, frame_index, MATRIX_COLS, MATRIX_ROWS, text_bar_height, current_pixels, current_frame_delay)) {
        // leave canvas as-is if load fails
      }
    }

    // Copy current pixels into canvas
    if (!current_pixels.empty()) {
      CopyPixelsToCanvas(current_pixels, offscreen, MATRIX_COLS, MATRIX_ROWS);
    }
    
    // ============= TEXT SCROLLING =============
    DrawTextBar(offscreen, 
          MATRIX_COLS, 
          MATRIX_ROWS, 
          text_bar_height,
          DEFAULT_TEXT_BAR_COLOR,
          font,
          text_ptr,
          text_x,
          TEXT_COLOR);
    
    // ============= SWAP =============
    offscreen = canvas->SwapOnVSync(offscreen);
    
    // ============= UPDATE =============
    // Text-Position aktualisieren
    text_x -= SCROLL_SPEED;
    
    // Text-Breite approximieren für Wrap
    int approx_text_width = 0;
    for (const char* p = text_ptr; *p; ++p) {
      approx_text_width += font.CharacterWidth(*p);
    }
    
    if (text_x < -approx_text_width - 20) {
      text_x = MATRIX_COLS;
    }
    
    // GIF-Timing (mit Geschwindigkeitsanpassung)
    struct timespec current_time;
    clock_gettime(CLOCK_MONOTONIC, &current_time);
    
    float elapsed_ms = (current_time.tv_sec - last_frame_time.tv_sec) * 1000.0f +
                       (current_time.tv_nsec - last_frame_time.tv_nsec) / 1000000.0f;
    
    float frame_delay = current_frame_delay / GIF_SPEED;

    if (elapsed_ms >= frame_delay) {
      // advance to next frame
      frame_index = (frame_index + 1) % std::max(1, frame_count);
      // mark pixels empty so next loop will load it
      current_pixels.clear();
      // update expected delay if we have metadata
      if (!frame_delays.empty()) current_frame_delay = frame_delays[frame_index];
      clock_gettime(CLOCK_MONOTONIC, &last_frame_time);
    }

    // Check for GIF file changes; if changed, re-ping and restart
    if (stat(gif_path, &gif_stat) == 0) {
      if (gif_stat.st_mtime != last_mtime) {
        printf("GIF changed on disk; reloading metadata...\n");
        last_mtime = gif_stat.st_mtime;
        if (!PingGif(gif_path, frame_count, frame_delays, gif_w, gif_h)) {
          fprintf(stderr, "Fehler beim Re-Pingen der GIF\n");
        } else {
          frame_index = 0;
          current_pixels.clear();
          if (!frame_delays.empty()) current_frame_delay = frame_delays[0];
        }
      }
    }
    
    // Framerate-Begrenzung (optional, für Konsistenz)
    usleep(16000);  // ~60 FPS Max
  }
  
  printf("\nAnimation beendet.\n");
  
  // Aufrä±±umen
  offscreen->Clear();
  delete canvas;
  
  return 0;
}