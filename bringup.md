# Hardware Bring-Up & Bench Validation Guide (`bringup.md`)

Procédure de validation matérielle progressive pour **une carte seule** puis **deux cartes en banc miroir (ECU ↔ Scanner)**.

> [!CAUTION]
> **Règles de sécurité impératives (voir [`AGENTS.md`](AGENTS.md) & [`README.md`](README.md)) :**
> 1. **Ordre de câblage anti-backfeeding :** Toujours connecter `GND` et `+12V` (via `H3` ou `J1`) **avant** de connecter `CANH`, `CANL` ou `K_LINE`.
> 2. **Isolation USB-C sur véhicule réel :** Si la carte est reliée à un véhicule, le PC branché sur `J2` doit impérativement fonctionner **sur batterie uniquement** (jamais sur secteur).
> 3. **Scope `12V` uniquement :** Ne jamais alimenter au-delà des spécifications 12V automobile (protection buck `U4` / TVS `D1` à `26.0 V`).
> 4. **Limites `3.3V` :** `H1`, `H2`, `IO1..IO20` ne sont **pas tolérants 5V** (`Vmax = 3.6 V`). Ne jamais brancher une sonde logique directement sur `K_LINE` (`12V`) ou `CANH`/`CANL`.

---

## 1. Tests sans flasher (carte vierge — Multimètre uniquement)

À effectuer avant le premier flashage sur chaque nouvelle carte.

### 1.1 Contrôles hors tension (Ohmmètre)
1. **Absence de court-circuit alimentation :**
   * `+12V` ↔ `GND` (`H3` ou `J1`) : pas de court-circuit (impédance élevée / charge capacitive).
   * `3.3V` ↔ `GND` : pas de court-circuit.
2. **Terminaison CAN (`JP1`) :**
   * `CANH` ↔ `CANL` (`J1`) avec `JP1` **fermé** : `~120 Ω`.
   * `CANH` ↔ `CANL` (`J1`) avec `JP1` **ouvert** : haute impédance (`> kΩ`).
   * Banc 2 cartes reliées avec `JP1` fermé aux deux extrémités : `~60 Ω`.

### 1.2 Contrôles sous tension `+12V` (carte vierge, avant USB)
1. Brancher `GND` puis `+12V` (`12.0 V` – `12.6 V`) sur `H3`.
2. **Rails & régulation :**
   * Vérifier `3.3V` présent (alimentation `ESP32-S3-WROOM-1` et `VIO` de `U2` TJA1051T).
   * Vérifier le seuil UVLO si testé à tension variable : `U4` (TPS54331) coupe proprement sous `<= 8.01 V`.
3. **Pont diviseur `VBAT_SENSE` (`R12` `100 kΩ` / `R13` `12 kΩ`, `k = 12 / 112 ≈ 0.10714`) :**
   * Mesurer la tension entre `IO1` et `GND` au multimètre :
     * À `V_BAT = 12.00 V` → `V_ADC ≈ 1.286 V`
     * À `V_BAT = 12.60 V` → `V_ADC ≈ 1.350 V`
     * En USB seul (sans `+12V`) → `V_ADC < 0.300 V` (`+12V_PROT` isolé par `Q1`).

### 1.3 Énumération USB-C (carte vierge, `J2` / `IO19`, `IO20`)
1. Brancher `J2` en USB-C au PC (sans `+12V` ou avec `+12V` déjà établi).
2. Vérifier qu'un port série virtuel (`COMx` sous Windows, `/dev/ttyACM*` sous Linux) apparaît.
3. Si aucun port n'apparaît : maintenir `IO0` (`TP10`) à la masse tout en appuyant sur `EN` (`SW1` / `TP11`) pour forcer le bootloader ROM.

---

## 2. Flash & Tests mono-carte (USB-C seul)

1. Flasher et ouvrir la console série :
   ```powershell
   idf.py -p COMx flash monitor
   ```
2. Vérifier le prompt `obd2> ` et exécuter :
   * `info` → doit afficher `ESP32-S3`, `Flash : 16 MB`, `PSRAM : 2 MB` et le mapping GPIO.
   * `led breath`, `led blink`, `led solid`, `led burst`, `led sos`, `led duty 50` → vérifier visuellement `LED1` (`IO2`).
   * `vbat` → en USB seul, doit afficher `V_ADC < 300 mV` (`V_BAT < 2.8 V`).

