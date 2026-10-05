#include <SPI.h>
#include <MFRC522.h>

#define SS_PIN   5
#define RST_PIN  22
#define BLOC     4      // bloc de donnees (secteur 1). Ne jamais utiliser 0, 3, 7, 11, ...

MFRC522 rfid(SS_PIN, RST_PIN);
MFRC522::MIFARE_Key key;

void setup() {
  Serial.begin(115200);
  SPI.begin();          // SCK 18, MISO 19, MOSI 23
  rfid.PCD_Init();
  for (byte i = 0; i < 6; i++) key.keyByte[i] = 0xFF;  // cle usine

  Serial.println(F("Pret."));
  Serial.println(F("  r          -> lire le bloc"));
  Serial.println(F("  w <texte>  -> ecrire (16 car. max)"));
}

String commande = "";

void loop() {
  if (Serial.available()) {
    commande = Serial.readStringUntil('\n');
    commande.trim();
    Serial.println("> " + commande + " : approche le badge");
  }

  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;

  Serial.print(F("UID : "));
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) Serial.print('0');
    Serial.print(rfid.uid.uidByte[i], HEX);
    Serial.print(' ');
  }
  Serial.println();
  Serial.print(F("Type : "));
  Serial.println(rfid.PICC_GetTypeName(rfid.PICC_GetType(rfid.uid.sak)));

  if (commande == "r") {
    lireBloc();
  } else if (commande.startsWith("w ")) {
    ecrireBloc(commande.substring(2));
  }

  commande = "";
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}

bool authentifier() {
  MFRC522::StatusCode s = rfid.PCD_Authenticate(
      MFRC522::PICC_CMD_MF_AUTH_KEY_A, BLOC, &key, &(rfid.uid));
  if (s != MFRC522::STATUS_OK) {
    Serial.print(F("Auth KO : "));
    Serial.println(rfid.GetStatusCodeName(s));
    return false;
  }
  return true;
}

void lireBloc() {
  if (!authentifier()) return;

  byte buf[18];
  byte taille = sizeof(buf);
  MFRC522::StatusCode s = rfid.MIFARE_Read(BLOC, buf, &taille);
  if (s != MFRC522::STATUS_OK) {
    Serial.print(F("Lecture KO : "));
    Serial.println(rfid.GetStatusCodeName(s));
    return;
  }

  Serial.print(F("Hex   : "));
  for (byte i = 0; i < 16; i++) {
    if (buf[i] < 0x10) Serial.print('0');
    Serial.print(buf[i], HEX);
    Serial.print(' ');
  }
  Serial.print(F("\nTexte : "));
  for (byte i = 0; i < 16; i++)
    Serial.write(buf[i] >= 32 && buf[i] < 127 ? buf[i] : '.');
  Serial.println();
}

void ecrireBloc(String texte) {
  if (!authentifier()) return;

  byte buf[16] = {0};
  texte.getBytes(buf, min((unsigned int)17, texte.length() + 1));

  MFRC522::StatusCode s = rfid.MIFARE_Write(BLOC, buf, 16);
  if (s != MFRC522::STATUS_OK) {
    Serial.print(F("Ecriture KO : "));
    Serial.println(rfid.GetStatusCodeName(s));
    return;
  }
  Serial.println(F("Ecriture OK"));
  lireBloc();
}
