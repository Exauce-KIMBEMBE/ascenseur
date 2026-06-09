#include <WiFi.h>
#include <WebServer.h>
#include <SPI.h>
#include <SPIFFS.h>
#include <MFRC522.h>
#include <Preferences.h>
#include <B_Stepper.h>


// =====================
// WIFI AP
// =====================
const char* ssid = "Ascenseur_ESP32";
const char* password = "12345678";

WebServer server(80);
Preferences prefs;

// =====================
// MOTEUR
// =====================
#define IN1 21
#define IN2 22
#define IN3 27
#define IN4 26

#define RPM_MOTEUR   50
#define PAS_PAR_BLOC 1

#define SENS_MONTEE true
#define SENS_DESCENTE false

B_Stepper moteur(IN1, IN2, IN3, IN4);

// =====================
// REGISTRE LEDS
// =====================
#define PIN_DATA_DS            32
#define PIN_LATCH_STC          33
#define PIN_HORLOGE_SERIE_SHC  25

#define NB_SORTIES 8
bool sorties[NB_SORTIES];

struct LedEtage {
  uint8_t rouge;
  uint8_t vert;
};

LedEtage etages[4] = {
  {5, 3}, // RDC
  {4, 0}, // ETAGE 1
  {1, 7}, // ETAGE 2
  {6, 2}  // ETAGE 3
};

// =====================
// CAPTEURS
// =====================
#define CAPTEUR_RDC     34
#define CAPTEUR_ETAGE1  39
#define CAPTEUR_ETAGE2  36
#define CAPTEUR_ETAGE3  35

#define COMP_DEFAULT 0 // compensation par défaut

int compMontee[4]   = {0, 0, 0, 0};
int compDescente[4] = {0, 0, 0, 0};

const uint8_t capteurs[4] = {
  CAPTEUR_RDC,
  CAPTEUR_ETAGE1,
  CAPTEUR_ETAGE2,
  CAPTEUR_ETAGE3
};


// =====================
// BOUTONS
// =====================
#define BTN_RDC_BAS      12
#define BTN_RDC_HAUT     14

#define BTN_ETAGE1_BAS   3
#define BTN_ETAGE1_HAUT  5

#define BTN_ETAGE2_BAS   1
#define BTN_ETAGE2_HAUT  16

#define BTN_ETAGE3       15

// =====================
// RFID
// =====================
#define RFID_RST 17
#define RFID_SDA 4

MFRC522 rfid(RFID_SDA, RFID_RST);

enum ModeRFID {
  RFID_NORMAL,
  RFID_AJOUT,
  RFID_SUPPRESSION
};


ModeRFID modeRFID = RFID_NORMAL;

String dernierUID = "";
bool accesEtage3 = false;
unsigned long tempsAccesEtage3 = 0;
int cibleEnCours = -1;

#define DUREE_ACCES_ETAGE3 10000


// =====================
// COMPENSATION
// =====================

void chargerCompensations() {
  compMontee[1] = prefs.getInt("cm_e1", 0);
  compMontee[2] = prefs.getInt("cm_e2", 0);

  compDescente[1] = prefs.getInt("cd_e1", 0);
  compDescente[2] = prefs.getInt("cd_e2", 0);
}

void sauvegarderCompensation(int etage, int pasMontee, int pasDescente) {
  if (etage == 1) {
    prefs.putInt("cm_e1", pasMontee);
    prefs.putInt("cd_e1", pasDescente);
  }

  if (etage == 2) {
    prefs.putInt("cm_e2", pasMontee);
    prefs.putInt("cd_e2", pasDescente);
  }

  chargerCompensations();
}

void appliquerCompensation(int etage, bool sensArrivee) {
  int pas = 0;

  if (etage != 1 && etage != 2) return;

  if (sensArrivee == SENS_MONTEE) {
    pas = compMontee[etage];
  } else {
    pas = compDescente[etage];
  }

  if (pas <= 0) return;

  moteur.newMove(pas);

  while (moteur.getStepsLeft() > 0) {
    moteur.move(sensArrivee);

    server.handleClient();
    gererRFID();
    verifierExpirationBadge();

    afficherToutRouge();
  }

  moteur.stop();
}

