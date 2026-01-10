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
*   Checkbox2 is for Smear mode (enabled is smear pixel colors, disabled is no smearing)
*   Checkbox3 is for whether the ants will bump into each other (disabled) or just pass by each other (enabled). Ignored if Gathering Food option is enabled.
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
constexpr unsigned ANT_SIZE_MIN = 1;
constexpr unsigned ANT_SIZE_MAX = 20;

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
    return SEGMENT.color_from_palette(antIndex * 255 / numAnts, false, PALETTE_SOLID_WRAP, 255);
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
  unsigned antSize = map(SEGMENT.custom1, 0, 255, ANT_SIZE_MIN, ANT_SIZE_MAX) + (gatherFood ? 1 : 0);

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
*   aux1 saves settings: check3 (1 bit), check3 (1 bit), text hash (4 bits) and pattern length (10 bits)
*   The first slider (sx) selects the scrolling speed
*   Checkbox1 selects the color mode
*   Checkbox2 displays punctuation or not
*   Checkbox3 displays the End-of-message code or not
*   We get the text from the SEGMENT.name and convert it to morse code
*   This effect uses a bit array, instead of bool array, for efficient storage - 8x memory reduction (128 bytes vs 1024 bytes)
*
*   Morse Code rules:
*    - a dot is 1 pixel/LED; a dash is 3 pixels/LEDs
*    - there is 1 space between each dot or dash that make up a letter/number/punctuation
*    - there are 3 spaces between each letter/number/punctuation
*    - there are 7 spaces between each word
*/

// Bit manipulation macros
#define SET_BIT8(arr, i) ((arr)[(i) >> 3] |= (1 << ((i) & 7)))
#define GET_BIT8(arr, i) (((arr)[(i) >> 3] & (1 << ((i) & 7))) != 0)

// Build morse code pattern into a buffer
void build_morsecode_pattern(const char *morse_code, uint8_t *pattern, uint16_t &index, int maxSize) {
  const char *c = morse_code;
  
  // Build the dots and dashes into pattern array
  while (*c != '\0') {
    // it's a dot which is 1 pixel
    if (*c == '.') {
      if (index >= maxSize - 1) return;
      SET_BIT8(pattern, index);
      index++;
    }
    else { // Must be a dash which is 3 pixels
      if (index >= maxSize - 3) return;
      SET_BIT8(pattern, index);
      index++;
      SET_BIT8(pattern, index);
      index++;
      SET_BIT8(pattern, index);
      index++;
    }

    c++;

    // 1 space between parts of a letter/number/punctuation (but not after the last one)
    if (*c != '\0') {
      if (index >= maxSize) return;
      index++;
    }
  }

  // 3 spaces between two letters/numbers/punctuation
  if (index >= maxSize - 2) return;
  index++;
  if (index >= maxSize - 1) return;
  index++;
  if (index >= maxSize) return;
  index++;
}

