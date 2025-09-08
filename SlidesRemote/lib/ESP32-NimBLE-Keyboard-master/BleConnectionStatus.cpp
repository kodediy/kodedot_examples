#include "BleConnectionStatus.h"
#include "NimBLEDevice.h"

BleConnectionStatus::BleConnectionStatus(void) {
}

void BleConnectionStatus::onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo)
{
  // Iniciar emparejamiento/bonding al conectar
  NimBLEDevice::startSecurity(connInfo.getConnHandle());
}

void BleConnectionStatus::onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason)
{
  this->connected = false;
}

// Se invoca al completar el emparejamiento y la autenticación segura
void BleConnectionStatus::onAuthenticationComplete(NimBLEConnInfo& connInfo)
{
  this->connected = true;
}
