/* =====================================================================
   RideToWooshHID  —  ESP32-S3
   ---------------------------------------------------------------------
   Liest die Zwift Ride Lenker-Controller per BLE (Central-Rolle),
   dekodiert die Button-Bitmaske (MAKINOLO-Protokoll, Msg-ID 0x23) und
   gibt frei konfigurierbare Tastendrücke als BLE-HID-Tastatur aus.

   Konfiguration + Live-Anzeige über eingebaute Weboberfläche
   (WiFi-Accesspoint, WebSocket auf Port 80 unter /ws).

   Stack: NimBLE-Arduino 2.x  (Central + HID-Peripheral koexistieren)
   Ziel:  ESP32-S3 (mit PSRAM)
   =====================================================================

   --- Benötigte Arduino-Libraries (Library Manager) ---
     • NimBLE-Arduino            (h2zero)          >= 2.1.0
     • ESPAsyncWebServer         (ESP32Async)
     • AsyncTCP                  (ESP32Async)
   --- HID-Tastatur ---
     Wir nutzen die NimBLE-2.x-kompatible Keyboard-Lib:
     • ESP32-NIMBLE-Keyboard     (Berg0162)        -> NimBleKeyboard.h
       (Sketch > Include Library > Add .ZIP Library)

   --- Board-Einstellungen (Arduino IDE) ---
     Board:        "ESP32S3 Dev Module"
     PSRAM:        "OPI PSRAM"   (oder QSPI, je nach Modul)
     Partition:    egal — die mitgelieferte partitions.csv im Sketch-Ordner
                   überschreibt die Menüauswahl automatisch (eigene cfg-NVS)
     USB CDC On Boot: Enabled    (für Serial-Logs über USB)
   ===================================================================== */

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <NimBleKeyboard.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <nvs_flash.h>
#include <esp_partition.h>
#include "webui.h"
#include "i18n.h"     // Sprachdateien (EN/DE) als PROGMEM-JSON

// --------------------------------------------------------------------
//  Konfiguration
// --------------------------------------------------------------------
#define FW_VERSION   "1.0.0"
#define AP_SSID      "RideToWooshHID"
// WPA2-PSK Key: 8–63 ASCII-Zeichen. ACHTUNG: Dieser Default steht im
// öffentlichen Repo, ist also allgemein bekannt. Für echte Privatsphäre der
// Funkstrecke hier ein eigenes Passwort eintragen (vor dem Flashen).
#define AP_PASS      "ridetowoosh"    // geteilter Default — bei Bedarf ändern
#define HID_NAME     "RideToWooshHID"   // so erscheint die Tastatur im BT-Menü

// WiFi-Sendeleistung des Config-AP. Aktuell VOLLE Leistung (zuverlässige
// Verbindung). Optional als „Nähe = Zugangsschutz" reduzieren — der frühere
// Verbindungs-Timeout lag aber an der BLE-Koexistenz (Scan-Duty), nicht an
// der TX-Leistung; Reduzieren ist also wieder gefahrlos möglich.
// Betrifft NUR WiFi, nicht die BLE-Strecke zur Ride/zum Endgerät.
// Skala (Arduino-ESP32): WIFI_POWER_19_5dBm (max) · 17 · 15 · 13 · 11 · 8_5 · 7 · 5 · 2 · MINUS_1dBm
#define AP_TX_POWER  WIFI_POWER_19_5dBm

// Zwift RideOn-Handshake + GATT (Zwift Ride, unverschlüsselt)
static const char* RIDEON_MAGIC = "RideOn";   // 6 Bytes, ASCII

// Hinweis: Die echten Service-/Characteristic-UUIDs der Ride werden zur
// Laufzeit per Service-Discovery ermittelt (siehe findZwiftChars()), daher
// sind hier keine festen UUIDs nötig. Das Advertising filtern wir über den
// Gerätenamen "Zwift Ride".

static const uint8_t MSG_ID_KEYPAD = 0x23;    // Button-Status-Nachricht

// Zwift-GATT (Service 0xFC82, gilt für Ride / Click / Click V2 / Play):
//   ...0002 notify         async: Button-Daten (0x23 ...)
//   ...0003 write-no-resp  Control Point: hier geht "RideOn" hin
//   ...0004 indicate       Sync-Antworten auf den Handshake
static const char* ZW_UUID_ASYNC = "00000002-19ca-4651-86e5-fa29dcdd09d1";
static const char* ZW_UUID_CTRL  = "00000003-19ca-4651-86e5-fa29dcdd09d1";
static const char* ZW_UUID_SYNC  = "00000004-19ca-4651-86e5-fa29dcdd09d1";

// BLE-Debug (Handshake, Rohdaten). Auf 0 setzen für ruhige Logs.
#define BLE_DEBUG 0
#define HANDSHAKE_TIMEOUT_MS 5000   // so lange ohne 0x23-Daten -> Warnung + Diagnose

static void hexDump(const char* tag, const uint8_t* d, size_t len){
  // Zeile erst in Puffer bauen, dann EIN Serial-Write (BLE-Task und loop()
  // schreiben sonst ineinander).
  char buf[256]; int n = snprintf(buf, sizeof buf, "[BLE] %s (%u B):", tag, (unsigned)len);
  for(size_t i=0;i<len && i<48 && n<(int)sizeof buf-6;i++) n += snprintf(buf+n, sizeof buf-n, " %02x", d[i]);
  if(len>48 && n<(int)sizeof buf-6) n += snprintf(buf+n, sizeof buf-n, " ...");
  buf[n++]='\n'; Serial.write((const uint8_t*)buf, n);
}