// =====================
// ETAT SYSTEME
// =====================
bool moteurEnMarche = false;
String dernierMessage = "Systeme pret";

#define TEMPS_ATTENTE_ETAGE 2000

// =====================
// OUTILS
// =====================
bool capteurActif(uint8_t pin) {
  return digitalRead(pin) == LOW;
}

int positionActuelle() {
  for (int i = 0; i < 4; i++) {
    if (capteurActif(capteurs[i])) return i;
  }
  return -1;
}

// =====================
// LEDS
// =====================
void envoyerRegistre() {
  digitalWrite(PIN_LATCH_STC, LOW);

  for (int i = NB_SORTIES - 1; i >= 0; i--) {
    digitalWrite(PIN_HORLOGE_SERIE_SHC, LOW);
    digitalWrite(PIN_DATA_DS, sorties[i]);
    digitalWrite(PIN_HORLOGE_SERIE_SHC, HIGH);
  }

  digitalWrite(PIN_LATCH_STC, HIGH);
}

void afficherPosition(int etageActif) {
  for (int i = 0; i < NB_SORTIES; i++) sorties[i] = false;

  for (int etage = 0; etage < 4; etage++) {
    if (etage == etageActif) {
      sorties[etages[etage].vert] = true;
    } else {
      sorties[etages[etage].rouge] = true;
    }
  }

  envoyerRegistre();
}

void afficherToutRouge() {
  for (int i = 0; i < NB_SORTIES; i++) sorties[i] = false;

  for (int etage = 0; etage < 4; etage++) {
    sorties[etages[etage].rouge] = true;
  }

  envoyerRegistre();
}

void mettreAJourLeds() {
  if (moteurEnMarche) {
    afficherToutRouge();
    return;
  }

  int pos = positionActuelle();

  if (pos >= 0) {
    afficherPosition(pos);
  } else {
    afficherToutRouge();
  }
}

// =====================
// RFID BADGES
// =====================
String lireUID() {
  if (!rfid.PICC_IsNewCardPresent()) return "";
  if (!rfid.PICC_ReadCardSerial()) return "";

  String uid = "";

  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(rfid.uid.uidByte[i], HEX);
    if (i < rfid.uid.size - 1) uid += ":";
  }

  uid.toUpperCase();

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  return uid;
}

String listeBadges() {
  return prefs.getString("badges", "");
}

bool badgeExiste(String uid) {
  String badges = listeBadges();
  return badges.indexOf(uid) >= 0;
}

void ajouterBadge(String uid) {
  String badges = listeBadges();

  if (!badgeExiste(uid)) {
    badges += uid;
    badges += ";";
    prefs.putString("badges", badges);
  }
}

void supprimerBadge(String uid) {
  String badges = listeBadges();
  badges.replace(uid + ";", "");
  prefs.putString("badges", badges);
}

void gererRFID() {
  String uid = lireUID();
  if (uid == "") return;

  dernierUID = uid;

  if (modeRFID == RFID_AJOUT) {
    ajouterBadge(uid);
    dernierMessage = "Badge ajoute : " + uid;
    modeRFID = RFID_NORMAL;
  }
  else if (modeRFID == RFID_SUPPRESSION) {
    supprimerBadge(uid);
    dernierMessage = "Badge supprime : " + uid;
    modeRFID = RFID_NORMAL;
  }
  else {
    if (badgeExiste(uid)) {
      accesEtage3 = true;
      tempsAccesEtage3 = millis();
      dernierMessage = "Badge valide : montee vers etage 3";

      if (!moteurEnMarche) {
        allerAEtage(3);
      }
    }
    else {
      dernierMessage = "Badge refuse : " + uid;
      clignoterRougeRDC(3);
    }
  }
}

