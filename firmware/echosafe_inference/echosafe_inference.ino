/*
 * EchoSafe Real-Time Inference Firmware
 * ESP32-S3-N16R8 + ICS43434 I2S MEMS Microphone
 *
 * Captures audio, extracts MFCC features on-chip, runs MLP inference,
 * prints classification results to Serial Monitor.
 *
 * Microphone Pins (I2S_NUM_0 - RX):
 *   GPIO 5  -> WS  (LRCLK)
 *   GPIO 6  -> SD  (DOUT)
 *   GPIO 7  -> SCK (BCLK)
 *
 * Model: MLP 403->128->64->5
 * Classes: horns(0), noise(1), bells(2), gunshots(3), sirens(4)
 *
 * Commands:
 *   'i' - Single inference
 *   'r' - Continuous mode
 *   's' - Stop
 */

#include <driver/i2s_std.h>
#include <math.h>
#include "model_weights.h"

// ============= CONFIGURATION =============
#define SAMPLE_RATE  16000
#define FRAME_SIZE   512
#define FRAME_STEP   256
#define NUM_MFCC     13
#define NUM_FRAMES   31
#define FFT_SIZE     512
#define MEL_BINS     26
#define MIC_WS       5
#define MIC_SD       6
#define MIC_SCK      7
#define MIC_PORT     I2S_NUM_0
#define PRE_EMPHASIS 0.97f
#define THRESHOLD    0.65f
// ICS43434 L/R pin: LOW (GND) = left channel, HIGH (VCC) = right channel
// Change to I2S_STD_SLOT_RIGHT if your L/R pin is tied HIGH
#define MIC_CHANNEL  I2S_STD_SLOT_LEFT

// ============= GLOBAL BUFFERS =============
float   frame_buf[FRAME_SIZE];
float   mfcc_out[NUM_FRAMES][NUM_MFCC];
int32_t i2s_buf[FRAME_SIZE];
float   fft_real[FFT_SIZE];
float   fft_imag[FFT_SIZE];
float   pspec[FFT_SIZE / 2 + 1];
float   mel_e[MEL_BINS];
float   mel_fb[MEL_BINS][FFT_SIZE / 2 + 1];
float   dct_m[NUM_MFCC][MEL_BINS];
float   feat[INPUT_DIM];
float   l1[128], l2[64], probs[NUM_CLASSES];
bool    cont_mode = false;
static i2s_chan_handle_t mic_rx_handle = NULL;

// ============= MEL / DCT INIT =============

void init_mel_filters() {
  float mel_min = 2595.0f * log10f(1.0f + 0.0f / 700.0f);
  float mel_max = 2595.0f * log10f(1.0f + 8000.0f / 700.0f);
  float step = (mel_max - mel_min) / (MEL_BINS + 1);
  float pts[MEL_BINS + 2];
  for (int i = 0; i < MEL_BINS + 2; i++)
    pts[i] = 700.0f * (powf(10.0f, (mel_min + i * step) / 2595.0f) - 1.0f);
  for (int i = 0; i < MEL_BINS; i++)
    for (int j = 0; j <= FFT_SIZE / 2; j++) {
      float f = (float)j * SAMPLE_RATE / FFT_SIZE;
      if      (f >= pts[i]   && f <= pts[i+1]) mel_fb[i][j] = (f-pts[i])   / (pts[i+1]-pts[i]);
      else if (f >= pts[i+1] && f <= pts[i+2]) mel_fb[i][j] = (pts[i+2]-f) / (pts[i+2]-pts[i+1]);
      else                                      mel_fb[i][j] = 0.0f;
    }
}

void init_dct() {
  for (int i = 0; i < NUM_MFCC; i++) {
    float sc = (i == 0) ? sqrtf(1.0f/MEL_BINS) : sqrtf(2.0f/MEL_BINS);
    for (int j = 0; j < MEL_BINS; j++)
      dct_m[i][j] = sc * cosf(M_PI * i * (j + 0.5f) / MEL_BINS);
  }
}

// ============= FFT =============