static uint16_t mode_morsecode(void) {
  if (SEGLEN < 1) return mode_static();
  
  // A-Z in Morse Code
  static const char * letters[] = {".-", "-...", "-.-.", "-..", ".", "..-.", "--.", "....", "..", ".---", "-.-", ".-..", "--",
                     "-.", "---", ".--.", "--.-", ".-.", "...", "-", "..-", "...-", ".--", "-..-", "-.--", "--.."};
  // 0-9 in Morse Code
  static const char * numbers[] = {"-----", ".----", "..---", "...--", "....-", ".....", "-....", "--...", "---..", "----."};

  // Punctuation in Morse Code
  struct PunctuationMapping {
    char character;
    const char* code;
  };

  static const PunctuationMapping punctuation[] = {
    {'.', ".-.-.-"}, {',', "--..--"}, {'?', "..--.."}, 
    {':', "---..."}, {'-', "-....-"}, {'!', "-.-.--"},
    {'&', ".-..."}, {'@', ".--.-."}, {')', "-.--.-"},
    {'(', "-.--."}, {'/', "-..-."}, {'\'', ".----."}
  };

  // Get the text to display
  char text[WLED_MAX_SEGNAME_LEN+1] = {'\0'};
  size_t len = 0;

  if (SEGMENT.name) len = strlen(SEGMENT.name);
  if (len == 0) {
    strcpy_P(text, PSTR("I Love WLED!"));
  } else {
    strcpy(text, SEGMENT.name);
  }

  // Convert to uppercase in place
  for (char *p = text; *p; p++) {
    *p = toupper(*p);
  }

  // Allocate per-segment storage for pattern (1024 bits = 128 bytes)
  constexpr size_t MORSECODE_MAX_PATTERN_SIZE = 1024;
  constexpr size_t MORSECODE_PATTERN_BYTES = MORSECODE_MAX_PATTERN_SIZE / 8; // 128 bytes
  if (!SEGENV.allocateData(MORSECODE_PATTERN_BYTES)) return mode_static();
  uint8_t* morsecodePattern = reinterpret_cast<uint8_t*>(SEGENV.data);

  // SEGENV.aux1 stores: [bit 15: check2] [bit 14: check3] [bits 10-13: text hash (4 bits)] [bits 0-9: pattern length]
  bool lastCheck2 = (SEGENV.aux1 & 0x8000) != 0;
  bool lastCheck3 = (SEGENV.aux1 & 0x4000) != 0;
  uint16_t lastHashBits = (SEGENV.aux1 >> 10) & 0xF; // 4 bits of hash
  uint16_t patternLength = SEGENV.aux1 & 0x3FF; // Lower 10 bits for length (up to 1023)

  // Compute text hash
  uint16_t textHash = 0;
  for (char *p = text; *p; p++) {
    textHash = ((textHash << 5) + textHash) + *p;
  }
  uint16_t currentHashBits = (textHash >> 12) & 0xF; // Use upper 4 bits of hash

  bool textChanged = (currentHashBits != lastHashBits) && (SEGENV.call > 0);

  // Check if we need to rebuild the pattern
  bool needsRebuild = (SEGENV.call == 0) || textChanged || (SEGMENT.check2 != lastCheck2) || (SEGMENT.check3 != lastCheck3);

  // Initialize on first call or rebuild pattern
  if (needsRebuild) {
    patternLength = 0;

    // Clear the bit array first
    memset(morsecodePattern, 0, MORSECODE_PATTERN_BYTES);

    // Build complete morse code pattern
    for (char *c = text; *c; c++) {
      if (patternLength >= MORSECODE_MAX_PATTERN_SIZE - 10) break;

      if (*c >= 'A' && *c <= 'Z') {
        build_morsecode_pattern(letters[*c - 'A'], morsecodePattern, patternLength, MORSECODE_MAX_PATTERN_SIZE);
      }
      else if (*c >= '0' && *c <= '9') {
        build_morsecode_pattern(numbers[*c - '0'], morsecodePattern, patternLength, MORSECODE_MAX_PATTERN_SIZE);
      }
      else if (*c == ' ') {
        for (int x = 0; x < 4; x++) {
          if (patternLength >= MORSECODE_MAX_PATTERN_SIZE) break;
          patternLength++;
        }
      }
      else if (SEGMENT.check2) {
        const char *punctuationCode = nullptr;
        for (const auto& p : punctuation) {
          if (*c == p.character) {
            punctuationCode = p.code;
            break;
          }
        }
        if (punctuationCode) {
          build_morsecode_pattern(punctuationCode, morsecodePattern, patternLength, MORSECODE_MAX_PATTERN_SIZE);
        }
      }
    }

    if (SEGMENT.check3) {
      build_morsecode_pattern(".-.-.", morsecodePattern, patternLength, MORSECODE_MAX_PATTERN_SIZE);
    }

    for (int x = 0; x < 7; x++) {
      if (patternLength >= MORSECODE_MAX_PATTERN_SIZE) break;
      patternLength++;
    }

    // Store pattern length, checkbox states, and hash bits in aux1
    SEGENV.aux1 = patternLength | (currentHashBits << 10) | (SEGMENT.check2 ? 0x8000 : 0) | (SEGMENT.check3 ? 0x4000 : 0);

    // Reset the scroll offset
    SEGENV.aux0 = 0;
  }

  // if pattern is empty for some reason, display black background only
  if (patternLength == 0) {
    SEGMENT.fill(BLACK);
    return FRAMETIME;
  }

  // Update offset to make the morse code scroll
  // Use step for scroll timing only
  uint32_t cycleTime = 50 + (255 - SEGMENT.speed)*3;
  uint32_t it = strip.now / cycleTime;
  if (SEGENV.step != it) {
    SEGENV.aux0++;
    SEGENV.step = it;
  }

  // Clear background
  SEGMENT.fill(BLACK);

  // Draw the scrolling pattern
  int offset = SEGENV.aux0 % patternLength;

  for (int i = 0; i < SEGLEN; i++) {
    int patternIndex = (offset + i) % patternLength;
    if (GET_BIT8(morsecodePattern, patternIndex)) {
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
*  Uses particles to simulate rising blobs of "lava"
*  Particles slowly rise, merge to create organic flowing shapes, and then fall to the bottom to start again
*  Created by Bob Loeffler using claude.ai
*  The first slider sets the speed of the rising and falling blobs
*  The second slider sets the number of active blobs
*  The third slider sets the size range of the blobs
*  The first checkbox sets the color mode (color wheel or palette)
*  The second checkbox sets the attraction of blobs (checked will make the blobs attract other close blobs horizontally)
*  aux0 keeps track of the blob size changes
*/

typedef struct LavaParticle {
  float x, y;           // Position
  float vx, vy;         // Velocity
  float size;           // Blob size
  uint8_t hue;          // Color
  uint8_t life;         // Lifetime/opacity
  bool active;          // will not be displayed if false
} LavaParticle;

uint16_t mode_2D_lavalamp(void) {
  if (!strip.isMatrix || !SEGMENT.is2D()) return mode_static(); // not a 2D set-up
  
  const uint16_t cols = SEGMENT.virtualWidth();
  const uint16_t rows = SEGMENT.virtualHeight();
  
  // Allocate per-segment storage
  constexpr size_t MAX_LAVA_PARTICLES = 35;
  if (!SEGENV.allocateData(sizeof(LavaParticle) * MAX_LAVA_PARTICLES)) return mode_static();
  LavaParticle* lavaParticles = reinterpret_cast<LavaParticle*>(SEGENV.data);

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
  uint8_t lastSizeControl = SEGENV.aux0;
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
        lavaParticles[i].size = minSize + (float)random16((int)(newRange));
        // Ensure minimum size
        if (lavaParticles[i].size < minSize) lavaParticles[i].size = minSize;
      }
    }
    SEGENV.aux0 = currentSizeControl;
  }

  // Spawn new particles at the bottom near the center
  for (int i = 0; i < MAX_LAVA_PARTICLES; i++) {
    if (!lavaParticles[i].active && hw_random8() < 32) { // sporadically spawn when slot available
      // Spawn in the middle 60% of the width
      float centerStart = cols * 0.20f;
      float centerWidth = cols * 0.60f;
      lavaParticles[i].x = centerStart + (float)random16((int)(centerWidth));
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
      lavaParticles[i].size = minSize + (float)random16((int)(sizeRange));

      lavaParticles[i].hue = hw_random8();
      lavaParticles[i].life = 255;
      lavaParticles[i].active = true;
      break;
    }
  }

  // Fade background slightly for trailing effect
  SEGMENT.fadeToBlackBy(40);
  
  // Update and draw particles
  int activeCount = 0;
  unsigned long currentMillis = strip.now;
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
        
        // Skip attraction if moving in same vertical direction (both up or both down)
        if ((p->vy < 0 && other->vy < 0) || (p->vy > 0 && other->vy > 0)) continue;
        
        float dx = other->x - p->x;
        float dy = other->y - p->y;

        // Apply weak horizontal attraction only
        float attractRange = p->size + other->size;
        float distSq = dx*dx + dy*dy;
        float attractRangeSq = attractRange * attractRange;
        if (distSq > 0 && distSq < attractRangeSq) {
          float dist = sqrt(distSq); // Only compute sqrt when needed
          float force = (1.0f - (dist / attractRange)) * 0.0001f;
          p->vx += (dx / dist) * force;
        }
      }
    }

    // Horizontal oscillation (makes it more organic)
    p->vx += sin((currentMillis / 1000.0f + i) * 0.5f) * 0.002f; // Reduced oscillation
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

    // Keep blobs alive forever (no fading) - maybe change in the future?
    p->life = 255;
    
    // Get color
    uint32_t color;
    if (SEGMENT.check1) {
      color = SEGMENT.color_wheel(p->hue);  // Random colors mode
    } else {
      color = SEGMENT.color_from_palette(p->hue, false, true, 0);   // Palette mode
    }
    
    // Extract RGB and apply life/opacity
    uint8_t w = (W(color) * p->life) >> 8;
    uint8_t r = (R(color) * p->life) >> 8;
    uint8_t g = (G(color) * p->life) >> 8;
    uint8_t b = (B(color) * p->life) >> 8;

    // Draw blob with soft edges (gaussian-like falloff)
    float sizeSq = p->size * p->size;
    for (int dy = -(int)p->size; dy <= (int)p->size; dy++) {
      for (int dx = -(int)p->size; dx <= (int)p->size; dx++) {
        int px = (int)(p->x + dx);
        int py = (int)(p->y + dy);
        
        if (px >= 0 && px < cols && py >= 0 && py < rows) {
          float distSq = dx*dx + dy*dy;
          if (distSq < sizeSq) {
            // Soft falloff using squared distance (faster)
            float intensity = 1.0f - (distSq / sizeSq);
            intensity = intensity * intensity; // Square for smoother falloff
            
            uint8_t bw = w * intensity;
            uint8_t br = r * intensity;
            uint8_t bg = g * intensity;
            uint8_t bb = b * intensity;

            // Additive blending for organic merging
            uint32_t existing = SEGMENT.getPixelColorXY(px, py);
            uint32_t newColor = RGBW32(br, bg, bb, bw);
            uint32_t blended = color_add(existing, newColor);
            SEGMENT.setPixelColorXY(px, py, blended);
          }
        }
      }
    }
  }

  return FRAMETIME;
}
static const char _data_FX_MODE_2D_LAVALAMP[] PROGMEM = "Lava Lamp@Speed,# of blobs,Blob size,,,Color mode,Attract;;!;2;ix=64,o2=1,pal=47";