// --------------------------------------------------------------------
//  Button-Tabelle  (muss exakt zur Reihenfolge im Web-UI passen)
// --------------------------------------------------------------------
struct BtnDef { const char* id; uint32_t mask; };
// Belegung empirisch an echter Zwift Ride ("Zwift SF2") ermittelt.
// Digitale Bits 0..14 (Bit 11 ungenutzt). Analog-Hebel = virtuelle Buttons in
// den oberen Bits (24..27), aus field 3 (id0=links, id1=rechts) per Schwellwert.
static const BtnDef BUTTONS[] = {
  {"LEFT_BTN",      0x00001},   // bit0  D-Pad links
  {"UP_BTN",        0x00002},   // bit1  D-Pad hoch
  {"RIGHT_BTN",     0x00004},   // bit2  D-Pad rechts
  {"DOWN_BTN",      0x00008},   // bit3  D-Pad runter
  {"A_BTN",         0x00010},   // bit4
  {"B_BTN",         0x00020},   // bit5
  {"Y_BTN",         0x00040},   // bit6
  {"Z_BTN",         0x00080},   // bit7
  {"SHFT_UP_L_BTN", 0x00100},   // bit8  Schalt links hoch
  {"SHFT_DN_L_BTN", 0x00200},   // bit9  Schalt links runter
  {"AUX_L_BTN",     0x00400},   // bit10 Zusatztaste links (an Bremse)
  {"SHFT_UP_R_BTN", 0x01000},   // bit12 Schalt rechts hoch
  {"SHFT_DN_R_BTN", 0x02000},   // bit13 Schalt rechts runter
  {"AUX_R_BTN",     0x04000},   // bit14 Zusatztaste rechts (an Bremse)
  // virtuelle Analog-Hebel (field 3), je Richtung ein mappbarer "Button"
  {"STEER_LL_BTN",  0x1000000}, // Hebel links  nach links  (id0 <= -SCHWELLE)
  {"STEER_LR_BTN",  0x2000000}, // Hebel links  nach rechts (id0 >= +SCHWELLE)
  {"STEER_RL_BTN",  0x4000000}, // Hebel rechts nach links  (id1 <= -SCHWELLE)
  {"STEER_RR_BTN",  0x8000000}, // Hebel rechts nach rechts (id1 >= +SCHWELLE)
};
#define DIGITAL_MASK 0x00FFFFFF   // Bits 0..23 = digitale Buttons; 24+ = Analog
#define STEER_THRESH 40           // Hebel-Ausschlag ab dem ein virtueller Button "gedrückt" ist
static const size_t N_BTN = sizeof(BUTTONS)/sizeof(BUTTONS[0]);

// Achtung: Die Ride sendet den Button-Status INVERTIERT — ein gedrückter
// Knopf setzt sein Bit auf 0 (siehe MAKINOLO: "first bit zero = left
// pressed"). Wir normalisieren das in onRideNotify(), sodass intern
// 1 == gedrückt gilt.

// --------------------------------------------------------------------
//  Globale Objekte / Zustand
// --------------------------------------------------------------------
BleKeyboard       bleKeyboard(HID_NAME, "DIY", 100);
AsyncWebServer    httpServer(80);       // Weboberfläche + WebSocket (ein Server)
AsyncWebSocket    wsLive("/ws");        // WebSocket-Live-Daten unter /ws (Port 80)
Preferences       prefs;

String keyMap[32];                     // Index = Bitposition -> Key-Token
volatile uint32_t lastPressedMask = 0; // intern: 1 == gedrückt
uint32_t          prevPressedMask = 0;

NimBLEClient*           rideClient   = nullptr;
NimBLERemoteCharacteristic* rideMeasure = nullptr; // notify (0x23 kommt hier)
NimBLERemoteCharacteristic* rideControl = nullptr; // write (RideOn)
NimBLERemoteCharacteristic* rideSync    = nullptr; // indicate (Handshake-Antwort)
volatile int      rideBattery   = -1;     // Akkustand der Click/Ride in % (-1 = unbekannt)
volatile uint32_t rideRxCount   = 0;      // empfangene Notifications (Debug)
volatile uint32_t rideConnectMs = 0;      // Zeitpunkt des Handshakes
volatile bool     rideConnected = false;
volatile bool     wantReconnect = false;
volatile bool     stateDirty    = false;   // Statuswechsel aus BLE-Task -> Broadcast in loop()
NimBLEAdvertisedDevice* foundRide = nullptr;
String            rideDevName  = "";        // Name+MAC der verbundenen Ride (fürs UI)
SemaphoreHandle_t mapMutex     = nullptr;    // schützt keyMap[] gegen Task-Races
const char*       cfgPartInUse = nullptr;    // "cfg" wenn vorhanden, sonst NULL(=Default-NVS)
#define CFG_PART  "cfg"

// Forward-Deklarationen
void broadcastButtons(uint32_t mask);
void broadcastState();
void sendMapping(AsyncWebSocketClient* client=nullptr);

// --------------------------------------------------------------------
//  Helfer: Bitposition aus Maske
// --------------------------------------------------------------------
static int maskToIndex(uint32_t m){
  for(int i=0;i<32;i++) if(m==(1u<<i)) return i;
  return -1;
}

// Minimaler JSON-String-Escaper (für Gerätenamen im State-Broadcast).
static String jsonEsc(const String& s){
  String o; o.reserve(s.length()+4);
  for(size_t i=0;i<s.length();i++){
    char c=s[i];
    if(c=='"'||c=='\\'){ o+='\\'; o+=c; }
    else if(c=='\n'){ o+="\\n"; }
    else if((uint8_t)c>=0x20){ o+=c; }   // Steuerzeichen verwerfen
  }
  return o;
}

