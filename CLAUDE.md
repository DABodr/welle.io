# welle.io — Référence projet pour Claude

## Vue d'ensemble

**welle.io** est un récepteur DAB/DAB+ open-source composé de deux applications :
- **welle-io** : interface graphique Qt6
- **welle-cli** : interface CLI + serveur web HTTP embarqué

Version courante : lire `src/welle-gui/_current_version`
Standard C++ : C++14
Build : CMake ≥ 3.2

---

## Structure du projet

```
src/
├── backend/          # Décodeur DAB/DAB+ (OFDM, FIC, MSC, audio)
├── input/            # Drivers SDR (RTL-SDR, Airspy, SoapySDR, fichier)
├── various/          # Utilitaires (Socket, FFT, channels, profiling)
├── libs/             # Bibliothèques embarquées (fec, kiss_fft, faad2, mpg123)
├── welle-cli/        # Application CLI + serveur web
└── welle-gui/        # Application GUI Qt6
```

---

## welle-cli — Architecture détaillée

### Fichiers sources

| Fichier | Rôle |
|---|---|
| `welle-cli.cpp` | Point d'entrée, parsing CLI (getopt), orchestration |
| `webradiointerface.cpp/.h` | Serveur HTTP, API REST, gestion multi-clients |
| `webprogrammehandler.cpp/.h` | Encodage MP3/FLAC, distribution aux clients |
| `jsonconvert.cpp/.h` | Sérialisation JSON (nlohmann) des données radio |
| `alsa-output.cpp/.h` | Sortie audio ALSA (lecture locale) |
| `tests.cpp/.h` | Tests de résilience (bruit gaussien, multipath) |
| `index.html` | Interface web embarquée (thème sombre, responsive) |
| `index.js` | Logique web (polling HTTP, Canvas, audio HTML5) |

### Modes de fonctionnement (welle-cli)

```
welle-cli [OPTIONS]
├─ (défaut)     → RadioReceiver + AlsaOutput (lecture directe)
├─ --programme  → Sélection d'un programme par nom/SId
├─ --write-to-file → Dump WAV (WavProgrammeHandler)
├─ --web-port N → Serveur HTTP sur port N (WebRadioInterface)
│   Stratégies de décodage :
│   ├─ OnDemand      : décode uniquement si client connecté
│   ├─ All           : tous les services en permanence
│   ├─ Carousel10    : rotation 10s par service
│   └─ CarouselPAD   : rotation pilotée par DLS+slide (80s max)
└─ --tests N    → Suite de tests (bruit, multipath)
```

### API HTTP (WebRadioInterface)

| Endpoint | Méthode | Description |
|---|---|---|
| `/` | GET | Interface web (index.html embarqué via xxd) |
| `/index.js` | GET | JavaScript embarqué |
| `/favicon.ico` | GET | Icône embarquée |
| `/mux.json` | GET | État complet du mux en JSON |
| `/mux.m3u` | GET | Playlist M3U de tous les services |
| `/stream/<SId>` | GET | Stream audio MP3 ou FLAC en continu |
| `/slide/<SId>` | GET | Image MOT/slideshow courante |
| `/spectrum` | GET | Spectre RF (float32 binaire) |
| `/impulseresponse` | GET | Réponse impulsionnelle CIR (float32) |
| `/constellation` | GET | Points de constellation OFDM (float32) |
| `/fic` | GET | Stream FIB brut (32 bytes / 23 ms) |
| `/channel` | GET/POST | Lecture ou changement de canal |
| `/fftwindowplacement` | POST | Placement fenêtre FFT |
| `/enablecoarsecorrector` | POST | Activation correction fréquence grossière |
| `/restart` | POST | Envoie SIGTERM → systemd relance le service |

### Flux de données radio

