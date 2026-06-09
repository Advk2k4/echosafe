/*
 * EchoSafe Integrated Firmware
 * ESP32-S3-WROOM-1 NZN16R8
 *
 * TOP pair  (I2S_NUM_0): WS=GPIO4  BCLK=GPIO5  DOUT=GPIO16  TL:SEL=GND  TR:SEL=3V3
 * BOT pair  (I2S_NUM_1): WS=GPIO12 BCLK=GPIO21 DOUT=GPIO18  BL:SEL=GND  BR:SEL=3V3
 * Speaker   (I2S_NUM_1, shared): DIN=GPIO47  BCLK=GPIO48  LRC=GPIO45
 * Haptics   (TCA9548A): SDA=GPIO9  SCL=GPIO11  addr=0x70
 * Commands: i=inference  r=continuous  s=stop  h=haptic test  d=direction  t=tone  p=speaker
 *
 * ML Inference:  16000 Hz, ONLY_LEFT,  MIC_TOP_PORT (I2S_NUM_0)
 * Direction TDOA: 44100 Hz, RIGHT_LEFT, both ports
 * Classes: horns=0  noise=1  bells=2  gunshots=3  sirens=4
 */

#include <driver/i2s.h>
#include <math.h>
#include <Wire.h>
#include <Adafruit_DRV2605.h>
#include "model_weights.h"
#include "FS.h"
#include "LittleFS.h"

// ── ML INFERENCE PARAMS ───────────────────────────────────────────────────────
#define ML_SAMPLE_RATE    16000
#define ML_NORM_DIV       2147483648.0f

// ── DIRECTION DETECTION PARAMS ────────────────────────────────────────────────
#define DIR_SAMPLE_RATE   44100
#define DIR_SAMPLE_SHIFT  8
#define DIR_NORM_DIV      8388608.0f

// ── SHARED MFCC / FFT PARAMS ──────────────────────────────────────────────────
#define FRAME_SIZE        512
#define FRAME_STEP        256
#define NUM_MFCC          13
#define NUM_FRAMES        31
#define FFT_SIZE          512
#define MEL_BINS          26
#define PRE_EMPHASIS      0.97f
#define MEL_MIN_FREQ      0.0f
#define MEL_MAX_FREQ      8000.0f
#define CONFIDENCE_THR    0.65f

#define TDOA_FRAMES       1024
#define MAX_LAG           24
#define TDOA_MIN_RMS      0.0005f

// ── PINS (UNCHANGED) ──────────────────────────────────────────────────────────
#define MIC_TOP_WS        4
#define MIC_TOP_BCLK      5
#define MIC_TOP_DOUT      16
#define MIC_TOP_PORT      I2S_NUM_0

#define MIC_BOT_WS        12
#define MIC_BOT_BCLK      21
#define MIC_BOT_DOUT      18
#define MIC_BOT_PORT      I2S_NUM_1

#define SPK_DIN           47
#define SPK_BCLK          48
#define SPK_LRC           45
#define SPK_PORT          I2S_NUM_1

#define I2C_SDA           9
#define I2C_SCL           11
#define TCA_ADDR          0x70
#define NUM_HAPTIC        4
#define HAPTIC_TL         0
#define HAPTIC_TR         1
#define HAPTIC_BL         2
#define HAPTIC_BR         3
#define HAPTIC_EFFECT     47
#define HAPTIC_DURATION   500

// ── WAV FILES (horns=0, noise=1, bells=2, gunshots=3, sirens=4) ───────────────
const char* WAV_FILES[] = {
  "/vehiclehorn.wav",
  NULL,
  "/bicyclebell.wav",
  "/gun_shot.wav",
  "/emergencysiren.wav"
};

struct WavHeader {
  char     riff[4];
  uint32_t fileSize;
  char     wave[4];
  char     fmt[4];
  uint32_t fmtLength;
  uint16_t audioFormat;
  uint16_t numChannels;
  uint32_t sampleRate;
  uint32_t byteRate;
  uint16_t blockAlign;
  uint16_t bitsPerSample;
  char     data[4];
  uint32_t dataSize;
};

