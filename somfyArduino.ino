#include <EEPROM.h>
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>

#define PORT_TX 5
#define SYMBOL 640
#define HAUT 0x2
#define STOP 0x1
#define BAS 0x4
#define PROG 0x8
#define EEPROM_ADDRESS 0
#define NUMBER_REMOTE 6
#define VERSION 1
#define PIN_LED_VERTE 5
#define PIN_LED_ROUGE 14
#define PIN_BP 4
#define PIN_RELAY 13

byte frame[7];
byte checksum;

const char * SSID = "Ckocinelle_Power_2G";
const char * PASSWORD = "ckocinelle";

void onConnected(const WiFiEventStationModeConnected& event);
void onGotIP(const WiFiEventStationModeGotIP& event);

ESP8266WebServer webServer(80);

struct Remote
{
  unsigned long remoteID;
  unsigned int rollingCode;
};

struct SomfyController
{
  int appVersion;
  Remote remotes[NUMBER_REMOTE];
};

SomfyController somfyControllers;
Remote newRemotes [NUMBER_REMOTE] = {
  {0x123451, 0},
  {0x123452, 0},
  {0x123453, 0},
  {0x123454, 0},
  {0x123455, 0},
  {0x123456, 0},
};

void BuildFrame(unsigned long remoteID, unsigned int rollingCode, byte *frame, byte button);
void SendCommand(byte *frame, byte sync);

void setup()
{
  ////// SERVER
  // Definition de la liaison serie
  Serial.begin(115200L);
  Serial.println(" ");

  // Declaration du mode des pins
  // pinMode(PIN_LED_VERTE, OUTPUT);
  // pinMode(PIN_LED_ROUGE, OUTPUT);
  // pinMode(PIN_RELAY, OUTPUT);
  // pinMode(PIN_RELAY_2, INPUT);
  // pinMode(PIN_BP, INPUT_PULLUP);

  // // initialisation des positions
  // digitalWrite(PIN_LED_VERTE, LOW);
  // digitalWrite(PIN_LED_ROUGE, LOW);
  // digitalWrite(PIN_RELAY,HIGH);

  // Definition de l'adresse IP fixe
  IPAddress ip(192,168,0,201);
  IPAddress gateway(192,168,0,254);
  IPAddress subnet(255,255,255,0);
  IPAddress dns(192,168,0,254);

  // Connexion WiFi
  WiFi.mode(WIFI_STA);
  WiFi.softAP("somfyByArduino");
  WiFi.config(ip,gateway,subnet,dns);
  WiFi.begin(SSID,PASSWORD);
  static WiFiEventHandler onConnectedHandler = WiFi.onStationModeConnected(onConnected);
  static WiFiEventHandler onGotIPHandler = WiFi.onStationModeGotIP(onGotIP);

  // Demerrage et mise en place de serveur web
  webServer.on("/somfy/volet/chambre",POST,piloteChambre);
  webServer.on("/",handleRoot);
  webServer.enableCORS(true);
  webServer.begin();

  ////// SOMFY
  Serial.begin(115200);
  DDRD |= 1<<PORT_TX;
  PORTD &= !(1<<PORT_TX);

  EEPROM.get(EEPROM_ADDRESS, somfyControllers);

  if (somfyControllers.appVersion < VERSION)
  {
    Serial.println("La version de l'application en mémoire n'est pas dans la bonne version ou la mémoire est vide");
    somfyControllers.appVersion = VERSION;
    memcpy(&somfyControllers.remotes, &newRemotes, sizeof(newRemotes));
    EEPROM.put(EEPROM_ADDRESS, somfyControllers);
  }

  for (int i = 0; i < (sizeof(somfyControllers.remotes) / sizeof(Remote)); i++)
  {
    Remote currentRemote = somfyControllers.remotes[i];
    Serial.print("Commande ["); Serial.print(i); Serial.println("]");
    Serial.print("\tID de la commande : "); Serial.println(currentRemote.remoteID, HEX);
    Serial.print("\tCompteur actuel : "); Serial.println(currentRemote.rollingCode);
  }
}

void loop()
{
  if(WiFi.isConnected()){
    webServer.handleClient();
    getData();
  }

}

void getData() {
    String data = "";
    char serie = webServer.args("serie");
    char numeroChambre = webServer.args("numeroChambre");
    data += String(serie);
    data += String(numeroChambre);
    delay(10);

  char serie = data[0];

  for (int i = 1; i < data.length(); i++)
  {
    char cRemotePosition = data[i];

    int remotePosition = cRemotePosition - '0';
    Serial.print("Commande "); Serial.println(remotePosition);
    Remote remote = somfyControllers.remotes[remotePosition];
    unsigned long remoteID = remote.remoteID;
    unsigned int rollingCode = remote.rollingCode;

    Serial.println("");
    if (serie == 'm') {
      Serial.println("Monte");
      BuildFrame(remoteID, rollingCode, frame, HAUT);
    }
    else if (serie == 's') {
      Serial.println("Stop");
      BuildFrame(remoteID, rollingCode, frame, STOP);
    }
    else if (serie == 'd') {
      Serial.println("Descend");
      BuildFrame(remoteID, rollingCode, frame, BAS);
    }
    else if (serie == 'p') {
      Serial.println("Prog");
      BuildFrame(remoteID, rollingCode, frame, PROG);
    }
    else {
      Serial.println("Code custom");
      BuildFrame(remoteID, rollingCode, frame, serie);
    }

    Serial.println("");
    SendCommand(frame, 2);
    for (int i = 0; i < 2; i++) {
      SendCommand(frame, 7);
    }

    //Incrémente le compteur et le sauvegarde en mémoire
    somfyControllers.remotes[remotePosition].rollingCode++;
    EEPROM.put(EEPROM_ADDRESS, somfyControllers);
  }
}