```
CVirtualInput (RTL-SDR / Airspy / SoapySDR / fichier .iq / rtl_tcp)
    ↓ samples IQ (DSPCOMPLEX)
RadioReceiver (décodage OFDM → FIC + MSC)
    ↓ callbacks RadioControllerInterface + ProgrammeHandlerInterface
WebRadioInterface / RadioInterface
    ↓
ProgrammeHandler (un par service actif)
    ├─ AlsaProgrammeHandler → ALSA → haut-parleur
    ├─ WavProgrammeHandler  → fichier WAV
    └─ WebProgrammeHandler
           ├─ LameEncoder   → MP3 → HTTP /stream/<SId>
           └─ FlacEncoder   → FLAC → HTTP /stream/<SId>
```

### Structure JSON /mux.json

```json
{
  "receiver": {
    "hardware": { "name": "...", "gain": 0.0 },
    "software": { "name": "welle-cli", "version": "...", "fftwindowplacement": "...", "coarsecorrectorenabled": true }
  },
  "ensemble": { "label": {...}, "id": "0xABCD", "ecc": "..." },
  "services": [{
    "sid": "0x1234",
    "label": {...},
    "components": [...],
    "dls": { "label": "...", "time": 0, "lastchange": 0 },
    "mot": { "time": 0, "lastchange": 0 },
    "audiolevel": { "left": 0, "right": 0 },
    "errorcounters": { "frameerrors": 0, "rserrors": 0, "aacerrors": 0 }
  }],
  "demodulator": {
    "synced": true,
    "snr": 15.3,
    "frequencycorrection": -1234,
    "fic": { "numcrcerrors": 5 },
    "time_last_fct0_frame": 1234567890
  },
  "utctime": { "year": 2024, "month": 1, "day": 1, "hour": 12, "minutes": 0 },
  "tii": [{ "comb": 2, "pattern": 67, "delay": 128, "delay_km": 18.75, "error": 131.8 }],
  "cir_peaks": [{ "index": 0, "value": 0.0 }]
}
```

### Gestion des threads (WebRadioInterface)

| Mutex | Protège |
|---|---|
| `data_mut` | État radio (synced, SNR, corrections fréquence) |
| `plotdata_mut` | Données spectre, CIR, constellation |
| `fib_mut` | Queue des blocs FIC |
| `rx_mut` | Accès au RadioReceiver |
| `phs_mutex` | Map des WebProgrammeHandler |
| `stats_mutex` (par WebProgrammeHandler) | Stats audio, erreurs, DLS, MOT |

---

## welle-cli — Classes principales

### `WebProgrammeHandler` (webprogrammehandler.h)
Implémente `ProgrammeHandlerInterface`. Gère un service DAB actif.
- `registerSender(socket)` / `removeSender()` : gestion connexions clients
- `needsToBeDecoded()` : vrai si au moins un client connecté
- `cancelAll()` : coupe toutes les transmissions
- `onNewAudio(pcm, rate, mode)` → encode MP3/FLAC → envoie aux clients
- `getDLS()` / `getMOT()` / `getAudioLevels()` / `getErrorCounters()`

### `WebRadioInterface` (webradiointerface.h)
Implémente `RadioControllerInterface`.
- Serveur HTTP mono-thread avec dispatch par path
- Thread séparé `programme_handler_thread` pour gérer le carousel
- Méthode `dispatch_client(socket)` : parse + route les requêtes HTTP

### `AlsaOutput` (alsa-output.h)
- Constructeur : `AlsaOutput(channels, samplerate)`
- `playPCM(vector<int16_t>&&)` : envoie frames PCM à ALSA, gère XRUN

### `LameEncoder` / `FlacEncoder` (webprogrammehandler.cpp)
- Interface `IEncoder::process_interleaved(vector<int16_t>&)`
- LameEncoder : VBR qualité 2
- FlacEncoder : compression niveau 5, header séparé des frames

---

## Backend DAB (src/backend/)

### Fichiers clés

