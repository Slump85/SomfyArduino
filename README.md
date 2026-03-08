# SomfyArduino

Firmware ESP8266 pour contrôler des volets **Somfy RTS** via une interface web. Permet de piloter jusqu'à 8 volets individuels et 6 groupes, avec des scènes programmées (heure fixe, lever/coucher du soleil).

---

## Fonctionnalités

- **Contrôle multi-volets** : jusqu'à 8 volets indépendants, regroupables en 6 groupes
- **Interface web** (port 8090) avec deux onglets :
  - *Utilisation* : commandes Monter / Stop / Descendre par volet ou groupe, gestion des scènes
  - *Configuration* : ajout/suppression de volets et de groupes, sans recompiler
- **Scènes / Horaires** : jusqu'à 8 scènes automatiques déclenchées par :
  - heure fixe (avec sélection des jours de la semaine)
  - lever du soleil ± décalage en minutes
  - coucher du soleil ± décalage en minutes
- **Calcul astronomique** lever/coucher du soleil (précision ±2 min, basé sur NOAA)
- **Portail WiFi** : au premier démarrage, le device crée un AP `SomfyConfig` pour configurer le réseau WiFi sans recompiler
- **OTA** : mise à jour du firmware par WiFi (ArduinoOTA), la LED clignote pendant le transfert
- **LEDs d'état** : LED fonctionnement (désactivable depuis l'interface), LED montée, LED descente
- **Persistance EEPROM** : toute la configuration (volets, groupes, scènes, codes tournants, credentials WiFi) est sauvegardée et survivre aux redémarrages

---

## Matériel requis

| Composant | Détails |
|-----------|---------|
| Carte | ESP8266 (Wemos D1 Mini ou compatible) |
| Émetteur RF | Module 433/868 MHz Somfy RTS sur **GPIO5** |
| LED fonctionnement | GPIO4 |
| LED montée | GPIO14 |
| LED descente | GPIO12 |

### Schéma des broches

```
GPIO5  (D1) ── TX Somfy RTS
GPIO4  (D2) ── LED fonctionnement
GPIO14 (D5) ── LED montée
GPIO12 (D6) ── LED descente
```

---

## Premier démarrage

1. Flasher le firmware sur l'ESP8266
2. Le device démarre en mode **Point d'Accès** : réseau WiFi `SomfyConfig` (sans mot de passe)
3. Se connecter au réseau depuis un téléphone ou PC
4. Le navigateur s'ouvre automatiquement sur `192.168.4.1` (ou y accéder manuellement)
5. Sélectionner son réseau WiFi, saisir le mot de passe, cliquer **Enregistrer**
6. Le device redémarre et se connecte au réseau configuré
7. L'interface est accessible à `http://<IP>:8090/`

> L'IP locale est affichée dans la barre de statut en haut de l'interface web, et dans les logs série (115200 baud).

---

## Interface web

Accessible sur `http://<IP>:8090/`

### Onglet Utilisation

- **Groupes** : cartes avec boutons Monter / Stop / Descendre pour chaque groupe
- **Volets** : idem pour chaque volet individuel + bouton PROG (appairage)
- **Scènes / Horaires** : formulaire de création d'une scène + tableau des scènes actives

### Onglet Configuration

- **Volets** : tableau éditable en ligne (nom, indice de télécommande), suppression par slot
- **Groupes** : tableau avec membres (bitmask), ajout via cases à cocher

---

## Mise à jour OTA

- Hostname : `somfy-rts`
- Mot de passe : `somfy1234`

Depuis l'IDE Arduino ou `espota.py` :
```bash
python espota.py -i <IP> -p 8266 -a somfy1234 -f firmware.bin
```

La **LED fonctionnement clignote** pendant le transfert.

---

## API REST

Toutes les routes répondent en JSON sur le port **8090**.

| Endpoint | Méthode | Description |
|----------|---------|-------------|
| `/` | GET | Interface web principale |
| `/api/status` | GET | État complet (WiFi, heure, volets, groupes) |
| `/api/volet?id=N&cmd=up\|stop\|down\|prog` | GET | Commander un volet |
| `/api/groupe?id=N&cmd=up\|stop\|down` | GET | Commander un groupe |
| `/api/volet-config` | GET / POST / DELETE | Lire / modifier / supprimer un volet |
| `/api/groupe-config` | GET / POST / DELETE | Lire / modifier / supprimer un groupe |
| `/api/scenes` | GET | Lister les scènes |
| `/api/scene` | POST | Créer / modifier une scène |
| `/api/scene` | DELETE | Supprimer une scène |
| `/api/sun` | GET | Heures lever/coucher du soleil |
| `/api/led` | GET / POST | État / toggle de la LED fonctionnement |

---

## Configuration avancée

Les paramètres suivants sont des `#define` dans le `.ino` :

| Define | Valeur par défaut | Description |
|--------|-------------------|-------------|
| `OTA_HOSTNAME` | `"somfy-rts"` | Nom du device pour l'OTA |
| `OTA_PASSWORD` | `"somfy1234"` | Mot de passe OTA |
| `NTP_SERVER` | `"pool.ntp.org"` | Serveur de temps |
| `TZ_INFO` | `"CET-1CEST,..."` | Fuseau horaire (Europe/Paris) |
| `LATITUDE` | `43.607` | Latitude pour le calcul solaire |
| `LONGITUDE` | `1.333` | Longitude pour le calcul solaire |
| `MAX_VOLETS` | `8` | Nombre max de volets |
| `MAX_GROUPES` | `6` | Nombre max de groupes |
| `NB_SCENES` | `8` | Nombre max de scènes |

Pour activer l'authentification HTTP Basic, décommenter :
```cpp
#define USE_BASIC_AUTH 1
```

---

## Structure EEPROM (VERSION 5)

| Champ | Taille | Description |
|-------|--------|-------------|
| `appVersion` | 4 o | Version du schéma EEPROM |
| `remotes[16]` | 96 o | ID Somfy + code tournant par télécommande |
| `scenes[8]` | 72 o | Scènes / horaires |
| `volets[8]` | 176 o | Configuration des volets |
| `groupes[6]` | 138 o | Configuration des groupes |
| `wifiSSID[33]` | 33 o | SSID WiFi |
| `wifiPass[64]` | 64 o | Mot de passe WiFi |
| **Total** | **583 o** | < 1024 o alloués |

Les migrations automatiques sont gérées lors du boot (v1 → v5, v2 → v5, etc.).

---

## Dépendances

- **Arduino core for ESP8266** (`esp8266/Arduino`)
- `ESP8266WiFi`
- `ESP8266WebServer`
- `DNSServer`
- `ArduinoOTA`
- `EEPROM`
- `time.h` (POSIX, inclus dans le core ESP8266)