void BuildFrame(unsigned long remoteID, unsigned int rollingCode, byte *frame, byte button)
{
  frame[0] = 0xA7;
  frame[1] = button << 4;
  frame[2] = rollingCode >> 8;
  frame[3] = rollingCode;
  frame[4] = remoteID >> 16;
  frame[5] = remoteID >>  8;
  frame[6] = remoteID;

  Serial.print("Frame         : ");
  for (byte i = 0; i < 7; i++)
  {
    if (frame[i] >> 4 == 0)
    {
      Serial.print("0");
    }
    Serial.print(frame[i], HEX); Serial.print(" ");
  }

  checksum = 0;
  for (byte i = 0; i < 7; i++)
  {
    checksum = checksum ^ frame[i] ^ (frame[i] >> 4);
  }
  checksum &= 0b1111;

  frame[1] |= checksum;


  Serial.println(""); Serial.print("Avec checksum : ");
  for (byte i = 0; i < 7; i++)
  {
    if (frame[i] >> 4 == 0)
    {
      Serial.print("0");
    }
    Serial.print(frame[i], HEX); Serial.print(" ");
  }

  for (byte i = 1; i < 7; i++)
  {
    frame[i] ^= frame[i-1];
  }

  Serial.println(""); Serial.print("Obfuscation    : ");
  for (byte i = 0; i < 7; i++)
  {
    if (frame[i] >> 4 == 0)
    {
      Serial.print("0");
    }
    Serial.print(frame[i], HEX); Serial.print(" ");
  }
  Serial.println("");
  Serial.print("Compteur  : "); Serial.println(rollingCode);
}

void SendCommand(byte *frame, byte sync)
{
  if (sync == 2)
  {
    PORTD |= 1<<PORT_TX;
    delayMicroseconds(9415);
    PORTD &= !(1<<PORT_TX);
    delayMicroseconds(89565);
  }

  for (int i = 0; i < sync; i++)
  {
    PORTD |= 1<<PORT_TX;
    delayMicroseconds(4*SYMBOL);
    PORTD &= !(1<<PORT_TX);
    delayMicroseconds(4*SYMBOL);
  }

  PORTD |= 1<<PORT_TX;
  delayMicroseconds(4550);
  PORTD &= !(1<<PORT_TX);
  delayMicroseconds(SYMBOL);

  for (byte i = 0; i < 56; i++)
  {
    if (((frame[i/8] >> (7 - (i%8))) & 1) == 1)
    {
      PORTD &= !(1<<PORT_TX);
      delayMicroseconds(SYMBOL);
      PORTD ^= 1<<PORT_TX;
      delayMicroseconds(SYMBOL);
    }
    else
  {
      PORTD |= (1<<PORT_TX);
      delayMicroseconds(SYMBOL);
      PORTD ^= 1<<PORT_TX;
      delayMicroseconds(SYMBOL);
    }
  }

  PORTD &= !(1<<PORT_TX);
  delayMicroseconds(30415);
}

void onConnected(const WiFiEventStationModeConnected& event){
  Serial.println("Wifi connecte");
}

void onGotIP(const WiFiEventStationModeGotIP& event){
  Serial.println("Adresse IP : "+WiFi.localIP().toString());
  Serial.println("Adresse IP Passerelle : "+WiFi.gatewayIP().toString());
  Serial.println("Adresse IP DNS : "+WiFi.dnsIP().toString());
  Serial.print("Puissance du signal : ");
  Serial.println(WiFi.RSSI());
}

void handleRoot(){
  String reponse = "That's work.";
  sendResponse(reponse);
}

void piloteChambre(){
  
}

void setLedOn(){
  digitalWrite(PIN_LED_VERTE, HIGH);
  digitalWrite(PIN_RELAY,LOW);
  t1=millis();
  Serial.println("t1: "+ (String)t1 + "ms");
  sendResponse("1");
}

void setLedOff(){
  digitalWrite(PIN_LED_VERTE, LOW);
  digitalWrite(PIN_RELAY,HIGH);
  Serial.println("t2: "+ (String)t2 + "ms");
  OPEN=false;
  sendResponse("0");
}

// void setLedTemp(){
//     t1=0L;
//     t2=0L;
//     setLedOn();
//     OPEN=true;
// }

void sendResponse(String value){
  webServer.sendHeader("Access-Control-Max-Age", "10000");
  webServer.sendHeader("Access-Control-Allow-Methods", "PUT,POST,GET,OPTIONS");
  webServer.sendHeader("Access-Control-Allow-Headers", "*");
  webServer.send(200,"text/plain",value);
}