// --------------------------------------------------------------------
//  Persistenter Speicher: getrennte NVS-Partition für die feste Config
// --------------------------------------------------------------------
//  Die Tastenbelegung liegt in einer EIGENEN NVS-Partition ("cfg"), bewusst
//  getrennt von der System-NVS (dort schreibt der BLE-Stack laufend Bonding-
//  Keys und WiFi seine PHY-Kalibrierung). Trennung = keine Schreib-Races und
//  ein volllaufendes/fragmentiertes System-NVS kann die Belegung nicht
//  beschädigen. Fehlt die 'cfg'-Partition (partitions.csv nicht geflasht),
//  fallen wir sauber auf die Default-NVS zurück.
void initConfigStore(){
  const esp_partition_t* p = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, CFG_PART);
  if(!p){
    Serial.println("[NVS] 'cfg'-Partition nicht gefunden -> Default-NVS. "
                   "(partitions.csv flashen fuer getrennten Config-Speicher)");
    cfgPartInUse = nullptr;
    return;
  }
  esp_err_t err = nvs_flash_init_partition(CFG_PART);
  if(err==ESP_ERR_NVS_NO_FREE_PAGES || err==ESP_ERR_NVS_NEW_VERSION_FOUND){
    Serial.println("[NVS] 'cfg' wird formatiert...");
    nvs_flash_erase_partition(CFG_PART);
    err = nvs_flash_init_partition(CFG_PART);
  }
  if(err!=ESP_OK){
    Serial.printf("[NVS] 'cfg' init-Fehler (%s) -> Default-NVS.\n", esp_err_to_name(err));
    cfgPartInUse = nullptr;
    return;
  }
  cfgPartInUse = CFG_PART;
  Serial.println("[NVS] Config in eigener 'cfg'-Partition.");
}

// --------------------------------------------------------------------
//  NVS laden / speichern
// --------------------------------------------------------------------
void loadMapping(){
  // Read-WRITE öffnen (legt den Namespace beim Erststart an) — sonst loggt
  // Preferences bei noch leerer 'cfg' ein hässliches "nvs_open NOT_FOUND".
  prefs.begin("zrhid", false, cfgPartInUse);
  for(size_t i=0;i<N_BTN;i++){
    int bit = maskToIndex(BUTTONS[i].mask);
    if(bit<0) continue;
    // isKey()-Guard: getString auf einen fehlenden Key loggt sonst einen [E]-Fehler
    keyMap[bit] = prefs.isKey(BUTTONS[i].id) ? prefs.getString(BUTTONS[i].id, "") : "";
  }
  prefs.end();
  // sinnvolle Defaults beim allerersten Start (MyWhoosh: I/K)
  bool empty=true;
  for(int i=0;i<32;i++) if(keyMap[i].length()){ empty=false; break; }
  if(empty){
    keyMap[ maskToIndex(0x00001) ] = "LEFT";   // D-Pad links
    keyMap[ maskToIndex(0x00002) ] = "UP";     // D-Pad hoch
    keyMap[ maskToIndex(0x00004) ] = "RIGHT";  // D-Pad rechts
    keyMap[ maskToIndex(0x00008) ] = "DOWN";   // D-Pad runter
    keyMap[ maskToIndex(0x00100) ] = "i";      // Schalt links hoch  (MyWhoosh)
    keyMap[ maskToIndex(0x00200) ] = "k";      // Schalt links runter
    keyMap[ maskToIndex(0x01000) ] = "i";      // Schalt rechts hoch
    keyMap[ maskToIndex(0x02000) ] = "k";      // Schalt rechts runter
  }
}

void saveMapping(){
  // keyMap unter Mutex in eine lokale Kopie ziehen, dann OHNE Lock in NVS
  // schreiben (Flash-Commit dauert ms — kein Blockieren von handlePresses).
  String snap[32];
  if(mapMutex) xSemaphoreTake(mapMutex, portMAX_DELAY);
  for(int i=0;i<32;i++) snap[i]=keyMap[i];
  if(mapMutex) xSemaphoreGive(mapMutex);
  prefs.begin("zrhid", false, cfgPartInUse);
  for(size_t i=0;i<N_BTN;i++){
    int bit = maskToIndex(BUTTONS[i].mask);
    if(bit<0) continue;
    prefs.putString(BUTTONS[i].id, snap[bit]);
  }
  prefs.end();
  Serial.println("[NVS] Mapping gespeichert.");
}

// --------------------------------------------------------------------
//  Key-Token -> HID-Tastendruck
// --------------------------------------------------------------------
void pressToken(const String& tok){
  if(tok.length()==0) return;
  if(tok=="UP")        bleKeyboard.write(KEY_UP_ARROW);
  else if(tok=="DOWN") bleKeyboard.write(KEY_DOWN_ARROW);
  else if(tok=="LEFT") bleKeyboard.write(KEY_LEFT_ARROW);
  else if(tok=="RIGHT")bleKeyboard.write(KEY_RIGHT_ARROW);
  else if(tok=="SPACE")bleKeyboard.write(' ');
  else if(tok=="ENTER")bleKeyboard.write(KEY_RETURN);
  else if(tok=="ESC")  bleKeyboard.write(KEY_ESC);
  else if(tok=="TAB")  bleKeyboard.write(KEY_TAB);
  else                 bleKeyboard.write(tok[0]);  // einzelnes Zeichen
}

