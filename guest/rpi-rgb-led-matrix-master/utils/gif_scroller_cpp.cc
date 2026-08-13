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
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <vector>
#include <string>

using rgb_matrix::RGBMatrix;
using rgb_matrix::FrameCanvas;
using rgb_matrix::Canvas;
using rgb_matrix::Color;
using rgb_matrix::Font;
using rgb_matrix::DrawText;

// ============= KONFIGURATION =============
const char* GIF_FILE = "animation.gif";
const char* TEXT = "Dies ist ein langer scrollender Text für die LED-Matrix";
const char* FONT_PATH = "fonts/7x13.bdf";

// Matrix-Konfiguration
const int MATRIX_ROWS = 64;
const int MATRIX_COLS = 64;
const int CHAIN_LENGTH = 1;
const char* HARDWARE_MAPPING = "regular";  // oder "adafruit-hat"

// Text-Balken-Konfiguration
const int TEXT_BAR_HEIGHT = 10;  // Höhe des Text-Balkens in Pixeln
const Color TEXT_COLOR(255, 255, 0);  // Gelb
const Color TEXT_BAR_COLOR(0, 0, 50);  // Dunkelblau für Hintergrund
const int SCROLL_SPEED = 2;  // Pixel pro Frame
const float GIF_SPEED = 1.0f;  // 1.0 = Original, 2.0 = 2×± schneller

// ============= GLOBALE VARIABLEN =============
volatile bool interrupt_received = false;

static void InterruptHandler(int signo) {
  interrupt_received = true;
}

// ============= GIF FRAMES LADEN =============
struct GifFrame {
  std::vector<Color> pixels;  // Pre-allocierte Pixel
  int delay_ms;  // Frame-Delay in ms
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
      fprintf(stderr, "Keine Frames in GIF gefunden\n");
      return false;
    }
    
    // Coalesce für korrekte Animation (alle Frames vollständig)
    std::vector<Magick::Image> coalesced;
    Magick::coalesceImages(&coalesced, images.begin(), images.end());
    
    int gif_height = height;
    
    for (size_t i = 0; i < coalesced.size(); ++i) {
      Magick::Image& img = coalesced[i];
      
      // Frame erstellen
      GifFrame frame;
      frame.pixels.resize(width * height);
      frame.delay_ms = static_cast<int>(img.animationDelay() * 10);  // 1/100s → ms
      
      // Pixel kopieren (nur GIF-Bereich, Rest transparent/schwarz)
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
      
      // Text-Balken-Bereich mit Schwarz füllen (wird später überschrieben)
      for (int y = height-TEXT_BAR_HEIGHT; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
          size_t idx = y * width + x;
          frame.pixels[idx] = Color(0, 0, 0);
        }
      }
      
      frames.push_back(frame);
      
      if (i % 10 == 0) {
        printf("  Frame %zu/%zu geladen\n", i, coalesced.size());
      }
    }
    
    printf("Insgesamt %zu Frames geladen\n", frames.size());
    return true;
    
  } catch (Magick::Error& error) {
    fprintf(stderr, "Fehler beim Laden der GIF: %s\n", error.what());
    return false;
  }
}

// ============= FRAME IN CANVAS KOPIEREN =============
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
  int text_y = height;
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
  
  printf("Lade GIF: %s...\n", GIF_FILE);
  std::vector<GifFrame> frames;
  if (!LoadGifFrames(GIF_FILE, MATRIX_COLS, MATRIX_ROWS, TEXT_BAR_HEIGHT, frames)) {
    delete canvas;
    return 1;
  }
  
  printf("Lade Font: %s...\n", FONT_PATH);
  Font font;
  if (!font.LoadFont(FONT_PATH)) {
    fprintf(stderr, "Fehler: Font konnte nicht geladen werden\n");
    delete canvas;
    return 1;
  }
  
  // Double-Buffering Canvas
  FrameCanvas* offscreen = canvas->CreateFrameCanvas();
  
  printf("Starte Animation...\n");
  
  // Animations-Variablen
  int text_x = MATRIX_COLS;  // Startposition Text (rechts auÃ±erhalb)
  size_t frame_index = 0;
  struct timespec last_frame_time;
  clock_gettime(CLOCK_MONOTONIC, &last_frame_time);
  
  while (!interrupt_received) {
    // ============= GIF FRAME =============
    const GifFrame& current_frame = frames[frame_index];
    
    // Frame in Canvas kopieren
    CopyFrameToCanvas(current_frame, offscreen, MATRIX_COLS, MATRIX_ROWS);
    
    // ============= TEXT SCROLLING =============
    DrawTextBar(offscreen, 
                MATRIX_COLS, 
                MATRIX_ROWS, 
                TEXT_BAR_HEIGHT,
                TEXT_BAR_COLOR,
                font,
                TEXT,
                text_x,
                TEXT_COLOR);
    
    // ============= SWAP =============
    offscreen = canvas->SwapOnVSync(offscreen);
    
    // ============= UPDATE =============
    // Text-Position aktualisieren
    text_x -= SCROLL_SPEED;
    
    // Text-Breite approximieren für Wrap
    int approx_text_width = 0;
    for (const char* p = TEXT; *p; ++p) {
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
    
    float frame_delay = frames[frame_index].delay_ms / GIF_SPEED;
    
    if (elapsed_ms >= frame_delay) {
      frame_index = (frame_index + 1) % frames.size();
      clock_gettime(CLOCK_MONOTONIC, &last_frame_time);
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