void verifierExpirationBadge() {
  if (accesEtage3 && millis() - tempsAccesEtage3 > DUREE_ACCES_ETAGE3) {
    accesEtage3 = false;
  }
}

// =====================
// MOTEUR
// =====================
void attendreAvecService(unsigned long duree) {
  unsigned long debut = millis();

  while (millis() - debut < duree) {
    server.handleClient();
    gererRFID();
    verifierExpirationBadge();
    mettreAJourLeds();
    delay(10);
  }
}


void avancerBloc(bool sens) {
  moteur.newMove(PAS_PAR_BLOC);

  while (moteur.getStepsLeft() > 0) {

    if (cibleEnCours >= 0 && capteurActif(capteurs[cibleEnCours])) {
      moteur.stop();
      return;
    }

    moteur.move(sens);

    server.handleClient();
    gererRFID();
    verifierExpirationBadge();
    mettreAJourLeds();
  }
}


void allerAEtage(int cible) {
  int pos = positionActuelle();

  if (pos == cible) {
    afficherPosition(cible);
    return;
  }

  if (cible == 3 && !accesEtage3) {
    dernierMessage = "Acces etage 3 refuse";
    return;
  }

  if (pos < 0) {
    dernierMessage = "Position inconnue, retour RDC";
    cible = 0;
    pos = 3;
  }

  bool sens = cible > pos ? SENS_MONTEE : SENS_DESCENTE;

  moteurEnMarche = true;
  dernierMessage = "Deplacement vers etage " + String(cible);

  afficherToutRouge();

  cibleEnCours = cible;

  while (!capteurActif(capteurs[cible])) {
    avancerBloc(sens);
  }

  cibleEnCours = -1;

  appliquerCompensation(cible, sens);

  moteurEnMarche = false;
  moteur.stop();

  afficherPosition(cible);
}


void appelEtDeplacement(int etageAppel, int destination) {
  allerAEtage(etageAppel);
  attendreAvecService(TEMPS_ATTENTE_ETAGE);

  if (destination != etageAppel) {
    allerAEtage(destination);
    attendreAvecService(TEMPS_ATTENTE_ETAGE);
  }
}

// =====================
// BOUTONS
// =====================

const uint8_t boutonsPins[] = {
  BTN_RDC_BAS,
  BTN_RDC_HAUT,
  BTN_ETAGE1_BAS,
  BTN_ETAGE1_HAUT,
  BTN_ETAGE2_BAS,
  BTN_ETAGE2_HAUT,
  BTN_ETAGE3
};

bool ancienBouton[7];

void initialiserBoutons() {
  for (int i = 0; i < 7; i++) {
    ancienBouton[i] = digitalRead(boutonsPins[i]);
  }
}

bool boutonDeclenche(int index) {
  bool etat = digitalRead(boutonsPins[index]);

  bool declenche = (ancienBouton[index] == HIGH && etat == LOW);

  ancienBouton[index] = etat;

  return declenche;
}


bool appuye(uint8_t pin) {
  return digitalRead(pin) == LOW;
}


void gererBoutons() {
  if (moteurEnMarche) return;

  if (boutonDeclenche(0)) {
    allerAEtage(0);
  }

  else if (boutonDeclenche(1)) {
    allerAEtage(1);
  }

  else if (boutonDeclenche(2)) {
    allerAEtage(1);
    attendreAvecService(TEMPS_ATTENTE_ETAGE);
    allerAEtage(0);
  }

  else if (boutonDeclenche(3)) {
    allerAEtage(1);
    attendreAvecService(TEMPS_ATTENTE_ETAGE);
    allerAEtage(2);
  }

  else if (boutonDeclenche(4)) {
    allerAEtage(2);
    attendreAvecService(TEMPS_ATTENTE_ETAGE);
    allerAEtage(1);
  }

  else if (boutonDeclenche(5)) {
    allerAEtage(2);
  }

  else if (boutonDeclenche(6)) {

    int pos = positionActuelle();

    if (pos == 3) {
      allerAEtage(2);
    }
    else {
      bool ancienAcces = accesEtage3;

      accesEtage3 = true;
      allerAEtage(3);
      accesEtage3 = ancienAcces;

      attendreAvecService(TEMPS_ATTENTE_ETAGE);

      allerAEtage(2);
    }
  }
}