---

## 3. Tests mono-carte sous `+12V` (Multimètre + Analyseur logique 8 canaux USB)

### 3.1 Branchement des sondes (`3.3V` uniquement)
Connecter l'analyseur logique USB 8 canaux (ou oscilloscope) sur les headers de debug `3.3V` :
* **Header `H1` :** `KLINE_RX` (`IO4`), `KLINE_TX` (`IO5`), `GND`
* **Header `H2` :** `TWAI_RX` (`IO16`), `TWAI_TX` (`IO15`), `GND`

### 3.2 Calibration `VBAT_SENSE`
1. Appliquer `GND` puis `+12V` sur `H3` (ex. `12.60 V` mesurés au multimètre sur `H3`).
2. Lancer `vbat` dans la console `obd2> ` et comparer `V_ADC` / `V_BAT` avec la mesure multimètre.

### 3.3 K-Line `10,400 baud` & Fast-Init (`U3` L9637D)
> [!NOTE]
> `kline echo` nécessite le `+12V` sur `H3`/`J1` afin d'alimenter le côté 12V du transceiver `U3` (L9637D).

1. **`kline echo` :**
   * Résultat CLI attendu : `K-Line TX 4 bytes, RX echo 4 bytes: 81 10 F1 81 -> ECHO OK`.
   * Analyseur logique (`H1`) : trame UART `10,400 baud` (8N1, bit = `96.15 µs`) visible sur `KLINE_TX` (`IO5`) et recopiée quasi instantanément sur `KLINE_RX` (`IO4`).
2. **`kline pulse` :**
   * Résultat CLI attendu : `K-Line 25 ms dominant / 25 ms recessive Fast-Init pulse sent on IO5`.
   * Analyseur logique (`H1`) : mesurer sur `KLINE_TX` et `KLINE_RX` une impulsion basse de **`25 ± 1 ms`** suivie d'un retour haut de **`25 ± 1 ms`**.

### 3.4 TWAI / CAN `500 kbps` en mono-carte (`U2` TJA1051T)
1. Lancer `can ping 500`.
2. Analyseur logique (`H2`) : observer l'activité `500 kbps` (bit = `2 µs`) sur `TWAI_TX` (`IO15`). En mono-carte, le CLI doit signaler un timeout RX (aucun autre nœud pour acquitter la trame).

---

## 4. Tests à deux cartes (Banc miroir ECU ↔ Scanner)

### 4.1 Câblage du banc
1. Flasher **le même firmware `obd2_firmware.bin`** sur les deux cartes (**Carte A = ECU**, **Carte B = Scanner/Bench**).
2. Vérifier que `JP1` est **fermé** sur les deux cartes (`~60 Ω` entre `CANH` et `CANL`).
3. **Ordre de mise sous tension :**
   * Couper l'alimentation `+12V`, relier le faisceau `J1` (ou `H3` + bus `CANH`, `CANL`, `K_LINE`, `GND`), puis allumer le `+12V`.
   * Brancher les deux cartes en USB-C au PC (ex. `COMx` pour ECU, `COMy` pour Scanner) et ouvrir deux moniteurs `idf.py -p COMx monitor` / `idf.py -p COMy monitor`.

### 4.2 Test Ping / Pong CAN `500 kbps`
1. Sur la **Carte A (ECU)** :
   ```text
   obd2> can pong 10000
   ```
2. Dans les 10 secondes, sur la **Carte B (Scanner)** :
   ```text
   obd2> can ping
   ```
3. **Critère de réussite :**
   * Carte A affiche `CAN RX ID=0x7DF, replying 0x7E8...` puis `CAN TX 0x7E8: ESP_OK`.
   * Carte B affiche `CAN RX ID=0x7E8 DLC=8 data=06 41 00 BF ...`.
   * Sur l'analyseur logique (`H2`), les trames `0x7DF` et `0x7E8` à `500 kbps` sont propres avec bit ACK valide.
