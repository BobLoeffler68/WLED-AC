#include "wled.h"

// for information how FX metadata strings work see https://kno.wled.ge/interfaces/json-api/#effect-metadata

#if !(defined(WLED_DISABLE_PARTICLESYSTEM2D) && defined(WLED_DISABLE_PARTICLESYSTEM1D))
  #include "FXparticleSystem.h" // include particle system code only if at least one system is enabled
  #ifdef WLED_DISABLE_PARTICLESYSTEM2D
    #define WLED_PS_DONT_REPLACE_2D_FX
  #endif
  #ifdef WLED_DISABLE_PARTICLESYSTEM1D
    #define WLED_PS_DONT_REPLACE_1D_FX
  #endif
  #ifdef ESP8266
    #if !defined(WLED_DISABLE_PARTICLESYSTEM2D) && !defined(WLED_DISABLE_PARTICLESYSTEM1D)
      #error ESP8266 does not support 1D and 2D particle systems simultaneously. Please disable one of them.
    #endif
  #endif
#else
  #define WLED_PS_DONT_REPLACE_1D_FX
  #define WLED_PS_DONT_REPLACE_2D_FX
#endif
#ifdef WLED_PS_DONT_REPLACE_FX
  #define WLED_PS_DONT_REPLACE_1D_FX
  #define WLED_PS_DONT_REPLACE_2D_FX
#endif

// paletteBlend: 0 - wrap when moving, 1 - always wrap, 2 - never wrap, 3 - none (undefined)
#define PALETTE_SOLID_WRAP   (strip.paletteBlend == 1 || strip.paletteBlend == 3)
#define PALETTE_MOVING_WRAP !(strip.paletteBlend == 2 || (strip.paletteBlend == 0 && SEGMENT.speed == 0))

#define indexToVStrip(index, stripNr) ((index) | (int((stripNr)+1)<<16))

// static effect, used if an effect fails to initialize
static uint16_t mode_static(void) {
  SEGMENT.fill(SEGCOLOR(0));
  return strip.isOffRefreshRequired() ? FRAMETIME : 350;
}


/////////////////////////
//  User FX functions  //
/////////////////////////

// Diffusion Fire: fire effect intended for 2D setups smaller than 16x16
static uint16_t mode_diffusionfire(void) {
  if (!strip.isMatrix || !SEGMENT.is2D())
    return mode_static();  // not a 2D set-up

  const int cols = SEG_W;
  const int rows = SEG_H;
  const auto XY = [&](int x, int y) { return x + y * cols; };

  const uint8_t refresh_hz = map(SEGMENT.speed, 0, 255, 20, 80);
  const unsigned refresh_ms = 1000 / refresh_hz;
  const int16_t diffusion = map(SEGMENT.custom1, 0, 255, 0, 100);
  const uint8_t spark_rate = SEGMENT.intensity;
  const uint8_t turbulence = SEGMENT.custom2;

unsigned dataSize = cols * rows;  // SEGLEN (virtual length) is equivalent to vWidth()*vHeight() for 2D
  if (!SEGENV.allocateData(dataSize))
    return mode_static();  // allocation failed

  if (SEGENV.call == 0) {
    SEGMENT.fill(BLACK);
    SEGENV.step = 0;
  }

  if ((strip.now - SEGENV.step) >= refresh_ms) {
    // Keep for ≤~1 KiB; otherwise consider heap or reuse SEGENV.data as scratch.
    uint8_t tmp_row[cols];
    SEGENV.step = strip.now;
    // scroll up
    for (unsigned y = 1; y < rows; y++)
      for (unsigned x = 0; x < cols; x++) {
        unsigned src = XY(x, y);
        unsigned dst = XY(x, y - 1);
        SEGENV.data[dst] = SEGENV.data[src];
      }

    if (hw_random8() > turbulence) {
      // create new sparks at bottom row
      for (unsigned x = 0; x < cols; x++) {
        uint8_t p = hw_random8();
        if (p < spark_rate) {
          unsigned dst = XY(x, rows - 1);
          SEGENV.data[dst] = 255;
        }
      }
    }

    // diffuse
    for (unsigned y = 0; y < rows; y++) {
      for (unsigned x = 0; x < cols; x++) {
        unsigned v = SEGENV.data[XY(x, y)];
        if (x > 0) {
          v += SEGENV.data[XY(x - 1, y)];
        }
        if (x < (cols - 1)) {
          v += SEGENV.data[XY(x + 1, y)];
        }
        tmp_row[x] = min(255, (int)(v * 100 / (300 + diffusion)));
      }

      for (unsigned x = 0; x < cols; x++) {
        SEGENV.data[XY(x, y)] = tmp_row[x];
        if (SEGMENT.check1) {
          uint32_t color = SEGMENT.color_from_palette(tmp_row[x], true, false, 0);
          SEGMENT.setPixelColorXY(x, y, color);
        } else {
          uint32_t base = SEGCOLOR(0);
          SEGMENT.setPixelColorXY(x, y, color_fade(base, tmp_row[x]));
        }
      }
    }
  }
  return FRAMETIME;
}
static const char _data_FX_MODE_DIFFUSIONFIRE[] PROGMEM = "Diffusion Fire@!,Spark rate,Diffusion Speed,Turbulence,,Use palette;;Color;;2;pal=35";