// ── GLOBAL BUFFERS ────────────────────────────────────────────────────────────
float frame_buffer[FRAME_SIZE];
float mfcc_features[NUM_FRAMES][NUM_MFCC];
int32_t ml_i2s_buf[FRAME_SIZE];
float fft_real[FFT_SIZE];
float fft_imag[FFT_SIZE];
float power_spectrum[FFT_SIZE / 2 + 1];
float mel_energies[MEL_BINS];
float mel_filters[MEL_BINS][FFT_SIZE / 2 + 1];
float dct_matrix[NUM_MFCC][MEL_BINS];
float input_features[INPUT_DIM];
float layer1_output[128];
float layer2_output[64];

int32_t tdoa_top_raw[TDOA_FRAMES * 2];
int32_t tdoa_bot_raw[TDOA_FRAMES * 2];
float tl_buf[TDOA_FRAMES], tr_buf[TDOA_FRAMES];
float bl_buf[TDOA_FRAMES], br_buf[TDOA_FRAMES];

int16_t audio_buf[512];

Adafruit_DRV2605 drv;

bool continuous_mode = false;
bool fs_ok           = false;
bool speaker_ok      = false;
bool haptic_ok[NUM_HAPTIC] = { false, false, false, false };

// ── I2C MUX ───────────────────────────────────────────────────────────────────
void tca_select(uint8_t ch) {
  Wire.beginTransmission(TCA_ADDR);
  Wire.write(1 << ch);
  Wire.endTransmission();
  delay(20);
}

void tca_deselect() {
  Wire.beginTransmission(TCA_ADDR);
  Wire.write(0x00);
  Wire.endTransmission();
  delay(10);
}

// ── HAPTICS ───────────────────────────────────────────────────────────────────
void init_haptics() {
  tca_deselect();
  Serial.println("Initializing haptic motors...");
  for (int ch = 0; ch < NUM_HAPTIC; ch++) {
    Serial.printf("  Ch%d: ", ch);
    tca_select(ch);
    if (drv.begin()) {
      drv.selectLibrary(1);
      drv.setMode(DRV2605_MODE_INTTRIG);
      drv.setWaveform(0, HAPTIC_EFFECT);
      drv.setWaveform(1, 0);
      haptic_ok[ch] = true;
      Serial.println("OK");
    } else {
      haptic_ok[ch] = false;
      Serial.println("FAILED");
    }
    tca_deselect();
  }
}

void fire_haptic(int ch) {
  if (ch < 0 || ch >= NUM_HAPTIC || !haptic_ok[ch]) return;
  tca_select(ch);
  drv.go();
  tca_deselect();
  delay(100);
  tca_select(ch);
  drv.go();
  tca_deselect();
  Serial.printf("  [haptic] Motor %d fired\n", ch);
}

void haptic_test_all() {
  const char* labels[] = { "Top-Left", "Top-Right", "Bottom-Left", "Bottom-Right" };
  for (int i = 0; i < NUM_HAPTIC; i++) {
    Serial.printf("  Motor %d (%s)\n", i, labels[i]);
    fire_haptic(i);
    delay(HAPTIC_DURATION);
  }
}

// ── MEL / DCT (use ML_SAMPLE_RATE — must match training) ─────────────────────
float hz_to_mel(float hz) { return 2595.0f * log10f(1.0f + hz / 700.0f); }
float mel_to_hz(float mel) { return 700.0f * (powf(10.0f, mel / 2595.0f) - 1.0f); }

