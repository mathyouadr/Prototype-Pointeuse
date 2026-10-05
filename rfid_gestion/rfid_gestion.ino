/*  ESP32 WROOM + RC522 : point d'acces WiFi + interface web
 *  Un badge = un seul usage, choisi a la creation :
 *    OUTIL     -> 1er scan = sortie de stock, 2e scan = retour
 *    PERSONNEL -> 1er scan = arrivee, 2e scan = depart
 *  Synthese vocale assuree par le navigateur (Web Speech API, fonctionne hors ligne)
 *
 *  Cablage : SDA 5 | SCK 18 | MOSI 23 | MISO 19 | RST 22 | 3V3 | GND
 */

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <SPI.h>
#include <MFRC522.h>
#include <time.h>

#define SS_PIN      5
#define RST_PIN     22
#define BLOC_DATA   4          // bloc efface par la fonction "effacer le badge"
#define MAX_FICHES  32
#define MAX_JOURNAL 30

const char* AP_SSID = "RFID-Gestion";
const char* AP_PASS = "badge1234";      // 8 caracteres minimum

MFRC522 rfid(SS_PIN, RST_PIN);
MFRC522::MIFARE_Key key;
WebServer  server(80);
DNSServer  dns;
Preferences prefs;

struct Fiche {
  String uid;
  String nom;
  String type;        // "outil" ou "perso" - fige a la creation
  bool   actif;       // outil : sorti du stock | personnel : present
  time_t ts;
};

struct Evt {
  uint32_t id;
  String   txt;
  String   say;
  String   ton;       // ok | warn
  time_t   ts;
};

Fiche fiches[MAX_FICHES];   int nFiches  = 0;
Evt   journal[MAX_JOURNAL]; int nJournal = 0;
uint32_t evtId = 0;

// --- etat des modes armes -----------------------------------------------
bool   armCreer   = false;   // le prochain badge scanne sera propose a la creation
bool   armEffacer = false;   // le prochain badge scanne sera efface
String pendingUid = "";      // UID capture, en attente de nommage
String pendingNom = "";      // rempli si l'UID est deja au registre (=> refus)
bool   heureOk    = false;

// --- anti-rebond ---------------------------------------------------------
unsigned long dernierScan = 0;
String        dernierUid  = "";

void reinitAntiRebond() { dernierUid = ""; dernierScan = 0; }

// ---------------------------------------------------------------- utilitaires

String uidToString(MFRC522::Uid &u) {
  String s = "";
  for (byte i = 0; i < u.size; i++) {
    if (u.uidByte[i] < 0x10) s += '0';
    s += String(u.uidByte[i], HEX);
  }
  s.toUpperCase();
  return s;
}

String jsonEsc(const String &in) {
  String o = "";
  for (unsigned int i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if (c >= 32) o += c;
  }
  return o;
}

String heure(time_t t) {
  if (!heureOk || t == 0) return "--:--";
  struct tm tm;
  localtime_r(&t, &tm);
  char buf[6];
  snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
  return String(buf);
}

String duree(time_t depuis) {
  if (!heureOk || depuis == 0) return "";
  long d = time(nullptr) - depuis;
  if (d < 60)   return String(d) + " s";
  if (d < 3600) return String(d / 60) + " min";
  return String(d / 3600) + " h " + String((d % 3600) / 60) + " min";
}

// ------------------------------------------------------------- persistance NVS

void sauver() {
  String s = "";
  for (int i = 0; i < nFiches; i++) {
    s += fiches[i].uid + "|" + fiches[i].nom + "|" + fiches[i].type + "|" + (fiches[i].actif ? "1" : "0");
    if (i < nFiches - 1) s += ";";
  }
  prefs.putString("fiches", s);
}

void charger() {
  String data = prefs.getString("fiches", "");
  nFiches = 0;
  int start = 0;
  while (start < (int)data.length() && nFiches < MAX_FICHES) {
    int fin = data.indexOf(';', start);
    if (fin < 0) fin = data.length();
    String b = data.substring(start, fin);
    int p1 = b.indexOf('|');
    int p2 = b.indexOf('|', p1 + 1);
    int p3 = b.indexOf('|', p2 + 1);
    if (p1 > 0 && p2 > p1 && p3 > p2) {
      fiches[nFiches].uid   = b.substring(0, p1);
      fiches[nFiches].nom   = b.substring(p1 + 1, p2);
      fiches[nFiches].type  = b.substring(p2 + 1, p3);
      fiches[nFiches].actif = b.substring(p3 + 1) == "1";
      fiches[nFiches].ts    = 0;
      nFiches++;
    }
    start = fin + 1;
  }
}

