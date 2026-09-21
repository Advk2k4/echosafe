/*
 * EchoSafe Full System Firmware
 * ESP32-S3-WROOM-1 N16R8
 *
 * 4x ICS43434 MEMS Microphones (2 I2S buses, stereo pairs)
 * 2x MAX98357A Speakers (parallel, I2S_NUM_1 time-muxed with bottom mics)
 * 4x DRV2605L Haptic Drivers via TCA9548A I2C Mux
 *
 * ── Microphone Wiring ──────────────────────────────────────────────────────────
 *   TOP pair  (I2S_NUM_0):  LRCLK→GPIO4  DOUT→GPIO16  BCLK→GPIO5
 *     Top-Left  (TL): SEL→GND  →  LEFT  channel (WS low)
 *     Top-Right (TR): SEL→3V   →  RIGHT channel (WS high)
 *   BOT pair  (I2S_NUM_1):  LRCLK→GPIO12 DOUT→GPIO18  BCLK→GPIO21
 *     Bot-Left  (BL): SEL→GND  →  LEFT  channel (WS low)
 *     Bot-Right (BR): SEL→3V   →  RIGHT channel (WS high)
 *
 * ── Speaker Wiring ─────────────────────────────────────────────────────────────
 *   Both MAX98357A in parallel: DIN→GPIO47  BCLK→GPIO48  LRC→GPIO45  SD→VIN
 *   Shares I2S_NUM_1 with BOT mics (time-multiplexed: mics during capture,
 *   speaker during alert playback, then mics restored)
 *
 * ── Haptic Wiring ──────────────────────────────────────────────────────────────
 *   TCA9548A I2C Mux: SDA→GPIO9  SCL→GPIO11  A0-A3→GND → address 0x70
 *     Ch 0 → Top-Left  DRV2605L
 *     Ch 1 → Top-Right DRV2605L
 *     Ch 2 → Bot-Left  DRV2605L
 *     Ch 3 → Bot-Right DRV2605L
 *   DRV2605L fixed I2C address: 0x5A on each channel
 *
 * ── Two-Phase Operation ────────────────────────────────────────────────────────
 *   Phase 1 — ML Inference:  I2S_NUM_0 @ 16 kHz ONLY_LEFT (TL mic)
 *   Phase 2 — Direction:     Both ports @ 44.1 kHz RIGHT_LEFT (all 4 mics)
 *   Phase 3 — Alert:         I2S_NUM_1 as speaker TX (WAV from LittleFS)
 *
 * ── WAV Files (upload to LittleFS via Arduino plugin from data/ folder) ─────────
 *   /vehiclehorn.wav   → class 0 (horns)
 *   /bicyclebell.wav   → class 2 (bells)
 *   /gun_shot.wav      → class 3 (gunshots)
 *   /emergencysiren.wav → class 4 (sirens)
 *
 * ── Required Arduino Libraries ────────────────────────────────────────────────
 *   Adafruit DRV2605 Library  (Library Manager: "Adafruit DRV2605")
 *
 * ── Commands ──────────────────────────────────────────────────────────────────
 *   i = single inference   r = continuous   s = stop
 *   h = haptic test        d = direction test
 *   t = tone test (440 Hz) p = speaker test (all WAVs)
 */

#include <driver/i2s.h>
#include <Wire.h>
#include <Adafruit_DRV2605.h>
#include <FS.h>
#include <LittleFS.h>
#include <math.h>
#include "../echosafe_inference/model_weights.h"

// ─── ML INFERENCE PARAMETERS (must match training) ────────────────────────────
#define ML_SAMPLE_RATE     16000
#define ML_NORM_DIV        2147483648.0f  // 2^31 — ICS43434 24-bit left-justified

// ─── DIRECTION DETECTION PARAMETERS ──────────────────────────────────────────
#define DIR_SAMPLE_RATE    44100   // higher rate → finer TDOA resolution
#define DIR_NORM_DIV       8388608.0f  // 2^23 for 24-bit at higher rate

// ─── SHARED MFCC / FFT PARAMETERS ────────────────────────────────────────────
#define FRAME_SIZE         512
#define NUM_MFCC           13
#define NUM_FRAMES         31
#define FFT_SIZE           512
#define MEL_BINS           26
#define PRE_EMPHASIS       0.97f
#define CONFIDENCE_THR     0.65f

// ─── ML MIC CHANNEL ──────────────────────────────────────────────────────────
// ONLY_LEFT  = TL mic (SEL→GND) — switch back to this once TL hardware is fixed
// ONLY_RIGHT = TR mic (SEL→3V)  — in use while TL shows 0.0000 RMS
// IMPORTANT: retrain the model after swapping channels for best accuracy.
// NOTE: model_weights.h was trained on audio from the single-mic reference
// rig (echosafe_feature_collector.ino, GPIO 5/6/7), not from either TL or TR
// on this board's mic array — switching this define does not by itself
// resolve that mismatch. Retraining on audio captured from whichever channel
// is actually used here is the real fix, not just a channel flip.
#define ML_MIC_CHANNEL     I2S_CHANNEL_FMT_ONLY_RIGHT