void init_mel_filters() {
  float mel_min  = hz_to_mel(MEL_MIN_FREQ);
  float mel_max  = hz_to_mel(MEL_MAX_FREQ);
  float mel_step = (mel_max - mel_min) / (MEL_BINS + 1);
  float mel_points[MEL_BINS + 2];
  for (int i = 0; i < MEL_BINS + 2; i++)
    mel_points[i] = mel_to_hz(mel_min + i * mel_step);
  for (int i = 0; i < MEL_BINS; i++) {
    for (int j = 0; j <= FFT_SIZE / 2; j++) {
      float freq = (float)j * ML_SAMPLE_RATE / FFT_SIZE;
      if      (freq >= mel_points[i]   && freq <= mel_points[i+1])
        mel_filters[i][j] = (freq - mel_points[i]) / (mel_points[i+1] - mel_points[i]);
      else if (freq >= mel_points[i+1] && freq <= mel_points[i+2])
        mel_filters[i][j] = (mel_points[i+2] - freq) / (mel_points[i+2] - mel_points[i+1]);
      else
        mel_filters[i][j] = 0.0f;
    }
  }
}

void init_dct_matrix() {
  for (int i = 0; i < NUM_MFCC; i++)
    for (int j = 0; j < MEL_BINS; j++) {
      dct_matrix[i][j] = cosf(M_PI * i * (j + 0.5f) / MEL_BINS);
      dct_matrix[i][j] *= (i == 0) ? sqrtf(1.0f / MEL_BINS) : sqrtf(2.0f / MEL_BINS);
    }
}

// ── FFT ───────────────────────────────────────────────────────────────────────
void fft(float* real, float* imag, int n) {
  int j = 0;
  for (int i = 0; i < n - 1; i++) {
    if (i < j) {
      float t = real[i]; real[i] = real[j]; real[j] = t;
            t = imag[i]; imag[i] = imag[j]; imag[j] = t;
    }
    int k = n / 2;
    while (k <= j) { j -= k; k /= 2; }
    j += k;
  }
  for (int len = 2; len <= n; len *= 2) {
    float angle  = -2.0f * M_PI / len;
    float wlen_r = cosf(angle), wlen_i = sinf(angle);
    for (int i = 0; i < n; i += len) {
      float wr = 1.0f, wi = 0.0f;
      for (int jj = 0; jj < len / 2; jj++) {
        int a = i + jj, b = i + jj + len / 2;
        float tr = wr * real[b] - wi * imag[b];
        float ti = wr * imag[b] + wi * real[b];
        real[b] = real[a] - tr; imag[b] = imag[a] - ti;
        real[a] += tr;          imag[a] += ti;
        float tw = wr;
        wr = wr * wlen_r - wi * wlen_i;
        wi = tw * wlen_i + wi * wlen_r;
      }
    }
  }
}

// ── MFCC ──────────────────────────────────────────────────────────────────────
void apply_hamming(float* buf, int n) {
  for (int i = 0; i < n; i++)
    buf[i] *= 0.54f - 0.46f * cosf(2.0f * M_PI * i / (n - 1));
}

void extract_mfcc_frame(float* frame, float* mfcc) {
  for (int i = FRAME_SIZE - 1; i > 0; i--)
    frame[i] -= PRE_EMPHASIS * frame[i - 1];
  apply_hamming(frame, FRAME_SIZE);
  for (int i = 0; i < FRAME_SIZE; i++) { fft_real[i] = frame[i]; fft_imag[i] = 0.0f; }
  for (int i = FRAME_SIZE; i < FFT_SIZE; i++) { fft_real[i] = 0.0f; fft_imag[i] = 0.0f; }
  fft(fft_real, fft_imag, FFT_SIZE);
  for (int i = 0; i <= FFT_SIZE / 2; i++)
    power_spectrum[i] = fft_real[i]*fft_real[i] + fft_imag[i]*fft_imag[i];
  for (int i = 0; i < MEL_BINS; i++) {
    mel_energies[i] = 0.0f;
    for (int j = 0; j <= FFT_SIZE / 2; j++)
      mel_energies[i] += power_spectrum[j] * mel_filters[i][j];
    mel_energies[i] = logf(mel_energies[i] + 1e-10f);
  }
  for (int i = 0; i < NUM_MFCC; i++) {
    mfcc[i] = 0.0f;
    for (int j = 0; j < MEL_BINS; j++)
      mfcc[i] += dct_matrix[i][j] * mel_energies[j];
  }
}