/*
/  Ants (created by making modifications to the Rolling Balls code) - Bob Loeffler 2025
*   First slider is for the ants' speed.
*   Second slider is for the # of ants.
*   Third slider is for the Ants' size.
*   Fourth slider (custom2) is for blurring the LEDs in the segment.
*   Checkbox1 is for Gathering food (enabled if you want the ants to gather food, disabled if they are just walking).
*     We will switch directions when they get to the beginning or end of the segment when gathering food.
*     When gathering food, the Pass By option will automatically be enabled so they can drop off their food easier (and look for more food).
*   Checkbox2 is for Overlay mode (enabled is Overlay, disabled is no overlay)
*   Checkbox3 is for whether the ants will bump into each other (disabled) or just pass by each other (enabled)
*/

// Ant structure representing each ant's state
struct Ant {
  unsigned long lastBumpUpdate;  // the last time the ant bumped into another ant
  bool hasFood;
  float velocity;
  float position;  // (0.0 to 1.0 range)
};

constexpr unsigned MAX_ANTS = 32;
constexpr float MIN_COLLISION_TIME_MS = 2.0f;
constexpr float VELOCITY_MIN = 2.0f;
constexpr float VELOCITY_MAX = 10.0f;

// Helper function to get food pixel color based on ant and background colors
static uint32_t getFoodColor(uint32_t antColor, uint32_t backgroundColor) {
  if (antColor == WHITE)
    return (backgroundColor == YELLOW) ? GRAY : YELLOW;
  return (backgroundColor == WHITE) ? YELLOW : WHITE;
}

// Helper function to handle ant boundary wrapping or bouncing
static void handleBoundary(Ant& ant, float& position, bool gatherFood, bool atStart, unsigned long currentTime) {
  if (gatherFood) {
    // Bounce mode: reverse direction and update food status
    position = atStart ? 0.0f : 1.0f;
    ant.velocity = -ant.velocity;
    ant.lastBumpUpdate = currentTime;
    ant.position = position;
    ant.hasFood = atStart;  // Has food when leaving start, drops it at end
  } else {
    // Wrap mode: teleport to opposite end
    position = atStart ? 1.0f : 0.0f;
    ant.lastBumpUpdate = currentTime;
    ant.position = position;
  }
}

// Helper function to calculate ant color
static uint32_t getAntColor(int antIndex, int numAnts, bool usePalette) {
  if (usePalette)
    return SEGMENT.color_from_palette(antIndex * 255 / numAnts, false, (strip.paletteBlend == 1 || strip.paletteBlend == 3), 255);
  // Alternate between two colors for default palette
  return (antIndex % 3 == 1) ? SEGCOLOR(0) : SEGCOLOR(2);
}

// Helper function to render a single ant pixel with food handling
static void renderAntPixel(int pixelIndex, int pixelOffset, int antSize, const Ant& ant, uint32_t antColor, uint32_t backgroundColor, bool gatherFood) {
  bool isMovingBackward = (ant.velocity < 0);
  bool isFoodPixel = gatherFood && ant.hasFood && ((isMovingBackward && pixelOffset == 0) || (!isMovingBackward && pixelOffset == antSize - 1));
  if (isFoodPixel) {
    SEGMENT.setPixelColor(pixelIndex, getFoodColor(antColor, backgroundColor));
  } else {
    SEGMENT.setPixelColor(pixelIndex, antColor);
  }
}