void do_fft(float* re, float* im, int n) {
  int j = 0;
  for (int i = 0; i < n-1; i++) {
    if (i < j) { float t=re[i]; re[i]=re[j]; re[j]=t; t=im[i]; im[i]=im[j]; im[j]=t; }
    int k = n/2;
    while (k <= j) { j -= k; k /= 2; }
    j += k;
  }
  for (int len = 2; len <= n; len *= 2) {
    float ang = -2.0f * M_PI / len, wr = cosf(ang), wi = sinf(ang);
    for (int i = 0; i < n; i += len) {
      float cr=1, ci=0;
      for (int k = 0; k < len/2; k++) {
        int a=i+k, b=i+k+len/2;
        float tr=cr*re[b]-ci*im[b], ti=cr*im[b]+ci*re[b];
        re[b]=re[a]-tr; im[b]=im[a]-ti; re[a]+=tr; im[a]+=ti;
        float tmp=cr; cr=cr*wr-ci*wi; ci=tmp*wi+ci*wr;
      }
    }
  }
}

// ============= MFCC EXTRACTION =============

void extract_mfcc(float* frame, float* mfcc) {
  for (int i = FRAME_SIZE-1; i > 0; i--) frame[i] -= PRE_EMPHASIS * frame[i-1];
  for (int i = 0; i < FRAME_SIZE; i++)
    frame[i] *= 0.54f - 0.46f * cosf(2.0f * M_PI * i / (FRAME_SIZE-1));
  for (int i = 0; i < FRAME_SIZE; i++) { fft_real[i]=frame[i]; fft_imag[i]=0; }
  for (int i = FRAME_SIZE; i < FFT_SIZE; i++) { fft_real[i]=0; fft_imag[i]=0; }
  do_fft(fft_real, fft_imag, FFT_SIZE);
  for (int i = 0; i <= FFT_SIZE/2; i++)
    pspec[i] = fft_real[i]*fft_real[i] + fft_imag[i]*fft_imag[i];
  for (int i = 0; i < MEL_BINS; i++) {
    mel_e[i] = 0;
    for (int j = 0; j <= FFT_SIZE/2; j++) mel_e[i] += pspec[j] * mel_fb[i][j];
    mel_e[i] = logf(mel_e[i] + 1e-10f);
  }
  for (int i = 0; i < NUM_MFCC; i++) {
    mfcc[i] = 0;
    for (int j = 0; j < MEL_BINS; j++) mfcc[i] += dct_m[i][j] * mel_e[j];
  }
}

// ============= MICROPHONE =============

void init_mic() {
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(MIC_PORT, I2S_ROLE_MASTER);
  // Match the legacy driver's dma_buf_count=8/dma_buf_len=256 explicitly --
  // the new API's own defaults (6 descriptors x 240 frames) are close but
  // not identical, and this migration isn't meant to also retune buffering.
  chan_cfg.dma_desc_num  = 8;
  chan_cfg.dma_frame_num = 256;
  i2s_new_channel(&chan_cfg, NULL, &mic_rx_handle);  // NULL tx handle: RX only

  i2s_std_config_t std_cfg = {
    .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,
      .bclk = (gpio_num_t)MIC_SCK,
      .ws   = (gpio_num_t)MIC_WS,
      .dout = I2S_GPIO_UNUSED,
      .din  = (gpio_num_t)MIC_SD,
      .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
    },
  };
  // I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG's mono case only defaults slot_mask
  // to LEFT on the original ESP32/ESP32-S2 -- on the S3 (this target) the
  // driver's own macro leaves it at BOTH regardless of mono/stereo, so the
  // single physical slot to capture has to be selected explicitly here.
  std_cfg.slot_cfg.slot_mask = MIC_CHANNEL;

  i2s_channel_init_std_mode(mic_rx_handle, &std_cfg);
  i2s_channel_enable(mic_rx_handle);
  // No RX equivalent of the legacy i2s_zero_dma_buffer(): the new driver's
  // only "auto clear" options are documented as TX-buffer-only, and an RX
  // buffer's prior contents don't matter since real samples overwrite it on
  // every DMA transfer regardless.
}

void read_frame(float* buf) {
  size_t bytes = 0;
  // timeout_ms is milliseconds here, not RTOS ticks like the legacy
  // i2s_read()'s portMAX_DELAY -- reusing the same constant still means
  // "block for billions of ms," i.e. effectively forever, same as before.
  i2s_channel_read(mic_rx_handle, i2s_buf, FRAME_SIZE * sizeof(int32_t), &bytes, portMAX_DELAY);
  for (int i = 0; i < FRAME_SIZE; i++)
    buf[i] = (float)i2s_buf[i] / 2147483648.0f;
}

