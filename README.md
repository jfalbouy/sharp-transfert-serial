# sharp_comm — Transfert série PC ⇄ Sharp PC-E500S

*(English version: [README.en.md](README.en.md))*

Outil en ligne de commande (Windows) pour transférer des programmes **BASIC en texte ASCII** entre un PC et un ordinateur de poche **Sharp PC-E500S**, via une liaison série RS-232.

- **Émission** (PC → Sharp) : le PC envoie un fichier `.BAS`, le Sharp le reçoit avec `LOAD`.
- **Réception** (Sharp → PC) : le Sharp émet avec `SAVE "COM:"`, le PC enregistre le fichier.

>
> Le programme **n'automatise pas** le Sharp : il vous affiche la commande à taper sur la
> calculatrice, puis attend que vous appuyiez sur Entrée. Vous gardez le contrôle du Sharp.
>

---

## 1. Prérequis

- **Windows** avec un port série (natif ou adaptateur **USB-série**).
- Un **câble / convertisseur de niveau** Sharp (ex. CE-130T) reliant le PC au PC-E500S.
- Un compilateur C (MinGW `gcc` ou MSVC `cl`) pour construire l'exécutable.

### Réglage important de l'adaptateur USB-série

Le flow control XON/XOFF doit réagir vite. Dans le **Gestionnaire de périphériques** → *Ports (COM et LPT)* → votre port → **Propriétés** → **Paramètres du port** → **Avancés…** :

- **Prolific PL2303** : passer le **FIFO « Receive Buffer » sur Low (1)** (et « Transmit Buffer » sur Low (1)).
- **FTDI** : mettre le **Latency Timer à 1 ms**.

Sans ce réglage, le XOFF du Sharp est détecté trop tard et le transfert peut échouer (I/O ERROR).

---

## 2. Compilation

```bash
# MinGW (gcc) — le -lwinmm est requis (timeBeginPeriod)
gcc sharp_pc_e500s_comm.c -o sharp_comm.exe -lwinmm

# ou MSVC (winmm est lié automatiquement via #pragma)
cl sharp_pc_e500s_comm.c
```

Vérifier :

```bash
sharp_comm.exe --help
sharp_comm.exe --show-serial -p COM1     # affiche la config actuelle du port
```

---

## 3. Réglages côté Sharp PC-E500S

Le PC-E500S doit être configuré au **même débit** que le PC. Le programme affiche la bonne ligne à taper (avec le débit choisi). Config de référence :


| Paramètre  | Valeur                       |
|-------------|------------------------------|
| Débit      | 9600 bauds (recommandé)     |
| Format      | 8 bits, parité None, 1 stop |
| Code        | A (ASCII)                    |
| Délimiteur | **L** (CR + LF)              |
| EOF         | **&H1A** (Ctrl+Z)        |
| XON/XOFF    | **X** (activé)              |
| Shift       | N                            |


Commande d'ouverture (tapée **sur le Sharp**) :

```
OPEN "COM:9600,N,8,1,A,L,&H1A,X,N"
```

puis `LOAD` pour recevoir, ou `SAVE "COM:"` pour émettre.

> Le Sharp n'accepte **pas** de débit supérieur à 9600 bauds dans la commande
> `OPEN "COM:xxxx,..."`. Pour utiliser 19200 bauds, il faut modifier la valeur du SIO baud
> à l'adresse `0BFD33h`, comme indiqué page 53 du manuel technique.
>
> Pour passer à **19200 bauds**, saisir sur le Sharp :
>
> ```
> POKE &HBFD33, PEEK &HBFD33 OR &H70
> ```
>
> Valeur obtenue `&H78` / 120d, soit les bits 6, 5 et 4 à la valeur 1.
>
> Pour revenir à **9600 bauds** sans modifier les autres paramètres, saisir sur le Sharp :
>
> ```
> POKE &HBFD33, (PEEK &HBFD33 AND &H8F) OR &H60
> ```
>
> Valeur obtenue `&H68` / 104d, soit les bits 6, 5, 4 remis à 110.