static uint16_t mode_ants(void) {
  if (SEGLEN <= 1) return mode_static();

  // Allocate memory for ant data
  uint32_t backgroundColor = SEGCOLOR(1);
  unsigned dataSize = sizeof(Ant) * MAX_ANTS;
  if (!SEGENV.allocateData(dataSize)) return mode_static();  // Allocation failed

  Ant* ants = reinterpret_cast<Ant*>(SEGENV.data);

  // Extract configuration from segment settings
  unsigned numAnts = min(1 + (SEGLEN * SEGMENT.intensity >> 12), MAX_ANTS);
  bool gatherFood = SEGMENT.check1;
  bool SmearMode = SEGMENT.check2;
  bool passBy = SEGMENT.check3 || gatherFood;  // global no‑collision when gathering food is enabled
  unsigned antSize = map(SEGMENT.custom1, 0, 255, 1, 20) + (gatherFood ? 1 : 0);

  // Initialize ants on first call
  if (SEGENV.call == 0) {
    int confusedAntIndex = hw_random(0, numAnts);   // the first random ant to go backwards

    for (int i = 0; i < MAX_ANTS; i++) {
      ants[i].lastBumpUpdate = strip.now;

      // Random velocity
      float velocity = VELOCITY_MIN + (VELOCITY_MAX - VELOCITY_MIN) * hw_random16(1000, 5000) / 5000.0f;
      // One random ant moves in opposite direction
      ants[i].velocity = (i == confusedAntIndex) ? -velocity : velocity;
      // Random starting position (0.0 to 1.0)
      ants[i].position = hw_random16(0, 10000) / 10000.0f;
      // Ants don't have food yet
      ants[i].hasFood = false;
    }
  }

  // Calculate time conversion factor based on speed slider
  float timeConversionFactor = float(scale8(8, 255 - SEGMENT.speed) + 1) * 20000.0f;

  // Clear background if not in Smear mode
  if (!SmearMode) SEGMENT.fill(backgroundColor);

  // Update and render each ant
  for (int i = 0; i < numAnts; i++) {
    float timeSinceLastUpdate = float(int(strip.now - ants[i].lastBumpUpdate)) / timeConversionFactor;
    float newPosition = ants[i].position + ants[i].velocity * timeSinceLastUpdate;

    // Reset ants that wandered too far off-track (e.g., after intensity change)
    if (newPosition < -0.5f || newPosition > 1.5f) {
      newPosition = ants[i].position = hw_random16(0, 10000) / 10000.0f;
      ants[i].lastBumpUpdate = strip.now;
    }

    // Handle boundary conditions (bounce or wrap)
    if (newPosition <= 0.0f && ants[i].velocity < 0.0f) {
      handleBoundary(ants[i], newPosition, gatherFood, true, strip.now);
    } else if (newPosition >= 1.0f && ants[i].velocity > 0.0f) {
      handleBoundary(ants[i], newPosition, gatherFood, false, strip.now);
    }

    // Handle collisions between ants (if not passing by)
    if (!passBy) {
      for (int j = i + 1; j < numAnts; j++) {
        if (fabsf(ants[j].velocity - ants[i].velocity) < 0.001f) continue;  // Moving in same direction at same speed; avoids tiny denominators

        // Calculate collision time using physics -  collisionTime formula adapted from rolling_balls
        float timeOffset = float(int(ants[j].lastBumpUpdate - ants[i].lastBumpUpdate));
        float collisionTime = (timeConversionFactor * (ants[i].position - ants[j].position) + ants[i].velocity * timeOffset) / (ants[j].velocity - ants[i].velocity);

        // Check if collision occurred in valid time window
        float timeSinceJ = float(int(strip.now - ants[j].lastBumpUpdate));
        if (collisionTime > MIN_COLLISION_TIME_MS && collisionTime < timeSinceJ) {
          // Update positions to collision point
          float adjustedTime = (collisionTime + float(int(ants[j].lastBumpUpdate - ants[i].lastBumpUpdate))) / timeConversionFactor;
          ants[i].position += ants[i].velocity * adjustedTime;
          ants[j].position = ants[i].position;

          // Update collision time
          unsigned long collisionMoment = static_cast<unsigned long>(collisionTime + 0.5f) + ants[j].lastBumpUpdate;
          ants[i].lastBumpUpdate = collisionMoment;
          ants[j].lastBumpUpdate = collisionMoment;

          // Reverse the ant with greater speed magnitude
          if (fabsf(ants[i].velocity) > fabsf(ants[j].velocity)) {
            ants[i].velocity = -ants[i].velocity;
          } else {
            ants[j].velocity = -ants[j].velocity;
          }

          // Recalculate position after collision
          newPosition = ants[i].position + ants[i].velocity * float(int(strip.now - ants[i].lastBumpUpdate)) / timeConversionFactor;
        }
      }
    }

    // Clamp position to valid range
    newPosition = constrain(newPosition, 0.0f, 1.0f);
    unsigned pixelPosition = roundf(newPosition * (SEGLEN - 1));

    // Determine ant color
    uint32_t antColor = getAntColor(i, numAnts, SEGMENT.palette != 0);

    // Render ant pixels
    for (int pixelOffset = 0; pixelOffset < antSize; pixelOffset++) {
      unsigned currentPixel = pixelPosition + pixelOffset;
      if (currentPixel >= SEGLEN) break;
      renderAntPixel(currentPixel, pixelOffset, antSize, ants[i], antColor, backgroundColor, gatherFood);
    }

    // Update ant state
    ants[i].lastBumpUpdate = strip.now;
    ants[i].position = newPosition;
  }

  SEGMENT.blur(SEGMENT.custom2>>1);
  return FRAMETIME;
}
static const char _data_FX_MODE_ANTS[] PROGMEM = "Ants@Ant speed,# of ants,Ant size,Blur,,Gathering food,Smear,Pass by;!,!,!;!;1;sx=192,ix=255,c1=32,c2=0,o1=1,o3=1";