// --------------------------------------------------------------------
//  Protobuf-Mini-Parser für die 0x23-KeyPad-Nachricht
//  Wir brauchen nur Field 1 (ButtonMap, varint). Aufbau der Notification:
//    [0]      = 0x23  (Message-ID, vorangestellt)
//    [1..]    = protobuf: field 1 (tag 0x08) varint = ButtonMap
//               field 2 (tag 0x12) = nested AnalogButtons (ignorieren)
//  Rückgabe: rohe ButtonMap (Ride-Konvention: 0 == gedrückt)
// --------------------------------------------------------------------
bool parseButtonMap(const uint8_t* d, size_t len, uint32_t& outMap){
  if(len<2 || d[0]!=MSG_ID_KEYPAD) return false;
  size_t i=1;
  while(i<len){
    uint8_t tag = d[i++];
    uint8_t field = tag>>3;
    uint8_t wire  = tag&0x07;
    if(field==1 && wire==0){                 // ButtonMap, varint
      uint32_t v=0; int shift=0;
      while(i<len){
        uint8_t b=d[i++];
        v |= (uint32_t)(b&0x7F)<<shift;
        if(!(b&0x80)) break;
        shift+=7;
      }
      outMap=v; return true;
    }else if(wire==0){                        // anderes varint -> überspringen
      while(i<len && (d[i]&0x80)) i++;
      i++;
    }else if(wire==2){                        // length-delimited -> überspringen
      uint32_t l=0; int sh=0;                  // Länge ist selbst ein Varint
      while(i<len){ uint8_t b=d[i++]; l|=(uint32_t)(b&0x7F)<<sh; if(!(b&0x80)) break; sh+=7; }
      if(i+l>len) break;                        // Schutz gegen Buffer-Over-Read
      i+=l;
    }else{ break; }
  }
  return false;
}

// --------------------------------------------------------------------
//  Analog-Hebel (field 3): wiederholtes nested {id, value}. value ist
//  zigzag-kodiert (protobuf sint32) -> -100..100. out wird per id indexiert.
// --------------------------------------------------------------------
void parseAnalog(const uint8_t* d, size_t len, int8_t out[4]){
  out[0]=out[1]=out[2]=out[3]=0;
  if(len<2 || d[0]!=MSG_ID_KEYPAD) return;
  size_t i=1;
  while(i<len){
    uint8_t tag=d[i++], field=tag>>3, wire=tag&0x07;
    if(wire==0){ while(i<len && (d[i]&0x80)) i++; i++; }          // varint überspringen
    else if(wire==2){
      uint32_t l=0; int sh=0;
      while(i<len){ uint8_t b=d[i++]; l|=(uint32_t)(b&0x7F)<<sh; if(!(b&0x80))break; sh+=7; }
      if(i+l>len) break;
      if(field==3){                                                // nested AnalogButton {08 id, 10 value}
        size_t j=i, end=i+l; int id=-1; uint32_t v=0; bool haveV=false;
        while(j<end){
          uint8_t t=d[j++], f=t>>3, w=t&0x07;
          if(w==0){ uint32_t x=0; int s=0;
                    while(j<end){ uint8_t b=d[j++]; x|=(uint32_t)(b&0x7F)<<s; if(!(b&0x80))break; s+=7; }
                    if(f==1) id=(int)x; else if(f==2){ v=x; haveV=true; } }
          else if(w==2){ uint32_t ll=0; int s2=0;
                    while(j<end){ uint8_t b=d[j++]; ll|=(uint32_t)(b&0x7F)<<s2; if(!(b&0x80))break; s2+=7; }
                    j+=ll; }
          else break;
        }
        if(id>=0 && id<4 && haveV){
          int32_t zz = (int32_t)(v>>1) ^ -(int32_t)(v&1);          // zigzag -> signed
          if(zz>100) zz=100; if(zz<-100) zz=-100;
          out[id]=(int8_t)zz;
        }
      }
      i+=l;
    } else break;
  }
}

// --------------------------------------------------------------------
//  Notification-Callback der Ride
// --------------------------------------------------------------------
void onRideSync(NimBLERemoteCharacteristic* c, uint8_t* data, size_t len, bool isNotify){
  hexDump("Sync-Antwort (0004)", data, len);   // beginnt mit "RideOn" (52 69 64 65 4f 6e)
}

void onRideNotify(NimBLERemoteCharacteristic* c, uint8_t* data, size_t len, bool isNotify){
  uint32_t n = ++rideRxCount;
#if BLE_DEBUG
  // Nach Message-ID einordnen. 0x23 = Button-Status (bei jedem Druck/Loslassen,
  // daher immer loggen); alles andere sind Status-/Info-Pakete (Seriennummer,
  // Batterie ...) -> nur die ersten 20 loggen, sonst Log-Flut.
  uint32_t dummy;
  bool ok = parseButtonMap(data, len, dummy);
  if(len>0 && data[0]==MSG_ID_KEYPAD){
    hexDump(ok ? "BUTTONS 0x23" : "BUTTONS 0x23 (nicht parsebar)", data, len);
  }else{
    char tag[48]; snprintf(tag, sizeof tag, "Status #%u id=0x%02x", (unsigned)n, len?data[0]:0);
    hexDump(tag, data, len);
  }
#endif
  uint32_t raw;
  if(!parseButtonMap(data, len, raw)) return;
  // Ride: 0 == gedrückt -> invertieren; nur digitale Bits (0..23).
  uint32_t pressed = (~raw) & DIGITAL_MASK;
  // Analog-Hebel (field 3) -> virtuelle Buttons (Bits 24..27) per Schwellwert.
  int8_t an[4]; parseAnalog(data, len, an);
  if(an[0] <= -STEER_THRESH) pressed |= 0x1000000;   // Hebel links  ◄
  if(an[0] >=  STEER_THRESH) pressed |= 0x2000000;   // Hebel links  ►
  if(an[1] <= -STEER_THRESH) pressed |= 0x4000000;   // Hebel rechts ◄
  if(an[1] >=  STEER_THRESH) pressed |= 0x8000000;   // Hebel rechts ►
  lastPressedMask = pressed;
}

