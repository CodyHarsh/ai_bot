/*
  Step test 3: speaker (MAX98357A)
  Wiring: VIN->5V, GND->GND, BCLK->GPIO14, LRC->GPIO25, DIN->GPIO22, speaker on + and -
  You should hear a soft beep-boop every 2 seconds. Keep VOLUME low at first.
*/
#include <ESP_I2S.h>
I2SClass i2s;
#define VOLUME 6000   // 0..32767

void playTone(float freq, int ms) {
  static int32_t out[2 * 160];
  int total = 16000 * ms / 1000;
  static float phase = 0;
  for (int done = 0; done < total; done += 160) {
    for (int i = 0; i < 160; i++) {
      int32_t v = (int32_t)(sin(phase) * VOLUME);
      phase += 2 * PI * freq / 16000;
      out[2 * i] = out[2 * i + 1] = v << 16;
    }
    i2s.write((uint8_t*)out, sizeof(out));
  }
}

void setup() {
  Serial.begin(115200);
  i2s.setPins(14, 25, 22, 33);
  if (!i2s.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
    Serial.println("I2S failed");
    while (true) delay(1000);
  }
}

void loop() {
  Serial.println("beep");
  playTone(660, 150);
  playTone(880, 250);
  delay(2000);
}