/*
/  Scrolling Morse Code by Bob Loeffler
*   Adapted from code by automaticaddison.com and then optimized by claude.ai
*   aux0 is the pattern offset for scrolling
*   aux1 is the total pattern length
*   The sx slider selects the scrolling speed
*   Checkbox1 selects the color mode
*   Checkbox2 displays punctuation or not
*   Checkbox3 displays the End-of-message code or not
*   We get the text from the SEGMENT.name and convert it to morse code
*   Morse Code rules:
*    - there is one space between each part of a letter or number
*    - there are 3 spaces between each letter or number
*    - there are 7 spaces between each word
*/

// Build morse pattern into a buffer
void build_morsecode_pattern(const char *morse_code, bool *pattern, int &index, int maxSize) {
  const char *c = morse_code;
  
  // Build the dots and dashes into pattern array
  while (*c != '\0') {
    // it's a dot which is 1 pixel
    if (*c == '.') {
      if (index >= maxSize - 1) return; // Reserve space for spacing
      pattern[index++] = true;
    }
    else { // Must be a dash which is 3 pixels
      if (index >= maxSize - 3) return;
      pattern[index++] = true;
      pattern[index++] = true;
      pattern[index++] = true;
    }
    
    // 1 space between parts of a letter/number
    if (index >= maxSize) return;
    pattern[index++] = false;
    c++;
  }
    
  // 3 spaces between two letters
  if (index >= maxSize - 2) return;
  pattern[index++] = false;
  if (index >= maxSize - 1) return;
  pattern[index++] = false;
  if (index >= maxSize) return;
  pattern[index++] = false;
}