// ------------------------------------------------------------------- journal

void ajouterEvt(String txt, String say, String ton) {
  if (nJournal >= MAX_JOURNAL) {
    for (int i = 0; i < MAX_JOURNAL - 1; i++) journal[i] = journal[i + 1];
    nJournal = MAX_JOURNAL - 1;
  }
  journal[nJournal] = { ++evtId, txt, say, ton, time(nullptr) };
  nJournal++;
  Serial.println("[EVT] " + txt);
}

// ------------------------------------------------------------ logique de scan

int trouver(const String &uid) {
  for (int i = 0; i < nFiches; i++) if (fiches[i].uid == uid) return i;
  return -1;
}

// mode creation arme : on capture l'UID quoi qu'il arrive, sans toucher aux etats
void capturerBadge(const String &uid) {
  int i = trouver(uid);
  pendingUid = uid;
  pendingNom = (i >= 0) ? fiches[i].nom : "";
  armCreer   = false;

  if (i >= 0) ajouterEvt("Badge deja enregistre : " + fiches[i].nom,
                         "Ce badge est deja enregistre", "warn");
  else        ajouterEvt("Badge " + uid + " pret a etre nomme",
                         "Badge detecte", "ok");
}

void traiterBadge(const String &uid) {
  int i = trouver(uid);

  if (i < 0) {                       // inconnu hors mode creation : on propose quand meme
    pendingUid = uid;
    pendingNom = "";
    ajouterEvt("Badge inconnu " + uid, "Badge inconnu", "warn");
    return;
  }

  String avant = duree(fiches[i].ts);
  fiches[i].actif = !fiches[i].actif;
  fiches[i].ts    = time(nullptr);

  if (fiches[i].type == "outil") {
    if (fiches[i].actif)
      ajouterEvt(fiches[i].nom + " - sortie de stock",
                 fiches[i].nom + " sorti du stock", "warn");
    else
      ajouterEvt(fiches[i].nom + " - retour en stock" + (avant.length() ? " apres " + avant : ""),
                 fiches[i].nom + " de retour en stock", "ok");
  } else {
    if (fiches[i].actif)
      ajouterEvt(fiches[i].nom + " - arrivee", "Bonjour " + fiches[i].nom, "ok");
    else
      ajouterEvt(fiches[i].nom + " - depart" + (avant.length() ? " apres " + avant : ""),
                 "Au revoir " + fiches[i].nom, "warn");
  }
  sauver();
}

void effacerBadge(const String &uid) {
  int i = trouver(uid);
  String nom = (i >= 0) ? fiches[i].nom : uid;

  bool memoireOk = false;
  MFRC522::StatusCode s = rfid.PCD_Authenticate(
      MFRC522::PICC_CMD_MF_AUTH_KEY_A, BLOC_DATA, &key, &(rfid.uid));
  if (s == MFRC522::STATUS_OK) {
    byte vide[16] = {0};
    if (rfid.MIFARE_Write(BLOC_DATA, vide, 16) == MFRC522::STATUS_OK) memoireOk = true;
  }

  if (i >= 0) {
    for (int k = i; k < nFiches - 1; k++) fiches[k] = fiches[k + 1];
    nFiches--;
    sauver();
  }

  armEffacer = false;
  ajouterEvt(nom + " - badge efface" + (memoireOk ? " (memoire remise a zero)" : " (registre seul)"),
             nom + " efface", "warn");
}

// --------------------------------------------------------------- API HTTP

