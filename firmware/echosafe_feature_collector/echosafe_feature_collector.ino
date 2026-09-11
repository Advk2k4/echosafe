/*
 * EchoSafe Feature Collection Firmware
 * ESP32-S3-N16R8 with I2S MEMS Microphone
 * 
 * Purpose: Capture audio, extract features, output to serial for labeling
 * 
 * Hardware:
 * - ESP32-S3-N16R8 (16MB Flash, 8MB PSRAM)
 * - I2S MEMS Microphone (ICS-43434 or INMP441)
 * 
 * Commands:
 * - 'c': Capture audio and extract features
 * - 'r': Start continuous recording mode (auto-capture every 2 seconds)
 * - 's': Stop continuous mode
 */

#include <driver/i2s.h>
#include <math.h>

// ============= CONFIGURATION =============
#define SAMPLE_RATE       16000    // 16kHz - good for voice/environmental sounds
#define SAMPLE_BITS       32       // I2S bits per sample
#define CHANNELS          1        // Mono
#define FRAME_SIZE        512      // Samples per frame for FFT
#define FRAME_STEP        256      // Hop size (50% overlap)
#define NUM_MFCC          13       // Number of MFCC coefficients
#define NUM_FRAMES        31       // ~1 second of audio (16000 / 512)
#define FFT_SIZE          512      // FFT size (must be power of 2)
#define MEL_BINS          26       // Number of mel filter banks

// I2S Pin Configuration (adjust for your board)
#define I2S_WS            5        // Word Select (LRCLK)
#define I2S_SD            6        // Serial Data (DOUT)
#define I2S_SCK           7        // Serial Clock (BCLK)
#define I2S_PORT          I2S_NUM_0

// Feature extraction parameters
#define PRE_EMPHASIS      0.97f
#define MEL_MIN_FREQ      0.0f
#define MEL_MAX_FREQ      8000.0f  // Nyquist frequency

// ============= GLOBAL BUFFERS =============
float audio_buffer[FRAME_SIZE];
float mfcc_features[NUM_FRAMES][NUM_MFCC];
int32_t i2s_buffer[FRAME_SIZE];  // I2S read buffer (32-bit samples)

// FFT and Mel filter bank buffers
float fft_real[FFT_SIZE];
float fft_imag[FFT_SIZE];
float power_spectrum[FFT_SIZE / 2 + 1];
float mel_energies[MEL_BINS];

// Pre-computed mel filter bank
float mel_filters[MEL_BINS][FFT_SIZE / 2 + 1];

// DCT matrix for MFCC
float dct_matrix[NUM_MFCC][MEL_BINS];

// State
bool continuous_mode = false;

// ============= HELPER FUNCTIONS =============

// Convert frequency to mel scale
float hz_to_mel(float hz) {
  return 2595.0f * log10f(1.0f + hz / 700.0f);
}

// Convert mel to frequency
float mel_to_hz(float mel) {
  return 700.0f * (powf(10.0f, mel / 2595.0f) - 1.0f);
}

// Initialize mel filter banks
void init_mel_filters() {
  float mel_min = hz_to_mel(MEL_MIN_FREQ);
  float mel_max = hz_to_mel(MEL_MAX_FREQ);
  float mel_step = (mel_max - mel_min) / (MEL_BINS + 1);
  
  float mel_points[MEL_BINS + 2];
  for (int i = 0; i < MEL_BINS + 2; i++) {
    mel_points[i] = mel_to_hz(mel_min + i * mel_step);
  }
  
  for (int i = 0; i < MEL_BINS; i++) {
    for (int j = 0; j <= FFT_SIZE / 2; j++) {
      float freq = (float)j * SAMPLE_RATE / FFT_SIZE;
      
      if (freq >= mel_points[i] && freq <= mel_points[i + 1]) {
        mel_filters[i][j] = (freq - mel_points[i]) / (mel_points[i + 1] - mel_points[i]);
      } else if (freq >= mel_points[i + 1] && freq <= mel_points[i + 2]) {
        mel_filters[i][j] = (mel_points[i + 2] - freq) / (mel_points[i + 2] - mel_points[i + 1]);
      } else {
        mel_filters[i][j] = 0.0f;
      }
    }
  }
}