static uint16_t mode_morsecode(void) {
  if (SEGLEN < 1) return mode_static();
  
  // A-Z in Morse Code
  static const char * letters[] = {".-", "-...", "-.-.", "-..", ".", "..-.", "--.", "....", "..", ".---", "-.-", ".-..", "--",
                     "-.", "---", ".--.", "--.-", ".-.", "...", "-", "..-", "...-", ".--", "-..-", "-.--", "--.."};
  // 0-9 in Morse Code
  static const char * numbers[] = {"-----", ".----", "..---", "...--", "....-", ".....", "-....", "--...", "---..", "----."};

  // Get the text to display
  char text[WLED_MAX_SEGNAME_LEN+1] = {'\0'};
  size_t len = 0;

  if (SEGMENT.name) len = strlen(SEGMENT.name);
  if (len == 0) { // fallback if empty segment name
    strcpy_P(text, PSTR("I Love WLED!"));
  } else {
    strcpy(text, SEGMENT.name);
  }

  // Convert to uppercase in place
  for (char *p = text; *p; p++) {
    *p = toupper(*p);
  }

  // Allocate per-segment storage for pattern
  constexpr size_t MORSECODE_MAX_PATTERN_SIZE = 1024;
  if (!SEGENV.allocateData(MORSECODE_MAX_PATTERN_SIZE)) return mode_static();
  bool* morsecodePattern = reinterpret_cast<bool*>(SEGENV.data);

  static bool lastCheck2 = false;
  static bool lastCheck3 = false;
  static char lastText[WLED_MAX_SEGNAME_LEN+1] = {'\0'};  // Track last text

  bool settingsChanged = (SEGMENT.check2 != lastCheck2) || (SEGMENT.check3 != lastCheck3);  // check if any checkbox settings were changed since last frame
  bool textChanged = (strcmp(text, lastText) != 0);   // check if the text has changed since the last frame

  // Initialize on first call or rebuild pattern
  if (SEGENV.call == 0 || textChanged || settingsChanged) {
    strcpy(lastText, text); // Save current text
    lastCheck2 = SEGMENT.check2;  // Save current state
    lastCheck3 = SEGMENT.check3;  // Save current state
    int patternLength = 0;

    // Build complete morse code pattern
    for (char *c = text; *c; c++) {
      if (patternLength >= MORSECODE_MAX_PATTERN_SIZE - 10) break; // Reserve space for trailing pattern
      // Check for letters
      if (*c >= 'A' && *c <= 'Z') {
        build_morsecode_pattern(letters[*c - 'A'], morsecodePattern, patternLength, MORSECODE_MAX_PATTERN_SIZE);
      }
      // Check for numbers
      else if (*c >= '0' && *c <= '9') {
        build_morsecode_pattern(numbers[*c - '0'], morsecodePattern, patternLength, MORSECODE_MAX_PATTERN_SIZE);
      }
      // Check for a space between words
      else if (*c == ' ') {
        for (int x = 0; x < 4; x++) {   // 7 spaces after the morse code pattern (3 after the last character and now 4 more)
          if (patternLength >= MORSECODE_MAX_PATTERN_SIZE) break;
          morsecodePattern[patternLength++] = false;
        }
      }
      // Check for punctuation
      else if (SEGMENT.check2) {
        const char *punctuationCode = nullptr;
        switch (*c) {
          case '.': punctuationCode = ".-.-.-"; break;
          case ',': punctuationCode = "--..--"; break;
          case '?': punctuationCode = "..--.."; break;
          case ':': punctuationCode = "---..."; break;
          case '-': punctuationCode = "-....-"; break;
          case '!': punctuationCode = "-.-.--"; break;
          case '&': punctuationCode = ".-..."; break;
          case '@': punctuationCode = ".--.-."; break;
          case ')': punctuationCode = "-.--.-"; break;
          case '(': punctuationCode = "-.--."; break;
          case '/': punctuationCode = "-..-."; break;
          case '\'': punctuationCode = ".----."; break;  // apostrophe character must be escaped with a \ character
        }
        if (punctuationCode) {
          build_morsecode_pattern(punctuationCode, morsecodePattern, patternLength, MORSECODE_MAX_PATTERN_SIZE);
        }
      }
    }

    // Build the End-of-message pattern
    if (SEGMENT.check3) {
      build_morsecode_pattern(".-.-.", morsecodePattern, patternLength, MORSECODE_MAX_PATTERN_SIZE);
    }

    for (int x = 0; x < 7; x++) {   // 10 spaces after the last pattern (3 after the last character and now 7 more)
      if (patternLength >= MORSECODE_MAX_PATTERN_SIZE) break;
      morsecodePattern[patternLength++] = false;
    }

    SEGENV.aux1 = patternLength; // Store pattern length
  }

  // Update offset to make the morse code scroll
  uint32_t cycleTime = 50 + (255 - SEGMENT.speed)*3;
  uint32_t it = strip.now / cycleTime;
  if (SEGENV.step != it) {
    SEGENV.aux0++; // Increment scroll offset
    SEGENV.step = it;
  }

  int patternLength = SEGENV.aux1;

  // Clear background
  SEGMENT.fill(BLACK);

  // Draw the scrolling pattern
  int offset = SEGENV.aux0 % patternLength;

  for (int i = 0; i < SEGLEN; i++) {
    int patternIndex = (offset + i) % patternLength;
    if (morsecodePattern[patternIndex]) {
      if (SEGMENT.check1)
        SEGMENT.setPixelColor(i, SEGMENT.color_wheel(SEGENV.aux0 + i));
      else
        SEGMENT.setPixelColor(i, SEGMENT.color_from_palette(i, true, PALETTE_SOLID_WRAP, 0));
    }
  }

  return FRAMETIME;
}
static const char _data_FX_MODE_MORSECODE[] PROGMEM = "Morse Code@Speed,,,,,Color mode,Punctuation,EndOfMessage;;!;1;sx=128,o1=1,o2=1";


/*
/  Lava Lamp 2D effect
*   Uses particles to simulate rising blobs of "lava"
*   Particles slowly rise, merge to create organic flowing shapes, and then fall to the bottom to start again
*   Created by Bob Loeffler using claude.ai
*/

#define MAX_LAVA_PARTICLES 50

typedef struct LavaParticle {
  float x, y;           // Position
  float vx, vy;         // Velocity
  float size;           // Blob size
  uint8_t hue;          // Color
  uint8_t life;         // Lifetime/opacity
  bool active;          // will not be displayed if false
} LavaParticle;

static LavaParticle lavaParticles[MAX_LAVA_PARTICLES];