void apiState() {
  String j = "{\"evtId\":" + String(evtId);
  j += ",\"pending\":\""    + pendingUid + "\"";
  j += ",\"pendingNom\":\"" + jsonEsc(pendingNom) + "\"";
  j += ",\"creer\":"   + String(armCreer   ? "true" : "false");
  j += ",\"effacer\":" + String(armEffacer ? "true" : "false");
  j += ",\"heure\":"   + String(heureOk    ? "true" : "false");

  j += ",\"fiches\":[";
  for (int i = 0; i < nFiches; i++) {
    if (i) j += ",";
    j += "{\"uid\":\"" + fiches[i].uid + "\",\"nom\":\"" + jsonEsc(fiches[i].nom) +
         "\",\"type\":\"" + fiches[i].type +
         "\",\"actif\":" + (fiches[i].actif ? "true" : "false") +
         ",\"h\":\"" + heure(fiches[i].ts) + "\"}";
  }
  j += "],\"journal\":[";
  for (int i = nJournal - 1; i >= 0; i--) {
    if (i < nJournal - 1) j += ",";
    j += "{\"id\":" + String(journal[i].id) +
         ",\"txt\":\"" + jsonEsc(journal[i].txt) +
         "\",\"say\":\"" + jsonEsc(journal[i].say) +
         "\",\"ton\":\"" + journal[i].ton +
         "\",\"h\":\"" + heure(journal[i].ts) + "\"}";
  }
  j += "]}";

  server.send(200, "application/json", j);
}

// arme / desarme le mode creation
void apiCreer() {
  armCreer = server.arg("on") == "1";
  if (armCreer) { armEffacer = false; pendingUid = ""; pendingNom = ""; reinitAntiRebond(); }
  server.send(200, "text/plain", "ok");
}

void apiEffacer() {
  armEffacer = server.arg("on") == "1";
  if (armEffacer) { armCreer = false; pendingUid = ""; pendingNom = ""; reinitAntiRebond(); }
  server.send(200, "text/plain", "ok");
}

// reponses JSON explicites : le client ne ferme la modale que sur succes
void apiRegister() {
  String uid  = server.arg("uid");
  String nom  = server.arg("nom");
  String type = server.arg("type") == "perso" ? "perso" : "outil";

  if (uid.length() == 0 || nom.length() == 0) {
    server.send(400, "application/json", "{\"ok\":false,\"err\":\"UID ou nom manquant\"}");
    return;
  }
  if (nFiches >= MAX_FICHES) {
    server.send(507, "application/json", "{\"ok\":false,\"err\":\"Registre plein\"}");
    return;
  }
  int dejaLa = trouver(uid);
  if (dejaLa >= 0) {
    server.send(409, "application/json",
                "{\"ok\":false,\"err\":\"Badge deja attribue a " + jsonEsc(fiches[dejaLa].nom) + "\"}");
    return;
  }

  fiches[nFiches] = { uid, nom, type, false, 0 };
  nFiches++;
  sauver();
  pendingUid = "";
  pendingNom = "";
  reinitAntiRebond();                       // le badge peut etre rescanne tout de suite
  ajouterEvt(nom + " cree (" + (type == "outil" ? "outillage" : "personnel") + ")",
             nom + " enregistre", "ok");
  server.send(200, "application/json", "{\"ok\":true}");
}

void apiDelete() {
  int i = trouver(server.arg("uid"));
  if (i >= 0) {
    String nom = fiches[i].nom;
    for (int k = i; k < nFiches - 1; k++) fiches[k] = fiches[k + 1];
    nFiches--;
    sauver();
    ajouterEvt(nom + " - retire du registre", nom + " retire", "warn");
  }
  server.send(200, "text/plain", "ok");
}

void apiTime() {
  if (server.hasArg("epoch")) {
    struct timeval tv = { (time_t)server.arg("epoch").toInt(), 0 };
    settimeofday(&tv, nullptr);
    heureOk = true;
  }
  server.send(200, "text/plain", "ok");
}

void apiAnnuler() {
  pendingUid = "";
  pendingNom = "";
  armCreer   = false;
  reinitAntiRebond();
  server.send(200, "text/plain", "ok");
}

// ------------------------------------------------------------------ page web