| Fichier | Rôle |
|---|---|
| `radio-receiver.cpp/.h` | Orchestrateur principal, expose l'API publique |
| `ofdm-processor.cpp/.h` | Démodulation OFDM (synchronisation, FFT, dé-interleaving) |
| `ofdm-decoder.cpp/.h` | Décodage OFDM (correction de phase, dé-mapping) |
| `fic-handler.cpp/.h` | Traitement FIC (Fast Information Channel) |
| `fib-processor.cpp/.h` | Parsing FIB (Fast Information Blocks) → services, composants |
| `msc-handler.cpp/.h` | Main Service Channel → extraction sous-canaux |
| `dab-audio.cpp/.h` | Décodage audio DAB (MP2) via mpg123 |
| `dabplus_decoder.cpp/.h` | Décodage audio DAB+ (AAC) via faad2 |
| `pad_decoder.cpp/.h` | Décodage PAD (Programme Associated Data) → DLS, MOT |
| `mot_manager.cpp/.h` | Gestion MOT (Multimedia Object Transfer) → images slideshow |
| `tii-decoder.cpp/.h` | TII (Transmitter Identification Information) |
| `viterbi.cpp/.h` | Décodage de Viterbi (correction d'erreurs convolutionnels) |
| `eep-protection.cpp/.h` | Protection EEP (Equal Error Protection) |
| `uep-protection.cpp/.h` | Protection UEP (Unequal Error Protection) |
| `phasereference.cpp/.h` | Référence de phase (synchronisation symbole nul) |

### Interfaces principales (radio-receiver.h)

```cpp
// Callbacks état récepteur
class RadioControllerInterface {
    virtual void onSyncChange(bool isSync);
    virtual void onServiceDetected(uint32_t sId);
    virtual void onNewEnsemble(uint16_t eId);
    virtual void onSetEnsembleLabel(DabLabel label);
    virtual void onDateTimeUpdate(const dab_date_time_t& dateTime);
    virtual void onFIBDecodeSuccess(bool crcOk, const uint8_t* fib);
    virtual void onTIIMeasurement(tii_measurement_t m);
    virtual void onMessage(message_level_t level, const std::string& msg, const std::string& filename);
    virtual void onInputFailure();
};

// Callbacks données audio/PAD par service
class ProgrammeHandlerInterface {
    virtual void onFrameErrors(int frameErrors);
    virtual void onNewAudio(std::vector<int16_t>&& audioData, int sampleRate, const std::string& mode);
    virtual void onRsErrors(bool uncorrectedErrors, int numRsErrors);
    virtual void onAacErrors(int aacErrors);
    virtual void onNewDynamicLabel(const std::string& label);
    virtual void onMOT(const mot_file_t& mot_file, int subtype);
    virtual void onPADLengthError(size_t announced_xpad_len, size_t xpad_len);
};
```

---

## Inputs SDR (src/input/)

| Fichier | Driver | Option CMake |
|---|---|---|
| `rtl_sdr.cpp` | RTL-SDR (librtlsdr) | `-DRTLSDR=ON` |
| `airspy_sdr.cpp` | Airspy (libairspy) | `-DAIRSPY=ON` |
| `soapy_sdr.cpp` | SoapySDR générique | `-DSOAPYSDR=ON` |
| `rtl_tcp.cpp` | RTL-TCP (réseau) | toujours |
| `raw_file.cpp` | Fichier .iq brut | toujours |
| `null_device.cpp` | Entrée nulle (tests) | toujours |
| `input_factory.cpp` | Factory de création | toujours |

---

## Build

> **Attention** : un binaire système peut exister dans `/usr/local/bin/welle-cli`.
> Vérifier lequel tourne avec `readlink /proc/$(pgrep welle-cli)/exe`.
> Après recompilation, soit lancer `build/welle-cli` directement, soit `sudo make install`.

```bash
mkdir build && cd build
# Build minimal (CLI only, RTL-SDR)
cmake .. -DBUILD_WELLE_IO=OFF -DRTLSDR=ON
make -j$(nproc)

# Build avec FLAC et Airspy
cmake .. -DBUILD_WELLE_IO=OFF -DRTLSDR=ON -DAIRSPY=ON -DFLAC=ON
make -j$(nproc)

# Les fichiers web sont compilés via xxd :
# index.html → index.html.h (variable index_html)
# index.js   → index.js.h   (variable index_js)
# favicon.ico → favicon.ico.h (variable favicon_ico)
```

### Dépendances runtime welle-cli

| Bibliothèque | Usage | Obligatoire |
|---|---|---|
| libfftw3f | FFT (alternative : KissFFT embarqué) | Oui (sauf KISS_FFT=ON) |
| libfaad2 | Décodage AAC (DAB+) | Oui |
| libmpg123 | Décodage MP2 (DAB) | Oui |
| libmp3lame | Encodage MP3 streaming | Oui |
| libFLAC++ | Encodage FLAC streaming | Non (-DFLAC=ON) |
| libasound (ALSA) | Sortie audio locale | Non (détecté auto) |
| librtlsdr | Driver RTL-SDR | Non (-DRTLSDR=ON) |
| libairspy | Driver Airspy | Non (-DAIRSPY=ON) |
| SoapySDR | Driver générique SDR | Non (-DSOAPYSDR=ON) |

---

## Interface web (index.html / index.js)

- **Polling** toutes les 1s sur `/mux.json` (pas de WebSocket)
- **Graphiques** : Canvas HTML5 (spectre, CIR, constellation) — fetch binaire float32
- **Audio** : `<audio>` HTML5 natif, src = `/stream/<SId>`
- **Responsive** : thème sombre, cards mobile (< 900px), colonnes masquées
- **MOT/SLS inline** : vignette 70×70 dans la colonne 13 du tableau ; préchargement via `new Image()` dans `slsCache` avant rebuild DOM (évite le "?" de chargement) ; URL `/slide/<decimal_sid>?t=<mot.time>` (`parseInt(sid)` obligatoire, le JSON donne l'hex)
- **Modal slide** : nom station (22px gras) + DLS (16px italique) ; fermeture par clic sur l'overlay ; pas de bouton ✕
- **SNR widget** : bargraphe segmenté (20 segments, rouge→orange→jaune→vert, 0–30 dB)
- **TII** : affichage MainId (pattern) / SubId (comb) ; lookup site via `tii_db` (variable JS) ; entrées sans site connues masquées
- **Bouton Restart** : footer, envoie `POST /restart` → welle-cli reçoit SIGTERM → systemd relance en 3s
- **Sélecteur canal** : désactivé pendant la requête POST, timeout 8s pour éviter blocage permanent (bug librtlsdr cancel_async)
- **Ordre scripts** : les balises `<script>` sont placées après le footer pour que tous les éléments DOM soient disponibles
- **Scan de canaux** : bouton ⊕ Scan dans la barre de contrôle ; modale de progression avec barre et compteur ; résultats persistants en badges cliquables sous la barre de contrôle
- **Logique scan** : `postChannel()` XHR 8s timeout → attente sync 3s → si `demodulator.synced` = true : polling `/mux.json` toutes les 1s jusqu'à 20 tentatives → label nettoyé (`\x00` strippés) → ajouté à `scanFoundMux[]` seulement si label non vide
- **Barre de contrôle** : `align-items: stretch` sur `.channel-ctrl` → hauteur uniforme entre `<select>`, bouton Scan et bouton Paramètres, desktop et mobile

---

## Scripts utilitaires (scripts/)

| Fichier | Rôle |
|---|---|
| `generate-tii-db.py` | Génère `var tii_db` dans index.js depuis un CSV FMLIST (séparateur `;`, encodage latin-1) |
| `welle-cli.service` | Unit systemd avec `Restart=always`, `RestartSec=3` |

### Workflow mise à jour avec base TII locale

```bash
# Sur la machine de déploiement (après git pull)
git checkout src/welle-cli/index.js        # réinitialiser avant pull si modifié localement
git pull
python3 scripts/generate-tii-db.py /chemin/vers/dab-tx-list.csv
make -j$(nproc) -C build welle-cli
sudo make -C build install
sudo systemctl restart welle-cli
```

**Format clé tii_db** : `EID_MMSS` où `EID` = ensemble ID sans `0x` en majuscules, `MM` = pattern (2 chiffres), `SS` = comb (2 chiffres). Ex : `F069_6702`.

**Attention** : ne pas committer index.js après injection (données FMLIST sous copyright).

---

## Canaux DAB (Europe)

Bande III : 5A–13F (174–240 MHz)
L-Band : LA–LW (1452–1492 MHz)
Définis dans `src/various/channels.cpp`