// =====================
// WEB API
// =====================
void envoyerJSON(String json) {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", json);
}

void routeStatus() {
  String json = "{";
  json += "\"position\":" + String(positionActuelle()) + ",";
  json += "\"moteur\":" + String(moteurEnMarche ? "true" : "false") + ",";
  json += "\"accesEtage3\":" + String(accesEtage3 ? "true" : "false") + ",";
  json += "\"dernierUID\":\"" + dernierUID + "\",";
  json += "\"modeRFID\":\"";

  if (modeRFID == RFID_AJOUT) json += "ajout";
  else if (modeRFID == RFID_SUPPRESSION) json += "suppression";
  else json += "normal";

  json += "\",";
  json += "\"message\":\"" + dernierMessage + "\",";
  json += "\"badges\":\"" + listeBadges() + "\"";
  json += "}";

  envoyerJSON(json);
}

void routeCommande() {
  if (!server.hasArg("etage")) {
    envoyerJSON("{\"ok\":false,\"message\":\"etage manquant\"}");
    return;
  }

  int etage = server.arg("etage").toInt();

  if (etage < 0 || etage > 3) {
    envoyerJSON("{\"ok\":false,\"message\":\"etage invalide\"}");
    return;
  }

  if (etage == 3 && !accesEtage3) {
    dernierMessage = "Commande web refusee : badge requis";
    envoyerJSON("{\"ok\":false,\"message\":\"badge requis pour etage 3\"}");
    return;
  }

  allerAEtage(etage);
  envoyerJSON("{\"ok\":true,\"message\":\"commande executee\"}");
}

void routeModeRFID() {
  if (!server.hasArg("mode")) {
    envoyerJSON("{\"ok\":false,\"message\":\"mode manquant\"}");
    return;
  }

  String mode = server.arg("mode");

  if (mode == "ajout") {
    modeRFID = RFID_AJOUT;
    dernierMessage = "Approche un badge a enregistrer";
  }
  else if (mode == "suppression") {
    modeRFID = RFID_SUPPRESSION;
    dernierMessage = "Approche un badge a supprimer";
  }
  else {
    modeRFID = RFID_NORMAL;
    dernierMessage = "Mode RFID normal";
  }

  envoyerJSON("{\"ok\":true}");
}

void routeEffacerBadges() {
  prefs.putString("badges", "");
  dernierMessage = "Tous les badges ont ete supprimes";
  envoyerJSON("{\"ok\":true}");
}


bool systemeInitialise = false;

void initialiserAuRDC() {
  dernierMessage = "Initialisation : retour RDC";
  moteurEnMarche = true;

  while (!capteurActif(CAPTEUR_RDC)) {
    avancerBloc(SENS_DESCENTE);
    mettreAJourLeds();
  }

  moteurEnMarche = false;
  moteur.stop();

  mettreAJourLeds();
  dernierMessage = "Systeme pret au RDC";
  systemeInitialise = true;
}