// ── ML MIC I2S — 16000 Hz, ONLY_LEFT, MIC_TOP_PORT ───────────────────────────
void init_ml_mic_i2s() {
  i2s_driver_uninstall(MIC_TOP_PORT);
  i2s_config_t cfg = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate          = ML_SAMPLE_RATE,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
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
  if (e != ESP_OK) { Serial.printf("  ERROR ML mic I2S install: %d\n", e); return; }
  e = i2s_set_pin(MIC_TOP_PORT, &pins);
  if (e != ESP_OK) { Serial.printf("  ERROR ML mic I2S pin: %d\n", e); return; }
  i2s_zero_dma_buffer(MIC_TOP_PORT);
  delay(50);
}

void read_ml_frame(float* buf) {
  size_t bytes_read = 0;
  i2s_read(MIC_TOP_PORT, ml_i2s_buf, FRAME_SIZE * sizeof(int32_t), &bytes_read, portMAX_DELAY);
  for (int i = 0; i < FRAME_SIZE; i++)
    buf[i] = (float)ml_i2s_buf[i] / ML_NORM_DIV;
}

// ── DIRECTION MIC I2S — 44100 Hz, RIGHT_LEFT ─────────────────────────────────
void init_dir_mic_i2s(i2s_port_t port, int ws, int bclk, int dout) {
  i2s_driver_uninstall(port);
  i2s_config_t cfg = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate          = DIR_SAMPLE_RATE,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S,
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
  if (e != ESP_OK) { Serial.printf("  ERROR dir mic I2S install (port %d): %d\n", port, e); return; }
  e = i2s_set_pin(port, &pins);
  if (e != ESP_OK) { Serial.printf("  ERROR dir mic I2S pin (port %d): %d\n", port, e); return; }
  i2s_zero_dma_buffer(port);
  delay(100);
}

// ── SPEAKER ───────────────────────────────────────────────────────────────────
void init_speaker_i2s(uint32_t sample_rate = DIR_SAMPLE_RATE) {
  i2s_driver_uninstall(SPK_PORT);
  i2s_config_t cfg = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate          = sample_rate,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
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
  if (e != ESP_OK) { Serial.printf("  ERROR speaker I2S: %d\n", e); speaker_ok = false; return; }
  i2s_set_pin(SPK_PORT, &pins);
  i2s_zero_dma_buffer(SPK_PORT);
  speaker_ok = true;
}

void play_wav(const char* filename) {
  if (!fs_ok || !filename) return;
  File f = LittleFS.open(filename, "r");
  if (!f) { Serial.printf("  ERROR: cannot open %s\n", filename); return; }
  WavHeader header;
  if (f.read((uint8_t*)&header, sizeof(WavHeader)) != sizeof(WavHeader)) {
    Serial.println("  ERROR: bad WAV header"); f.close(); return;
  }
  Serial.printf("  Playing %s (%lu Hz, %uch, %ubit)\n",
                filename, header.sampleRate, header.numChannels, header.bitsPerSample);
  init_speaker_i2s(header.sampleRate);
  size_t bw;
  while (f.available()) {
    size_t br = f.read((uint8_t*)audio_buf, sizeof(audio_buf));
    if (!br) break;
    i2s_write(SPK_PORT, audio_buf, br, &bw, portMAX_DELAY);
  }
  i2s_zero_dma_buffer(SPK_PORT);
  f.close();
  // SPK_PORT == MIC_BOT_PORT (I2S_NUM_1) — restore direction mic after playback
  init_dir_mic_i2s(MIC_BOT_PORT, MIC_BOT_WS, MIC_BOT_BCLK, MIC_BOT_DOUT);
}