uint16_t mode_2D_lavalamp(void) {
  if (!strip.isMatrix || !SEGMENT.is2D()) return mode_static(); // not a 2D set-up
  
  const uint16_t cols = SEGMENT.virtualWidth();
  const uint16_t rows = SEGMENT.virtualHeight();
  
  // Initialize particles on first call
  if (SEGENV.call == 0) {
    for (int i = 0; i < MAX_LAVA_PARTICLES; i++) {
      lavaParticles[i].active = false;
    }
  }
  
  // Speed control (slower = more lava lamp like)
  uint8_t speed = SEGMENT.speed >> 3; // 0-31 range
  if (speed < 1) speed = 1;
  
  // Intensity controls number of active particles
  uint8_t numParticles = (SEGMENT.intensity >> 3) + 3; // 3-34 particles (fewer blobs)
  if (numParticles > MAX_LAVA_PARTICLES) numParticles = MAX_LAVA_PARTICLES;
  
  // Track size slider changes
  static uint8_t lastSizeControl = 128;
  uint8_t currentSizeControl = SEGMENT.custom1;
  bool sizeChanged = (currentSizeControl != lastSizeControl);

  if (sizeChanged) {
    // Recalculate size range based on new slider value
    float minSize = cols * 0.15f;
    float maxSize = cols * 0.4f;
    float newRange = (maxSize - minSize) * (currentSizeControl / 255.0f);
    
    for (int i = 0; i < MAX_LAVA_PARTICLES; i++) {
      if (lavaParticles[i].active) {
        // Assign new random size within the new range
        lavaParticles[i].size = minSize + random16((int)(newRange * 10)) / 10.0f;
        // Ensure minimum size
        if (lavaParticles[i].size < minSize) lavaParticles[i].size = minSize;
      }
    }
    
    lastSizeControl = currentSizeControl;
  }

  // Spawn new particles at the bottom near the center
  for (int i = 0; i < MAX_LAVA_PARTICLES; i++) {
    if (!lavaParticles[i].active && hw_random8() < 32) { // Always spawn when slot available
      // Spawn in the middle 60% of the width
      float centerStart = cols * 0.20f;
      float centerWidth = cols * 0.60f;
      lavaParticles[i].x = centerStart + (random16((int)(centerWidth * 10)) / 10.0f);
      lavaParticles[i].y = rows - 1;
      lavaParticles[i].vx = (random16(7) - 3) / 250.0f;
      
      // Speed slider controls vertical velocity (faster = more speed)
      float speedFactor = (SEGMENT.speed + 30) / 100.0f; // 0.3 to 2.85 range
      lavaParticles[i].vy = -(random16(20) + 10) / 100.0f * speedFactor;
      
      // Custom1 slider controls blob size (based on matrix width)
      uint8_t sizeControl = SEGMENT.custom1; // 0-255
      float minSize = cols * 0.15f; // Minimum 15% of width
      float maxSize = cols * 0.4f;  // Maximum 40% of width
      float sizeRange = (maxSize - minSize) * (sizeControl / 255.0f);
      lavaParticles[i].size = minSize + random16((int)(sizeRange * 10)) / 10.0f;

      lavaParticles[i].hue = SEGMENT.check1 ? hw_random8() : random16(256);
      lavaParticles[i].life = 255;
      lavaParticles[i].active = true;
      break;
    }
  }

  // Fade background slightly for trailing effect
  SEGMENT.fadeToBlackBy(40);
  
  // Update and draw particles
  int activeCount = 0;
  for (int i = 0; i < MAX_LAVA_PARTICLES; i++) {
    if (!lavaParticles[i].active) continue;
    activeCount++;

    // Keep particle count on target by deactivating excess particles
    if (activeCount > numParticles) {
      lavaParticles[i].active = false;
      activeCount--;
      continue;
    }

    LavaParticle *p = &lavaParticles[i];
    
    // Physics update
    p->x += p->vx;
    p->y += p->vy;
    
    // Optional blob attraction (enabled with check2)
    if (SEGMENT.check2) {
      for (int j = 0; j < MAX_LAVA_PARTICLES; j++) {
        if (i == j || !lavaParticles[j].active) continue;
        
        LavaParticle *other = &lavaParticles[j];
        
        // Only attract if moving in opposite vertical directions
        if ((p->vy < 0 && other->vy < 0) || (p->vy > 0 && other->vy > 0)) continue;
        
        float dx = other->x - p->x;
        float dy = other->y - p->y;
        float dist = sqrt(dx*dx + dy*dy);
        
        // Apply weak horizontal attraction only
        float attractRange = (p->size + other->size) * 1.0f;
        if (dist > 0 && dist < attractRange) {
          // Very weak horizontal-only attraction
          float force = (1.0f - (dist / attractRange)) * 0.0001f;
          p->vx += (dx / dist) * force;
        }
      }
    }

    // Horizontal oscillation (makes it more organic)
    p->vx += sin((millis() / 1000.0f + i) * 0.5f) * 0.002f; // Reduced oscillation
    p->vx *= 0.92f; // Stronger damping for less drift
    
    // Bounce off sides (don't affect vertical velocity)
    if (p->x <= 0) {
      p->x = 1;
      p->vx = abs(p->vx); // Just reverse horizontal, don't reduce
    }
    if (p->x >= cols) {
      p->x = cols - 1;
      p->vx = -abs(p->vx); // Just reverse horizontal, don't reduce
    }
    
    // Boundary handling with proper reversal
    // When reaching TOP (y=0 area), reverse to fall back down
    if (p->y <= 0.5f * p->size) {
      p->y = 0.5f * p->size;
      if (p->vy < 0) {
        p->vy = -p->vy * 0.5f; // Reverse to positive (fall down) at HALF speed
        // Ensure minimum downward velocity
        if (p->vy < 0.06f) p->vy = 0.06f;
      }
    }

    // When reaching BOTTOM (y=rows-1 area), reverse to rise back up
    if (p->y >= rows - 0.5f * p->size) {
      p->y = rows - 0.5f * p->size;
      if (p->vy > 0) {
        p->vy = -p->vy; // Reverse to negative (rise up)
        // Add random speed boost when rising
        p->vy -= random16(15) / 100.0f; // Subtract to make MORE negative (faster up)
        // Ensure minimum upward velocity
        if (p->vy > -0.10f) p->vy = -0.10f;
      }
    }

    // Keep blobs alive forever - no fading
    p->life = 255;
    
    // Get color
    uint32_t color;
    if (SEGMENT.check1) {
      color = SEGMENT.color_wheel(p->hue);  // Random colors mode
    } else {
      color = SEGMENT.color_from_palette(p->hue, false, true, 0);   // Palette mode
    }
    
    // Extract RGB
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;
    
    // Apply life/opacity
    r = (r * p->life) >> 8;
    g = (g * p->life) >> 8;
    b = (b * p->life) >> 8;
    
    // Draw blob with soft edges (gaussian-like falloff)
    for (int dy = -(int)p->size; dy <= (int)p->size; dy++) {
      for (int dx = -(int)p->size; dx <= (int)p->size; dx++) {
        int px = (int)(p->x + dx);
        int py = (int)(p->y + dy);
        
        if (px >= 0 && px < cols && py >= 0 && py < rows) {
          float dist = sqrt(dx*dx + dy*dy);
          if (dist < p->size) {
            // Soft falloff
            float intensity = 1.0f - (dist / p->size);
            intensity = intensity * intensity; // Square for smoother falloff
            
            uint8_t br = r * intensity;
            uint8_t bg = g * intensity;
            uint8_t bb = b * intensity;
            
            // Additive blending for organic merging
            uint32_t existing = SEGMENT.getPixelColorXY(px, py);
            uint8_t er = (existing >> 16) & 0xFF;
            uint8_t eg = (existing >> 8) & 0xFF;
            uint8_t eb = existing & 0xFF;
            
            er = qadd8(er, br);
            eg = qadd8(eg, bg);
            eb = qadd8(eb, bb);
            
            SEGMENT.setPixelColorXY(px, py, RGBW32(er, eg, eb, 0));
          }
        }
      }
    }
  }

  return FRAMETIME;
}
static const char _data_FX_MODE_2D_LAVALAMP[] PROGMEM = "Lava Lamp@Speed,# of blobs,Blob size,,,Color mode,Attract;;!;2;ix=64,o2=1,pal=47";
#undef MAX_LAVA_PARTICLES