// Initialize DCT matrix
void init_dct_matrix() {
  for (int i = 0; i < NUM_MFCC; i++) {
    for (int j = 0; j < MEL_BINS; j++) {
      dct_matrix[i][j] = cosf(M_PI * i * (j + 0.5f) / MEL_BINS);
      if (i == 0) {
        dct_matrix[i][j] *= sqrtf(1.0f / MEL_BINS);
      } else {
        dct_matrix[i][j] *= sqrtf(2.0f / MEL_BINS);
      }
    }
  }
}

// Simple FFT (Cooley-Tukey radix-2)
void fft(float* real, float* imag, int n) {
  // Bit-reversal permutation
  int j = 0;
  for (int i = 0; i < n - 1; i++) {
    if (i < j) {
      float temp = real[i];
      real[i] = real[j];
      real[j] = temp;
      temp = imag[i];
      imag[i] = imag[j];
      imag[j] = temp;
    }
    int k = n / 2;
    while (k <= j) {
      j -= k;
      k /= 2;
    }
    j += k;
  }
  
  // FFT computation
  for (int len = 2; len <= n; len *= 2) {
    float angle = -2.0f * M_PI / len;
    float wlen_real = cosf(angle);
    float wlen_imag = sinf(angle);
    
    for (int i = 0; i < n; i += len) {
      float w_real = 1.0f;
      float w_imag = 0.0f;
      
      for (int j = 0; j < len / 2; j++) {
        int idx1 = i + j;
        int idx2 = i + j + len / 2;
        
        float t_real = w_real * real[idx2] - w_imag * imag[idx2];
        float t_imag = w_real * imag[idx2] + w_imag * real[idx2];
        
        real[idx2] = real[idx1] - t_real;
        imag[idx2] = imag[idx1] - t_imag;
        real[idx1] += t_real;
        imag[idx1] += t_imag;
        
        float temp_w = w_real;
        w_real = w_real * wlen_real - w_imag * wlen_imag;
        w_imag = temp_w * wlen_imag + w_imag * wlen_real;
      }
    }
  }
}

// Hamming window
void apply_hamming_window(float* buffer, int size) {
  for (int i = 0; i < size; i++) {
    float window = 0.54f - 0.46f * cosf(2.0f * M_PI * i / (size - 1));
    buffer[i] *= window;
  }
}

// Extract MFCC from a single frame
void extract_mfcc_frame(float* frame, float* mfcc) {
  // Pre-emphasis
  for (int i = FRAME_SIZE - 1; i > 0; i--) {
    frame[i] = frame[i] - PRE_EMPHASIS * frame[i - 1];
  }
  
  // Apply Hamming window
  apply_hamming_window(frame, FRAME_SIZE);
  
  // Prepare FFT buffers
  for (int i = 0; i < FRAME_SIZE; i++) {
    fft_real[i] = frame[i];
    fft_imag[i] = 0.0f;
  }
  for (int i = FRAME_SIZE; i < FFT_SIZE; i++) {
    fft_real[i] = 0.0f;
    fft_imag[i] = 0.0f;
  }
  
  // Compute FFT
  fft(fft_real, fft_imag, FFT_SIZE);
  
  // Compute power spectrum
  for (int i = 0; i <= FFT_SIZE / 2; i++) {
    power_spectrum[i] = fft_real[i] * fft_real[i] + fft_imag[i] * fft_imag[i];
  }
  
  // Apply mel filter banks
  for (int i = 0; i < MEL_BINS; i++) {
    mel_energies[i] = 0.0f;
    for (int j = 0; j <= FFT_SIZE / 2; j++) {
      mel_energies[i] += power_spectrum[j] * mel_filters[i][j];
    }
    // Log mel energies (add small epsilon to avoid log(0))
    mel_energies[i] = logf(mel_energies[i] + 1e-10f);
  }
  
  // Apply DCT to get MFCCs
  for (int i = 0; i < NUM_MFCC; i++) {
    mfcc[i] = 0.0f;
    for (int j = 0; j < MEL_BINS; j++) {
      mfcc[i] += dct_matrix[i][j] * mel_energies[j];
    }
  }
}

// ============= I2S FUNCTIONS =============

void init_i2s() {
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = (i2s_comm_format_t)(I2S_COMM_FORMAT_STAND_I2S),
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 256,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };

  esp_err_t err = i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
  if (err != ESP_OK) {
    Serial.print("ERROR: I2S driver install failed: ");
    Serial.println(err);
    return;
  }
  Serial.println("DEBUG: I2S driver installed");
  Serial.flush();

  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_SD
  };

  err = i2s_set_pin(I2S_PORT, &pin_config);
  if (err != ESP_OK) {
    Serial.print("ERROR: I2S set pin failed: ");
    Serial.println(err);
    return;
  }
  Serial.println("DEBUG: I2S pins configured");
  Serial.flush();

  i2s_zero_dma_buffer(I2S_PORT);
}