// ── DIRECTION DETECTION ───────────────────────────────────────────────────────
static inline float rms(float* buf, int n) {
  float s = 0.0f;
  for (int i = 0; i < n; i++) s += buf[i] * buf[i];
  return sqrtf(s / n);
}

int xcorr_lag(float* a, float* b, int n) {
  float ea = 0.0f, eb = 0.0f;
  for (int i = 0; i < n; i++) { ea += a[i]*a[i]; eb += b[i]*b[i]; }
  float norm = sqrtf(ea * eb) + 1e-12f;
  float best = -1e30f; int best_lag = 0;
  for (int lag = -MAX_LAG; lag <= MAX_LAG; lag++) {
    float acc = 0.0f;
    int lo = (lag < 0) ? -lag : 0;
    int hi = (lag < 0) ? n    : n - lag;
    for (int i = lo; i < hi; i++) acc += a[i] * b[i + lag];
    acc /= norm;
    if (acc > best) { best = acc; best_lag = lag; }
  }
  return best_lag;
}

int detect_direction() {
  size_t br;
  i2s_read(MIC_TOP_PORT, tdoa_top_raw, sizeof(tdoa_top_raw), &br, portMAX_DELAY);
  i2s_read(MIC_BOT_PORT, tdoa_bot_raw, sizeof(tdoa_bot_raw), &br, portMAX_DELAY);

  for (int i = 0; i < TDOA_FRAMES; i++) {
    tl_buf[i] = (float)(tdoa_top_raw[i*2]   >> DIR_SAMPLE_SHIFT) / DIR_NORM_DIV;
    tr_buf[i] = (float)(tdoa_top_raw[i*2+1] >> DIR_SAMPLE_SHIFT) / DIR_NORM_DIV;
    bl_buf[i] = (float)(tdoa_bot_raw[i*2]   >> DIR_SAMPLE_SHIFT) / DIR_NORM_DIV;
    br_buf[i] = (float)(tdoa_bot_raw[i*2+1] >> DIR_SAMPLE_SHIFT) / DIR_NORM_DIV;
  }

  float sum_tl=0, sum_tr=0, sum_bl=0, sum_br=0;
  for (int i = 0; i < TDOA_FRAMES; i++) {
    sum_tl += tl_buf[i]; sum_tr += tr_buf[i];
    sum_bl += bl_buf[i]; sum_br += br_buf[i];
  }
  for (int i = 0; i < TDOA_FRAMES; i++) {
    tl_buf[i] -= sum_tl/TDOA_FRAMES; tr_buf[i] -= sum_tr/TDOA_FRAMES;
    bl_buf[i] -= sum_bl/TDOA_FRAMES; br_buf[i] -= sum_br/TDOA_FRAMES;
  }

  float rms_tl = rms(tl_buf, TDOA_FRAMES);
  float rms_tr = rms(tr_buf, TDOA_FRAMES);
  float rms_bl = rms(bl_buf, TDOA_FRAMES);
  float rms_br = rms(br_buf, TDOA_FRAMES);

  float max_rms = max(max(rms_tl, rms_tr), max(rms_bl, rms_br));
  Serial.printf("  [TDOA] RMS TL=%.4f TR=%.4f BL=%.4f BR=%.4f\n", rms_tl, rms_tr, rms_bl, rms_br);
  if (max_rms < TDOA_MIN_RMS) { Serial.println("  [TDOA] Too quiet"); return -1; }

  int lag_top = xcorr_lag(tl_buf, tr_buf, TDOA_FRAMES);
  int lag_bot = xcorr_lag(bl_buf, br_buf, TDOA_FRAMES);
  Serial.printf("  [TDOA] lag_top=%d lag_bot=%d\n", lag_top, lag_bot);

  bool top_left  = lag_top >  2;
  bool top_right = lag_top < -2;
  bool bot_left  = lag_bot >  2;
  bool bot_right = lag_bot < -2;

  bool agree_left  = top_left  && bot_left;
  bool agree_right = top_right && bot_right;

  float top_energy = (rms_tl + rms_tr) / 2.0f;
  float bot_energy = (rms_bl + rms_br) / 2.0f;
  float tb_diff    = (top_energy - bot_energy) / max(top_energy, bot_energy);
  bool is_top      = tb_diff >  0.00f;
  bool is_bottom   = tb_diff < -0.15f;

  Serial.printf("  [TDOA] tb_diff=%.3f is_top=%d is_bottom=%d agree_left=%d agree_right=%d\n",
                tb_diff, is_top, is_bottom, agree_left, agree_right);

  int quadrant;
  if      (agree_left  && is_top)    quadrant = HAPTIC_TL;
  else if (agree_left  && is_bottom) quadrant = HAPTIC_BL;
  else if (agree_right && is_top)    quadrant = HAPTIC_TR;
  else if (agree_right && is_bottom) quadrant = HAPTIC_BR;
  else if (agree_left)               quadrant = HAPTIC_TL;
  else if (agree_right)              quadrant = HAPTIC_TR;
  else if (top_right && is_top)      quadrant = HAPTIC_TR;
  else if (top_right && is_bottom)   quadrant = HAPTIC_BR;
  else if (top_right)                quadrant = HAPTIC_TR;
  else if (top_left  && is_top)      quadrant = HAPTIC_TL;
  else if (top_left  && is_bottom)   quadrant = HAPTIC_BL;
  else if (top_left)                 quadrant = HAPTIC_TL;
  else if (bot_right)                quadrant = HAPTIC_BR;
  else if (bot_left)                 quadrant = HAPTIC_BL;
  else {
    float rms_vals[4] = { rms_tl, rms_tr, rms_bl, rms_br };
    int loudest = 0;
    for (int i = 1; i < 4; i++) if (rms_vals[i] > rms_vals[loudest]) loudest = i;
    int map[4] = { HAPTIC_TL, HAPTIC_TR, HAPTIC_BL, HAPTIC_BR };
    quadrant = map[loudest];
    Serial.printf("  [TDOA] Fallback to loudest mic %d\n", loudest);
  }

  float mic_angles[4] = { 45.0f, 135.0f, 225.0f, 315.0f };
  float mic_rms[4]    = { rms_tr, rms_tl, rms_bl, rms_br };
  float sum_sin = 0.0f, sum_cos = 0.0f, sum_w = 0.0f;
  for (int i = 0; i < 4; i++) {
    float w = mic_rms[i] * mic_rms[i];
    sum_sin += w * sinf(mic_angles[i] * M_PI / 180.0f);
    sum_cos += w * cosf(mic_angles[i] * M_PI / 180.0f);
    sum_w   += w;
  }
  float azimuth = atan2f(sum_sin / sum_w, sum_cos / sum_w) * 180.0f / M_PI;
  if (azimuth < 0) azimuth += 360.0f;

  const char* labels[] = { "Top-Left", "Top-Right", "Bottom-Left", "Bottom-Right" };
  Serial.printf("  [TDOA] Quadrant: %s (motor %d)\n", labels[quadrant], quadrant);
  Serial.printf("  [TDOA] Azimuth: %.1f deg\n", azimuth);
  return quadrant;
}