/*
 * Spinning Wheel effect - LED animates around 1D strip, slows down and stops at random position
 *  Created by Bob Loeffler and claude.ai
 *  First slider (Spin speed) is for the speed of the moving/spinning LED
 *  Second slider (Spin time) is for how long before the slowdown phase starts
 *  Third slider (Spin delay) is for how long it takes for the LED to start spinning again after the previous spin
 *  The first checkbox sets the color mode (color wheel or palette)
 *  The second checkbox sets the spin speed to a random number range
 *  The third checkbox sets the spin time to a random number range
 */

uint16_t mode_spinning_wheel(void) {
  if (SEGLEN < 1) return mode_static();
  unsigned dataSize = 28; // 7 state variables × 4 bytes each
  if (!SEGENV.allocateData(dataSize)) return mode_static();
  uint32_t* state = (uint32_t*)SEGENV.data;
  // state[0] = current position (fixed point: upper 16 bits = position, lower 16 bits = fraction)
  // state[1] = velocity (fixed point: pixels per frame * 65536)
  // state[2] = phase (0=fast spin, 1=slowing, 2=wobble, 3=stopped)
  // state[3] = stop time (when phase 3 was entered)
  // state[4] = wobble step (0=at stop pos, 1=moved back, 2=returned to stop)
  // state[5] = slowdown start time (when to transition from phase 0 to phase 1)
  // state[6] = wobble timing (for 100ms / 300ms / 300ms delays)

  // state[] index values for easier readability
  constexpr unsigned CUR_POS_IDX       = 0;
  constexpr unsigned VELOCITY_IDX      = 1;
  constexpr unsigned PHASE_IDX         = 2;
  constexpr unsigned STOP_TIME_IDX     = 3;
  constexpr unsigned WOBBLE_STEP_IDX   = 4;
  constexpr unsigned SLOWDOWN_TIME_IDX = 5;
  constexpr unsigned WOBBLE_TIME_IDX   = 6;

  SEGMENT.fill(SEGCOLOR(1));

  uint8_t phase = state[PHASE_IDX];
  uint32_t now = strip.now;

  uint16_t spin_delay = map(SEGMENT.custom3, 0, 31, 2000, 15000);  // delay up to 15 seconds after the LED has stopped moving

  // Initial setup and auto-restart after being stopped
  if (SEGENV.call == 0 || (phase == 3 && state[STOP_TIME_IDX] != 0 && (now >= state[STOP_TIME_IDX] + spin_delay))) {
    if (SEGENV.call == 0)
      random16_set_seed(analogRead(0));
    else
      random16_add_entropy(analogRead(0));
    state[CUR_POS_IDX] = 0;
    if (SEGMENT.check2) {  // if random speed is selected
      state[VELOCITY_IDX] = random16(200, 900) * 655;
    } else {
      uint16_t speed = map(SEGMENT.speed, 0, 255, 300, 800);
      state[VELOCITY_IDX] = random16(speed - 100, speed + 100) * 655;
    }
    state[PHASE_IDX] = 0;
    state[STOP_TIME_IDX] = 0;
    state[WOBBLE_STEP_IDX] = 0;
    state[WOBBLE_TIME_IDX] = 0;
    // Set new slowdown start time
    if (SEGMENT.check3) {  // if random slowdown is selected
      uint16_t slowdown_delay = random16(2000, 6000);
      state[SLOWDOWN_TIME_IDX] = now + slowdown_delay;
    } else {
      uint16_t slowdown = map(SEGMENT.intensity, 0, 255, 3000, 5000);
      uint16_t slowdown_delay = random16(slowdown - 1000, slowdown + 1000);
      state[SLOWDOWN_TIME_IDX] = now + slowdown_delay;
    }
    phase = 0;
  }
  
  uint32_t pos_fixed = state[CUR_POS_IDX];
  uint32_t velocity = state[VELOCITY_IDX];
  
  // Phase management (0=fast spin, 1=slowing down, 2=wobble at end, 3=stopped)
  if (phase == 0) {
    // Fast spinning phase
    if (now >= state[SLOWDOWN_TIME_IDX]) {
      phase = 1;
      state[PHASE_IDX] = 1;
    }
  } else if (phase == 1) {
    // Slowing phase - apply deceleration
    uint32_t decel = velocity / 80;
    if (decel < 100) decel = 100;
    
    if (velocity > decel) {
      velocity -= decel;
      state[VELOCITY_IDX] = velocity;
    } else {
      velocity = 0;
      state[VELOCITY_IDX] = 0;
    }
    
    // Check if stopped
    if (velocity < 2000) {
      velocity = 0;
      state[VELOCITY_IDX] = 0;
      phase = 2;
      state[PHASE_IDX] = 2;
      state[WOBBLE_STEP_IDX] = 0;
      // Save the stop position
      uint16_t stop_pos = (pos_fixed >> 16) % SEGLEN;
      SEGENV.step = stop_pos;
      state[WOBBLE_TIME_IDX] = now; // Start wobble timing
    }
  } else if (phase == 2) {
    // Wobble phase - wobble around the saved stop position
    uint32_t wobble_step = state[WOBBLE_STEP_IDX];
    uint16_t stop_pos = SEGENV.step;
    
    if (wobble_step == 0 && (now - state[WOBBLE_TIME_IDX] >= 100)) {
      // Move back one LED from stop position
      uint16_t back_pos = (stop_pos == 0) ? SEGLEN - 1 : stop_pos - 1;
      pos_fixed = ((uint32_t)back_pos) << 16;
      state[CUR_POS_IDX] = pos_fixed;
      state[WOBBLE_STEP_IDX] = 1;
      state[WOBBLE_TIME_IDX] = now;
    } else if (wobble_step == 1 && (now - state[WOBBLE_TIME_IDX] >= 300)) {
      // Move forward back to stop position
      pos_fixed = ((uint32_t)stop_pos) << 16;
      state[CUR_POS_IDX] = pos_fixed;
      state[WOBBLE_STEP_IDX] = 2;
      state[WOBBLE_TIME_IDX] = now;
    } else if (wobble_step == 2 && (now - state[WOBBLE_TIME_IDX] >= 300)) {
      // Wobble complete, enter stopped phase
      phase = 3;
      state[PHASE_IDX] = 3;
      state[STOP_TIME_IDX] = now;
    }
  }
  
  // Update position only if not in wobble or stopped phase
  if (phase != 2 && phase != 3) {
    pos_fixed += velocity;
    state[CUR_POS_IDX] = pos_fixed;
  }
  
  // Get integer position
  uint16_t pos = (pos_fixed >> 16) % SEGLEN;

  // Get color
  uint32_t scale = (255 << 16) / SEGLEN;
  uint8_t hue = (scale * pos) >> 16;
  uint32_t color;
  if (SEGMENT.check1) {
    color = SEGMENT.color_wheel(hue);  // Random colors mode
  } else {
    color = SEGMENT.color_from_palette(hue, false, true, 0);   // Palette mode
  }

  // Light up current position
  SEGMENT.setPixelColor(pos, color);

  return FRAMETIME;
}
static const char _data_FX_MODE_SPINNINGWHEEL[] PROGMEM = "Spinning Wheel@Spin speed,Spin time,,,Spin delay,Color mode,Random speed,Random time;!,!;!;;m12=1,c3=8";



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
    strip.addEffect(255, &mode_spinning_wheel, _data_FX_MODE_SPINNINGWHEEL);

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