// --------------------------------------------------------------------
//  Verarbeitung der Tastendrücke (Flankenerkennung) im loop()
// --------------------------------------------------------------------
void handlePresses(){
  uint32_t now  = lastPressedMask;
  uint32_t rising = now & ~prevPressedMask;   // neu gedrückt
  if(rising){
    for(size_t k=0;k<N_BTN;k++){
      if(rising & BUTTONS[k].mask){
        int bit=maskToIndex(BUTTONS[k].mask);
        if(bit>=0 && bleKeyboard.isConnected()){
          // keyMap kann vom WebServer-Task (setKeyFor) verändert werden ->
          // unter Mutex in eine lokale Kopie ziehen, dann erst senden.
          String tok;
          if(mapMutex) xSemaphoreTake(mapMutex, portMAX_DELAY);
          tok = keyMap[bit];
          if(mapMutex) xSemaphoreGive(mapMutex);
          pressToken(tok);
        }
      }
    }
  }
  if(now != prevPressedMask){
    prevPressedMask = now;
    broadcastButtons(now);                    // Live-Anzeige aktualisieren
  }
}

// --------------------------------------------------------------------
//  WebSocket: Live-Daten an Browser
// --------------------------------------------------------------------
void broadcastButtons(uint32_t mask){
  String names="[";
  bool first=true;
  for(size_t k=0;k<N_BTN;k++){
    if(mask & BUTTONS[k].mask){
      if(!first) names+=",";
      names+="\""; names+=BUTTONS[k].id; names+="\"";
      first=false;
    }
  }
  names+="]";
  String msg = "{\"t\":\"btn\",\"mask\":"+String(mask)+",\"names\":"+names+"}";
  wsLive.textAll(msg);
}

// Kleiner OUI->Hersteller-Lookup. Greift nur bei PUBLIC-Adressen — BLE-Hosts
// wie iPhone/iPad/Apple TV nutzen aus Datenschutz meist ZUFÄLLIGE Adressen,
// die sich nicht auflösen lassen (dann zeigen wir "random").
static const char* ouiVendor(const String& oui){
  struct Map { const char* p; const char* v; };
  static const Map T[] = {
    {"DC:A6:32","Raspberry Pi"}, {"B8:27:EB","Raspberry Pi"}, {"E4:5F:01","Raspberry Pi"},
    {"24:0A:C4","Espressif"},    {"A0:B7:65","Espressif"},    {"7C:DF:A1","Espressif"},
    {"3C:22:FB","Apple"},        {"A8:51:AB","Apple"},        {"F0:18:98","Apple"},
    {"FC:FB:FB","Samsung"},      {"00:1A:11","Google"},       {"00:50:F2","Microsoft"},
  };
  for(const auto& e : T) if(oui == e.p) return e.v;
  return nullptr;
}

// Label des verbundenen HID-Hosts: best effort. Sprachneutral/Englisch, da das
// UI primär Englisch ist und die Firmware die Sprache nicht kennt.
String hidPeerLabel(){
  NimBLEServer* srv = NimBLEDevice::getServer();
  if(!srv || srv->getConnectedCount()==0) return "";
  NimBLEAddress a = srv->getPeerInfo(0).getAddress();
  String mac = a.toString().c_str(); mac.toUpperCase();
  if(a.getType() != 0) return String("random · ") + mac;   // 0 = public, sonst zufällig
  const char* v = ouiVendor(mac.substring(0,8));
  return v ? (String(v) + " · " + mac) : mac;
}

void broadcastState(){
  bool hid = bleKeyboard.isConnected();
  String hidDev = hid ? hidPeerLabel() : "";
  // rideDevName ist ein String, der vom BLE-Task verändert wird -> Snapshot
  // unter Mutex ziehen (kein Reallocation-Race beim Lesen).
  String rideDev;
  if(mapMutex) xSemaphoreTake(mapMutex, portMAX_DELAY);
  rideDev = rideConnected ? rideDevName : "";
  if(mapMutex) xSemaphoreGive(mapMutex);
  String msg = "{\"t\":\"state\",\"ride\":";
  msg += rideConnected ? "true":"false";
  msg += ",\"hid\":";
  msg += hid ? "true":"false";
  msg += ",\"ip\":\""+WiFi.softAPIP().toString()+"\"";
  msg += ",\"rideDev\":\""+jsonEsc(rideDev)+"\"";
  msg += ",\"hidDev\":\""+jsonEsc(hidDev)+"\"";
  msg += ",\"bat\":"+String((int)rideBattery);
  msg += "}";
  wsLive.textAll(msg);
}

void sendMapping(AsyncWebSocketClient* client){
  String snap[32];
  if(mapMutex) xSemaphoreTake(mapMutex, portMAX_DELAY);
  for(int i=0;i<32;i++) snap[i]=keyMap[i];
  if(mapMutex) xSemaphoreGive(mapMutex);
  String m = "{\"t\":\"map\",\"map\":{";
  bool first=true;
  for(size_t k=0;k<N_BTN;k++){
    int bit=maskToIndex(BUTTONS[k].mask);
    if(bit<0) continue;
    if(!first) m+=",";
    m+="\""; m+=BUTTONS[k].id; m+="\":\""+snap[bit]+"\"";
    first=false;
  }
  m+="}}";
  if(client) client->text(m); else wsLive.textAll(m);
}

void setKeyFor(const String& btnId, const String& key){
  for(size_t k=0;k<N_BTN;k++){
    if(btnId==BUTTONS[k].id){
      int bit=maskToIndex(BUTTONS[k].mask);
      if(bit>=0){
        if(mapMutex) xSemaphoreTake(mapMutex, portMAX_DELAY);
        keyMap[bit]=key;
        if(mapMutex) xSemaphoreGive(mapMutex);
      }
      return;
    }
  }
}