// ── NEURAL NETWORK ────────────────────────────────────────────────────────────
void relu(float* d, int n)    { for (int i=0;i<n;i++) if(d[i]<0) d[i]=0; }
void softmax(float* d, int n) {
  float mx=d[0]; for(int i=1;i<n;i++) if(d[i]>mx) mx=d[i];
  float s=0; for(int i=0;i<n;i++){d[i]=expf(d[i]-mx); s+=d[i];}
  for(int i=0;i<n;i++) d[i]/=s;
}
void dense(const float* in, int ni, const float* W, const float* b, float* out, int no) {
  for(int j=0;j<no;j++){
    out[j]=b[j];
    for(int i=0;i<ni;i++) out[j]+=in[i]*W[i*no+j];
  }
}
void standardize(float* f, int n) {
  for(int i=0;i<n;i++) f[i]=(f[i]-SCALER_MEAN[i])/SCALER_SCALE[i];
}

int run_inference(float* confidence) {
  for(int i=0;i<NUM_FRAMES;i++)
    for(int j=0;j<NUM_MFCC;j++)
      input_features[i*NUM_MFCC+j]=mfcc_features[i][j];
  standardize(input_features, INPUT_DIM);
  dense(input_features, LAYER0_INPUT, LAYER0_WEIGHTS, LAYER0_BIAS, layer1_output, LAYER0_OUTPUT);
  relu(layer1_output, LAYER0_OUTPUT);
  dense(layer1_output, LAYER2_INPUT, LAYER2_WEIGHTS, LAYER2_BIAS, layer2_output, LAYER2_OUTPUT);
  relu(layer2_output, LAYER2_OUTPUT);
  dense(layer2_output, LAYER4_INPUT, LAYER4_WEIGHTS, LAYER4_BIAS, confidence, LAYER4_OUTPUT);
  softmax(confidence, NUM_CLASSES);
  int best=0;
  for(int i=1;i<NUM_CLASSES;i++) if(confidence[i]>confidence[best]) best=i;
  return best;
}