// ============= INFERENCE =============

void relu(float* d, int n) { for (int i=0;i<n;i++) if(d[i]<0) d[i]=0; }

void softmax(float* d, int n) {
  float mx=d[0]; for(int i=1;i<n;i++) if(d[i]>mx) mx=d[i];
  float s=0; for(int i=0;i<n;i++){d[i]=expf(d[i]-mx);s+=d[i];}
  for(int i=0;i<n;i++) d[i]/=s;
}

void dense(const float* in, int in_n, const float* W, const float* b, float* out, int out_n) {
  for(int j=0;j<out_n;j++){
    out[j]=b[j];
    for(int i=0;i<in_n;i++) out[j]+=in[i]*W[i*out_n+j];
  }
}

int classify() {
  for(int i=0;i<NUM_FRAMES;i++)
    for(int j=0;j<NUM_MFCC;j++)
      feat[i*NUM_MFCC+j] = (mfcc_out[i][j] - SCALER_MEAN[i*NUM_MFCC+j]) / SCALER_SCALE[i*NUM_MFCC+j];
  dense(feat, LAYER0_INPUT, LAYER0_WEIGHTS, LAYER0_BIAS, l1, LAYER0_OUTPUT); relu(l1, LAYER0_OUTPUT);
  dense(l1,   LAYER2_INPUT, LAYER2_WEIGHTS, LAYER2_BIAS, l2, LAYER2_OUTPUT); relu(l2, LAYER2_OUTPUT);
  dense(l2,   LAYER4_INPUT, LAYER4_WEIGHTS, LAYER4_BIAS, probs, LAYER4_OUTPUT); softmax(probs, NUM_CLASSES);
  int best=0; for(int i=1;i<NUM_CLASSES;i++) if(probs[i]>probs[best]) best=i;
  return best;
}

// ============= CAPTURE + PRINT =============

void run() {
  Serial.println("Listening...");
  unsigned long t0 = millis();
  for(int i=0;i<NUM_FRAMES;i++){ read_frame(frame_buf); extract_mfcc(frame_buf, mfcc_out[i]); }
  unsigned long cap = millis()-t0;

  t0 = millis();
  int pred = classify();
  unsigned long inf = millis()-t0;

  Serial.println("----------------------------");
  if(probs[pred] >= THRESHOLD) {
    Serial.print("DETECTED: "); Serial.print(CLASS_NAMES[pred]);
    Serial.print("  ("); Serial.print(probs[pred]*100,1); Serial.println("%)");
  } else {
    Serial.print("UNCERTAIN: "); Serial.print(CLASS_NAMES[pred]);
    Serial.print("  ("); Serial.print(probs[pred]*100,1); Serial.println("%)");
  }
  for(int i=0;i<NUM_CLASSES;i++){
    Serial.print("  "); Serial.print(CLASS_NAMES[i]);
    Serial.print(": "); Serial.print(probs[i]*100,1); Serial.println("%");
  }
  Serial.print("  cap="); Serial.print(cap);
  Serial.print("ms  inf="); Serial.print(inf); Serial.println("ms");
  Serial.println("----------------------------");
}

// ============= SETUP / LOOP =============

void setup() {
  Serial.begin(115200);
  // Wait up to 3 s for USB-CDC to enumerate; harmless on UART-based connections
  {
    unsigned long t = millis();
    while (!Serial && millis() - t < 3000) delay(10);
  }
  delay(200);

  Serial.println("\n========================================");
  Serial.println("  EchoSafe — Mic-Only Inference");
  Serial.println("  MLP 403->128->64->5");
  Serial.println("  horns | noise | bells | gunshots | sirens");
  Serial.println("========================================");
  Serial.flush();

  init_mel_filters();
  init_dct();
  init_mic();

  Serial.println("Ready!  Commands: i=single  r=continuous  s=stop");
  Serial.println("Starting continuous classification...\n");
  cont_mode = true;
}

void loop() {
  if (Serial.available()) {
    char c = Serial.read();
    if      (c == 'i') run();
    else if (c == 'r') { cont_mode = true;  Serial.println("Continuous ON"); }
    else if (c == 's') { cont_mode = false; Serial.println("Continuous OFF"); }
  }
  if (cont_mode) { run(); delay(250); }
  delay(10);
}