// ─── ENERGY-TRIGGERED CAPTURE ────────────────────────────────────────────────
// Inference window starts on the first frame whose RMS exceeds this value.
// This aligns the window to the sound onset, which is critical for transient
// sounds like gunshots (otherwise most of the window is silence → "noise").
// Tune: raise if spurious triggers in quiet room, lower for distant sounds.
#define CAPTURE_TRIGGER_RMS  0.003f
// If no sound is heard within this many ms, classify background anyway.
#define CAPTURE_TIMEOUT_MS   2000

// ─── TDOA PARAMETERS ─────────────────────────────────────────────────────────
#define TDOA_FRAMES        1024    // samples per mic for direction capture
#define MAX_LAG            24     // max sample lag to search (≈ 0.54 ms at 44.1 kHz)
#define TDOA_MIN_RMS       0.0005f // below this → signal too quiet to locate

// ─── TOP MIC PAIR (I2S_NUM_0) ─────────────────────────────────────────────────
#define MIC_TOP_WS         4
#define MIC_TOP_BCLK       5
#define MIC_TOP_DOUT       16
#define MIC_TOP_PORT       I2S_NUM_0

// ─── BOTTOM MIC PAIR (I2S_NUM_1, shared with speaker) ────────────────────────
#define MIC_BOT_WS         12
#define MIC_BOT_BCLK       21
#define MIC_BOT_DOUT       18
#define MIC_BOT_PORT       I2S_NUM_1

// ─── SPEAKERS (both MAX98357A in parallel, I2S_NUM_1 TX) ──────────────────────
#define SPK_DIN            47
#define SPK_BCLK           48
#define SPK_LRC            45
#define SPK_PORT           I2S_NUM_1   // same port as BOT mics — time-muxed

// ─── I2C / HAPTICS ────────────────────────────────────────────────────────────
#define I2C_SDA            9
#define I2C_SCL            11
#define TCA_ADDR           0x70   // TCA9548A: A0-A3→GND
#define NUM_HAPTIC         4
#define HAPTIC_TL          0
#define HAPTIC_TR          1
#define HAPTIC_BL          2
#define HAPTIC_BR          3
#define HAPTIC_EFFECT      47   // DRV2605L waveform effect #47 (long buzz)

// ─── WAV ALERT FILES ──────────────────────────────────────────────────────────
const char* WAV_FILES[NUM_CLASSES] = {
  "/vehiclehorn.wav",    // 0: horns
  nullptr,               // 1: noise — no alert
  "/bicyclebell.wav",    // 2: bells
  "/gun_shot.wav",       // 3: gunshots
  "/emergencysiren.wav", // 4: sirens
};

// ─── GLOBAL STATE ─────────────────────────────────────────────────────────────
Adafruit_DRV2605 drv;
bool g_continuous   = false;
bool g_fs_ok        = false;
bool g_haptic_ok[NUM_HAPTIC] = {};

// ─── GLOBAL BUFFERS ───────────────────────────────────────────────────────────
// ML inference (16 kHz, mono TL)
float    g_frame[FRAME_SIZE];
int32_t  g_ml_raw[FRAME_SIZE];
float    g_mfcc[NUM_FRAMES][NUM_MFCC];
float    g_feat[INPUT_DIM];
float    g_l1[128], g_l2[64], g_probs[NUM_CLASSES];

// MFCC intermediates
float    g_fft_re[FFT_SIZE], g_fft_im[FFT_SIZE];
float    g_pspec[FFT_SIZE / 2 + 1];
float    g_mel_e[MEL_BINS];
float    g_mel_fb[MEL_BINS][FFT_SIZE / 2 + 1];
float    g_dct_m[NUM_MFCC][MEL_BINS];

// Direction detection (44.1 kHz, all 4 mics)
int32_t  g_top_raw[TDOA_FRAMES * 2];   // interleaved [TL, TR, TL, TR, ...]
int32_t  g_bot_raw[TDOA_FRAMES * 2];   // interleaved [BL, BR, ...]
float    g_tl[TDOA_FRAMES], g_tr[TDOA_FRAMES];
float    g_bl[TDOA_FRAMES], g_br[TDOA_FRAMES];

// WAV / tone output (stereo int16)
int16_t  g_spk_buf[512];

// ══════════════════════════════════════════════════════════════════════════════
//  TCA9548A I2C MUX
// ══════════════════════════════════════════════════════════════════════════════
static void tca_select(uint8_t ch) {
  Wire.beginTransmission(TCA_ADDR);
  Wire.write(1 << ch);
  Wire.endTransmission();
  delay(10);
}

static void tca_deselect() {
  Wire.beginTransmission(TCA_ADDR);
  Wire.write(0x00);
  Wire.endTransmission();
  delay(5);
}

// ══════════════════════════════════════════════════════════════════════════════
//  DRV2605L HAPTICS
// ══════════════════════════════════════════════════════════════════════════════
void init_haptics() {
  tca_deselect();
  Serial.println("Haptics:");
  const char* labels[4] = { "TL", "TR", "BL", "BR" };
  for (int ch = 0; ch < NUM_HAPTIC; ch++) {
    tca_select(ch);
    if (drv.begin()) {
      drv.selectLibrary(1);
      drv.setMode(DRV2605_MODE_INTTRIG);
      drv.setWaveform(0, HAPTIC_EFFECT);
      drv.setWaveform(1, 0);
      g_haptic_ok[ch] = true;
      Serial.printf("  Ch%d (%s): OK\n", ch, labels[ch]);
    } else {
      Serial.printf("  Ch%d (%s): FAILED\n", ch, labels[ch]);
    }
    tca_deselect();
  }
}

