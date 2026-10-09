/*
  Step test 2: microphone (INMP441)
  Wiring: VDD->3V3, GND->GND, L/R->GND, SCK->GPIO14, WS->GPIO25, SD->GPIO33
  Open Serial Plotter or Serial Monitor (115200), talk or clap: the bar should grow.
  Note the quiet level and the loud level, you do not need to enter them anywhere.
*/
#include <ESP_I2S.h>
I2SClass i2s;

void setup() {
  Serial.begin(115200);
  i2s.setPins(14, 25, 22, 33);   // BCLK, WS, data out (unused here), data in
  if (!i2s.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
    Serial.println("I2S failed");
    while (true) delay(1000);
  }
}

void loop() {
  static int32_t buf[2 * 320];
  size_t n = i2s.readBytes((char*)buf, sizeof(buf)) / 8;
  long long sum = 0;
  for (size_t i = 0; i < n; i++) { int32_t s = buf[2 * i] >> 14; sum += (long long)s * s; }
  int rms = n ? (int)sqrt((double)(sum / n)) : 0;
  Serial.printf("level %5d  ", rms);
  for (int i = 0; i < min(rms / 60, 60); i++) Serial.print('#');
  Serial.println();
}