// Maschinenlesbarer Status als JSON (für GET /status — headless-Healthcheck).
String statusJson(){
  bool hid = bleKeyboard.isConnected();
  String hidDev = hid ? hidPeerLabel() : "";
  String rideDev;
  if(mapMutex) xSemaphoreTake(mapMutex, portMAX_DELAY);
  rideDev = rideConnected ? rideDevName : "";
  if(mapMutex) xSemaphoreGive(mapMutex);
  String j = "{\"fw\":\"" FW_VERSION "\"";
  j += ",\"ride\":";     j += rideConnected ? "true":"false";
  j += ",\"hid\":";      j += hid ? "true":"false";
  j += ",\"rideDev\":\""+jsonEsc(rideDev)+"\"";
  j += ",\"hidDev\":\""+jsonEsc(hidDev)+"\"";
  j += ",\"bat\":"+String((int)rideBattery);
  j += ",\"ip\":\""+WiFi.softAPIP().toString()+"\"";
  j += ",\"uptime_s\":"+String(millis()/1000);
  j += ",\"heap\":"+String((uint32_t)ESP.getFreeHeap());
  j += "}";
  return j;
}

void onWsEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
               AwsEventType type, void* arg, uint8_t* data, size_t len){
  if(type==WS_EVT_CONNECT){
    broadcastState();
    sendMapping(client);
  }else if(type==WS_EVT_DATA){
    AwsFrameInfo* info=(AwsFrameInfo*)arg;
    if(!(info->final && info->index==0 && info->len==len)) return;
    String s; s.reserve(len+1);
    for(size_t i=0;i<len;i++) s+=(char)data[i];
    // sehr einfacher JSON-Feldzugriff (kein Full-Parser nötig)
    auto field=[&](const char* key)->String{
      String pat=String("\"")+key+"\":\"";
      int a=s.indexOf(pat); if(a<0) return "";
      a+=pat.length(); int b=s.indexOf('"',a);
      return s.substring(a,b);
    };
    String t=field("t");
    if(t=="getmap"){ sendMapping(client); }
    else if(t=="save"){ saveMapping(); }
    else if(t=="setmap"){
      setKeyFor(field("btn"), field("key"));
    }
  }
}

// --------------------------------------------------------------------
//  BLE Central: Scan + Verbindung zur Ride
// --------------------------------------------------------------------
class ScanCB : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    String name = dev->getName().c_str();
    // Ride heißt im BLE "Zwift SF2" (NICHT "Zwift Ride") -> breit auf "Zwift" matchen.
    if(name.indexOf("Zwift")>=0){
      Serial.printf("[BLE] Ride gefunden: '%s' (%s)\n",
                    name.c_str(), dev->getAddress().toString().c_str());
      if(foundRide){ delete foundRide; }
      foundRide = new NimBLEAdvertisedDevice(*dev);
      NimBLEDevice::getScan()->stop();
      wantReconnect = true;
    }
  }
};

class ClientCB : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient* c) override { Serial.println("[BLE] verbunden."); }
  void onConnectFail(NimBLEClient* c, int reason) override {
    Serial.printf("[BLE] Verbindungsaufbau fehlgeschlagen (reason %d)\n", reason);
  }
  void onAuthenticationComplete(NimBLEConnInfo& info) override {
    Serial.printf("[BLE] Pairing/Verschlüsselung: encrypted=%d authenticated=%d bonded=%d\n",
                  info.isEncrypted(), info.isAuthenticated(), info.isBonded());
  }
  void onDisconnect(NimBLEClient* c, int reason) override {
    Serial.printf("[BLE] getrennt (reason %d). Scanne neu...\n", reason);
    rideConnected=false; rideMeasure=nullptr; rideControl=nullptr; rideSync=nullptr;
    rideBattery=-1;
    if(mapMutex) xSemaphoreTake(mapMutex, portMAX_DELAY);
    rideDevName="";
    if(mapMutex) xSemaphoreGive(mapMutex);
    stateDirty=true;   // WS-Broadcast aus loop() anstoßen (nicht aus dem BLE-Task)
    NimBLEDevice::getScan()->start(0, false, true);
  }
};

// Sucht in allen Services nach: einer notify- und einer write-Characteristic.
// Die Zwift-Ride nutzt eine herstellereigene Service-UUID; wir greifen die
// erste notify-fähige (Measurement) und erste write-fähige (Control) ab.
bool findZwiftChars(NimBLEClient* c){
  auto services = c->getServices(true);
  Serial.printf("[BLE] Discovery: %d Service(s)\n", (int)services.size());
  NimBLERemoteCharacteristic *notifyCh=nullptr, *indicateCh=nullptr, *writeCh=nullptr;
  for(auto* svc : services){
    Serial.printf("  SVC %s\n", svc->getUUID().toString().c_str());
    auto chars = svc->getCharacteristics(true);
    for(auto* ch : chars){
      // Properties: N=notify I=indicate W=write(resp) w=writeNoResp R=read
      Serial.printf("    CHR %s  [%s%s%s%s%s]\n", ch->getUUID().toString().c_str(),
                    ch->canNotify()?"N":"-", ch->canIndicate()?"I":"-",
                    ch->canWrite()?"W":"-", ch->canWriteNoResponse()?"w":"-",
                    ch->canRead()?"R":"-");
      if(!notifyCh   && ch->canNotify())                              notifyCh   = ch;
      if(!indicateCh && ch->canIndicate())                           indicateCh = ch;
      if(!writeCh    && (ch->canWrite()||ch->canWriteNoResponse()))  writeCh    = ch;
    }
  }
  // Bevorzugt die bekannten Zwift-UUIDs (Service 0xFC82). Die generische Suche
  // ("erste schreibbare Char") greift sonst GAP 0x2a00 (Device Name) und der
  // RideOn-Handshake landet im Nirwana -> Gerät sendet nie Button-Daten.
  NimBLERemoteService* zsvc = c->getService(NimBLEUUID((uint16_t)0xFC82));
  if(zsvc){
    NimBLERemoteCharacteristic* a = zsvc->getCharacteristic(NimBLEUUID(ZW_UUID_ASYNC));
    NimBLERemoteCharacteristic* w = zsvc->getCharacteristic(NimBLEUUID(ZW_UUID_CTRL));
    NimBLERemoteCharacteristic* y = zsvc->getCharacteristic(NimBLEUUID(ZW_UUID_SYNC));
    Serial.printf("[BLE] Zwift-Service 0xFC82: async=%s ctrl=%s sync=%s\n",
                  a?"ok":"FEHLT", w?"ok":"FEHLT", y?"ok":"FEHLT");
    if(a) notifyCh = a;
    if(w) writeCh  = w;
    rideSync = y;
  }else{
    Serial.println("[BLE] WARNUNG: Service 0xFC82 nicht gefunden — Fallback auf generische Auswahl (unzuverlässig).");
    rideSync = nullptr;
  }
  // Button-Daten (0x23) kommen per Notify (ASYNC); Indicate nur als Fallback.
  rideMeasure = notifyCh ? notifyCh : indicateCh;
  rideControl = writeCh;
  Serial.printf("[BLE] gewählt: measure=%s (%s), control=%s\n",
    rideMeasure?rideMeasure->getUUID().toString().c_str():"-",
    rideMeasure?(rideMeasure->canNotify()?"notify":"indicate"):"-",
    rideControl?rideControl->getUUID().toString().c_str():"-");
  return rideMeasure && rideControl;
}

