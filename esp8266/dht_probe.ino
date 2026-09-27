#include <Arduino.h>

#define DHT_PIN 12

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(DHT_PIN, OUTPUT);
  digitalWrite(DHT_PIN, LOW);
  delay(18);
  pinMode(DHT_PIN, INPUT_PULLUP);
  delay(40);

  const int samples = 100;
  int levels[samples];

  unsigned long start = micros();
  for (int i = 0; i < samples; i++) {
    levels[i] = digitalRead(DHT_PIN);
    delayMicroseconds(10);
  }
  unsigned long elapsed = micros() - start;

  Serial.println("BOOTLOG START");
  Serial.printf("DHT probe pin=%d elapsed=%lums\n", DHT_PIN, (unsigned long)(elapsed / 1000));
  Serial.printf("Initial sample levels: ");
  for (int i = 0; i < samples; i++) {
    Serial.print(levels[i]);
    Serial.print(' ');
  }
  Serial.println();

  int lowCount = 0;
  int highCount = 0;
  for (int i = 0; i < samples; i++) {
    if (levels[i] == 0) lowCount++; else highCount++;
  }
  Serial.printf("lowCount=%d highCount=%d\n", lowCount, highCount);

  if (lowCount == 0) {
    Serial.println("RESULT: line never went low after start pulse -> DHT11 not responding / wiring/power problem");
  } else if (lowCount < 10) {
    Serial.println("RESULT: weak/short response -> likely noisy data line or weak pull-up");
  } else {
    Serial.println("RESULT: response looks plausible -> try normal DHT read again");
  }
  Serial.println("BOOTLOG END");
}

void loop() {
  delay(1000);
}