// =====================
// SETUP
// =====================
void setup() {
  pinMode(PIN_DATA_DS, OUTPUT);
  pinMode(PIN_LATCH_STC, OUTPUT);
  pinMode(PIN_HORLOGE_SERIE_SHC, OUTPUT);

  pinMode(CAPTEUR_RDC, INPUT);
  pinMode(CAPTEUR_ETAGE1, INPUT);
  pinMode(CAPTEUR_ETAGE2, INPUT);
  pinMode(CAPTEUR_ETAGE3, INPUT);

  pinMode(BTN_RDC_BAS, INPUT_PULLUP);
  pinMode(BTN_RDC_HAUT, INPUT_PULLUP);

  pinMode(BTN_ETAGE1_BAS, INPUT_PULLUP);
  pinMode(BTN_ETAGE1_HAUT, INPUT_PULLUP);

  pinMode(BTN_ETAGE2_BAS, INPUT_PULLUP);
  pinMode(BTN_ETAGE2_HAUT, INPUT_PULLUP);

  pinMode(BTN_ETAGE3, INPUT_PULLUP);

  moteur.setRpm(RPM_MOTEUR);

  SPI.begin(18, 19, 23, RFID_SDA);
  rfid.PCD_Init();

  if (!SPIFFS.begin(true)) {
    Serial.println("Erreur SPIFFS");
    return;
  }

  prefs.begin("rfid", false);

  if (listeBadges().indexOf("61:85:A0:17") < 0) {
    ajouterBadge("61:85:A0:17");
  }
  chargerCompensations();

  WiFi.softAP(ssid, password);
  
  server.on("/", routePageWeb);
  server.on("/index.html", routePageWeb);
  server.on("/style.css", routeCSS);
  server.on("/script.js", routeJS);
  server.on("/status", routeStatus);
  server.on("/commande", routeCommande);
  server.on("/rfid", routeModeRFID);
  server.on("/badges/clear", routeEffacerBadges);
  server.on("/button", routeButton);

  server.begin();

  initialiserBoutons();

  afficherToutRouge();
  delay(500);
  initialiserAuRDC();
}

// =====================
// LOOP
// =====================
void loop() {
  server.handleClient();

  gererRFID();
  verifierExpirationBadge();

  mettreAJourLeds();

  if (systemeInitialise) {
    gererBoutons();
  }

  delay(20);
}


void clignoterRougeRDC(int fois) {
  for (int i = 0; i < fois; i++) {
    for (int j = 0; j < NB_SORTIES; j++) sorties[j] = false;
    sorties[etages[0].rouge] = true;
    envoyerRegistre();
    delay(200);

    sorties[etages[0].rouge] = false;
    envoyerRegistre();
    delay(200);
  }

  mettreAJourLeds();
}


void servirFichier(String chemin, String type) {
  if (!SPIFFS.exists(chemin)) {
    server.send(404, "text/plain", "Fichier introuvable");
    return;
  }

  File fichier = SPIFFS.open(chemin, "r");
  server.streamFile(fichier, type);
  fichier.close();
}

void routePageWeb() {
  servirFichier("/index.html", "text/html");
}

void routeCSS() {
  servirFichier("/style.css", "text/css");
}

void routeJS() {
  servirFichier("/script.js", "application/javascript");
}

void routeButton() {
  if (!server.hasArg("floor") || !server.hasArg("direction")) {
    envoyerJSON("{\"ok\":false,\"message\":\"parametres manquants\"}");
    return;
  }

  int floor = server.arg("floor").toInt();
  String direction = server.arg("direction");

  if (floor == 0 && direction == "down") {
    allerAEtage(0);
  }
  else if (floor == 0 && direction == "up") {
    allerAEtage(1);
  }
  else if (floor == 1 && direction == "down") {
    allerAEtage(1);
    attendreAvecService(TEMPS_ATTENTE_ETAGE);
    allerAEtage(0);
  }
  else if (floor == 1 && direction == "up") {
    allerAEtage(1);
    attendreAvecService(TEMPS_ATTENTE_ETAGE);
    allerAEtage(2);
  }
  else if (floor == 2 && direction == "down") {
    allerAEtage(2);
    attendreAvecService(TEMPS_ATTENTE_ETAGE);
    allerAEtage(1);
  }
  else if (floor == 2 && direction == "up") {
    allerAEtage(2);
  }
  else if (floor == 3 && direction == "down") {
    allerAEtage(3);
    attendreAvecService(TEMPS_ATTENTE_ETAGE);
    allerAEtage(2);
  }
  else {
    envoyerJSON("{\"ok\":false,\"message\":\"commande invalide\"}");
    return;
  }

  envoyerJSON("{\"ok\":true,\"message\":\"commande executee\"}");
}