// ── MAIN PIPELINE ─────────────────────────────────────────────────────────────
void capture_and_classify() {
  Serial.println("--- Listening ---");

  // ML capture: MIC_TOP_PORT is already in 16000Hz ONLY_LEFT mode from setup or
  // the end of the previous cycle — no reinit here to avoid the dead window.
  unsigned long t0 = millis();
  for (int i = 0; i < NUM_FRAMES; i++) {
    read_ml_frame(frame_buffer);
    extract_mfcc_frame(frame_buffer, mfcc_features[i]);
  }
  Serial.printf("  Capture: %lu ms\n", millis() - t0);

  t0 = millis();
  float confidence[NUM_CLASSES];
  int pred = run_inference(confidence);
  Serial.printf("  Inference: %lu ms\n", millis() - t0);
  float best_conf = confidence[pred];
  Serial.printf("  RESULT: %s (%.1f%%)\n", CLASS_NAMES[pred], best_conf*100.0f);
  for (int i = 0; i < NUM_CLASSES; i++)
    Serial.printf("    %s: %.1f%%\n", CLASS_NAMES[i], confidence[i]*100.0f);

  // Switch both ports to 44100Hz RIGHT_LEFT for direction detection
  init_dir_mic_i2s(MIC_TOP_PORT, MIC_TOP_WS, MIC_TOP_BCLK, MIC_TOP_DOUT);
  init_dir_mic_i2s(MIC_BOT_PORT, MIC_BOT_WS, MIC_BOT_BCLK, MIC_BOT_DOUT);
  int quadrant = detect_direction();

  // noise=1, skip alert for noise or low confidence
  if (best_conf >= CONFIDENCE_THR && pred != 1) {
    Serial.printf("  >>> ALERT: %s\n", CLASS_NAMES[pred]);
    if (quadrant >= 0) {
      fire_haptic(quadrant);
      delay(150);
      fire_haptic(quadrant);
      delay(100);
    }
    play_wav(WAV_FILES[pred]);
  } else {
    Serial.println("  (background / low confidence)");
  }
  Serial.println("-----------------");

  // Restore MIC_TOP_PORT to ML mode so the next capture has no dead window.
  init_ml_mic_i2s();
}

