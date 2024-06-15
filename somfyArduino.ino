#include <EEPROM.h>
#define PORT_TX 5

#define SYMBOL 640
#define HAUT 0x2
#define STOP 0x1
#define BAS 0x4
#define PROG 0x8
#define EEPROM_ADDRESS 0
#define NUMBER_REMOTE 6

#define VERSION 1

byte frame[7];
byte checksum;

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
  if (Serial.available())
  {
    String data = "";
    
    while (Serial.available())
    {
      char c = Serial.read();
      data += String(c);
      delay(10);
    }
 
    getData(data);
  }
}
 
void getData(String data) {

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