// Spark type is used for Spinner in user_fx usermod
// each needs 20 bytes
typedef struct Spark {
  float pos, posX;
  float vel, velX;
  uint16_t col;
  uint8_t colIndex;
} spark;


/*
/  Spinner effect
*   Uses palettes for particle colors
*   by Bob Loeffler (adapted from the Drip effect)
*/

uint16_t mode_spinner(void) {
  if (SEGLEN <= 1) return mode_static();
  //allocate segment data
  unsigned strips = SEGMENT.nrOfVStrips();
  const int maxNumDrops = 1;  // was 4
  unsigned dataSize = sizeof(spark) * maxNumDrops;
  if (!SEGENV.allocateData(dataSize * strips)) return mode_static(); //allocation failed
  Spark* drops = reinterpret_cast<Spark*>(SEGENV.data);

  SEGMENT.fill(SEGCOLOR(1));

  struct virtualStrip {
    static void runStrip(uint16_t stripNr, Spark* drops) {

      unsigned numDrops = 1; // + (SEGMENT.intensity >> 6); // 255>>6 = 3

      float gravity = -0.0005f - (SEGMENT.speed/50000.0f);
      gravity *= max(1, (int)SEGLEN-1);
      int sourcedrop = 12;

      for (unsigned j = 0; j < numDrops; j++) {
        if (SEGENV.call == 0) {   //if (drops[j].colIndex == 0) { //init
          drops[j].pos = SEGLEN-1;    // start at end
          drops[j].vel = 0;           // speed
          drops[j].col = sourcedrop;  // brightness
          drops[j].colIndex = 1;      // drop state (0 init, 1 forming, 2 falling, 5 bouncing)
        }

        SEGMENT.setPixelColor(indexToVStrip(SEGLEN-1, stripNr), color_blend(BLACK,SEGCOLOR(0), uint8_t(sourcedrop)));// water source
        if (drops[j].colIndex==1) {
          if (drops[j].col>255) drops[j].col=255;
          SEGMENT.setPixelColor(indexToVStrip(uint16_t(drops[j].pos), stripNr), color_blend(BLACK,SEGCOLOR(0),uint8_t(drops[j].col)));

          drops[j].col += map(SEGMENT.speed, 0, 255, 1, 6); // swelling

          if (hw_random8() < drops[j].col/10) {               // random drop
            drops[j].colIndex=2;               //fall
            drops[j].col=255;
          }
            
           //drops[j].colIndex=2;               //fall
        }
        if (drops[j].colIndex > 1) {           // falling
          if (drops[j].pos > 0) {              // fall until end of segment
            drops[j].pos += drops[j].vel;
            if (drops[j].pos < 0) drops[j].pos = 0;
            drops[j].vel += gravity;           // gravity is negative

            for (int i=1;i<7-drops[j].colIndex;i++) { // some minor math so we don't expand bouncing droplets
              unsigned pos = constrain(unsigned(drops[j].pos) +i, 0, SEGLEN-1); //this is BAD, returns a pos >= SEGLEN occasionally
              SEGMENT.setPixelColor(indexToVStrip(pos, stripNr), color_blend(BLACK,SEGCOLOR(0),uint8_t(drops[j].col/i))); //spread pixel with fade while falling
            }

            if (drops[j].colIndex > 2) {       // during bounce, some water is on the floor
              SEGMENT.setPixelColor(indexToVStrip(0, stripNr), color_blend(SEGCOLOR(0),BLACK,uint8_t(drops[j].col)));
            }
          } else {                             // we hit bottom
            //drops[j].pos = SEGLEN-1;           // wrap to end of segment
            if (drops[j].colIndex > 2) {       // already hit once, so back to forming
              drops[j].colIndex = 0;
              drops[j].col = sourcedrop;

            } else {

              if (drops[j].colIndex==2) {      // init bounce
                drops[j].vel = -drops[j].vel/4;// reverse velocity with damping
                drops[j].pos += drops[j].vel;
              }
              drops[j].col = sourcedrop*2;
              drops[j].colIndex = 5;           // bouncing
            }
          }
        }
      }
    }
  };

  for (unsigned stripNr=0; stripNr<strips; stripNr++)
    virtualStrip::runStrip(stripNr, &drops[stripNr*maxNumDrops]);

  return FRAMETIME;
}
static const char _data_FX_MODE_SPINNER[] PROGMEM = "Spinner@Gravity,# of drips,,,,,Overlay;!,!;!;;m12=1";



