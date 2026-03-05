#include <BleMouse.h>
#include <BLESecurity.h>

BleMouse bleMouse("Magic Mouse", "Apple", 100);

const int distance = 1;     //Pixel
const int interval = 60000; // 60 sec

unsigned long lastMove = 0;

void jiggle() {

  if (!bleMouse.isConnected()) return;

  int x = random(2) ? 2 : -2;
  int y = random(2) ? 2 : -2;

  for (int i = 0; i < distance; i++) {
    bleMouse.move(x, y);
    delay(5);
  }

  for (int i = 0; i < distance; i++) {
    bleMouse.move(-x, -y);
    delay(5);
  }
}

void setup() {
  BLESecurity *pSecurity = new BLESecurity();
  pSecurity->setAuthenticationMode(ESP_LE_AUTH_BOND);
  pSecurity->setCapability(ESP_IO_CAP_NONE);
  pSecurity->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  bleMouse.begin();
  bleMouse.setBatteryLevel(95);   // initial battery level
}

void loop() {

  if (!bleMouse.isConnected()) return;

  unsigned long now = millis();

  if (now - lastMove >= interval) {
    jiggle();
    lastMove = now;
  }

}