// Akkustand über den Standard-Battery-Service (0x180f / 0x2a19): einmal lesen,
// dann per Notify aktuell halten. Bei Click V2 meldet die linke Click ihren
// eigenen Stand; ob die rechte (weitergeleitete) einen eigenen liefert, ist offen.
static void onBatteryNotify(NimBLERemoteCharacteristic* c, uint8_t* data, size_t len, bool isNotify){
  if(len<1) return;
  rideBattery = data[0]; stateDirty = true;   // WS-Broadcast aus loop()
}
static void setupBattery(NimBLEClient* c){
  NimBLERemoteService* bs = c->getService(NimBLEUUID((uint16_t)0x180F));
  NimBLERemoteCharacteristic* bc = bs ? bs->getCharacteristic(NimBLEUUID((uint16_t)0x2A19)) : nullptr;
  if(!bc){ Serial.println("[BLE] Kein Battery-Service."); return; }
  if(bc->canRead()){
    NimBLEAttValue v = bc->readValue();
    if(v.size()>=1) rideBattery = v.data()[0];
  }
  if(bc->canNotify()) bc->subscribe(true, onBatteryNotify);
  Serial.printf("[BLE] Akku: %d %%\n", (int)rideBattery);
}

bool connectRide(){
  // Ownership atomar übernehmen: der ScanCB (BLE-Task) könnte foundRide sonst
  // mitten im Connect überschreiben/löschen -> lokal rausziehen und nullen.
  NimBLEAdvertisedDevice* dev = foundRide;
  foundRide = nullptr;
  if(!dev) return false;
  Serial.println("[BLE] verbinde zur Ride...");
  if(rideClient){ NimBLEDevice::deleteClient(rideClient); rideClient=nullptr; }
  rideClient = NimBLEDevice::createClient();
  rideClient->setClientCallbacks(new ClientCB(), false);
  if(!rideClient->connect(dev)){
    Serial.println("[BLE] connect() fehlgeschlagen.");
    delete dev;
    return false;
  }
  rideMeasure=nullptr; rideControl=nullptr;  // alte Pointer eines früheren Clients verwerfen
  if(!findZwiftChars(rideClient)){
    Serial.println("[BLE] keine passenden Characteristics gefunden.");
    rideClient->disconnect();
    delete dev;
    return false;
  }
  rideRxCount = 0;
  Serial.printf("[BLE] MTU=%u\n", (unsigned)rideClient->getMTU());
  // 1) Zuerst die Antwortkanäle abonnieren (Sync-Indicate + Button-Notify),
  //    damit keine Antwort auf den Handshake verloren geht.
  if(rideSync){
    bool ok = rideSync->subscribe(rideSync->canNotify(), onRideSync);
    Serial.printf("[BLE] subscribe Sync (0004): %s\n", ok?"ok":"FEHLER");
  }
  bool subOk = rideMeasure->subscribe(rideMeasure->canNotify(), onRideNotify);
  Serial.printf("[BLE] subscribe Measure (0002): %s\n", subOk?"ok":"FEHLER");
  setupBattery(rideClient);
  delay(100);
  // 2) RideOn-Handshake an den Control Point — mit Response nur, wenn unterstützt.
  bool useRsp = rideControl->canWrite();
  bool wOk = rideControl->writeValue((const uint8_t*)RIDEON_MAGIC, 6, useRsp);
  Serial.printf("[BLE] RideOn -> %s (%s): %s\n", rideControl->getUUID().toString().c_str(),
                useRsp?"write":"write-no-response", wOk?"ok":"FEHLER");
  rideConnectMs = millis();
  // Gerätenamen + Adresse für die UI-Anzeige merken (String -> unter Mutex)
  String nm = dev->getName().c_str();
  if(nm.isEmpty()) nm = "Zwift Ride";
  nm += " (" + String(rideClient->getPeerAddress().toString().c_str()) + ")";
  if(mapMutex) xSemaphoreTake(mapMutex, portMAX_DELAY);
  rideDevName = nm;
  if(mapMutex) xSemaphoreGive(mapMutex);
  delete dev;                              // nicht mehr gebraucht — kein Leak/UAF
  rideConnected = true;
  broadcastState();                         // connectRide läuft im loop()-Kontext
  Serial.println("[BLE] Ride aktiv — Buttons werden gelesen.");
  return true;
}