Puis taper `OPEN "COM:19200,..."` sur le Sharp. Le programme rappelle la ligne exacte à
taper, avec le débit réellement sélectionné.

---

## 4. Utilisation

### Émission : PC → Sharp

```bash
sharp_comm.exe --mode send monprog.bas -p COM1
```

Déroulé :

1. Le programme affiche la commande `OPEN … / LOAD` à taper.
2. Sur le Sharp, tapez `OPEN "COM:9600,N,8,1,A,L,&H1A,X,N"` puis `LOAD`.
3. Appuyez sur **Entrée** côté PC : le fichier est envoyé.
4. **Vérifiez avec `LIST` sur le Sharp** que le programme est bien chargé (voir §6).


### Réception : Sharp → PC

```bash
sharp_comm.exe --mode receive recupere.bas -p COM1
```

Déroulé :

1. Le programme affiche la commande `OPEN … / SAVE "COM:"`.
2. Sur le Sharp, tapez `OPEN "COM:9600,N,8,1,A,L,&H1A,X,N"`.
3. Appuyez sur **Entrée** côté PC **pour armer la réception**.
4. **Ensuite seulement**, lancez `SAVE "COM:"` sur le Sharp. *(L'ordre compte.)*
5. La réception s'arrête sur l'EOF (`0x1A`) ou après un silence prolongé.


---

## 5. Options

```
-p, --port <port>       Port COM (ex: COM1, COM2). Défaut: COM1
-b, --baud <rate>       Débit : 300, 600, 1200, 2400, 4800, 9600, 19200. Défaut: 9600
-m, --mode <mode>       'send' ou 'receive'
--char-delay <ms>   Délai entre paquets (0 = flow control XON/XOFF). Défaut: 0
--line-delay <ms>   Délai entre lignes BASIC. Défaut: 0
--chunk <n>         Octets par écriture série. Défaut: 16
--idle-timeout <ms> Silence max avant arrêt (réception). Défaut: 3000
--rts <on|off>      RTS (entrée CS du Sharp, requis pour XON/XOFF). Défaut: on
--dtr <on|off>      DTR (entrée CD du Sharp). Défaut: off
-v, --verbose           Affiche les données / lignes transférées
-s, --show-serial       Affiche les paramètres du port série puis quitte
-h, --help              Affiche l'aide
```

### Profil rapide (défaut) et repli

- **Défaut = rapide** : `--rts on --char-delay 0 --line-delay 0 --chunk 16`
  → ~**305 octets/s**, régulé par le XON/XOFF du Sharp. Rien à préciser.
- **Repli** si `I/O ERROR` côté Sharp (flow control indisponible) :
  ```bash
  sharp_comm.exe --mode send monprog.bas -p COM1 --chunk 1 --char-delay 16
  ```
  Envoi cadencé « aveugle », ~58 octets/s, mais qui fonctionne sans flow control.

---

## 6. Vérification — important

**En émission (PC → Sharp), il n'y a AUCUN accusé de réception.** Le message « Émission terminée (côté PC) » signifie seulement que le PC a fini d'écrire ses octets — **pas** que le Sharp a tout accepté.

➡️ **Vérifiez toujours avec `LIST` sur le Sharp** que le programme est complet et non corrompu. Une fin de transfert rapide n'est pas une preuve de succès.

---

## 7. Dépannage


| Symptôme                                    | Cause probable                                                     | Solution                                                                                                   |
|----------------------------------------------|--------------------------------------------------------------------|------------------------------------------------------------------------------------------------------------|
| `I/O ERROR` sur l'écran du Sharp            | Le Sharp ne peut pas réguler (XOFF impossible) car son CS est bas | Garder **`--rts on`** (défaut) ; régler le FIFO USB en Low (1) ; sinon repli `--chunk 1 --char-delay 16` |
| `LIST` montre un programme tronqué/corrompu | Envoi trop rapide sans flow control effectif                       | Repli `--chunk 1 --char-delay 16`, ou vérifier le réglage FIFO de l'adaptateur                           |
| Aucun octet reçu (réception)               | `SAVE` lancé avant d'avoir armé le PC, ou mauvais port           | Armer le PC **avant** `SAVE` ; vérifier `-p COMx` avec `--show-serial`                                    |
| « Impossible d'ouvrir le port »            | Port occupé (autre logiciel) ou inexistant                        | Fermer l'autre application ; vérifier le numéro de port                                                  |
| Caractères parasites dans le fichier reçu  | Débit PC ≠ débit Sharp                                         | Ouvrir le Sharp au **même débit** (`OPEN "COM:<baud>,…"`)                                        |


---

## 8. Notes techniques

- **RTS=ON est la clé de l'émission rapide.** Le Sharp ne transmet (ses données *et* le XOFF qui freine le PC) que si son entrée **CS (broche 5)** est haute, pilotée par le RTS du PC (*Technical Reference* p. 54, `SIO send port condition` = 04H). RTS bas ⇒ pas de XOFF ⇒ débordement ⇒ I/O ERROR.
- **Le débit n'accélère pas l'émission.** `LOAD` tokenise le BASIC à ~305 octets/s (limité par le CPU du Sharp) : 9600 et 19200 donnent le même temps. On garde 9600.
- **La réception, elle, profite du débit** : `SAVE` recrache la mémoire vite (limité par le fil), donc 19200 est ~2× plus rapide que 9600 en réception.
- **Fins de ligne** : quel que soit le format d'entrée (`\n`, `\r`, `\r\n`), le programme envoie du **CR+LF**, conforme au délimiteur `L`.
- **Fichiers** : ASCII simple, **sans BOM UTF-8 ni caractères accentués**.

>
> Un équivalent PowerShell existe (`Powershell/toSharp.ps1`, `Powershell/fromSharp.ps1`)
> avec le même profil rapide.
>

---

## 9. Parcours d'optimisation (58 → 305 octets/s)

Trace du raisonnement qui a mené au profil rapide, utile pour comprendre *pourquoi* les réglages sont ce qu'ils sont.

**Point de départ.** L'émission PC → Sharp prenait **3 min 41 s** pour 12 733 octets, soit **~58 octets/s**. Le débit série (9600) n'y changeait rien.


| Étape | Hypothèse testée                                   | Résultat                                                                                                                                                                           |
|--------|------------------------------------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| 1      | Le délai par octet est-il vraiment de 1 ms ?        | Non : `Sleep(1)` dure **~15,6 ms** (granularité du timer Windows). Ajout de `timeBeginPeriod(1)` pour des délais réels.                                                          |
| 2      | Réduire le délai par octet (8/4/2 ms)              | **I/O ERROR** côté Sharp. Le plancher fiable en octet-par-octet est ~10–16 ms → impasse pour la vitesse.                                                                      |
| 3      | Streaming en gros paquets, `--char-delay 0`          | I/O ERROR, même après avoir mis le FIFO USB en Low (1). Sans régulation, le débordement est inévitable.                                                                        |
| 4      | **Et si le Sharp ne pouvait pas envoyer XOFF ?**     | 🎯 **Trouvé.** Le Sharp ne transmet que si son entrée **CS est haute** (*Tech Ref* p. 54). Or CS = RTS du PC, et RTS était **OFF** en émission → le XOFF ne partait jamais. |
| 5      | `--rts on` + streaming                               | ✅ **304 octets/s, `LIST` intègre.** Le flow control régule enfin le PC à la vitesse de `LOAD`.                                                                                 |
| 6      | Pousser plus loin (chunk 32/64, line-delay 0, 19200) | Aucun effet : **305 o/s est le plafond de tokenisation de `LOAD`** (CPU-bound). Le fil n'est pas le goulot.                                                                         |


**Résultat : 58 → 305 octets/s (×5,3), et surtout fiable.**

**Enseignements clés :**

- La granularité du timer Windows masquait le vrai comportement des délais (`Sleep(1)` ≈ 15,6 ms).
- **RTS=ON est indispensable** pour que le XON/XOFF fonctionne en émission (il alimente le CS du Sharp).
- Une fois le flow control actif, tous les réglages côté PC (chunk, line-delay, débit) deviennent sans effet : c'est le Sharp qui dicte le rythme.
- L'émission (`LOAD`, CPU-bound) et la réception (`SAVE`, wire-bound) sont **asymétriques** : le débit aide la réception, pas l'émission.

## 10. Valeurs de transfert entre le Sharp et le PC


| Fichier | Taille (octets) | Débit (bits/s) | Sens           | Temps (s) | Vitesse (bits/s) |
|---------|-----------------|-----------------|----------------|-----------|------------------|
| ISOTOP  | 17487           | 9600            | PC -> Sharp | 56.27     | 311              |
| ISOTOP  | 17487           | 9600            | Sharp -> PC | 27.23     | 642              |
| ISOTOP  | 17487           | 19200           | PC -> Sharp | 55.27     | 316              |
| ISOTOP  | 17487           | 19200           | Sharp -> PC | 16.30     | 1073             |
| ELECTR  | 9297            | 9600            | PC -> Sharp | 28.05     | 331              |
| ELECTR  | 9297            | 9600            | Sharp -> PC | 13.97     | 666              |
| ELECTR  | 9297            | 19200           | PC -> Sharp | 27.56     | 337              |
| ELECTR  | 9297            | 19200           | Sharp -> PC | 8.36      | 1112             |
| 62015_2 | 20299           | 9600            | PC -> Sharp | 68.67     | 296              |
| 62015_2 | 20299           | 9600            | Sharp -> PC | 31.11     | 652              |
| 62015_2 | 20299           | 19200           | PC -> Sharp | 67.50     | 301              |
| 62015_2 | 20299           | 19200           | Sharp -> PC | 18.41     | 1103             |


## 11. Valeurs des paramètres internes de communication du Sharp

*(Extrait du Technical Reference, conservé en anglais.)*

- **SIO timer master** : **0BFD31h et 0BFD32h**
  - Time n on error timer * 0.5s. However, 0FFFFh is unlimited. Default value = 0FFFFh (unlimited)
- **SIO baud rate** : **0BFD33h**
  - Specify baud rate, length and parity. Default value = 3Ch / 60d / 00111100b
  - Bits 6, 5, 4 : baud rate -> 000 = None, 001 = 300, 010 = 600, 011 = 1200, 100 = 2400, 101 = 4800, 110 = 9600, 111 = 19200
  - Bits 3, 2 : parity -> 00 = Even parity, 01 = Odd parity, 10 = Non parity, 11 = Non parity
  - Bit 1 : length -> 0 = 8 bits, 1 = 7 bits
  - Bit 0 : stop bit -> 0 = 1 bit, 1 = 2 bits
- **SIO setup** : **0BFD34h**
  - Specify shift in/out, X on/off. Specify transfer of transmission code at open/close. Default value = 21h / 33d
  - Bit 6 = 0 : 1 byte data stored in SIO open send data is not transmitted at open state
  - Bit 6 = 1 : transferred SIO open send data = 0BFD61h
  - Bit 4 = 0 : 1 byte data stored in SIO close send data is not transmitted at close state
  - Bit 4 = 1 : transferred SIO close send data = 0BFD62h
  - Bit 2 = 0 : without X on/off designation at receiving
  - Bit 2 = 1 : with designation
  - Bit 1 = 0 : without X on/off designation at sending
  - Bit 1 = 1 : with designation
  - Bit 0 = 0 : without shift in/out designation
  - Bit 0 = 1 : with designation
- **SIO receive port condition** : **0BFD35h**
  - Control of receive port. Default value = 02h
  - Bit 2 CS = 0 : don't care
  - Bit 2 CS = 1 : take in as receiving data when the CS signal is high and ignore at low
  - Bit 1 CD = 0 : don't care
  - Bit 1 CD = 1 : take in as receiving data when the CD signal is high and ignore at low
- **SIO receive port control** : **0BFD36h**
  - Control of receive port. Default value = 0DFh
  - Bit 6 ER = 0 : when receiving buffer becomes full, ER signal becomes low
  - Bit 6 ER = 1 : don't care
  - Bit 5 RR = 0 : when receiving buffer becomes full, RR signal becomes low
  - Bit 5 RR = 1 : don't care
  - Bit 4 RS = 0 : when receiving buffer becomes full, RS signal becomes low
  - Bit 4 RS = 1 : don't care
- **SIO send port condition** : **0BFD37h**
  - Control of send port. Default value = 04h
  - Bit 2 CS = 0 : don't care
  - Bit 2 CS = 1 : transmit when the CS signal is low, wait until it becomes high
  - Bit 1 CD = 0 : don't care
  - Bit 1 CD = 1 : transmit when the CD signal becomes high. When the CD signal is low, wait until it becomes high
- **SIO send port control** : **0BFD38h**
  - Control of send port. Default value = 050h
  - Bit 6 ER = 0 : don't care
  - Bit 6 ER = 1 : ER signal becomes high before transfer of transmission data block and becomes low after transfer
  - Bit 5 RR = 0 : don't care
  - Bit 5 RR = 1 : RR signal becomes high before transfer of transmission data block and becomes low after transfer
  - Bit 4 RS = 0 : don't care
  - Bit 4 RS = 1 : RS signal becomes high before transfer of transmission data block and becomes low after transfer
- **SIO send delay** : **0BFD39h**
  - <00-0FFh> * 2 ms wait time is specified before or after transmission data block at transmission. Default value = 01h (2 ms)
- **SIO crlf** : **0BFD3Bh**
  - Specify the delimiter. External code is converted into internal delimiter (0Dh + 0Ah). Default value = 01h
  - Bits 1, 0 : 00 = not used, 01 = 0Dh, 10 = 0Ah, 11 = 0Dh + 0Ah
- **SIO eof code** : **0BFD3Ch**
  - To specify the end code. Default value = 1Ah
- **SIO open close wait** : **0BFD40h**
  - Wait n * 0.5 ms immediately after opening or immediately before closing. Default value = 04h (20 ms)
- **SIO open port control** : **0BFD41h**
  - Open of SIO port. Default value = 41h
  - Bit 6 ER = 0 : don't care
  - Bit 6 ER = 1 : ER signal becomes high at open and low at close
  - Bit 5 RR = 0 : don't care
  - Bit 5 RR = 1 : RR signal becomes high at open and low at close
  - Bit 4 RS = 0 : don't care
  - Bit 4 RS = 1 : RS signal becomes high at open and low at close
- **SIO send n byte wait** : **0BFD60h**
  - Specify insertion time of <00-0FFh> * 2 ms wait between send data 1 byte at sending. Default value = 00h (no wait)
- **SIO open send data** : **0BFD61h**
  - Default value = 11h
  - When SIO setup bit 6 is 1, SIO open send data is transferred by 1 byte at open
- **SIO close send data** : **0BFD62h**
  - Default value = 13h
  - When SIO setup bit 4 is 1, SIO close send data is transferred by 1 byte at close

**Transmission d'un octet :**

In case Xon-Xoff is specified, if the Xoff code is being received, the transmitting side
keeps waiting until the Xon code is received and the line is released.

And if the signal to be monitored (the port specified with `SIO send port condition`) is not
set ON (high level), it keeps waiting.

When the above conditions are satisfied and the CPU is ready and empty, 1 byte of data is output.