// ── SETUP ─────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n========================================");
  Serial.println("  EchoSafe Integrated Firmware");
  Serial.println("========================================\n");

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);
  delay(50);

  Serial.print("LittleFS... ");
  if (LittleFS.begin(true)) {
    fs_ok = true;
    Serial.println("OK");
    File root = LittleFS.open("/");
    File f = root.openNextFile();
    while (f) { Serial.printf("  %s (%u bytes)\n", f.name(), f.size()); f = root.openNextFile(); }
  } else {
    Serial.println("FAILED");
  }

  Serial.print("Speaker... ");
  init_speaker_i2s();
  Serial.println(speaker_ok ? "OK" : "FAILED");

  init_haptics();

  Serial.print("Mel filters + DCT... ");
  init_mel_filters();
  init_dct_matrix();
  Serial.println("OK");

  // Init MIC_TOP in ML mode (16000Hz ONLY_LEFT) — ready for first capture
  Serial.print("Mic TOP (ML 16kHz)... ");
  init_ml_mic_i2s();
  delay(200);
  Serial.println("OK");

  // Init MIC_BOT in direction mode (44100Hz RIGHT_LEFT)
  Serial.print("Mic BOT (dir 44.1kHz)... ");
  init_dir_mic_i2s(MIC_BOT_PORT, MIC_BOT_WS, MIC_BOT_BCLK, MIC_BOT_DOUT);
  Serial.println("OK");

  Serial.println("\nCommands: i=inference  r=continuous  s=stop  h=haptic  d=direction  t=tone  p=speaker\nReady!\n");
}

// ── LOOP ──────────────────────────────────────────────────────────────────────
void loop() {
  if (Serial.available()) {
    char cmd = Serial.read();
    switch (cmd) {
      case 'i': capture_and_classify(); break;
      case 'r': continuous_mode = true;  Serial.println("\n[Continuous STARTED]"); break;
      case 's': continuous_mode = false; Serial.println("\n[Continuous STOPPED]"); break;
      case 'h': Serial.println("\n[Haptic test]"); haptic_test_all(); break;
      case 'd': {
        Serial.println("\n[Direction test]");
        init_dir_mic_i2s(MIC_TOP_PORT, MIC_TOP_WS, MIC_TOP_BCLK, MIC_TOP_DOUT);
        init_dir_mic_i2s(MIC_BOT_PORT, MIC_BOT_WS, MIC_BOT_BCLK, MIC_BOT_DOUT);
        int q = detect_direction();
        if (q >= 0) fire_haptic(q);
        // Restore ML mode on MIC_TOP after direction test
        init_ml_mic_i2s();
        break;
      }
      case 't': {
        Serial.println("\n[Tone test 440Hz/2s]");
        init_speaker_i2s();
        int16_t tbuf[512];
        int total = ML_SAMPLE_RATE * 2, written = 0; size_t bw;
        while (written < total) {
          int chunk = min(256, total - written);
          for (int i = 0; i < chunk; i++) {
            int16_t s = (int16_t)(16000.0f * sinf(2.0f * M_PI * 440.0f * (written+i) / ML_SAMPLE_RATE));
            tbuf[i*2] = s; tbuf[i*2+1] = s;
          }
          i2s_write(SPK_PORT, tbuf, chunk*4, &bw, portMAX_DELAY);
          written += chunk;
        }
        i2s_zero_dma_buffer(SPK_PORT);
        init_dir_mic_i2s(MIC_BOT_PORT, MIC_BOT_WS, MIC_BOT_BCLK, MIC_BOT_DOUT);
        Serial.println("  Done.");
        break;
      }
      case 'p':
        Serial.println("\n[Speaker test]");
        for (int i = 0; i < NUM_CLASSES; i++) {
          Serial.printf("  '%s': ", CLASS_NAMES[i]);
          if (WAV_FILES[i]) play_wav(WAV_FILES[i]); else Serial.println("(no file)");
          delay(500);
        }
        // Restore ML mode on MIC_TOP after speaker test
        init_ml_mic_i2s();
        break;
    }
  }
  if (continuous_mode) { capture_and_classify(); delay(500); }
  delay(10);
}