// Fire one haptic motor by channel index (0=TL, 1=TR, 2=BL, 3=BR).
void fire_haptic(int ch) {
  if (ch < 0 || ch >= NUM_HAPTIC || !g_haptic_ok[ch]) return;
  tca_select(ch);
  drv.go();
  tca_deselect();
}

void haptic_test_all() {
  const char* labels[4] = { "Top-Left", "Top-Right", "Bot-Left", "Bot-Right" };
  for (int i = 0; i < NUM_HAPTIC; i++) {
    Serial.printf("  Motor %d (%s)\n", i, labels[i]);
    fire_haptic(i);
    delay(600);
  }
}

// ══════════════════════════════════════════════════════════════════════════════
//  MEL FILTER BANKS + DCT MATRIX
// ══════════════════════════════════════════════════════════════════════════════
void init_mel_filters() {
  float mel_min = 2595.0f * log10f(1.0f + 0.0f    / 700.0f);
  float mel_max = 2595.0f * log10f(1.0f + 8000.0f / 700.0f);
  float step = (mel_max - mel_min) / (MEL_BINS + 1);
  float pts[MEL_BINS + 2];
  for (int i = 0; i < MEL_BINS + 2; i++)
    pts[i] = 700.0f * (powf(10.0f, (mel_min + i * step) / 2595.0f) - 1.0f);
  for (int i = 0; i < MEL_BINS; i++)
    for (int j = 0; j <= FFT_SIZE / 2; j++) {
      float f = (float)j * ML_SAMPLE_RATE / FFT_SIZE;
      if      (f >= pts[i]   && f <= pts[i+1]) g_mel_fb[i][j] = (f - pts[i])    / (pts[i+1] - pts[i]);
      else if (f >= pts[i+1] && f <= pts[i+2]) g_mel_fb[i][j] = (pts[i+2] - f)  / (pts[i+2] - pts[i+1]);
      else                                      g_mel_fb[i][j] = 0.0f;
    }
}

void init_dct() {
  for (int i = 0; i < NUM_MFCC; i++) {
    float sc = (i == 0) ? sqrtf(1.0f / MEL_BINS) : sqrtf(2.0f / MEL_BINS);
    for (int j = 0; j < MEL_BINS; j++)
      g_dct_m[i][j] = sc * cosf(M_PI * i * (j + 0.5f) / MEL_BINS);
  }
}

// ══════════════════════════════════════════════════════════════════════════════
//  FFT (Cooley-Tukey, in-place)
// ══════════════════════════════════════════════════════════════════════════════
static void do_fft(float* re, float* im, int n) {
  int j = 0;
  for (int i = 0; i < n - 1; i++) {
    if (i < j) {
      float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
    }
    int k = n / 2;
    while (k <= j) { j -= k; k /= 2; }
    j += k;
  }
  for (int len = 2; len <= n; len *= 2) {
    float ang = -2.0f * M_PI / len;
    float wr = cosf(ang), wi = sinf(ang);
    for (int i = 0; i < n; i += len) {
      float cr = 1.0f, ci = 0.0f;
      for (int k = 0; k < len / 2; k++) {
        int a = i + k, b = i + k + len / 2;
        float tr = cr * re[b] - ci * im[b], ti = cr * im[b] + ci * re[b];
        re[b] = re[a] - tr; im[b] = im[a] - ti; re[a] += tr; im[a] += ti;
        float tmp = cr; cr = cr * wr - ci * wi; ci = tmp * wi + ci * wr;
      }
    }
  }
}

// ══════════════════════════════════════════════════════════════════════════════
//  MFCC EXTRACTION  (modifies frame[] in-place — pass a copy if needed)
// ══════════════════════════════════════════════════════════════════════════════
static void extract_mfcc(float* frame, float* mfcc) {
  // Pre-emphasis (backward traversal preserves unmodified prev sample)
  for (int i = FRAME_SIZE - 1; i > 0; i--) frame[i] -= PRE_EMPHASIS * frame[i - 1];
  // Hamming window
  for (int i = 0; i < FRAME_SIZE; i++)
    frame[i] *= 0.54f - 0.46f * cosf(2.0f * M_PI * i / (FRAME_SIZE - 1));
  // FFT
  for (int i = 0; i < FRAME_SIZE; i++) { g_fft_re[i] = frame[i]; g_fft_im[i] = 0.0f; }
  for (int i = FRAME_SIZE; i < FFT_SIZE; i++) { g_fft_re[i] = 0.0f; g_fft_im[i] = 0.0f; }
  do_fft(g_fft_re, g_fft_im, FFT_SIZE);
  // Power spectrum
  for (int i = 0; i <= FFT_SIZE / 2; i++)
    g_pspec[i] = g_fft_re[i] * g_fft_re[i] + g_fft_im[i] * g_fft_im[i];
  // Mel filter bank
  for (int i = 0; i < MEL_BINS; i++) {
    g_mel_e[i] = 0.0f;
    for (int j = 0; j <= FFT_SIZE / 2; j++) g_mel_e[i] += g_pspec[j] * g_mel_fb[i][j];
    g_mel_e[i] = logf(g_mel_e[i] + 1e-10f);
  }
  // DCT
  for (int i = 0; i < NUM_MFCC; i++) {
    mfcc[i] = 0.0f;
    for (int j = 0; j < MEL_BINS; j++) mfcc[i] += g_dct_m[i][j] * g_mel_e[j];
  }
}