/////////////////////
//  UserMod Class  //
/////////////////////

class UserFxUsermod : public Usermod {
 private:
 public:
  void setup() override {
    strip.addEffect(255, &mode_diffusionfire, _data_FX_MODE_DIFFUSIONFIRE);
    strip.addEffect(255, &mode_ants, _data_FX_MODE_ANTS);
    strip.addEffect(255, &mode_morsecode, _data_FX_MODE_MORSECODE);
    strip.addEffect(255, &mode_2D_lavalamp, _data_FX_MODE_2D_LAVALAMP);
    strip.addEffect(255, &mode_spinner, _data_FX_MODE_SPINNER);

    ////////////////////////////////////////
    //  add your effect function(s) here  //
    ////////////////////////////////////////

    // use id=255 for all custom user FX (the final id is assigned when adding the effect)

    // strip.addEffect(255, &mode_your_effect, _data_FX_MODE_YOUR_EFFECT);
    // strip.addEffect(255, &mode_your_effect2, _data_FX_MODE_YOUR_EFFECT2);
    // strip.addEffect(255, &mode_your_effect3, _data_FX_MODE_YOUR_EFFECT3);
  }
  void loop() override {} // nothing to do in the loop
  uint16_t getId() override { return USERMOD_ID_USER_FX; }
};

static UserFxUsermod user_fx;
REGISTER_USERMOD(user_fx);