const char PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="fr"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Gestion RFID</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;background:#0f1115;color:#e8e6e1;min-height:100vh;padding:18px}
.wrap{max-width:760px;margin:0 auto}
header{display:flex;align-items:center;justify-content:space-between;margin-bottom:18px}
h1{font-size:20px;font-weight:600;letter-spacing:-.02em}
.dot{display:inline-block;width:8px;height:8px;border-radius:50%;background:#3ddc84;margin-right:8px;animation:p 2s infinite}
@keyframes p{50%{opacity:.35}}
.tts{background:#1b1e26;border:1px solid #2b2f3a;color:#9aa0ad;padding:8px 14px;border-radius:999px;font-size:13px;cursor:pointer}
.tts.on{color:#3ddc84;border-color:#2c5c43}
.scan{background:#161920;border:1px solid #232733;border-radius:16px;padding:26px 20px;text-align:center;margin-bottom:14px;transition:.2s}
.scan .big{font-size:22px;font-weight:600;margin-bottom:6px}
.scan .small{font-size:13px;color:#7d8391}
.scan.flash{animation:f .7s}
@keyframes f{0%{background:#1c3a2a;border-color:#3ddc84}100%{background:#161920}}
.scan.danger{background:#2a1618;border-color:#a13b3b}
.scan.danger .big{color:#f87171}
.scan.creer{background:#141f31;border-color:#3b82f6}
.scan.creer .big{color:#7fb0f7}
.actions{display:flex;gap:8px;margin-bottom:8px}
.act{flex:1;padding:13px;border-radius:12px;background:#161920;border:1px solid #232733;color:#e8e6e1;font-size:14px;font-weight:500;cursor:pointer;transition:.15s}
.act:hover{border-color:#3b82f6}
.act.armedb{background:#17263c;border-color:#3b82f6;color:#7fb0f7}
.act.danger{color:#f87171}
.act.danger:hover{border-color:#a13b3b}
.act.armed{background:#3a1c1e;border-color:#a13b3b;color:#f87171}
h2{font-size:13px;text-transform:uppercase;letter-spacing:.08em;color:#6b7280;margin:24px 0 10px}
.row{display:flex;align-items:center;gap:10px;background:#161920;border:1px solid #232733;border-radius:12px;padding:13px 15px;margin-bottom:8px}
.row .nom{font-size:15px;font-weight:500}
.row .uid{font-family:ui-monospace,monospace;font-size:11px;color:#5c626e;margin-top:2px}
.badge{font-size:12px;font-weight:600;padding:5px 11px;border-radius:999px;white-space:nowrap}
.b-ok{background:#14301f;color:#4ade80}
.b-out{background:#3a2416;color:#fb9a4b}
.h{font-size:12px;color:#6b7280;min-width:42px;text-align:right}
.del{background:none;border:none;color:#4b5563;font-size:19px;cursor:pointer;padding:0 4px;line-height:1}
.del:hover{color:#ef4444}
.jrow{display:flex;gap:12px;padding:10px 14px;font-size:14px;border-left:2px solid #232733;margin-bottom:4px}
.jrow.ok{border-color:#3ddc84}.jrow.warn{border-color:#fb9a4b}
.jrow .jh{color:#5c626e;font-size:12px;min-width:40px}
.vide{color:#5c626e;font-size:14px;padding:14px;text-align:center}
.modal{position:fixed;inset:0;background:rgba(8,10,14,.85);display:flex;align-items:center;justify-content:center;padding:20px;z-index:9}
.card{background:#161920;border:1px solid #2b2f3a;border-radius:18px;padding:24px;width:100%;max-width:380px}
.card h3{font-size:17px;margin-bottom:6px}
.card p{font-size:13px;color:#7d8391;margin-bottom:16px}
.card p b{font-family:ui-monospace,monospace;color:#e8e6e1}
.attente{display:flex;align-items:center;gap:10px;background:#0f1115;border:1px dashed #2b2f3a;border-radius:12px;padding:16px;margin-bottom:16px;font-size:14px;color:#7fb0f7}
.spin{width:14px;height:14px;border:2px solid #2b2f3a;border-top-color:#3b82f6;border-radius:50%;animation:sp .8s linear infinite;flex:none}
@keyframes sp{to{transform:rotate(360deg)}}
.err{background:#2a1618;border:1px solid #a13b3b;color:#f87171;border-radius:10px;padding:11px 13px;font-size:13px;margin-bottom:12px}
.seg{display:flex;gap:8px;margin-bottom:14px}
.segb{flex:1;padding:14px 10px;border-radius:12px;background:#0f1115;border:1px solid #2b2f3a;color:#9aa0ad;cursor:pointer;text-align:center}
.segb .t{font-size:14px;font-weight:600}
.segb .s{font-size:11px;margin-top:3px;opacity:.75}
.segb.sel{background:#1d2735;border-color:#3b82f6;color:#e8e6e1}
input{width:100%;background:#0f1115;border:1px solid #2b2f3a;color:#e8e6e1;border-radius:10px;padding:12px;font-size:15px;margin-bottom:12px}
input:focus{outline:none;border-color:#3b82f6}
.btns{display:flex;gap:8px}
button.p{flex:1;background:#3b82f6;border:none;color:#fff;padding:12px;border-radius:10px;font-size:15px;font-weight:600;cursor:pointer}
button.p:disabled{background:#243044;color:#5c626e;cursor:not-allowed}
button.s{background:#1f232c;border:1px solid #2b2f3a;color:#9aa0ad;padding:12px 18px;border-radius:10px;cursor:pointer}
</style></head><body><div class="wrap">

<header>
  <h1><span class="dot"></span>Gestion RFID</h1>
  <button class="tts on" id="ttsBtn" onclick="toggleTts()">Voix activee</button>
</header>

<div class="scan" id="scan">
  <div class="big" id="scanBig">Approchez un badge</div>
  <div class="small" id="scanSmall">En attente de lecture</div>
</div>

<div class="actions">
  <button class="act" id="btnCree" onclick="toggleCreer()">Creer un badge</button>
  <button class="act danger" id="btnEff" onclick="toggleEffacer()">Effacer un badge</button>
</div>

<h2>Outillage</h2>
<div id="listeOutil"></div>

<h2>Personnel</h2>
<div id="listePerso"></div>

<h2>Journal</h2>
<div id="journal"></div>

</div>

<div class="modal" id="modal" style="display:none"><div class="card">
  <h3>Nouveau badge</h3>

  <div class="attente" id="mAttente"><div class="spin"></div>Approchez le badge du lecteur...</div>
  <p id="mSous" style="display:none">UID <b id="mUid"></b>. Choisissez son usage, il sera definitif.</p>
  <div class="err" id="mErr" style="display:none"></div>

  <div class="seg">
    <div class="segb sel" id="segOutil" onclick="choisir('outil')">
      <div class="t">Outillage</div><div class="s">sortie / retour</div>
    </div>
    <div class="segb" id="segPerso" onclick="choisir('perso')">
      <div class="t">Personnel</div><div class="s">arrivee / depart</div>
    </div>
  </div>
  <input id="mNom" placeholder="Perceuse Bosch / Marie Dupont" autocomplete="off">
  <div class="btns">
    <button class="s" onclick="annuler()">Annuler</button>
    <button class="p" id="mOk" disabled onclick="enregistrer()">Enregistrer</button>
  </div>
</div></div>

<script>
let dernierEvt = 0, tts = true, premierePasse = true;
let typeChoisi = 'outil';
let uidCourant = '';        // UID affiche dans la modale, '' = pas encore scanne
let modaleOuverte = false;
let envoiEnCours = false;

fetch('/api/time?epoch=' + Math.floor(Date.now()/1000));

function $(id){ return document.getElementById(id); }

function toggleTts(){
  tts = !tts;
  $('ttsBtn').textContent = tts ? 'Voix activee' : 'Voix coupee';
  $('ttsBtn').classList.toggle('on', tts);
  if (tts) parler('Voix activee');
}

function parler(t){
  if (!tts || !window.speechSynthesis) return;
  const u = new SpeechSynthesisUtterance(t);
  u.lang = 'fr-FR'; u.rate = 1.05;
  speechSynthesis.speak(u);
}

function choisir(t){
  typeChoisi = t;
  $('segOutil').classList.toggle('sel', t === 'outil');
  $('segPerso').classList.toggle('sel', t === 'perso');
}

function ouvrirModale(){
  modaleOuverte = true;
  uidCourant = '';
  $('mAttente').style.display = 'flex';
  $('mSous').style.display = 'none';
  $('mErr').style.display = 'none';
  $('mNom').value = '';
  $('mOk').disabled = true;
  choisir('outil');
  $('modal').style.display = 'flex';
}

function fermerModale(){
  modaleOuverte = false;
  uidCourant = '';
  $('modal').style.display = 'none';
}

// l'UID arrive apres coup, via le polling
function recevoirUid(uid, nomExistant){
  if (uid === uidCourant) return;
  uidCourant = uid;
  $('mAttente').style.display = 'none';
  $('mSous').style.display = 'block';
  $('mUid').textContent = uid;

  if (nomExistant){
    $('mErr').textContent = 'Ce badge est deja attribue a ' + nomExistant + '. Effacez-le d\'abord.';
    $('mErr').style.display = 'block';
    $('mOk').disabled = true;
  } else {
    $('mErr').style.display = 'none';
    $('mOk').disabled = false;
    setTimeout(() => $('mNom').focus(), 80);
  }
}

function toggleCreer(){
  const armed = $('btnCree').classList.contains('armedb');
  if (armed){ fetch('/api/creer?on=0').then(fermerModale).then(rafraichir); return; }
  ouvrirModale();
  fetch('/api/creer?on=1').then(rafraichir);
}

function toggleEffacer(){
  const armed = $('btnEff').classList.contains('armed');
  fetch('/api/effacer?on=' + (armed ? '0' : '1')).then(rafraichir);
}

function annuler(){
  fermerModale();
  fetch('/api/annuler').then(rafraichir);
}

function enregistrer(){
  if (envoiEnCours) return;
  const nom = $('mNom').value.trim();
  if (!uidCourant){ return; }
  if (!nom){
    $('mErr').textContent = 'Donnez un nom au badge.';
    $('mErr').style.display = 'block';
    $('mNom').focus();
    return;
  }
  envoiEnCours = true;
  $('mOk').disabled = true;

  fetch('/api/register?uid=' + encodeURIComponent(uidCourant)
        + '&type=' + typeChoisi + '&nom=' + encodeURIComponent(nom))
    .then(r => r.json())
    .then(res => {
      envoiEnCours = false;
      if (res.ok){
        fermerModale();
        annonce(nom + ' cree', typeChoisi === 'outil' ? 'Outillage - pret a etre scanne' : 'Personnel - pret a etre scanne');
        rafraichir();
      } else {
        $('mErr').textContent = res.err || 'Enregistrement refuse.';
        $('mErr').style.display = 'block';
        $('mOk').disabled = false;
      }
    })
    .catch(() => {
      envoiEnCours = false;
      $('mErr').textContent = 'Pas de reponse du lecteur, reessayez.';
      $('mErr').style.display = 'block';
      $('mOk').disabled = false;
    });
}

function annonce(gros, petit){
  $('scanBig').textContent = gros;
  $('scanSmall').textContent = petit;
  const sc = $('scan');
  sc.classList.remove('flash'); void sc.offsetWidth; sc.classList.add('flash');
}

function supprimer(uid, nom){
  if (!confirm('Retirer ' + nom + ' du registre ?')) return;
  fetch('/api/delete?uid=' + uid).then(rafraichir);
}

function ligne(o){
  const etat = o.type === 'outil'
    ? (o.actif ? ['b-out','Sorti'] : ['b-ok','En stock'])
    : (o.actif ? ['b-ok','Present'] : ['b-out','Absent']);
  const d = document.createElement('div');
  d.className = 'row';
  d.innerHTML = '<div style="flex:1"><div class="nom">' + o.nom + '</div><div class="uid">' + o.uid + '</div></div>'
    + '<span class="h">' + o.h + '</span>'
    + '<span class="badge ' + etat[0] + '">' + etat[1] + '</span>'
    + '<button class="del">&times;</button>';
  d.querySelector('.del').onclick = () => supprimer(o.uid, o.nom);
  return d;
}

function rafraichir(){
  fetch('/api/state').then(r => r.json()).then(s => {
    const lo = $('listeOutil'), lp = $('listePerso');
    const outils = s.fiches.filter(f => f.type === 'outil');
    const perso  = s.fiches.filter(f => f.type === 'perso');
    lo.innerHTML = outils.length ? '' : '<div class="vide">Aucun outil enregistre</div>';
    lp.innerHTML = perso.length  ? '' : '<div class="vide">Aucune personne enregistree</div>';
    outils.forEach(o => lo.appendChild(ligne(o)));
    perso.forEach(o  => lp.appendChild(ligne(o)));

    const j = $('journal');
    j.innerHTML = s.journal.length ? '' : '<div class="vide">Aucun mouvement</div>';
    s.journal.forEach(e => {
      const d = document.createElement('div');
      d.className = 'jrow ' + e.ton;
      d.innerHTML = '<span class="jh">' + e.h + '</span><span>' + e.txt + '</span>';
      j.appendChild(d);
    });

    const sc = $('scan');
    $('btnEff').classList.toggle('armed', s.effacer);
    $('btnCree').classList.toggle('armedb', s.creer || modaleOuverte);
    sc.classList.toggle('danger', s.effacer);
    sc.classList.toggle('creer', s.creer);
    if (s.effacer){
      $('scanBig').textContent = 'Mode effacement';
      $('scanSmall').textContent = 'Le prochain badge scanne sera efface';
    } else if (s.creer){
      $('scanBig').textContent = 'Mode creation';
      $('scanSmall').textContent = 'Approchez le badge a enregistrer';
    }

    // un UID en attente : soit la modale est ouverte, soit un badge inconnu vient d'etre lu
    if (s.pending){
      if (!modaleOuverte) ouvrirModale();
      recevoirUid(s.pending, s.pendingNom);
    } else if (modaleOuverte && uidCourant && !envoiEnCours){
      fermerModale();
    }

    if (s.evtId > dernierEvt){
      const nouv = s.journal.find(e => e.id === s.evtId);
      if (nouv && !premierePasse && !s.effacer && !s.creer && !modaleOuverte){
        annonce(nouv.txt, 'a ' + nouv.h);
        parler(nouv.say);
      }
      dernierEvt = s.evtId;
    }
    premierePasse = false;
  });
}

rafraichir();
setInterval(rafraichir, 800);
</script></body></html>
)rawliteral";

void handleRoot() { server.send_P(200, "text/html", PAGE); }

// ------------------------------------------------------------------- setup

void setup() {
  Serial.begin(115200);
  delay(200);

  SPI.begin();
  rfid.PCD_Init();
  rfid.PCD_SetAntennaGain(rfid.RxGain_max);
  for (byte i = 0; i < 6; i++) key.keyByte[i] = 0xFF;
  Serial.print(F("RC522 VersionReg = 0x"));
  Serial.println(rfid.PCD_ReadRegister(MFRC522::VersionReg), HEX);

  prefs.begin("rfid", false);
  charger();
  Serial.printf("Charge : %d fiches\n", nFiches);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.print(F("AP \"")); Serial.print(AP_SSID);
  Serial.print(F("\" -> http://")); Serial.println(WiFi.softAPIP());

  dns.start(53, "*", WiFi.softAPIP());

  server.on("/",             handleRoot);
  server.on("/api/state",    apiState);
  server.on("/api/creer",    apiCreer);
  server.on("/api/register", apiRegister);
  server.on("/api/delete",   apiDelete);
  server.on("/api/effacer",  apiEffacer);
  server.on("/api/time",     apiTime);
  server.on("/api/annuler",  apiAnnuler);
  server.onNotFound(handleRoot);
  server.begin();

  Serial.println(F("Serveur pret."));
}

// -------------------------------------------------------------------- loop

void loop() {
  dns.processNextRequest();
  server.handleClient();

  // tant qu'un UID attend d'etre nomme, on ignore les lectures suivantes
  if (pendingUid.length() > 0) {
    if (rfid.PICC_IsNewCardPresent()) rfid.PICC_ReadCardSerial();
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
    return;
  }

  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;

  String uid = uidToString(rfid.uid);

  // anti-rebond : meme badge ignore pendant 2 s, sauf si un mode vient d'etre arme
  if (uid == dernierUid && dernierScan > 0 && millis() - dernierScan < 2000) {
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
    return;
  }
  dernierUid  = uid;
  dernierScan = millis();

  Serial.print("[SCAN] " + uid);
  Serial.println(armEffacer ? " (effacement)" : (armCreer ? " (creation)" : ""));

  if      (armEffacer) effacerBadge(uid);
  else if (armCreer)   capturerBadge(uid);
  else                 traiterBadge(uid);

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}