// ══════════════════════════════════════════════════════════════════════════════
//  I2S INITIALIZATION HELPERS
// ══════════════════════════════════════════════════════════════════════════════

// Configure one I2S port as a stereo RX mic (for direction detection at 44.1 kHz).
static void init_dir_mic(i2s_port_t port, int ws, int bclk, int dout) {
  i2s_driver_uninstall(port);
  i2s_config_t cfg = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate          = DIR_SAMPLE_RATE,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT, // both channels → all 4 mics
    .communication_format = (i2s_comm_format_t)I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count        = 8,
    .dma_buf_len          = 256,
    .use_apll             = false,
    .tx_desc_auto_clear   = false,
    .fixed_mclk           = 0
  };
  i2s_pin_config_t pins = {
    .bck_io_num   = bclk,
    .ws_io_num    = ws,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num  = dout
  };
  esp_err_t e = i2s_driver_install(port, &cfg, 0, NULL);
  if (e != ESP_OK) { Serial.printf("  [I2S] dir_mic install error %d (port %d)\n", e, port); return; }
  i2s_set_pin(port, &pins);
  i2s_zero_dma_buffer(port);
  delay(50);
}

// Configure MIC_TOP_PORT as mono RX at 16 kHz for ML inference (TL mic only).
static void init_ml_mic() {
  i2s_driver_uninstall(MIC_TOP_PORT);
  i2s_config_t cfg = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate          = ML_SAMPLE_RATE,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format       = ML_MIC_CHANNEL,
    .communication_format = (i2s_comm_format_t)I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count        = 8,
    .dma_buf_len          = 256,
    .use_apll             = false,
    .tx_desc_auto_clear   = false,
    .fixed_mclk           = 0
  };
  i2s_pin_config_t pins = {
    .bck_io_num   = MIC_TOP_BCLK,
    .ws_io_num    = MIC_TOP_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num  = MIC_TOP_DOUT
  };
  esp_err_t e = i2s_driver_install(MIC_TOP_PORT, &cfg, 0, NULL);
  if (e != ESP_OK) { Serial.printf("  [I2S] ml_mic install error %d\n", e); return; }
  i2s_set_pin(MIC_TOP_PORT, &pins);
  i2s_zero_dma_buffer(MIC_TOP_PORT);
  delay(50);
}

// Configure SPK_PORT as stereo TX for the MAX98357A speakers.
// Caller must call restore_bot_mic() after playback.
static bool init_speaker(uint32_t sample_rate) {
  i2s_driver_uninstall(SPK_PORT);
  i2s_config_t cfg = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate          = sample_rate,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = (i2s_comm_format_t)I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count        = 8,
    .dma_buf_len          = 512,
    .use_apll             = false,
    .tx_desc_auto_clear   = true,
    .fixed_mclk           = 0
  };
  i2s_pin_config_t pins = {
    .bck_io_num   = SPK_BCLK,
    .ws_io_num    = SPK_LRC,
    .data_out_num = SPK_DIN,
    .data_in_num  = I2S_PIN_NO_CHANGE
  };
  esp_err_t e = i2s_driver_install(SPK_PORT, &cfg, 0, NULL);
  if (e != ESP_OK) { Serial.printf("  [I2S] speaker install error %d\n", e); return false; }
  i2s_set_pin(SPK_PORT, &pins);
  delay(50);  // settling time, matching init_dir_mic()/init_ml_mic()
  return true;
}

// Restore SPK_PORT (I2S_NUM_1) back to BOT mic direction mode.
static void restore_bot_mic() {
  init_dir_mic(MIC_BOT_PORT, MIC_BOT_WS, MIC_BOT_BCLK, MIC_BOT_DOUT);
}

// ══════════════════════════════════════════════════════════════════════════════
//  WAV PLAYBACK  (chunk-scanning parser handles non-standard metadata chunks)
// ══════════════════════════════════════════════════════════════════════════════
static bool scan_wav(File& f, uint16_t& channels, uint32_t& sample_rate,
                     uint16_t& bits, uint32_t& data_bytes) {
  char tag[4];
  uint32_t sz;
  // RIFF header
  if (f.read((uint8_t*)tag, 4) != 4 || memcmp(tag, "RIFF", 4)) return false;
  f.read((uint8_t*)&sz, 4);
  if (f.read((uint8_t*)tag, 4) != 4 || memcmp(tag, "WAVE", 4)) return false;

  bool got_fmt = false, got_data = false;
  while (f.available() && !got_data) {
    if (f.read((uint8_t*)tag, 4) != 4) break;
    if (f.read((uint8_t*)&sz, 4)  != 4) break;
    if (!memcmp(tag, "fmt ", 4)) {
      uint16_t fmt_code;
      f.read((uint8_t*)&fmt_code,  2);
      f.read((uint8_t*)&channels,  2);
      f.read((uint8_t*)&sample_rate, 4);
      f.seek(f.position() + 4); // byte rate
      f.seek(f.position() + 2); // block align
      f.read((uint8_t*)&bits, 2);
      if (sz > 16) f.seek(f.position() + sz - 16); // skip extra fmt bytes
      got_fmt = true;
    } else if (!memcmp(tag, "data", 4)) {
      data_bytes = sz;
      got_data = true;  // file position now at audio data start
    } else {
      // skip unknown chunk (pad to even boundary)
      f.seek(f.position() + sz + (sz & 1));
    }
  }
  return got_fmt && got_data;
}