// Read audio frame from I2S microphone
void read_audio_frame(float* buffer, int size) {
  size_t bytes_read = 0;
  i2s_read(I2S_PORT, i2s_buffer, size * sizeof(int32_t), &bytes_read, portMAX_DELAY);

  // Convert 32-bit I2S samples to float and normalize
  for (int i = 0; i < size; i++) {
    buffer[i] = (float)i2s_buffer[i] / 2147483648.0f;  // Normalize to [-1, 1]
  }
}

// ============= FEATURE EXTRACTION =============

void capture_and_extract_features() {
  Serial.println("START_CAPTURE");
  
  unsigned long start_time = millis();
  
  // Capture audio and extract features frame by frame
  float full_audio[FRAME_SIZE + (NUM_FRAMES - 1) * FRAME_STEP];
  
  // Read all audio samples
  for (int i = 0; i < NUM_FRAMES; i++) {
    int offset = i * FRAME_STEP;
    float frame_buffer[FRAME_SIZE];
    read_audio_frame(frame_buffer, FRAME_SIZE);
    
    // Store in full audio buffer
    for (int j = 0; j < FRAME_SIZE; j++) {
      if (offset + j < sizeof(full_audio) / sizeof(float)) {
        full_audio[offset + j] = frame_buffer[j];
      }
    }
    
    // Extract MFCC for this frame
    extract_mfcc_frame(frame_buffer, mfcc_features[i]);
  }
  
  unsigned long capture_time = millis() - start_time;
  
  // Output features in structured format
  Serial.println("FEATURES_START");
  Serial.print("TIMESTAMP:");
  Serial.println(millis());
  Serial.print("SAMPLE_RATE:");
  Serial.println(SAMPLE_RATE);
  Serial.print("NUM_FRAMES:");
  Serial.println(NUM_FRAMES);
  Serial.print("NUM_MFCC:");
  Serial.println(NUM_MFCC);
  Serial.print("CAPTURE_TIME_MS:");
  Serial.println(capture_time);
  
  // Output MFCC features (flattened)
  Serial.println("MFCC_DATA:");
  for (int i = 0; i < NUM_FRAMES; i++) {
    for (int j = 0; j < NUM_MFCC; j++) {
      Serial.print(mfcc_features[i][j], 6);
      if (j < NUM_MFCC - 1) Serial.print(",");
    }
    Serial.println();
  }
  
  Serial.println("FEATURES_END");
  Serial.println("READY_FOR_LABEL");
}

// ============= SETUP =============

void setup() {
  Serial.begin(115200);
  delay(2000);  // Give USB CDC time to enumerate

  Serial.println("\n=== EchoSafe Feature Collector ===");
  Serial.println("DEBUG: Serial working, about to init I2S...");
  Serial.flush();

  // Initialize I2S microphone
  init_i2s();
  Serial.println("DEBUG: I2S init done");
  Serial.flush();
  
  // Pre-compute mel filters and DCT matrix
  init_mel_filters();
  init_dct_matrix();
  Serial.println("✓ Mel filters and DCT matrix initialized");
  
  Serial.println("\nReady for data collection!");
  Serial.println("Commands:");
  Serial.println("  'c' - Capture one sample");
  Serial.println("  'r' - Start continuous recording (auto-capture every 2s)");
  Serial.println("  's' - Stop continuous recording");
  Serial.println();
}

// ============= LOOP =============

void loop() {
  // Check for serial commands
  if (Serial.available() > 0) {
    char cmd = Serial.read();
    
    if (cmd == 'c') {
      Serial.println("\n>>> Capturing single sample...");
      capture_and_extract_features();
    }
    else if (cmd == 'r') {
      continuous_mode = true;
      Serial.println("\n>>> Continuous mode STARTED (capturing every 2 seconds)");
      Serial.println(">>> Press 's' to stop");
    }
    else if (cmd == 's') {
      continuous_mode = false;
      Serial.println("\n>>> Continuous mode STOPPED");
    }
  }
  
  // Continuous recording mode
  if (continuous_mode) {
    capture_and_extract_features();
    delay(2000);  // Wait 2 seconds between captures
  }
  
  delay(10);
}
