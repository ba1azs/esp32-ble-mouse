#include "BleConnectionStatus.h"

BleConnectionStatus::BleConnectionStatus(void) {}

void BleConnectionStatus::onConnect(BLEServer *pServer) {
  this->connected = true;
  if (!this->inputMouse)
    return;

  BLE2902 *desc = (BLE2902 *)this->inputMouse->getDescriptorByUUID(
      BLEUUID((uint16_t)0x2902));
  if (desc)
    desc->setNotifications(true);
}

void BleConnectionStatus::onDisconnect(BLEServer *pServer) {
  this->connected = false;
  if (!this->inputMouse)
    return;

  BLE2902 *desc = (BLE2902 *)this->inputMouse->getDescriptorByUUID(
      BLEUUID((uint16_t)0x2902));
  if (desc)
    desc->setNotifications(false);
}