void play_wav(const char* path) {
  if (!g_fs_ok || !path) return;
  File f = LittleFS.open(path, "r");
  if (!f) { Serial.printf("  [WAV] cannot open %s\n", path); return; }

  uint16_t channels, bits; uint32_t sample_rate, data_bytes;
  if (!scan_wav(f, channels, sample_rate, bits, data_bytes)) {
    Serial.printf("  [WAV] bad header in %s\n", path);
    f.close(); return;
  }
  Serial.printf("  [WAV] %s  %lu Hz  %u ch  %u bit\n", path, sample_rate, channels, bits);

  // Playback below assumes 16-bit stereo unconditionally (matches all 4
  // WAV files currently in data/) -- a file that isn't would play back
  // garbled with no error, since channels/bits are parsed but otherwise
  // unused. Catch that here instead of failing silently.
  if (channels != 2 || bits != 16) {
    Serial.printf("  [WAV] unsupported format (%u ch, %u bit) -- only 16-bit stereo is supported\n",
                  channels, bits);
    f.close(); return;
  }

  // init_speaker() always uninstalls the existing I2S_NUM_1 driver before
  // attempting to install the TX one, even on failure -- so on failure the
  // port is left with no driver installed at all unless restored here.
  if (!init_speaker(sample_rate)) { f.close(); restore_bot_mic(); return; }

  size_t written;
  while (f.available()) {
    size_t n = f.read((uint8_t*)g_spk_buf, sizeof(g_spk_buf));
    if (!n) break;
    i2s_write(SPK_PORT, g_spk_buf, n, &written, portMAX_DELAY);
  }
  i2s_zero_dma_buffer(SPK_PORT);
  f.close();
  restore_bot_mic();
}

// Simple 440 Hz sine tone for speaker hardware test.
void play_tone_440() {
  // Same reasoning as play_wav(): init_speaker() uninstalls the existing
  // I2S_NUM_1 driver even when the subsequent install fails, so restore
  // it here rather than leaving the port with no driver at all.
  if (!init_speaker(ML_SAMPLE_RATE)) { restore_bot_mic(); return; }
  const int total = ML_SAMPLE_RATE * 2; // 2 seconds
  int done = 0;
  while (done < total) {
    int chunk = min(256, total - done);
    for (int i = 0; i < chunk; i++) {
      int16_t s = (int16_t)(18000.0f * sinf(2.0f * M_PI * 440.0f * (done + i) / ML_SAMPLE_RATE));
      g_spk_buf[i * 2]     = s;
      g_spk_buf[i * 2 + 1] = s;
    }
    size_t written;
    i2s_write(SPK_PORT, g_spk_buf, chunk * 4, &written, portMAX_DELAY);
    done += chunk;
  }
  i2s_zero_dma_buffer(SPK_PORT);
  restore_bot_mic();
}

// ══════════════════════════════════════════════════════════════════════════════
//  DIRECTION DETECTION  (GCC-PHAT lag + energy weighting)
// ══════════════════════════════════════════════════════════════════════════════
static float rms_of(float* buf, int n) {
  float s = 0.0f;
  for (int i = 0; i < n; i++) s += buf[i] * buf[i];
  return sqrtf(s / n);
}

// Returns the lag (in samples) at which cross-correlation between a[] and b[] peaks.
// Positive lag → a leads b → sound came from a's side.
//
// No longer normalizes by signal energy: the old norm = sqrt(ea*eb) was the
// same constant for every lag in the search, so dividing every candidate by
// it could never change which lag has the largest acc -- and the divided
// value was never used for anything else (only best_lag is returned). It
// was pure dead computation: two full n-sample energy sums plus a sqrtf(),
// for no effect on the result. Verified by the argmax-invariance argument
// itself (dividing a set of values by the same positive constant preserves
// their relative order), not just by inspection.
static int xcorr_lag(float* a, float* b, int n) {
  float best = -1e30f; int best_lag = 0;
  for (int lag = -MAX_LAG; lag <= MAX_LAG; lag++) {
    float acc = 0.0f;
    int lo = (lag < 0) ? -lag : 0;
    int hi = (lag < 0) ? n    : n - lag;
    for (int i = lo; i < hi; i++) acc += a[i] * b[i + lag];
    if (acc > best) { best = acc; best_lag = lag; }
  }
  return best_lag;
}