// --------------------------------------------------------------------
//  Setup
// --------------------------------------------------------------------
void setup(){
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== RideToWooshHID " FW_VERSION " startet ===");

  mapMutex = xSemaphoreCreateMutex();   // schützt keyMap[] (Loop vs. WebServer-Task)
  initConfigStore();                    // getrennte 'cfg'-NVS-Partition vorbereiten
  loadMapping();

  // 1) BLE-HID-Tastatur starten (Peripheral-Rolle)
  bleKeyboard.begin();
  Serial.println("[HID] Tastatur-Advertising aktiv — am Tablet/PC koppeln.");

  // 2) BLE-Central für die Ride. NimBLE teilt sich den Controller.
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(new ScanCB(), false);
  scan->setActiveScan(true);
  // Koexistenz mit WiFi-AP: NICHT dauerscannen (Einheiten 0.625 ms).
  // 160/48 = 100 ms Intervall, 30 ms Fenster -> ~30% Funk-Duty, damit der
  // WiFi-AP Association/DHCP beantworten kann (sonst Verbindungs-Timeout).
  scan->setInterval(160);
  scan->setWindow(48);
  scan->start(0, false, true);
  Serial.println("[BLE] Scanne nach 'Zwift Ride'...");

  // 3) WiFi Accesspoint + Webserver
  //
  // Sicherheit: Der AP dient nur zur lokalen Konfiguration (Tasten-Mapping,
  // keine sensiblen Daten) und das Web-UI läuft über HTTP. WPA2-PSK ist hier
  // ausreichend und vor allem maximal kompatibel — manche iPads/Apple-TV-
  // Generationen verbinden sich mit WPA3-only nur widerwillig.
  //
  // Optional härter (ESP32 Arduino-Core 3.x): WPA2/WPA3-Transition. Dann die
  // Zeile unten durch die auskommentierte Variante ersetzen. Auf Core 2.x
  // existiert der auth_mode-Parameter nicht — dort bei der einfachen Form bleiben.
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);                       // WPA2-PSK (Default)
  // WiFi.softAP(AP_SSID, AP_PASS, 1, 0, 4, false,    // WPA2/WPA3-Mixed (Core 3.x)
  //             WIFI_AUTH_WPA2_WPA3_PSK);
  WiFi.setTxPower(AP_TX_POWER);
  Serial.printf("[WiFi] AP '%s' — http://%s\n", AP_SSID, WiFi.softAPIP().toString().c_str());

  httpServer.on("/", HTTP_GET, [](AsyncWebServerRequest* r){
    r->send(200,"text/html", INDEX_HTML);
  });
  // i18n-Sprachdateien (werden vom UI per fetch geladen, EN ist Default)
  httpServer.on("/i18n/en.json", HTTP_GET, [](AsyncWebServerRequest* r){
    r->send(200,"application/json; charset=utf-8", LANG_EN_JSON);
  });
  httpServer.on("/i18n/de.json", HTTP_GET, [](AsyncWebServerRequest* r){
    r->send(200,"application/json; charset=utf-8", LANG_DE_JSON);
  });
  // Maschinenlesbarer Status ohne WebSocket (Healthcheck/Skripte)
  httpServer.on("/status", HTTP_GET, [](AsyncWebServerRequest* r){
    r->send(200, "application/json", statusJson());
  });

  // WebSocket auf DEMSELBEN Server (Port 80) unter /ws — spart einen Server + Task
  wsLive.onEvent(onWsEvent);
  httpServer.addHandler(&wsLive);
  httpServer.begin();
}

// --------------------------------------------------------------------
//  Loop
// --------------------------------------------------------------------
uint32_t lastState=0;
void loop(){
  if(wantReconnect){
    wantReconnect=false;
    // Scheitert der Connect, Scan wieder anwerfen (sonst hängt der Reconnect).
    if(!connectRide()) NimBLEDevice::getScan()->start(0, false, true);
  }
  if(stateDirty){              // vom BLE-Task angestoßen (z.B. Disconnect)
    stateDirty=false;
    broadcastState();          // WS-Send nur aus loop()-Kontext
  }
  handlePresses();

#if BLE_DEBUG
  // Handshake-Watchdog: verbunden, aber keine Daten -> Diagnose ausgeben.
  static bool warned=false;
  if(!rideConnected) warned=false;
  else if(!warned && rideRxCount==0 && millis()-rideConnectMs>HANDSHAKE_TIMEOUT_MS){
    warned=true;
    Serial.printf("[BLE] WARNUNG: %u ms nach RideOn keine Daten. connected=%d sync=%s\n",
                  (unsigned)HANDSHAKE_TIMEOUT_MS, rideClient?rideClient->isConnected():0,
                  rideSync?"vorhanden":"fehlt");
    Serial.println("[BLE]   -> Keine 0004-Antwort: Gerät verlangt evtl. anderen/verschlüsselten Handshake (Click V2).");
    Serial.println("[BLE]   -> Zwift-App/anderes Gerät noch mit der Click verbunden? Click erlaubt nur 1 Verbindung.");
  }
#endif

  // Statusupdate ~2x/s (HID-Connect kann sich ändern)
  if(millis()-lastState>500){
    lastState=millis();
    static bool lastHid=false; static bool lastRide=false;
    if(bleKeyboard.isConnected()!=lastHid || rideConnected!=lastRide){
      lastHid=bleKeyboard.isConnected(); lastRide=rideConnected;
      broadcastState();
    }
  }
  wsLive.cleanupClients();
  delay(5);
}