// Returns haptic channel (0-3) for the detected direction, or -1 if too quiet.
int detect_direction() {
  size_t br;
  i2s_read(MIC_TOP_PORT, g_top_raw, sizeof(g_top_raw), &br, portMAX_DELAY);
  i2s_read(MIC_BOT_PORT, g_bot_raw, sizeof(g_bot_raw), &br, portMAX_DELAY);

  // Deinterleave: even index = LEFT (SEL→GND), odd = RIGHT (SEL→3V)
  // If channels appear swapped on your hardware, swap the *2 and *2+1 indices.
  for (int i = 0; i < TDOA_FRAMES; i++) {
    g_tl[i] = (float)g_top_raw[i * 2]     / DIR_NORM_DIV;
    g_tr[i] = (float)g_top_raw[i * 2 + 1] / DIR_NORM_DIV;
    g_bl[i] = (float)g_bot_raw[i * 2]     / DIR_NORM_DIV;
    g_br[i] = (float)g_bot_raw[i * 2 + 1] / DIR_NORM_DIV;
  }

  // DC removal
  float mu_tl = 0, mu_tr = 0, mu_bl = 0, mu_br = 0;
  for (int i = 0; i < TDOA_FRAMES; i++) {
    mu_tl += g_tl[i]; mu_tr += g_tr[i]; mu_bl += g_bl[i]; mu_br += g_br[i];
  }
  mu_tl /= TDOA_FRAMES; mu_tr /= TDOA_FRAMES; mu_bl /= TDOA_FRAMES; mu_br /= TDOA_FRAMES;
  for (int i = 0; i < TDOA_FRAMES; i++) {
    g_tl[i] -= mu_tl; g_tr[i] -= mu_tr; g_bl[i] -= mu_bl; g_br[i] -= mu_br;
  }

  float rms_tl = rms_of(g_tl, TDOA_FRAMES);
  float rms_tr = rms_of(g_tr, TDOA_FRAMES);
  float rms_bl = rms_of(g_bl, TDOA_FRAMES);
  float rms_br = rms_of(g_br, TDOA_FRAMES);
  float max_rms = max(max(rms_tl, rms_tr), max(rms_bl, rms_br));

  Serial.printf("  [DIR] RMS  TL=%.4f TR=%.4f BL=%.4f BR=%.4f\n",
                rms_tl, rms_tr, rms_bl, rms_br);
  if (max_rms < TDOA_MIN_RMS) { Serial.println("  [DIR] Too quiet to locate."); return -1; }

  // TDOA cross-correlation: positive lag → sound came from left mic side
  int lag_top = xcorr_lag(g_tl, g_tr, TDOA_FRAMES); // +→TL earlier  -→TR earlier
  int lag_bot = xcorr_lag(g_bl, g_br, TDOA_FRAMES); // +→BL earlier  -→BR earlier
  Serial.printf("  [DIR] lag_top=%d lag_bot=%d\n", lag_top, lag_bot);

  // Top-vs-bottom energy ratio
  float top_e = (rms_tl + rms_tr) * 0.5f;
  float bot_e = (rms_bl + rms_br) * 0.5f;
  float tb    = (top_e - bot_e) / max(top_e + bot_e, 1e-6f); // +1→all top, -1→all bot
  // Symmetric +-0.15 dead-zone, matching the +-2-sample dead-zone used for
  // left/right below -- was tb > 0.0f (any top bias at all, however tiny),
  // which structurally biased the whole decision cascade toward the top
  // quadrants for any tb in (-0.15, 0]: the agree_left/agree_right fallback
  // a few lines down has no equivalent "default to bottom" case, so a
  // dead-even or up-to-15%-bottom-dominant signal always resolved to TL/TR.
  bool is_top = tb >  0.15f;
  bool is_bot = tb < -0.15f;

  // Left/right vote from both lag pairs
  bool left_top  = lag_top >  2;
  bool right_top = lag_top < -2;
  bool left_bot  = lag_bot >  2;
  bool right_bot = lag_bot < -2;
  bool agree_left  = left_top  && left_bot;
  bool agree_right = right_top && right_bot;

  Serial.printf("  [DIR] tb=%.3f is_top=%d is_bot=%d agree_L=%d agree_R=%d\n",
                tb, is_top, is_bot, agree_left, agree_right);

  // Every branch below requires BOTH a left/right read (from lag) AND a
  // confident top/bottom read (from energy) before committing to a
  // quadrant. Previously, several branches (agree_left/agree_right, and
  // left_top/right_top alone) fell back to the TOP quadrant whenever
  // top/bottom was ambiguous, and left_bot/right_bot alone committed to
  // BOTTOM without ever consulting is_top/is_bot at all -- a structural
  // bias toward top, not just a threshold tuning issue (confirmed by
  // simulating the old cascade: it gave the same top-biased split
  // regardless of where is_top's threshold was set, since the bias lived
  // in these fallback branches, not in the threshold value). Genuinely
  // ambiguous cases (left/right known, top/bottom not, or neither) now
  // fall through to the loudest-single-mic tiebreaker below instead of
  // guessing a quadrant.
  int quad = -1;
  // Strongest evidence: both mic pairs agree on left/right.
  if      (agree_left  && is_top) quad = HAPTIC_TL;
  else if (agree_left  && is_bot) quad = HAPTIC_BL;
  else if (agree_right && is_top) quad = HAPTIC_TR;
  else if (agree_right && is_bot) quad = HAPTIC_BR;
  // Weaker evidence: only one mic pair's lag gives a left/right read.
  // Treated symmetrically regardless of whether that pair is the top or
  // bottom one -- lag decides left/right, energy decides top/bottom,
  // independently of which pair happened to produce the usable lag.
  else if (left_top  && is_top)   quad = HAPTIC_TL;
  else if (left_top  && is_bot)   quad = HAPTIC_BL;
  else if (right_top && is_top)   quad = HAPTIC_TR;
  else if (right_top && is_bot)   quad = HAPTIC_BR;
  else if (left_bot  && is_top)   quad = HAPTIC_TL;
  else if (left_bot  && is_bot)   quad = HAPTIC_BL;
  else if (right_bot && is_top)   quad = HAPTIC_TR;
  else if (right_bot && is_bot)   quad = HAPTIC_BR;

  if (quad < 0) {
    // Fallback: loudest mic wins -- covers every case above where
    // left/right and/or top/bottom couldn't be confidently resolved.
    float rms_all[4] = { rms_tl, rms_tr, rms_bl, rms_br };
    int map[4] = { HAPTIC_TL, HAPTIC_TR, HAPTIC_BL, HAPTIC_BR };
    int best = 0;
    for (int i = 1; i < 4; i++) if (rms_all[i] > rms_all[best]) best = i;
    quad = map[best];
    Serial.printf("  [DIR] Fallback to loudest mic %d\n", best);
  }

  const char* qlabels[4] = { "Top-Left", "Top-Right", "Bot-Left", "Bot-Right" };
  Serial.printf("  [DIR] Quadrant → %s (motor %d)\n", qlabels[quad], quad);
  return quad;
}

// ══════════════════════════════════════════════════════════════════════════════
//  MLP INFERENCE
// ══════════════════════════════════════════════════════════════════════════════
static void relu    (float* d, int n) { for (int i=0;i<n;i++) if(d[i]<0) d[i]=0; }
static void softmax (float* d, int n) {
  float mx = d[0]; for (int i=1;i<n;i++) if(d[i]>mx) mx=d[i];
  float s  = 0;    for (int i=0;i<n;i++) { d[i]=expf(d[i]-mx); s+=d[i]; }
  for (int i=0;i<n;i++) d[i]/=s;
}
static void dense(const float* in, int ni, const float* W, const float* b, float* out, int no) {
  for (int j=0;j<no;j++) {
    out[j] = b[j];
    for (int i=0;i<ni;i++) out[j] += in[i] * W[i*no+j];
  }
}

int run_inference() {
  for (int i=0;i<NUM_FRAMES;i++)
    for (int j=0;j<NUM_MFCC;j++)
      g_feat[i*NUM_MFCC+j] = (g_mfcc[i][j] - SCALER_MEAN[i*NUM_MFCC+j]) / SCALER_SCALE[i*NUM_MFCC+j];
  dense(g_feat, LAYER0_INPUT, LAYER0_WEIGHTS, LAYER0_BIAS, g_l1, LAYER0_OUTPUT); relu(g_l1, LAYER0_OUTPUT);
  dense(g_l1,   LAYER2_INPUT, LAYER2_WEIGHTS, LAYER2_BIAS, g_l2, LAYER2_OUTPUT); relu(g_l2, LAYER2_OUTPUT);
  dense(g_l2,   LAYER4_INPUT, LAYER4_WEIGHTS, LAYER4_BIAS, g_probs, LAYER4_OUTPUT); softmax(g_probs, NUM_CLASSES);
  int best = 0;
  for (int i=1;i<NUM_CLASSES;i++) if(g_probs[i]>g_probs[best]) best=i;
  return best;
}

// ══════════════════════════════════════════════════════════════════════════════
//  MAIN PIPELINE
// ══════════════════════════════════════════════════════════════════════════════
void capture_and_classify() {
  Serial.println("─── Waiting for sound ───");

  // Phase 1: Energy-triggered ML capture.
  // Wait for a frame whose RMS exceeds CAPTURE_TRIGGER_RMS, then start the
  // 31-frame window from that frame so transient sounds (gunshots) are aligned
  // to the window start, matching how the training data was collected.
  size_t br;

  // Flush stale DMA samples accumulated while the system was busy.
  for (int i = 0; i < 4; i++)
    i2s_read(MIC_TOP_PORT, g_ml_raw, FRAME_SIZE * sizeof(int32_t), &br, portMAX_DELAY);

  unsigned long t0 = millis();
  unsigned long deadline = t0 + CAPTURE_TIMEOUT_MS;
  bool triggered = false;

  while (millis() < deadline) {
    i2s_read(MIC_TOP_PORT, g_ml_raw, FRAME_SIZE * sizeof(int32_t), &br, portMAX_DELAY);
    float rms = 0.0f;
    for (int i = 0; i < FRAME_SIZE; i++) {
      float s = (float)g_ml_raw[i] / ML_NORM_DIV;
      rms += s * s;
    }
    if (sqrtf(rms / FRAME_SIZE) >= CAPTURE_TRIGGER_RMS) {
      // Sound onset detected — use this frame as mfcc[0]
      for (int i = 0; i < FRAME_SIZE; i++) g_frame[i] = (float)g_ml_raw[i] / ML_NORM_DIV;
      extract_mfcc(g_frame, g_mfcc[0]);
      Serial.printf("  [trigger] onset RMS=%.4f\n", sqrtf(rms / FRAME_SIZE));
      triggered = true;
      break;
    }
  }
  if (!triggered) Serial.println("  [trigger] timeout — classifying background");

  // Capture the rest of the 31-frame window after the onset frame.
  int start = triggered ? 1 : 0;
  for (int f = start; f < NUM_FRAMES; f++) {
    i2s_read(MIC_TOP_PORT, g_ml_raw, FRAME_SIZE * sizeof(int32_t), &br, portMAX_DELAY);
    for (int i = 0; i < FRAME_SIZE; i++) g_frame[i] = (float)g_ml_raw[i] / ML_NORM_DIV;
    extract_mfcc(g_frame, g_mfcc[f]);
  }
  unsigned long cap_ms = millis() - t0;

  t0 = millis();
  int pred = run_inference();
  unsigned long inf_ms = millis() - t0;

  float conf = g_probs[pred];
  Serial.printf("  cap=%lums  inf=%lums\n", cap_ms, inf_ms);

  if (conf >= CONFIDENCE_THR) {
    Serial.printf("  DETECTED: %s  (%.1f%%)\n", CLASS_NAMES[pred], conf * 100.0f);
  } else {
    Serial.printf("  UNCERTAIN: %s  (%.1f%%)\n", CLASS_NAMES[pred], conf * 100.0f);
  }
  for (int i = 0; i < NUM_CLASSES; i++)
    Serial.printf("    %-10s %.1f%%\n", CLASS_NAMES[i], g_probs[i] * 100.0f);

  // Phase 2: Direction — switch both ports to 44.1 kHz stereo
  init_dir_mic(MIC_TOP_PORT, MIC_TOP_WS, MIC_TOP_BCLK, MIC_TOP_DOUT);
  init_dir_mic(MIC_BOT_PORT, MIC_BOT_WS, MIC_BOT_BCLK, MIC_BOT_DOUT);
  int quad = detect_direction();

  // Phase 3: Alert (skip for noise class or low confidence)
  if (conf >= CONFIDENCE_THR && pred != 1) {
    Serial.printf("  >>> ALERT: %s\n", CLASS_NAMES[pred]);
    if (quad >= 0) {
      fire_haptic(quad);
      delay(150);
      fire_haptic(quad);  // double pulse for emphasis
    }
    play_wav(WAV_FILES[pred]);  // play_wav() restores BOT mic internally
  } else {
    Serial.println("  (noise / low confidence — no alert)");
    restore_bot_mic();
  }

  // Restore MIC_TOP to ML mode for the next capture cycle
  init_ml_mic();
  Serial.println("─────────────────");
}

// ══════════════════════════════════════════════════════════════════════════════
//  SETUP / LOOP
// ══════════════════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  {
    unsigned long t = millis();
    while (!Serial && millis() - t < 3000) delay(10);
  }
  delay(200);

  Serial.println("\n========================================");
  Serial.println("  EchoSafe Full System");
  Serial.println("  4-Mic | 2-Speaker | 4-Haptic");
  Serial.println("  MLP 403→128→64→5");
  Serial.println("========================================\n");

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);

  // LittleFS for WAV files
  Serial.print("LittleFS... ");
  if (LittleFS.begin(true)) {
    g_fs_ok = true;
    Serial.println("OK");
    File root = LittleFS.open("/");
    for (File f = root.openNextFile(); f; f = root.openNextFile())
      Serial.printf("  /%s (%u B)\n", f.name(), f.size());
  } else {
    Serial.println("FAILED (WAV alerts disabled — upload data/ folder via LittleFS plugin)");
  }

  init_haptics();

  Serial.print("Mel filters + DCT... ");
  init_mel_filters();
  init_dct();
  Serial.println("OK");

  Serial.print("MIC_TOP (ML 16 kHz, TL only)... ");
  init_ml_mic();
  Serial.println("OK");

  Serial.print("MIC_BOT (DIR 44.1 kHz, stereo)... ");
  init_dir_mic(MIC_BOT_PORT, MIC_BOT_WS, MIC_BOT_BCLK, MIC_BOT_DOUT);
  Serial.println("OK");

  Serial.println("\nCommands: i=inference  r=continuous  s=stop");
  Serial.println("          h=haptic_test  d=direction_test  t=tone_test  p=speaker_test\n");
  Serial.println("Auto-starting continuous mode...\n");
  g_continuous = true;
}

void loop() {
  if (Serial.available()) {
    char cmd = Serial.read();
    switch (cmd) {
      case 'i': capture_and_classify(); break;
      case 'r': g_continuous = true;  Serial.println("\n[Continuous STARTED]"); break;
      case 's': g_continuous = false; Serial.println("\n[Continuous STOPPED]"); break;
      case 'h': Serial.println("\n[Haptic test]"); haptic_test_all(); break;
      case 'd':
        Serial.println("\n[Direction test]");
        init_dir_mic(MIC_TOP_PORT, MIC_TOP_WS, MIC_TOP_BCLK, MIC_TOP_DOUT);
        init_dir_mic(MIC_BOT_PORT, MIC_BOT_WS, MIC_BOT_BCLK, MIC_BOT_DOUT);
        { int q = detect_direction(); if (q >= 0) fire_haptic(q); }
        init_ml_mic(); // restore ML mode on top port
        break;
      case 't':
        Serial.println("\n[Tone test 440 Hz / 2 s]");
        play_tone_440();
        Serial.println("  Done.");
        break;
      case 'p':
        Serial.println("\n[Speaker test — all WAVs]");
        for (int i = 0; i < NUM_CLASSES; i++) {
          Serial.printf("  '%s': ", CLASS_NAMES[i]);
          if (WAV_FILES[i]) play_wav(WAV_FILES[i]);
          else              Serial.println("(no file)");
          delay(400);
        }
        init_ml_mic();
        break;
    }
  }
  if (g_continuous) { capture_and_classify(); delay(300); }
  delay(10);
}
